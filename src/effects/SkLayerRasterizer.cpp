
/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#include "SkLayerRasterizer.h"
#include "SkDraw.h"
#include "SkReadBuffer.h"
#include "SkWriteBuffer.h"
#include "SkMask.h"
#include "SkMaskFilter.h"
#include "SkPaint.h"
#include "SkPath.h"
#include "SkPathEffect.h"
#include "../core/SkRasterClip.h"
#include "SkXfermode.h"
#include <new>

#if SK_SUPPORT_GPU
#include "GrStrokeInfo.h"
#include "GrContext.h"
#include "GrGpu.h"
#include "GrDrawTargetCaps.h"
#include "GrPaint.h"
#include "SkGr.h"
#endif

struct SkLayerRasterizer_Rec {
    SkPaint     fPaint;
    SkVector    fOffset;
};

SkLayerRasterizer::SkLayerRasterizer()
    : fLayers(SkNEW_ARGS(SkDeque, (sizeof(SkLayerRasterizer_Rec))))
{
}

SkLayerRasterizer::SkLayerRasterizer(SkDeque* layers) : fLayers(layers)
{
}

// Helper function to call destructors on SkPaints held by layers and delete layers.
static void clean_up_layers(SkDeque* layers) {
    SkDeque::F2BIter        iter(*layers);
    SkLayerRasterizer_Rec*  rec;

    while ((rec = (SkLayerRasterizer_Rec*)iter.next()) != NULL)
        rec->fPaint.~SkPaint();

    SkDELETE(layers);
}

SkLayerRasterizer::~SkLayerRasterizer() {
    SkASSERT(fLayers);
    clean_up_layers(const_cast<SkDeque*>(fLayers));
}

static bool compute_bounds(const SkDeque& layers, const SkPath& path,
                           const SkMatrix& matrix,
                           const SkIRect* clipBounds, SkIRect* bounds) {
    SkDeque::F2BIter        iter(layers);
    SkLayerRasterizer_Rec*  rec;

    bounds->set(SK_MaxS32, SK_MaxS32, SK_MinS32, SK_MinS32);

    while ((rec = (SkLayerRasterizer_Rec*)iter.next()) != NULL) {
        const SkPaint&  paint = rec->fPaint;
        SkPath          fillPath, devPath;
        const SkPath*   p = &path;

        if (paint.getPathEffect() || paint.getStyle() != SkPaint::kFill_Style) {
            paint.getFillPath(path, &fillPath);
            p = &fillPath;
        }
        if (p->isEmpty()) {
            continue;
        }

        // apply the matrix and offset
        {
            SkMatrix m = matrix;
            m.preTranslate(rec->fOffset.fX, rec->fOffset.fY);
            p->transform(m, &devPath);
        }

        SkMask  mask;
        if (!SkDraw::DrawToMask(devPath, clipBounds, paint.getMaskFilter(),
                                &matrix, &mask,
                                SkMask::kJustComputeBounds_CreateMode,
                                SkPaint::kFill_Style)) {
            return false;
        }

        bounds->join(mask.fBounds);
    }
    return true;
}

bool SkLayerRasterizer::onRasterize(const SkPath& path, const SkMatrix& matrix,
                                    const SkIRect* clipBounds,
                                    SkMask* mask, SkMask::CreateMode mode) const {
    SkASSERT(fLayers);
    if (fLayers->empty()) {
        return false;
    }

    if (SkMask::kJustRenderImage_CreateMode != mode) {
        if (!compute_bounds(*fLayers, path, matrix, clipBounds, &mask->fBounds))
            return false;
    }

    if (SkMask::kComputeBoundsAndRenderImage_CreateMode == mode) {
        mask->fFormat   = SkMask::kA8_Format;
        mask->fRowBytes = mask->fBounds.width();
        size_t size = mask->computeImageSize();
        if (0 == size) {
            return false;   // too big to allocate, abort
        }
        mask->fImage = SkMask::AllocImage(size);
        memset(mask->fImage, 0, size);
    }

    if (SkMask::kJustComputeBounds_CreateMode != mode) {
        SkBitmap        device;
        SkRasterClip    rectClip;
        SkDraw          draw;
        SkMatrix        translatedMatrix;  // this translates us to our local pixels
        SkMatrix        drawMatrix;        // this translates the path by each layer's offset

        rectClip.setRect(SkIRect::MakeWH(mask->fBounds.width(), mask->fBounds.height()));

        translatedMatrix = matrix;
        translatedMatrix.postTranslate(-SkIntToScalar(mask->fBounds.fLeft),
                                       -SkIntToScalar(mask->fBounds.fTop));

        device.installMaskPixels(*mask);

        draw.fBitmap    = &device;
        draw.fMatrix    = &drawMatrix;
        draw.fRC        = &rectClip;
        draw.fClip      = &rectClip.bwRgn();
        // we set the matrixproc in the loop, as the matrix changes each time (potentially)

        SkDeque::F2BIter        iter(*fLayers);
        SkLayerRasterizer_Rec*  rec;

        while ((rec = (SkLayerRasterizer_Rec*)iter.next()) != NULL) {
            drawMatrix = translatedMatrix;
            drawMatrix.preTranslate(rec->fOffset.fX, rec->fOffset.fY);
            draw.drawPath(path, rec->fPaint);
        }
    }
    return true;
}

#if SK_SUPPORT_GPU
bool SkLayerRasterizer::canRasterizeGPU(const SkPath& path,
                                        const SkIRect& clipBounds,
                                        const SkMatrix& matrix,
                                        SkMaskFilter* filter,
                                        SkIRect* rasterRect) const {
    SkDeque::F2BIter iter(*fLayers);
    SkLayerRasterizer_Rec* rec;

    while ((rec = (SkLayerRasterizer_Rec*)iter.next()) != NULL) {
        if (rec->fPaint.getMaskFilter() ||
            rec->fPaint.getRasterizer())
            return false;
    }

    SkIRect pathBounds;
    SkRect b = path.getBounds();
    matrix.mapRect(&b);

    SkIRect storage;
    SkIRect bounds = clipBounds;

    if (filter) {
        SkIPoint margin;
        SkMask srcM, dstM;

        srcM.fFormat = SkMask::kA8_Format;
        srcM.fBounds.set(0, 0, 1, 1);
        srcM.fImage = NULL;

        if (!filter->filterMask(&dstM, srcM, matrix, &margin))
            return false;

        storage = clipBounds;
        storage.inset(-margin.fX, -margin.fY);
        pathBounds.inset(-margin.fX, -margin.fY);
        bounds = storage;
    }

    pathBounds.intersect(bounds);
    if (rasterRect)
            *rasterRect = pathBounds;
    return true;
}

bool SkLayerRasterizer::onRasterizeGPU(GrContext* context,
                                       const SkPath& path,
                                       const SkMatrix& matrix,
                                       const SkIRect* clipBounds, bool doAA,
                                       SkStrokeRec* stroke,
                                       GrTexture** result,
                                       SkMask::CreateMode mode) const {
    SkASSERT(fLayers);
    if (fLayers->empty() || !context)
        return false;

    if (SkMask::kComputeBoundsAndRenderImage_CreateMode == mode) {
        GrTextureDesc desc;
        desc.fFlags = kRenderTarget_GrTextureFlagBit;
        desc.fWidth = clipBounds->width();
        desc.fHeight = clipBounds->height();
        desc.fSampleCnt = 0;
        if (doAA) {
            int maxSampleCnt = context->getGpu()->caps()->maxSampleCount();
            // FIXME: default to 4?
            desc.fSampleCnt = maxSampleCnt >= 4 ? 4 : maxSampleCnt;
        }
        desc.fConfig = kRGBA_8888_GrPixelConfig;

        bool msaa = desc.fSampleCnt > 0;
        if (context->isConfigRenderable(kAlpha_8_GrPixelConfig, msaa))
            desc.fConfig = kAlpha_8_GrPixelConfig;

        // fine a texture that has approx match
        GrTexture* texture = context->refScratchTexture(desc,
            GrContext::kApprox_ScratchTexMatch);
        if (!texture)
            return false;

        GrContext::AutoRenderTarget art(context, texture->asRenderTarget());
        SkRect clipRect = SkRect::MakeWH(SkIntToScalar(clipBounds->width()), SkIntToScalar(clipBounds->height()));
        GrContext::AutoClip ac(context, clipRect);
        context->clear(NULL, 0x0, true, texture->asRenderTarget());

        SkMatrix translatedMatrix = matrix;
        translatedMatrix.postTranslate(-SkIntToScalar(clipBounds->fLeft),
                                       -SkIntToScalar(clipBounds->fTop));

        SkMatrix drawMatrix; // this translates the path by each layer's offset
        // we set the matrixproc in the loop as matrix changes each time (potentially)
        SkDeque::F2BIter iter(*fLayers);
        SkLayerRasterizer_Rec* rec;

        while ((rec = (SkLayerRasterizer_Rec*)iter.next()) != NULL) {
           drawMatrix = translatedMatrix;
           drawMatrix.preTranslate(rec->fOffset.fX, rec->fOffset.fY);
           GrContext::AutoMatrix amx;
           amx.set(context, drawMatrix, NULL);
           GrPaint grPaint;
           SkPaint2GrPaintShader(context, rec->fPaint, true, &grPaint);
           // we use alpha only
           grPaint.setColor(0xffffffff);
           if (doAA) {
                // see comments in SkGpuDevice.cpp, for MSAA render target,
                // it is not necessary to set this
                grPaint.setBlendFunc(kOne_GrBlendCoeff, kISC_GrBlendCoeff);
            }

            SkPath* pathPtr = const_cast<SkPath*>(&path);
            SkTLazy<SkPath> effectPath;

            SkPathEffect* pathEffect = rec->fPaint.getPathEffect();
            const SkRect* cullRect = NULL;
            if (pathEffect && pathEffect->filterPath(effectPath.init(),
                                                     *pathPtr, stroke,
                                                     cullRect)) {
                pathPtr = effectPath.get();
            }

            GrStrokeInfo strokeInfo(rec->fPaint);
            context->drawPath(grPaint, *pathPtr, strokeInfo);
        }

        *result = texture;
    }

    return true;
}
#endif

#ifdef SK_SUPPORT_LEGACY_DEEPFLATTENING
SkLayerRasterizer::SkLayerRasterizer(SkReadBuffer& buffer)
    : SkRasterizer(buffer), fLayers(ReadLayers(buffer)) {}
#endif

SkFlattenable* SkLayerRasterizer::CreateProc(SkReadBuffer& buffer) {
    return SkNEW_ARGS(SkLayerRasterizer, (ReadLayers(buffer)));
}

SkDeque* SkLayerRasterizer::ReadLayers(SkReadBuffer& buffer) {
    int count = buffer.readInt();
    
    SkDeque* layers = SkNEW_ARGS(SkDeque, (sizeof(SkLayerRasterizer_Rec)));
    for (int i = 0; i < count; i++) {
        SkLayerRasterizer_Rec* rec = (SkLayerRasterizer_Rec*)layers->push_back();

        SkNEW_PLACEMENT(&rec->fPaint, SkPaint);
        buffer.readPaint(&rec->fPaint);
        buffer.readPoint(&rec->fOffset);
    }
    return layers;
}

void SkLayerRasterizer::flatten(SkWriteBuffer& buffer) const {
    this->INHERITED::flatten(buffer);

    SkASSERT(fLayers);
    buffer.writeInt(fLayers->count());

    SkDeque::F2BIter                iter(*fLayers);
    const SkLayerRasterizer_Rec*    rec;

    while ((rec = (const SkLayerRasterizer_Rec*)iter.next()) != NULL) {
        buffer.writePaint(rec->fPaint);
        buffer.writePoint(rec->fOffset);
    }
}

SkLayerRasterizer::Builder::Builder()
        : fLayers(SkNEW_ARGS(SkDeque, (sizeof(SkLayerRasterizer_Rec))))
{
}

SkLayerRasterizer::Builder::~Builder()
{
    if (fLayers != NULL) {
        clean_up_layers(fLayers);
    }
}

void SkLayerRasterizer::Builder::addLayer(const SkPaint& paint, SkScalar dx,
                                          SkScalar dy) {
    SkASSERT(fLayers);
    SkLayerRasterizer_Rec* rec = (SkLayerRasterizer_Rec*)fLayers->push_back();

    SkNEW_PLACEMENT_ARGS(&rec->fPaint, SkPaint, (paint));
    rec->fOffset.set(dx, dy);
}

SkLayerRasterizer* SkLayerRasterizer::Builder::detachRasterizer() {
    SkLayerRasterizer* rasterizer;
    if (0 == fLayers->count()) {
        rasterizer = NULL;
        SkDELETE(fLayers);
    } else {
        rasterizer = SkNEW_ARGS(SkLayerRasterizer, (fLayers));
    }
    fLayers = NULL;
    return rasterizer;
}

SkLayerRasterizer* SkLayerRasterizer::Builder::snapshotRasterizer() const {
    if (0 == fLayers->count()) {
        return NULL;
    }
    SkDeque* layers = SkNEW_ARGS(SkDeque, (sizeof(SkLayerRasterizer_Rec), fLayers->count()));
    SkDeque::F2BIter                iter(*fLayers);
    const SkLayerRasterizer_Rec*    recOrig;
    SkDEBUGCODE(int                 count = 0;)
    while ((recOrig = static_cast<SkLayerRasterizer_Rec*>(iter.next())) != NULL) {
        SkDEBUGCODE(count++);
        SkLayerRasterizer_Rec* recCopy = static_cast<SkLayerRasterizer_Rec*>(layers->push_back());
        SkNEW_PLACEMENT_ARGS(&recCopy->fPaint, SkPaint, (recOrig->fPaint));
        recCopy->fOffset = recOrig->fOffset;
    }
    SkASSERT(fLayers->count() == count);
    SkASSERT(layers->count() == count);
    SkLayerRasterizer* rasterizer = SkNEW_ARGS(SkLayerRasterizer, (layers));
    return rasterizer;
}
