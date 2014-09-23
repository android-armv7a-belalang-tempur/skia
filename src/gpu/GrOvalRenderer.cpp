/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrOvalRenderer.h"

#include "GrEffect.h"
#include "gl/GrGLEffect.h"
#include "gl/GrGLSL.h"
#include "gl/GrGLVertexEffect.h"
#include "GrTBackendEffectFactory.h"

#include "GrDrawState.h"
#include "GrDrawTarget.h"
#include "GrGpu.h"

#include "SkRRect.h"
#include "SkStrokeRec.h"
#include "SkTLazy.h"

#include "effects/GrVertexEffect.h"
#include "effects/GrRRectEffect.h"

namespace {

struct CircleVertex {
    SkPoint  fPos;
    SkPoint  fOffset;
    SkScalar fOuterRadius;
    SkScalar fInnerRadius;
    GrColor  fColor;
};

struct CircleUVVertex {
    SkPoint  fPos;
    SkPoint  fOffset;
    SkScalar fOuterRadius;
    SkScalar fInnerRadius;
    GrColor  fColor;
    SkPoint  fLocalPos;
};

struct EllipseVertex {
    SkPoint  fPos;
    SkPoint  fOffset;
    SkPoint  fOuterRadii;
    SkPoint  fInnerRadii;
    GrColor  fColor;
};

struct EllipseUVVertex {
    SkPoint  fPos;
    SkPoint  fOffset;
    SkPoint  fOuterRadii;
    SkPoint  fInnerRadii;
    GrColor  fColor;
    SkPoint  fLocalPos;
};

struct DIEllipseVertex {
    SkPoint  fPos;
    SkPoint  fOuterOffset;
    SkPoint  fInnerOffset;
    GrColor  fColor;
};

struct DIEllipseUVVertex {
    SkPoint  fPos;
    SkPoint  fOuterOffset;
    SkPoint  fInnerOffset;
    GrColor  fColor;
    SkPoint  fLocalPos;
};

inline bool circle_stays_circle(const SkMatrix& m) {
    return m.isSimilarity();
}

}

///////////////////////////////////////////////////////////////////////////////

/**
 * The output of this effect is a modulation of the input color and coverage for a circle,
 * specified as offset_x, offset_y (both from center point), outer radius and inner radius.
 */

class CircleEdgeEffect : public GrVertexEffect {
public:
    static GrEffectRef* Create(bool stroke) {
        GR_CREATE_STATIC_EFFECT(gCircleStrokeEdge, CircleEdgeEffect, (true));
        GR_CREATE_STATIC_EFFECT(gCircleFillEdge, CircleEdgeEffect, (false));

        if (stroke) {
            gCircleStrokeEdge->ref();
            return gCircleStrokeEdge;
        } else {
            gCircleFillEdge->ref();
            return gCircleFillEdge;
        }
    }

    virtual void getConstantColorComponents(GrColor* color,
                                            uint32_t* validFlags) const SK_OVERRIDE {
        *validFlags = 0;
    }

    virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE {
        return GrTBackendEffectFactory<CircleEdgeEffect>::getInstance();
    }

    virtual ~CircleEdgeEffect() {}

    static const char* Name() { return "CircleEdge"; }

    inline bool isStroked() const { return fStroke; }

    class GLEffect : public GrGLVertexEffect {
    public:
        GLEffect(const GrBackendEffectFactory& factory, const GrDrawEffect&)
        : INHERITED (factory) {}

        virtual void emitCode(GrGLFullShaderBuilder* builder,
                              const GrDrawEffect& drawEffect,
                              EffectKey key,
                              const char* outputColor,
                              const char* inputColor,
                              const TransformedCoordsArray&,
                              const TextureSamplerArray& samplers) SK_OVERRIDE {
            const CircleEdgeEffect& circleEffect = drawEffect.castEffect<CircleEdgeEffect>();
            const char *vsName, *fsName;
            builder->addVarying(kVec4f_GrSLType, "CircleEdge", &vsName, &fsName);

            const SkString* attrName =
                builder->getEffectAttributeName(drawEffect.getVertexAttribIndices()[0]);
            builder->vsCodeAppendf("\t%s = %s;\n", vsName, attrName->c_str());

            builder->fsCodeAppendf("\tfloat d = length(%s.xy);\n", fsName);
            builder->fsCodeAppendf("\tfloat edgeAlpha = clamp(%s.z - d, 0.0, 1.0);\n", fsName);
            if (circleEffect.isStroked()) {
                builder->fsCodeAppendf("\tfloat innerAlpha = clamp(d - %s.w, 0.0, 1.0);\n", fsName);
                builder->fsCodeAppend("\tedgeAlpha *= innerAlpha;\n");
            }

            builder->fsCodeAppendf("\t%s = %s;\n", outputColor,
                                   (GrGLSLExpr4(inputColor) * GrGLSLExpr1("edgeAlpha")).c_str());
        }

        static inline EffectKey GenKey(const GrDrawEffect& drawEffect, const GrGLCaps&) {
            const CircleEdgeEffect& circleEffect = drawEffect.castEffect<CircleEdgeEffect>();

            return circleEffect.isStroked() ? 0x1 : 0x0;
        }

        virtual void setData(const GrGLUniformManager&, const GrDrawEffect&) SK_OVERRIDE {}

    private:
        typedef GrGLVertexEffect INHERITED;
    };


private:
    CircleEdgeEffect(bool stroke) : GrVertexEffect() {
        this->addVertexAttrib(kVec4f_GrSLType);
        fStroke = stroke;
    }

    virtual bool onIsEqual(const GrEffect& other) const SK_OVERRIDE {
        const CircleEdgeEffect& cee = CastEffect<CircleEdgeEffect>(other);
        return cee.fStroke == fStroke;
    }

    bool fStroke;

    GR_DECLARE_EFFECT_TEST;

    typedef GrVertexEffect INHERITED;
};

GR_DEFINE_EFFECT_TEST(CircleEdgeEffect);

GrEffectRef* CircleEdgeEffect::TestCreate(SkRandom* random,
                                          GrContext* context,
                                          const GrDrawTargetCaps&,
                                          GrTexture* textures[]) {
    return CircleEdgeEffect::Create(random->nextBool());
}

///////////////////////////////////////////////////////////////////////////////

/**
 * The output of this effect is a modulation of the input color and coverage for an axis-aligned
 * ellipse, specified as a 2D offset from center, and the reciprocals of the outer and inner radii,
 * in both x and y directions.
 *
 * We are using an implicit function of x^2/a^2 + y^2/b^2 - 1 = 0.
 */

class EllipseEdgeEffect : public GrVertexEffect {
public:
    static GrEffectRef* Create(bool stroke) {
        GR_CREATE_STATIC_EFFECT(gEllipseStrokeEdge, EllipseEdgeEffect, (true));
        GR_CREATE_STATIC_EFFECT(gEllipseFillEdge, EllipseEdgeEffect, (false));

        if (stroke) {
            gEllipseStrokeEdge->ref();
            return gEllipseStrokeEdge;
        } else {
            gEllipseFillEdge->ref();
            return gEllipseFillEdge;
        }
    }

    virtual void getConstantColorComponents(GrColor* color,
                                            uint32_t* validFlags) const SK_OVERRIDE {
        *validFlags = 0;
    }

    virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE {
        return GrTBackendEffectFactory<EllipseEdgeEffect>::getInstance();
    }

    virtual ~EllipseEdgeEffect() {}

    static const char* Name() { return "EllipseEdge"; }

    inline bool isStroked() const { return fStroke; }

    class GLEffect : public GrGLVertexEffect {
    public:
        GLEffect(const GrBackendEffectFactory& factory, const GrDrawEffect&)
        : INHERITED (factory) {}

        virtual void emitCode(GrGLFullShaderBuilder* builder,
                              const GrDrawEffect& drawEffect,
                              EffectKey key,
                              const char* outputColor,
                              const char* inputColor,
                              const TransformedCoordsArray&,
                              const TextureSamplerArray& samplers) SK_OVERRIDE {
            const EllipseEdgeEffect& ellipseEffect = drawEffect.castEffect<EllipseEdgeEffect>();

            const char *vsOffsetName, *fsOffsetName;
            const char *vsRadiiName, *fsRadiiName;

            builder->addVarying(kVec2f_GrSLType, "EllipseOffsets", &vsOffsetName, &fsOffsetName);
            const SkString* attr0Name =
                builder->getEffectAttributeName(drawEffect.getVertexAttribIndices()[0]);
            builder->vsCodeAppendf("\t%s = %s;\n", vsOffsetName, attr0Name->c_str());

            builder->addVarying(kVec4f_GrSLType, "EllipseRadii", &vsRadiiName, &fsRadiiName);
            const SkString* attr1Name =
                builder->getEffectAttributeName(drawEffect.getVertexAttribIndices()[1]);
            builder->vsCodeAppendf("\t%s = %s;\n", vsRadiiName, attr1Name->c_str());

            // for outer curve
            builder->fsCodeAppendf("\tvec2 scaledOffset = %s*%s.xy;\n", fsOffsetName, fsRadiiName);
            builder->fsCodeAppend("\tfloat test = dot(scaledOffset, scaledOffset) - 1.0;\n");
            builder->fsCodeAppendf("\tvec2 grad = 2.0*scaledOffset*%s.xy;\n", fsRadiiName);
            builder->fsCodeAppend("\tfloat grad_dot = dot(grad, grad);\n");
            // avoid calling inversesqrt on zero.
            builder->fsCodeAppend("\tgrad_dot = max(grad_dot, 1.0e-4);\n");
            builder->fsCodeAppend("\tfloat invlen = inversesqrt(grad_dot);\n");
            builder->fsCodeAppend("\tfloat edgeAlpha = clamp(0.5-test*invlen, 0.0, 1.0);\n");

            // for inner curve
            if (ellipseEffect.isStroked()) {
                builder->fsCodeAppendf("\tscaledOffset = %s*%s.zw;\n", fsOffsetName, fsRadiiName);
                builder->fsCodeAppend("\ttest = dot(scaledOffset, scaledOffset) - 1.0;\n");
                builder->fsCodeAppendf("\tgrad = 2.0*scaledOffset*%s.zw;\n", fsRadiiName);
                builder->fsCodeAppend("\tinvlen = inversesqrt(dot(grad, grad));\n");
                builder->fsCodeAppend("\tedgeAlpha *= clamp(0.5+test*invlen, 0.0, 1.0);\n");
            }

            builder->fsCodeAppendf("\t%s = %s;\n", outputColor,
                                   (GrGLSLExpr4(inputColor) * GrGLSLExpr1("edgeAlpha")).c_str());
        }

        static inline EffectKey GenKey(const GrDrawEffect& drawEffect, const GrGLCaps&) {
            const EllipseEdgeEffect& ellipseEffect = drawEffect.castEffect<EllipseEdgeEffect>();

            return ellipseEffect.isStroked() ? 0x1 : 0x0;
        }

        virtual void setData(const GrGLUniformManager&, const GrDrawEffect&) SK_OVERRIDE {
        }

    private:
        typedef GrGLVertexEffect INHERITED;
    };

private:
    EllipseEdgeEffect(bool stroke) : GrVertexEffect() {
        this->addVertexAttrib(kVec2f_GrSLType);
        this->addVertexAttrib(kVec4f_GrSLType);
        fStroke = stroke;
    }

    virtual bool onIsEqual(const GrEffect& other) const SK_OVERRIDE {
        const EllipseEdgeEffect& eee = CastEffect<EllipseEdgeEffect>(other);
        return eee.fStroke == fStroke;
    }

    bool fStroke;

    GR_DECLARE_EFFECT_TEST;

    typedef GrVertexEffect INHERITED;
};

GR_DEFINE_EFFECT_TEST(EllipseEdgeEffect);

GrEffectRef* EllipseEdgeEffect::TestCreate(SkRandom* random,
                                           GrContext* context,
                                           const GrDrawTargetCaps&,
                                           GrTexture* textures[]) {
    return EllipseEdgeEffect::Create(random->nextBool());
}

///////////////////////////////////////////////////////////////////////////////

/**
 * The output of this effect is a modulation of the input color and coverage for an ellipse,
 * specified as a 2D offset from center for both the outer and inner paths (if stroked). The
 * implict equation used is for a unit circle (x^2 + y^2 - 1 = 0) and the edge corrected by
 * using differentials.
 *
 * The result is device-independent and can be used with any affine matrix.
 */

class DIEllipseEdgeEffect : public GrVertexEffect {
public:
    enum Mode { kStroke = 0, kHairline, kFill };

    static GrEffectRef* Create(Mode mode) {
        GR_CREATE_STATIC_EFFECT(gEllipseStrokeEdge, DIEllipseEdgeEffect, (kStroke));
        GR_CREATE_STATIC_EFFECT(gEllipseHairlineEdge, DIEllipseEdgeEffect, (kHairline));
        GR_CREATE_STATIC_EFFECT(gEllipseFillEdge, DIEllipseEdgeEffect, (kFill));

        if (kStroke == mode) {
            gEllipseStrokeEdge->ref();
            return gEllipseStrokeEdge;
        } else if (kHairline == mode) {
            gEllipseHairlineEdge->ref();
            return gEllipseHairlineEdge;
        } else {
            gEllipseFillEdge->ref();
            return gEllipseFillEdge;
        }
    }

    virtual void getConstantColorComponents(GrColor* color,
                                            uint32_t* validFlags) const SK_OVERRIDE {
        *validFlags = 0;
    }

    virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE {
        return GrTBackendEffectFactory<DIEllipseEdgeEffect>::getInstance();
    }

    virtual ~DIEllipseEdgeEffect() {}

    static const char* Name() { return "DIEllipseEdge"; }

    inline Mode getMode() const { return fMode; }

    class GLEffect : public GrGLVertexEffect {
    public:
        GLEffect(const GrBackendEffectFactory& factory, const GrDrawEffect&)
        : INHERITED (factory) {}

        virtual void emitCode(GrGLFullShaderBuilder* builder,
                              const GrDrawEffect& drawEffect,
                              EffectKey key,
                              const char* outputColor,
                              const char* inputColor,
                              const TransformedCoordsArray&,
                              const TextureSamplerArray& samplers) SK_OVERRIDE {
            const DIEllipseEdgeEffect& ellipseEffect = drawEffect.castEffect<DIEllipseEdgeEffect>();

            SkAssertResult(builder->enableFeature(
                                              GrGLShaderBuilder::kStandardDerivatives_GLSLFeature));

            const char *vsOffsetName0, *fsOffsetName0;
            builder->addVarying(kVec2f_GrSLType, "EllipseOffsets0",
                                      &vsOffsetName0, &fsOffsetName0);
            const SkString* attr0Name =
                builder->getEffectAttributeName(drawEffect.getVertexAttribIndices()[0]);
            builder->vsCodeAppendf("\t%s = %s;\n", vsOffsetName0, attr0Name->c_str());
            const char *vsOffsetName1, *fsOffsetName1;
            builder->addVarying(kVec2f_GrSLType, "EllipseOffsets1",
                                      &vsOffsetName1, &fsOffsetName1);
            const SkString* attr1Name =
                builder->getEffectAttributeName(drawEffect.getVertexAttribIndices()[1]);
            builder->vsCodeAppendf("\t%s = %s;\n", vsOffsetName1, attr1Name->c_str());

            // for outer curve
            builder->fsCodeAppendf("\tvec2 scaledOffset = %s.xy;\n", fsOffsetName0);
            builder->fsCodeAppend("\tfloat test = dot(scaledOffset, scaledOffset) - 1.0;\n");
            builder->fsCodeAppendf("\tvec2 duvdx = dFdx(%s);\n", fsOffsetName0);
            builder->fsCodeAppendf("\tvec2 duvdy = dFdy(%s);\n", fsOffsetName0);
            builder->fsCodeAppendf("\tvec2 grad = vec2(2.0*%s.x*duvdx.x + 2.0*%s.y*duvdx.y,\n"
                                   "\t                 2.0*%s.x*duvdy.x + 2.0*%s.y*duvdy.y);\n",
                                   fsOffsetName0, fsOffsetName0, fsOffsetName0, fsOffsetName0);

            builder->fsCodeAppend("\tfloat grad_dot = dot(grad, grad);\n");
            // avoid calling inversesqrt on zero.
            builder->fsCodeAppend("\tgrad_dot = max(grad_dot, 1.0e-4);\n");
            builder->fsCodeAppend("\tfloat invlen = inversesqrt(grad_dot);\n");
            if (kHairline == ellipseEffect.getMode()) {
                // can probably do this with one step
                builder->fsCodeAppend("\tfloat edgeAlpha = clamp(1.0-test*invlen, 0.0, 1.0);\n");
                builder->fsCodeAppend("\tedgeAlpha *= clamp(1.0+test*invlen, 0.0, 1.0);\n");
            } else {
                builder->fsCodeAppend("\tfloat edgeAlpha = clamp(0.5-test*invlen, 0.0, 1.0);\n");
            }

            // for inner curve
            if (kStroke == ellipseEffect.getMode()) {
                builder->fsCodeAppendf("\tscaledOffset = %s.xy;\n", fsOffsetName1);
                builder->fsCodeAppend("\ttest = dot(scaledOffset, scaledOffset) - 1.0;\n");
                builder->fsCodeAppendf("\tduvdx = dFdx(%s);\n", fsOffsetName1);
                builder->fsCodeAppendf("\tduvdy = dFdy(%s);\n", fsOffsetName1);
                builder->fsCodeAppendf("\tgrad = vec2(2.0*%s.x*duvdx.x + 2.0*%s.y*duvdx.y,\n"
                                       "\t            2.0*%s.x*duvdy.x + 2.0*%s.y*duvdy.y);\n",
                                       fsOffsetName1, fsOffsetName1, fsOffsetName1, fsOffsetName1);
                builder->fsCodeAppend("\tinvlen = inversesqrt(dot(grad, grad));\n");
                builder->fsCodeAppend("\tedgeAlpha *= clamp(0.5+test*invlen, 0.0, 1.0);\n");
            }

            builder->fsCodeAppendf("\t%s = %s;\n", outputColor,
                                   (GrGLSLExpr4(inputColor) * GrGLSLExpr1("edgeAlpha")).c_str());
        }

        static inline EffectKey GenKey(const GrDrawEffect& drawEffect, const GrGLCaps&) {
            const DIEllipseEdgeEffect& ellipseEffect = drawEffect.castEffect<DIEllipseEdgeEffect>();

            return ellipseEffect.getMode();
        }

        virtual void setData(const GrGLUniformManager&, const GrDrawEffect&) SK_OVERRIDE {
        }

    private:
        typedef GrGLVertexEffect INHERITED;
    };

private:
    DIEllipseEdgeEffect(Mode mode) : GrVertexEffect() {
        this->addVertexAttrib(kVec2f_GrSLType);
        this->addVertexAttrib(kVec2f_GrSLType);
        fMode = mode;
    }

    virtual bool onIsEqual(const GrEffect& other) const SK_OVERRIDE {
        const DIEllipseEdgeEffect& eee = CastEffect<DIEllipseEdgeEffect>(other);
        return eee.fMode == fMode;
    }

    Mode fMode;

    GR_DECLARE_EFFECT_TEST;

    typedef GrVertexEffect INHERITED;
};

GR_DEFINE_EFFECT_TEST(DIEllipseEdgeEffect);

GrEffectRef* DIEllipseEdgeEffect::TestCreate(SkRandom* random,
                                             GrContext* context,
                                             const GrDrawTargetCaps&,
                                             GrTexture* textures[]) {
    return DIEllipseEdgeEffect::Create((Mode)(random->nextRangeU(0,2)));
}

///////////////////////////////////////////////////////////////////////////////

void GrOvalRenderer::reset() {
    SkSafeSetNull(fRRectFillIndexBuffer);
    SkSafeSetNull(fRRectStrokeIndexBuffer);
    SkSafeSetNull(fOvalIndexBuffer);
}

bool GrOvalRenderer::drawOval(GrDrawTarget* target, const GrContext* context, bool useAA,
                              const SkRect& oval, const SkStrokeRec& stroke)
{
    bool useCoverageAA = useAA && !target->shouldDisableCoverageAAForBlend();

    if (!useCoverageAA) {
        return false;
    }

    const SkMatrix& vm = context->getMatrix();

    // we can draw circles
    if (SkScalarNearlyEqual(oval.width(), oval.height())
        && circle_stays_circle(vm)) {
        this->drawCircle(target, useCoverageAA, oval, stroke);
    // if we have shader derivative support, render as device-independent
    } else if (target->caps()->shaderDerivativeSupport()) {
        return this->drawDIEllipse(target, useCoverageAA, oval, stroke);
    // otherwise axis-aligned ellipses only
    } else if (vm.rectStaysRect()) {
        return this->drawEllipse(target, useCoverageAA, oval, stroke);
    } else {
        return false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////

// position + edge
extern const GrVertexAttrib gCircleVertexAttribs[] = {
    {kVec2f_GrVertexAttribType, 0,               kPosition_GrVertexAttribBinding},
    {kVec4f_GrVertexAttribType, sizeof(SkPoint), kEffect_GrVertexAttribBinding},
    {kVec4ub_GrVertexAttribType, sizeof(SkPoint)+sizeof(SkPoint)*2, kColor_GrVertexAttribBinding}
};

extern const GrVertexAttrib gCircleUVVertexAttribs[] = {
    {kVec2f_GrVertexAttribType, 0,               kPosition_GrVertexAttribBinding},
    {kVec4f_GrVertexAttribType, sizeof(SkPoint), kEffect_GrVertexAttribBinding},
    {kVec4ub_GrVertexAttribType, sizeof(SkPoint)+sizeof(SkPoint)*2, kColor_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint)*3+sizeof(uint32_t), kLocalCoord_GrVertexAttribBinding}
};
///////////////////////////////////////////////////////////////////////////////

static const uint16_t gOvalIndices[] = {
    // corners
    0, 1, 2, 1, 2, 3
};

static const int MAX_OVALS = 1170; // 32768 * 4 / (28 * 4)

static inline void fill_indices(uint16_t *indices, const uint16_t *src,
                                const int srcSize, const int indicesCount, const int count)
{
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < srcSize; j++)
            indices[i * srcSize + j] = src[j] + i * indicesCount;
    }
}

GrIndexBuffer* GrOvalRenderer::ovalIndexBuffer(GrGpu* gpu) {
    if (NULL == fOvalIndexBuffer) {
        static const int SIZE = sizeof(gOvalIndices) * MAX_OVALS;
        fOvalIndexBuffer = gpu->createIndexBuffer(SIZE, false);
        if (NULL != fOvalIndexBuffer) {
            // FIXME use lock()/unlock() when port to later revision
            uint16_t *indices = (uint16_t *)fOvalIndexBuffer->map();
            if (NULL != indices) {
                fill_indices(indices, gOvalIndices,
                             sizeof(gOvalIndices)/sizeof(uint16_t),
                             4, MAX_OVALS);
                fOvalIndexBuffer->unmap();
            } else {
                indices = (uint16_t *)sk_malloc_throw(SIZE);
                fill_indices(indices, static_cast<const uint16_t *>(gOvalIndices),
                             sizeof(gOvalIndices) / sizeof(uint16_t),
                             4, MAX_OVALS);
                if (!fOvalIndexBuffer->updateData(indices, SIZE)) {
                    fOvalIndexBuffer->unref();
                    fOvalIndexBuffer = NULL;
                }
                sk_free(indices);
            }
        }
    }
    return fOvalIndexBuffer;
}
void GrOvalRenderer::drawCircle(GrDrawTarget* target,
                                bool useCoverageAA,
                                const SkRect& circle,
                                const SkStrokeRec& stroke)
{
    GrDrawState* drawState = target->drawState();
    GrColor color = drawState->getColor();
    GrContext* context = drawState->getRenderTarget()->getContext();
    bool useUV = false;
    SkMatrix localMatrixInv;

    const SkMatrix& vm = drawState->getViewMatrix();
    SkPoint center = SkPoint::Make(circle.centerX(), circle.centerY());
    vm.mapPoints(&center, 1);
    SkScalar radius = vm.mapRadius(SkScalarHalf(circle.width()));
    SkScalar strokeWidth = vm.mapRadius(stroke.getWidth());
    SkScalar localStrokeWidth = stroke.getWidth();
    SkScalar localRadius = SkScalarHalf(circle.width());

    GrDrawState::AutoViewMatrixRestore avmr;
    if (!avmr.setIdentity(drawState)) {
        return;
    }

    GrIndexBuffer* indexBuffer = this->ovalIndexBuffer(context->getGpu());
    if (NULL == indexBuffer) {
        GrPrintf("Failed to create index buffer for oval!\n");
        return;
    }

    // we set draw state's color to white here so that any batching performane in onDraw()
    // won't get a false from GrDrawState::op== due to a color mismatch
    GrDrawState::AutoColorRestore acr;
    acr.set(drawState, 0xFFFFFFFF);

    // use local coords for shader is bitmap
    if (drawState->canOptimizeForBitmapShader()) {
        const SkMatrix& localMatrix = drawState->getLocalMatrix();
        if (localMatrix.invert(&localMatrixInv)) {
            GrDrawState::AutoLocalMatrixChange almc;
            almc.set(drawState);
            useUV = true;
        }
    }

    if (!useUV) {
        drawState->setVertexAttribs<gCircleVertexAttribs>(SK_ARRAY_COUNT(gCircleVertexAttribs));
        SkASSERT(sizeof(CircleVertex) == drawState->getVertexSize());
    } else {
        drawState->setVertexAttribs<gCircleUVVertexAttribs>(SK_ARRAY_COUNT(gCircleUVVertexAttribs));
        SkASSERT(sizeof(CircleUVVertex) == drawState->getVertexSize());
    }

    GrDrawTarget::AutoReleaseGeometry geo(target, 4, 0);
    if (!geo.succeeded()) {
        GrPrintf("Failed to get space for vertices!\n");
        return;
    }

    SkStrokeRec::Style style = stroke.getStyle();
    bool isStrokeOnly = SkStrokeRec::kStroke_Style == style ||
                        SkStrokeRec::kHairline_Style == style;
    bool hasStroke = isStrokeOnly || SkStrokeRec::kStrokeAndFill_Style == style;

    SkScalar innerRadius = 0.0f;
    SkScalar outerRadius = radius;
    SkScalar halfWidth = 0;
    SkScalar localHalfWidth = 0;
    SkScalar localOuterRadius = localRadius;
    if (hasStroke) {
        if (SkScalarNearlyZero(strokeWidth)) {
            halfWidth = SK_ScalarHalf;
            localHalfWidth = SK_ScalarHalf;
        } else {
            halfWidth = SkScalarHalf(strokeWidth);
            localHalfWidth = SkScalarHalf(localStrokeWidth);
        }

        outerRadius += halfWidth;
        localOuterRadius += localHalfWidth;
        if (isStrokeOnly) {
            innerRadius = radius - halfWidth;
        }
    }

    GrEffectRef* effect = CircleEdgeEffect::Create(isStrokeOnly && innerRadius > 0);
    static const int kCircleEdgeAttrIndex = 1;
    drawState->addCoverageEffect(effect, kCircleEdgeAttrIndex)->unref();

    // The radii are outset for two reasons. First, it allows the shader to simply perform
    // clamp(distance-to-center - radius, 0, 1). Second, the outer radius is used to compute the
    // verts of the bounding box that is rendered and the outset ensures the box will cover all
    // pixels partially covered by the circle.
    outerRadius += SK_ScalarHalf;
    innerRadius -= SK_ScalarHalf;
    localOuterRadius += SK_ScalarHalf;

    SkRect bounds = SkRect::MakeLTRB(
        center.fX - outerRadius,
        center.fY - outerRadius,
        center.fX + outerRadius,
        center.fY + outerRadius
    );

    SkRect localBounds = SkRect::MakeLTRB(
        circle.centerX() - localOuterRadius,
        circle.centerY() - localOuterRadius,
        circle.centerX() + localOuterRadius,
        circle.centerY() + localOuterRadius
    );

    if (!useUV) {
        CircleVertex* verts = reinterpret_cast<CircleVertex*>(geo.vertices());

        verts[0].fPos = SkPoint::Make(bounds.fLeft,  bounds.fTop);
        verts[0].fOffset = SkPoint::Make(-outerRadius, -outerRadius);
        verts[0].fOuterRadius = outerRadius;
        verts[0].fInnerRadius = innerRadius;
        verts[0].fColor = color;

        verts[1].fPos = SkPoint::Make(bounds.fRight, bounds.fTop);
        verts[1].fOffset = SkPoint::Make(outerRadius, -outerRadius);
        verts[1].fOuterRadius = outerRadius;
        verts[1].fInnerRadius = innerRadius;
        verts[1].fColor = color;

        verts[2].fPos = SkPoint::Make(bounds.fLeft,  bounds.fBottom);
        verts[2].fOffset = SkPoint::Make(-outerRadius, outerRadius);
        verts[2].fOuterRadius = outerRadius;
        verts[2].fInnerRadius = innerRadius;
        verts[2].fColor = color;

        verts[3].fPos = SkPoint::Make(bounds.fRight, bounds.fBottom);
        verts[3].fOffset = SkPoint::Make(outerRadius, outerRadius);
        verts[3].fOuterRadius = outerRadius;
        verts[3].fInnerRadius = innerRadius;
        verts[3].fColor = color;
    }
    else {
        CircleUVVertex* verts = reinterpret_cast<CircleUVVertex*>(geo.vertices());

        SkRect localRect;
        localMatrixInv.mapRect(&localRect, localBounds);

        verts[0].fPos = SkPoint::Make(bounds.fLeft,  bounds.fTop);
        verts[0].fOffset = SkPoint::Make(-outerRadius, -outerRadius);
        verts[0].fOuterRadius = outerRadius;
        verts[0].fInnerRadius = innerRadius;
        verts[0].fColor = color;
        verts[0].fLocalPos = SkPoint::Make(localRect.fLeft, localRect.fTop);

        verts[1].fPos = SkPoint::Make(bounds.fRight, bounds.fTop);
        verts[1].fOffset = SkPoint::Make(outerRadius, -outerRadius);
        verts[1].fOuterRadius = outerRadius;
        verts[1].fInnerRadius = innerRadius;
        verts[1].fColor = color;
        verts[1].fLocalPos = SkPoint::Make(localRect.fRight, localRect.fTop);

        verts[2].fPos = SkPoint::Make(bounds.fLeft,  bounds.fBottom);
        verts[2].fOffset = SkPoint::Make(-outerRadius, outerRadius);
        verts[2].fOuterRadius = outerRadius;
        verts[2].fInnerRadius = innerRadius;
        verts[2].fColor = color;
        verts[2].fLocalPos = SkPoint::Make(localRect.fLeft, localRect.fBottom);

        verts[3].fPos = SkPoint::Make(bounds.fRight, bounds.fBottom);
        verts[3].fOffset = SkPoint::Make(outerRadius, outerRadius);
        verts[3].fOuterRadius = outerRadius;
        verts[3].fInnerRadius = innerRadius;
        verts[3].fColor = color;
        verts[3].fLocalPos = SkPoint::Make(localRect.fRight, localRect.fBottom);
    }

    target->setIndexSourceToBuffer(indexBuffer);
    target->drawIndexedInstances(kTriangles_GrPrimitiveType, 1, 4, 6, &bounds);
}

///////////////////////////////////////////////////////////////////////////////

// position + offset + 1/radii
extern const GrVertexAttrib gEllipseVertexAttribs[] = {
    {kVec2f_GrVertexAttribType, 0,                 kPosition_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint),   kEffect_GrVertexAttribBinding},
    {kVec4f_GrVertexAttribType, 2*sizeof(SkPoint), kEffect_GrVertexAttribBinding},
    {kVec4ub_GrVertexAttribType, sizeof(SkPoint)*2+sizeof(SkPoint)*2, kColor_GrVertexAttribBinding}
};

extern const GrVertexAttrib gEllipseUVVertexAttribs[] = {
    {kVec2f_GrVertexAttribType, 0,                 kPosition_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint),   kEffect_GrVertexAttribBinding},
    {kVec4f_GrVertexAttribType, 2*sizeof(SkPoint), kEffect_GrVertexAttribBinding},
    {kVec4ub_GrVertexAttribType, sizeof(SkPoint)*2+sizeof(SkPoint)*2, kColor_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint)*4+sizeof(uint32_t), kLocalCoord_GrVertexAttribBinding}
};
// position + offsets
extern const GrVertexAttrib gDIEllipseVertexAttribs[] = {
    {kVec2f_GrVertexAttribType, 0,                 kPosition_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint),   kEffect_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, 2*sizeof(SkPoint), kEffect_GrVertexAttribBinding},
    {kVec4ub_GrVertexAttribType, sizeof(SkPoint)+sizeof(SkPoint)*2, kColor_GrVertexAttribBinding}
};

extern const GrVertexAttrib gDIEllipseUVVertexAttribs[] = {
    {kVec2f_GrVertexAttribType, 0,                 kPosition_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint),   kEffect_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, 2*sizeof(SkPoint), kEffect_GrVertexAttribBinding},
    {kVec4ub_GrVertexAttribType, sizeof(SkPoint)+sizeof(SkPoint)*2, kColor_GrVertexAttribBinding},
    {kVec2f_GrVertexAttribType, sizeof(SkPoint)*3+sizeof(uint32_t), kLocalCoord_GrVertexAttribBinding}
};

bool GrOvalRenderer::drawEllipse(GrDrawTarget* target,
                                 bool useCoverageAA,
                                 const SkRect& ellipse,
                                 const SkStrokeRec& stroke)
{
    GrDrawState* drawState = target->drawState();
    GrColor color = drawState->getColor();
#ifdef SK_DEBUG
    {
        // we should have checked for this previously
        bool isAxisAlignedEllipse = drawState->getViewMatrix().rectStaysRect();
        SkASSERT(useCoverageAA && isAxisAlignedEllipse);
    }
#endif

    // do any matrix crunching before we reset the draw state for device coords
    const SkMatrix& vm = drawState->getViewMatrix();
    SkPoint center = SkPoint::Make(ellipse.centerX(), ellipse.centerY());
    vm.mapPoints(&center, 1);
    SkScalar ellipseXRadius = SkScalarHalf(ellipse.width());
    SkScalar ellipseYRadius = SkScalarHalf(ellipse.height());
    SkScalar xRadius = SkScalarAbs(vm[SkMatrix::kMScaleX]*ellipseXRadius +
                                   vm[SkMatrix::kMSkewY]*ellipseYRadius);
    SkScalar yRadius = SkScalarAbs(vm[SkMatrix::kMSkewX]*ellipseXRadius +
                                   vm[SkMatrix::kMScaleY]*ellipseYRadius);

    // do (potentially) anisotropic mapping of stroke
    SkVector scaledStroke;
    SkScalar strokeWidth = stroke.getWidth();
    scaledStroke.fX = SkScalarAbs(strokeWidth*(vm[SkMatrix::kMScaleX] + vm[SkMatrix::kMSkewY]));
    scaledStroke.fY = SkScalarAbs(strokeWidth*(vm[SkMatrix::kMSkewX] + vm[SkMatrix::kMScaleY]));

    SkStrokeRec::Style style = stroke.getStyle();
    bool isStrokeOnly = SkStrokeRec::kStroke_Style == style ||
                        SkStrokeRec::kHairline_Style == style;
    bool hasStroke = isStrokeOnly || SkStrokeRec::kStrokeAndFill_Style == style;

    SkScalar innerXRadius = 0;
    SkScalar innerYRadius = 0;
    if (hasStroke) {
        if (SkScalarNearlyZero(scaledStroke.length())) {
            scaledStroke.set(SK_ScalarHalf, SK_ScalarHalf);
        } else {
            scaledStroke.scale(SK_ScalarHalf);
        }

        // we only handle thick strokes for near-circular ellipses
        if (scaledStroke.length() > SK_ScalarHalf &&
            (SK_ScalarHalf*xRadius > yRadius || SK_ScalarHalf*yRadius > xRadius)) {
            return false;
        }

        // we don't handle it if curvature of the stroke is less than curvature of the ellipse
        if (scaledStroke.fX*(yRadius*yRadius) < (scaledStroke.fY*scaledStroke.fY)*xRadius ||
            scaledStroke.fY*(xRadius*xRadius) < (scaledStroke.fX*scaledStroke.fX)*yRadius) {
            return false;
        }

        // this is legit only if scale & translation (which should be the case at the moment)
        if (isStrokeOnly) {
            innerXRadius = xRadius - scaledStroke.fX;
            innerYRadius = yRadius - scaledStroke.fY;
        }

        xRadius += scaledStroke.fX;
        yRadius += scaledStroke.fY;
    }

    GrDrawState::AutoViewMatrixRestore avmr;
    if (!avmr.setIdentity(drawState)) {
        return false;
    }

    drawState->setVertexAttribs<gEllipseVertexAttribs>(SK_ARRAY_COUNT(gEllipseVertexAttribs));
    SkASSERT(sizeof(EllipseVertex) == drawState->getVertexSize());

    GrDrawTarget::AutoReleaseGeometry geo(target, 4, 0);
    if (!geo.succeeded()) {
        GrPrintf("Failed to get space for vertices!\n");
        return false;
    }

    EllipseVertex* verts = reinterpret_cast<EllipseVertex*>(geo.vertices());

    GrEffectRef* effect = EllipseEdgeEffect::Create(isStrokeOnly &&
                                                    innerXRadius > 0 && innerYRadius > 0);

    static const int kEllipseCenterAttrIndex = 1;
    static const int kEllipseEdgeAttrIndex = 2;
    drawState->addCoverageEffect(effect, kEllipseCenterAttrIndex, kEllipseEdgeAttrIndex)->unref();

    // Compute the reciprocals of the radii here to save time in the shader
    SkScalar xRadRecip = SkScalarInvert(xRadius);
    SkScalar yRadRecip = SkScalarInvert(yRadius);
    SkScalar xInnerRadRecip = SkScalarInvert(innerXRadius);
    SkScalar yInnerRadRecip = SkScalarInvert(innerYRadius);

    // We've extended the outer x radius out half a pixel to antialias.
    // This will also expand the rect so all the pixels will be captured.
    // TODO: Consider if we should use sqrt(2)/2 instead
    xRadius += SK_ScalarHalf;
    yRadius += SK_ScalarHalf;

    SkRect bounds = SkRect::MakeLTRB(
        center.fX - xRadius,
        center.fY - yRadius,
        center.fX + xRadius,
        center.fY + yRadius
    );

    verts[0].fPos = SkPoint::Make(bounds.fLeft,  bounds.fTop);
    verts[0].fOffset = SkPoint::Make(-xRadius, -yRadius);
    verts[0].fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
    verts[0].fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
    verts[0].fColor = color;

    verts[1].fPos = SkPoint::Make(bounds.fRight, bounds.fTop);
    verts[1].fOffset = SkPoint::Make(xRadius, -yRadius);
    verts[1].fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
    verts[1].fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
    verts[1].fColor = color;

    verts[2].fPos = SkPoint::Make(bounds.fLeft,  bounds.fBottom);
    verts[2].fOffset = SkPoint::Make(-xRadius, yRadius);
    verts[2].fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
    verts[2].fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
    verts[2].fColor = color;

    verts[3].fPos = SkPoint::Make(bounds.fRight, bounds.fBottom);
    verts[3].fOffset = SkPoint::Make(xRadius, yRadius);
    verts[3].fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
    verts[3].fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
    verts[3].fColor = color;

    target->drawNonIndexed(kTriangleStrip_GrPrimitiveType, 0, 4, &bounds);

    return true;
}

bool GrOvalRenderer::drawDIEllipse(GrDrawTarget* target,
                                   bool useCoverageAA,
                                   const SkRect& ellipse,
                                   const SkStrokeRec& stroke)
{
    GrDrawState* drawState = target->drawState();
    const SkMatrix& vm = drawState->getViewMatrix();
    if(vm.getSkewX() != 0 || vm.getSkewY() != 0 ||
       vm.hasPerspective())
        return false;

    GrColor color = drawState->getColor();
    GrContext* context = drawState->getRenderTarget()->getContext();
    SkMatrix localMatrixInv;
    bool useUV = false;

    SkPoint center = SkPoint::Make(ellipse.centerX(), ellipse.centerY());
    SkScalar xRadius = SkScalarHalf(ellipse.width());
    SkScalar yRadius = SkScalarHalf(ellipse.height());
    SkPoint localCenter = center;
    SkScalar xLocalRadius = xRadius;
    SkScalar yLocalRadius = yRadius;

    SkStrokeRec::Style style = stroke.getStyle();
    DIEllipseEdgeEffect::Mode mode = (SkStrokeRec::kStroke_Style == style) ?
                                    DIEllipseEdgeEffect::kStroke :
                                    (SkStrokeRec::kHairline_Style == style) ?
                                    DIEllipseEdgeEffect::kHairline : DIEllipseEdgeEffect::kFill;

    SkScalar innerXRadius = 0;
    SkScalar innerYRadius = 0;
    if (SkStrokeRec::kFill_Style != style && SkStrokeRec::kHairline_Style != style) {
        SkScalar strokeWidth = stroke.getWidth();

        if (SkScalarNearlyZero(strokeWidth)) {
            strokeWidth = SK_ScalarHalf;
        } else {
            strokeWidth *= SK_ScalarHalf;
        }

        // we only handle thick strokes for near-circular ellipses
        if (strokeWidth > SK_ScalarHalf &&
            (SK_ScalarHalf*xRadius > yRadius || SK_ScalarHalf*yRadius > xRadius)) {
            return false;
        }

        // we don't handle it if curvature of the stroke is less than curvature of the ellipse
        if (strokeWidth*(yRadius*yRadius) < (strokeWidth*strokeWidth)*xRadius ||
            strokeWidth*(xRadius*xRadius) < (strokeWidth*strokeWidth)*yRadius) {
            return false;
        }

        // set inner radius (if needed)
        if (SkStrokeRec::kStroke_Style == style) {
            innerXRadius = xRadius - strokeWidth;
            innerYRadius = yRadius - strokeWidth;
        }

        xRadius += strokeWidth;
        yRadius += strokeWidth;
        xLocalRadius += strokeWidth;
        yLocalRadius += strokeWidth;
    }

    if (DIEllipseEdgeEffect::kStroke == mode) {
        mode = (innerXRadius > 0 && innerYRadius > 0) ? DIEllipseEdgeEffect::kStroke :
                                                        DIEllipseEdgeEffect::kFill;
    }
    SkScalar innerRatioX = SkScalarDiv(xRadius, innerXRadius);
    SkScalar innerRatioY = SkScalarDiv(yRadius, innerYRadius);

    GrIndexBuffer* indexBuffer = this->ovalIndexBuffer(context->getGpu());
    if (NULL == indexBuffer) {
        GrPrintf("Failed to create index buffer for oval!\n");
        return false;
    }

    // we set draw state's color to white here so that any batching performane in onDraw()
    // won't get a false from GrDrawState::op== due to a color mismatch
    GrDrawState::AutoColorRestore acr;
    acr.set(drawState, 0xFFFFFFFF);

    // use local coords for shader is bitmap
    if (drawState->canOptimizeForBitmapShader()) {
        const SkMatrix& localMatrix = drawState->getLocalMatrix();
        if (localMatrix.invert(&localMatrixInv)) {
            GrDrawState::AutoLocalMatrixChange almc;
            almc.set(drawState);
            useUV = true;
        }
    }

    if (!useUV) {
        drawState->setVertexAttribs<gDIEllipseVertexAttribs>(SK_ARRAY_COUNT(gDIEllipseVertexAttribs));
        SkASSERT(sizeof(DIEllipseVertex) == drawState->getVertexSize());
    } else {
        drawState->setVertexAttribs<gDIEllipseUVVertexAttribs>(SK_ARRAY_COUNT(gDIEllipseUVVertexAttribs));
        SkASSERT(sizeof(DIEllipseUVVertex) == drawState->getVertexSize());
    }

    GrDrawTarget::AutoReleaseGeometry geo(target, 4, 0);
    if (!geo.succeeded()) {
        GrPrintf("Failed to get space for vertices!\n");
        return false;
    }

    // This expands the outer rect so that after CTM we end up with a half-pixel border
    SkScalar a = vm[SkMatrix::kMScaleX];
    SkScalar b = vm[SkMatrix::kMSkewX];
    SkScalar c = vm[SkMatrix::kMSkewY];
    SkScalar d = vm[SkMatrix::kMScaleY];
    SkScalar geoDx = SkScalarDiv(SK_ScalarHalf, SkScalarSqrt(a*a + c*c));
    SkScalar geoDy = SkScalarDiv(SK_ScalarHalf, SkScalarSqrt(b*b + d*d));
    // This adjusts the "radius" to include the half-pixel border
    SkScalar offsetDx = SkScalarDiv(geoDx, xRadius);
    SkScalar offsetDy = SkScalarDiv(geoDy, yRadius);

    SkRect bounds = SkRect::MakeLTRB(
        center.fX - xRadius - geoDx,
        center.fY - yRadius - geoDy,
        center.fX + xRadius + geoDx,
        center.fY + yRadius + geoDy
    );

    xLocalRadius += SK_ScalarHalf;
    yLocalRadius += SK_ScalarHalf;

    SkRect localBounds = SkRect::MakeLTRB(
        localCenter.fX - xLocalRadius,
        localCenter.fY - yLocalRadius,
        localCenter.fX + xLocalRadius,
        localCenter.fY + yLocalRadius
    );

    SkRect mappedBounds;
    vm.mapRect(&mappedBounds, bounds);
    SkPoint points[8];
    SkPoint mappedPoints[8];

    points[0] = SkPoint::Make(-1.0f - offsetDx, -1.0f - offsetDy);
    points[1] = SkPoint::Make(-innerRatioX - offsetDx, -innerRatioY - offsetDy);
    points[2] = SkPoint::Make(1.0f + offsetDx, -1.0f - offsetDy);
    points[3] = SkPoint::Make(innerRatioX + offsetDx, -innerRatioY - offsetDy);
    points[4] = SkPoint::Make(-1.0f - offsetDx, 1.0f + offsetDy);
    points[5] = SkPoint::Make(-innerRatioX - offsetDx, innerRatioY + offsetDy);
    points[6] = SkPoint::Make(1.0f + offsetDx, 1.0f + offsetDy);
    points[7] = SkPoint::Make(innerRatioX + offsetDx, innerRatioY + offsetDy);
    vm.mapPoints(mappedPoints, points, 8);

    GrDrawState::AutoViewMatrixRestore avmr;
    if (!avmr.setIdentity(drawState)) {
        if (useUV) {
            // restore transformation matrix
            GrDrawState::AutoLocalMatrixRestore almr;
            almr.set(drawState, localMatrixInv);
        }
        return false;
    }

    GrEffectRef* effect = DIEllipseEdgeEffect::Create(mode);

    static const int kEllipseOuterOffsetAttrIndex = 1;
    static const int kEllipseInnerOffsetAttrIndex = 2;
    drawState->addCoverageEffect(effect, kEllipseOuterOffsetAttrIndex,
                                         kEllipseInnerOffsetAttrIndex)->unref();


    if (!useUV) {
        DIEllipseVertex* verts = reinterpret_cast<DIEllipseVertex*>(geo.vertices());

        verts[0].fPos = SkPoint::Make(mappedBounds.fLeft, mappedBounds.fTop);
        verts[0].fOuterOffset = points[0];
        verts[0].fInnerOffset = points[1];
        verts[0].fColor = color;

        verts[1].fPos = SkPoint::Make(mappedBounds.fRight, mappedBounds.fTop);
        verts[1].fOuterOffset = points[2];
        verts[1].fInnerOffset = points[3];
        verts[1].fColor = color;

        verts[2].fPos = SkPoint::Make(mappedBounds.fLeft,  mappedBounds.fBottom);
        verts[2].fOuterOffset = points[4];
        verts[2].fInnerOffset = points[5];
        verts[2].fColor = color;

        verts[3].fPos = SkPoint::Make(mappedBounds.fRight, mappedBounds.fBottom);
        verts[3].fOuterOffset = points[6];
        verts[3].fInnerOffset = points[7];
        verts[3].fColor = color;
    }
    else {
        DIEllipseUVVertex* verts = reinterpret_cast<DIEllipseUVVertex*>(geo.vertices());

        SkRect localRect;
        localMatrixInv.mapRect(&localRect, localBounds);

        verts[0].fPos = SkPoint::Make(mappedBounds.fLeft, mappedBounds.fTop);
        verts[0].fOuterOffset = points[0];
        verts[0].fInnerOffset = points[1];
        verts[0].fColor = color;
        verts[0].fLocalPos = SkPoint::Make(localRect.fLeft, localRect.fTop);

        verts[1].fPos = SkPoint::Make(mappedBounds.fRight, mappedBounds.fTop);
        verts[1].fOuterOffset = points[2];
        verts[1].fInnerOffset = points[3];
        verts[1].fColor = color;
        verts[1].fLocalPos = SkPoint::Make(localRect.fRight, localRect.fTop);

        verts[2].fPos = SkPoint::Make(mappedBounds.fLeft,  mappedBounds.fBottom);
        verts[2].fOuterOffset = points[4];
        verts[2].fInnerOffset = points[5];
        verts[2].fColor = color;
        verts[2].fLocalPos = SkPoint::Make(localRect.fLeft,  localRect.fBottom);

        verts[3].fPos = SkPoint::Make(mappedBounds.fRight, mappedBounds.fBottom);
        verts[3].fOuterOffset = points[6];
        verts[3].fInnerOffset = points[7];
        verts[3].fColor = color;
        verts[3].fLocalPos = SkPoint::Make(localRect.fRight, localRect.fBottom);
    }

    target->setIndexSourceToBuffer(indexBuffer);
    target->drawIndexedInstances(kTriangles_GrPrimitiveType, 1, 4, 6, &bounds);

    return true;
}

///////////////////////////////////////////////////////////////////////////////

static const uint16_t gRRectIndices[] = {
    // corners
    0, 1, 5, 0, 5, 4,
    2, 3, 7, 2, 7, 6,
    8, 9, 13, 8, 13, 12,
    10, 11, 15, 10, 15, 14,

    // edges
    1, 2, 6, 1, 6, 5,
    4, 5, 9, 4, 9, 8,
    6, 7, 11, 6, 11, 10,
    9, 10, 14, 9, 14, 13,

    // center
    // we place this at the end so that we can ignore these indices when rendering stroke-only
    5, 6, 10, 5, 10, 9
};

static const uint16_t gRRectStrokeIndices[] = {
    // corners
    0, 1, 5, 0, 5, 4,
    2, 3, 7, 2, 7, 6,
    8, 9, 13, 8, 13, 12,
    10, 11, 15, 10, 15, 14,

    // edges
    1, 2, 6, 1, 6, 5,
    4, 5, 9, 4, 9, 8,
    6, 7, 11, 6, 11, 10,
    9, 10, 14, 9, 14, 13,
};

static const int MAX_RRECTS = 300; // 32768 * 4 / (28 * 16)

GrIndexBuffer* GrOvalRenderer::rRectFillIndexBuffer(GrGpu* gpu) {
    if (NULL == fRRectFillIndexBuffer) {
        static const int SIZE = sizeof(gRRectIndices) * MAX_RRECTS;
        fRRectFillIndexBuffer = gpu->createIndexBuffer(SIZE, false);
        if (NULL != fRRectFillIndexBuffer) {
            // FIXME use lock()/unlock() when port to later revision
            uint16_t *indices = (uint16_t *)fRRectFillIndexBuffer->map();
            if (NULL != indices) {
                fill_indices(indices, gRRectIndices,
                             sizeof(gRRectIndices)/sizeof(uint16_t),
                             16, MAX_RRECTS);
                fRRectFillIndexBuffer->unmap();
            } else {
                indices = (uint16_t *)sk_malloc_throw(SIZE);
                fill_indices(indices, static_cast<const uint16_t *>(gRRectIndices),
                             sizeof(gRRectIndices)/sizeof(uint16_t),
                             16, MAX_RRECTS);
                if (!fRRectFillIndexBuffer->updateData(indices, SIZE)) {
                    fRRectFillIndexBuffer->unref();
                    fRRectFillIndexBuffer = NULL;
                }
                sk_free(indices);
            }
        }
    }
    return fRRectFillIndexBuffer;
}

GrIndexBuffer* GrOvalRenderer::rRectStrokeIndexBuffer(GrGpu* gpu) {
    if (NULL == fRRectStrokeIndexBuffer) {
        static const int SIZE = sizeof(gRRectStrokeIndices) * MAX_RRECTS;
        fRRectStrokeIndexBuffer = gpu->createIndexBuffer(SIZE, false);
        if (NULL != fRRectStrokeIndexBuffer) {
            // FIXME use lock()/unlock() when port to later revision
            uint16_t *indices = (uint16_t *)fRRectStrokeIndexBuffer->map();
            if (NULL != indices) {
                fill_indices(indices, gRRectStrokeIndices,
                             sizeof(gRRectStrokeIndices)/sizeof(uint16_t),
                             16, MAX_RRECTS);
                fRRectStrokeIndexBuffer->unmap();
            } else {
                indices = (uint16_t *)sk_malloc_throw(SIZE);
                fill_indices(indices, static_cast<const uint16_t *>(gRRectStrokeIndices),
                             sizeof(gRRectStrokeIndices)/sizeof(uint16_t),
                             16, MAX_RRECTS);
                if (!fRRectStrokeIndexBuffer->updateData(indices, SIZE)) {
                    fRRectStrokeIndexBuffer->unref();
                    fRRectStrokeIndexBuffer = NULL;
                }
                sk_free(indices);
            }
        }
    }
    return fRRectStrokeIndexBuffer;
}

bool GrOvalRenderer::drawDRRect(GrDrawTarget* target, GrContext* context, bool useAA,
                                const SkRRect& origOuter, const SkRRect& origInner) {
    bool applyAA = useAA && !target->shouldDisableCoverageAAForBlend();

    GrDrawState::AutoRestoreEffects are;
    if (!origInner.isEmpty()) {
        SkTCopyOnFirstWrite<SkRRect> inner(origInner);
        if (!context->getMatrix().isIdentity()) {
            if (!origInner.transform(context->getMatrix(), inner.writable())) {
                return false;
            }
        }
        GrEffectEdgeType edgeType = applyAA ? kInverseFillAA_GrEffectEdgeType :
                                              kInverseFillBW_GrEffectEdgeType;
        GrEffectRef* effect = GrRRectEffect::Create(edgeType, *inner);
        if (NULL == effect) {
            return false;
        }
        are.set(target->drawState());
        target->drawState()->addCoverageEffect(effect)->unref();
    }

    SkStrokeRec fillRec(SkStrokeRec::kFill_InitStyle);
    if (this->drawRRect(target, context, useAA, origOuter, fillRec)) {
        return true;
    }

    SkASSERT(!origOuter.isEmpty());
    SkTCopyOnFirstWrite<SkRRect> outer(origOuter);
    if (!context->getMatrix().isIdentity()) {
        if (!origOuter.transform(context->getMatrix(), outer.writable())) {
            return false;
        }
    }
    GrEffectEdgeType edgeType = applyAA ? kFillAA_GrEffectEdgeType :
                                          kFillBW_GrEffectEdgeType;
    GrEffectRef* effect = GrRRectEffect::Create(edgeType, *outer);
    if (NULL == effect) {
        return false;
    }
    if (!are.isSet()) {
        are.set(target->drawState());
    }

    GrDrawState::AutoViewMatrixRestore avmr;
    if (!avmr.setIdentity(target->drawState())) {
        return false;
    }

    target->drawState()->addCoverageEffect(effect)->unref();
    SkRect bounds = outer->getBounds();
    if (applyAA) {
        bounds.outset(SK_ScalarHalf, SK_ScalarHalf);
    }
    target->drawRect(bounds, NULL, NULL, NULL);
    return true;
}

bool GrOvalRenderer::drawRRect(GrDrawTarget* target, GrContext* context, bool useAA,
                               const SkRRect& rrect, const SkStrokeRec& stroke) {
    if (rrect.isOval()) {
        return this->drawOval(target, context, useAA, rrect.getBounds(), stroke);
    }

    bool useCoverageAA = useAA && !target->shouldDisableCoverageAAForBlend();

    // only anti-aliased rrects for now
    if (!useCoverageAA) {
        return false;
    }

    const SkMatrix& vm = context->getMatrix();

    if (!vm.rectStaysRect() || !rrect.isSimple()) {
        return false;
    }

    // do any matrix crunching before we reset the draw state for device coords
    const SkRect& rrectBounds = rrect.getBounds();
    SkRect bounds;
    SkRect localBounds = rrectBounds;
    SkMatrix localMatrixInv;
    bool useUV = false;
    vm.mapRect(&bounds, rrectBounds);

    SkVector radii = rrect.getSimpleRadii();
    SkScalar xRadius = SkScalarAbs(vm[SkMatrix::kMScaleX]*radii.fX +
                                   vm[SkMatrix::kMSkewY]*radii.fY);
    SkScalar yRadius = SkScalarAbs(vm[SkMatrix::kMSkewX]*radii.fX +
                                   vm[SkMatrix::kMScaleY]*radii.fY);

    SkScalar xLocalRadius = radii.fX;
    SkScalar yLocalRadius = radii.fY;

    SkStrokeRec::Style style = stroke.getStyle();

    // do (potentially) anisotropic mapping of stroke
    SkVector scaledStroke;
    SkScalar strokeWidth = stroke.getWidth();
    SkScalar localStrokeWidth = strokeWidth;

    bool isStrokeOnly = SkStrokeRec::kStroke_Style == style ||
                        SkStrokeRec::kHairline_Style == style;
    bool hasStroke = isStrokeOnly || SkStrokeRec::kStrokeAndFill_Style == style;

    if (hasStroke) {
        if (SkStrokeRec::kHairline_Style == style) {
            scaledStroke.set(1, 1);
        } else {
            scaledStroke.fX = SkScalarAbs(strokeWidth*(vm[SkMatrix::kMScaleX] +
                                                       vm[SkMatrix::kMSkewY]));
            scaledStroke.fY = SkScalarAbs(strokeWidth*(vm[SkMatrix::kMSkewX] +
                                                       vm[SkMatrix::kMScaleY]));
        }

        // if half of strokewidth is greater than radius, we don't handle that right now
        if (SK_ScalarHalf*scaledStroke.fX > xRadius || SK_ScalarHalf*scaledStroke.fY > yRadius) {
            return false;
        }
    }

    // The way the effect interpolates the offset-to-ellipse/circle-center attribute only works on
    // the interior of the rrect if the radii are >= 0.5. Otherwise, the inner rect of the nine-
    // patch will have fractional coverage. This only matters when the interior is actually filled.
    // We could consider falling back to rect rendering here, since a tiny radius is
    // indistinguishable from a square corner.
    if (!isStrokeOnly && (SK_ScalarHalf > xRadius || SK_ScalarHalf > yRadius)) {
        return false;
    }

    // reset to device coordinates
    GrDrawState* drawState = target->drawState();
    GrColor color = drawState->getColor();
    GrDrawState::AutoViewMatrixRestore avmr;
    if (!avmr.setIdentity(drawState)) {
        return false;
    }

    GrIndexBuffer* indexBuffer = NULL;
    if (isStrokeOnly)
        indexBuffer = this->rRectStrokeIndexBuffer(context->getGpu());
    else
        indexBuffer = this->rRectFillIndexBuffer(context->getGpu());
    if (NULL == indexBuffer) {
        GrPrintf("Failed to create index buffer!\n");
        return false;
    }

    // we set draw state's color to white here so that any batching performane in onDraw()
    // won't get a false from GrDrawState::op== due to a color mismatch
    GrDrawState::AutoColorRestore acr;
    acr.set(drawState, 0xFFFFFFFF);

    // use local coords for shader is bitmap
    if (drawState->canOptimizeForBitmapShader()) {
        const SkMatrix& localMatrix = drawState->getLocalMatrix();
        if (localMatrix.invert(&localMatrixInv)) {
            GrDrawState::AutoLocalMatrixChange almc;
            almc.set(drawState);
            useUV = true;
        }
    }

    // if the corners are circles, use the circle renderer
    if ((!hasStroke || scaledStroke.fX == scaledStroke.fY) && xRadius == yRadius) {
        if (!useUV) {
            drawState->setVertexAttribs<gCircleVertexAttribs>(SK_ARRAY_COUNT(gCircleVertexAttribs));
            SkASSERT(sizeof(CircleVertex) == drawState->getVertexSize());
        } else {
            drawState->setVertexAttribs<gCircleUVVertexAttribs>(SK_ARRAY_COUNT(gCircleUVVertexAttribs));
            SkASSERT(sizeof(CircleUVVertex) == drawState->getVertexSize());
        }

        GrDrawTarget::AutoReleaseGeometry geo(target, 16, 0);
        if (!geo.succeeded()) {
            GrPrintf("Failed to get space for vertices!\n");
            return false;
        }

        SkScalar innerRadius = 0.0f;
        SkScalar outerRadius = xRadius;
        SkScalar localOuterRadius = xLocalRadius;
        SkScalar halfWidth = 0;
        SkScalar localHalfWidth = 0;
        if (hasStroke) {
            if (SkScalarNearlyZero(scaledStroke.fX)) {
                halfWidth = SK_ScalarHalf;
                localHalfWidth = SK_ScalarHalf;
            } else {
                halfWidth = SkScalarHalf(scaledStroke.fX);
                localHalfWidth = SkScalarHalf(localStrokeWidth);
            }

            if (isStrokeOnly) {
                innerRadius = xRadius - halfWidth;
            }
            outerRadius += halfWidth;
            localOuterRadius += localHalfWidth;
            bounds.outset(halfWidth, halfWidth);
            localBounds.outset(localHalfWidth, localHalfWidth);
           
        }

        isStrokeOnly = (isStrokeOnly && innerRadius >= 0);

        GrEffectRef* effect = CircleEdgeEffect::Create(isStrokeOnly);
        static const int kCircleEdgeAttrIndex = 1;
        drawState->addCoverageEffect(effect, kCircleEdgeAttrIndex)->unref();

        // The radii are outset for two reasons. First, it allows the shader to simply perform
        // clamp(distance-to-center - radius, 0, 1). Second, the outer radius is used to compute the
        // verts of the bounding box that is rendered and the outset ensures the box will cover all
        // pixels partially covered by the circle.
        outerRadius += SK_ScalarHalf;
        innerRadius -= SK_ScalarHalf;
        localOuterRadius += SK_ScalarHalf;

        // Expand the rect so all the pixels will be captured.
        bounds.outset(SK_ScalarHalf, SK_ScalarHalf);
        localBounds.outset(SK_ScalarHalf, SK_ScalarHalf);

        SkScalar yCoords[4] = {
            bounds.fTop,
            bounds.fTop + outerRadius,
            bounds.fBottom - outerRadius,
            bounds.fBottom
        };
        SkScalar yOuterRadii[4] = {
            -outerRadius,
            0,
            0,
            outerRadius
        };

        SkScalar yLocalCoords[4] = {
            localBounds.fTop,
            localBounds.fTop + localOuterRadius,
            localBounds.fBottom - localOuterRadius,
            localBounds.fBottom
        };

        if (!useUV) {
            CircleVertex* verts = reinterpret_cast<CircleVertex*>(geo.vertices());

            for (int i = 0; i < 4; ++i) {
                verts->fPos = SkPoint::Make(bounds.fLeft, yCoords[i]);
                verts->fOffset = SkPoint::Make(-outerRadius, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fLeft + outerRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(0, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight - outerRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(0, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight, yCoords[i]);
                verts->fOffset = SkPoint::Make(outerRadius, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                verts++;
            }
        }
        else {
            CircleUVVertex* verts = reinterpret_cast<CircleUVVertex*>(geo.vertices());
            SkPoint localPoint;
            SkPoint mappedPoint;

            for (int i = 0; i < 4; ++i) {
                verts->fPos = SkPoint::Make(bounds.fLeft, yCoords[i]);
                verts->fOffset = SkPoint::Make(-outerRadius, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                localPoint.fX = localBounds.fLeft;
                localPoint.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &localPoint, 1);
                verts->fLocalPos = mappedPoint;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fLeft + outerRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(0, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                localPoint.fX = localBounds.fLeft + localOuterRadius;
                localPoint.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &localPoint, 1);
                verts->fLocalPos = mappedPoint;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight - outerRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(0, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                localPoint.fX = localBounds.fRight - localOuterRadius;
                localPoint.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &localPoint, 1);
                verts->fLocalPos = mappedPoint;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight, yCoords[i]);
                verts->fOffset = SkPoint::Make(outerRadius, yOuterRadii[i]);
                verts->fOuterRadius = outerRadius;
                verts->fInnerRadius = innerRadius;
                verts->fColor = color;
                localPoint.fX = localBounds.fRight;
                localPoint.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &localPoint, 1);
                verts->fLocalPos = mappedPoint;
                verts++;
            }
        }

        // drop out the middle quad if we're stroked
        int indexCnt = isStrokeOnly ? SK_ARRAY_COUNT(gRRectStrokeIndices) :
                                      SK_ARRAY_COUNT(gRRectIndices);
        target->setIndexSourceToBuffer(indexBuffer);
        target->drawIndexedInstances(kTriangles_GrPrimitiveType, 1, 16, indexCnt, &bounds);

    // otherwise we use the ellipse renderer
    } else {
        if (!useUV) {
            drawState->setVertexAttribs<gEllipseVertexAttribs>(SK_ARRAY_COUNT(gEllipseVertexAttribs));
            SkASSERT(sizeof(EllipseVertex) == drawState->getVertexSize());
        } else {
            drawState->setVertexAttribs<gEllipseUVVertexAttribs>(SK_ARRAY_COUNT(gEllipseUVVertexAttribs));
            SkASSERT(sizeof(EllipseUVVertex) == drawState->getVertexSize());
        }

        SkScalar innerXRadius = 0.0f;
        SkScalar innerYRadius = 0.0f;
        SkScalar localHalfWidth = 0;
        if (hasStroke) {
            if (SkScalarNearlyZero(scaledStroke.length())) {
                scaledStroke.set(SK_ScalarHalf, SK_ScalarHalf);
                localHalfWidth = SK_ScalarHalf;
            } else {
                scaledStroke.scale(SK_ScalarHalf);
                localHalfWidth = SkScalarHalf(localStrokeWidth);
            }

            // we only handle thick strokes for near-circular ellipses
            if (scaledStroke.length() > SK_ScalarHalf &&
                (SK_ScalarHalf*xRadius > yRadius || SK_ScalarHalf*yRadius > xRadius)) {
                if (useUV) {
                    GrDrawState::AutoLocalMatrixRestore almr;
                    almr.set(drawState, localMatrixInv);
                }
                return false;
            }

            // we don't handle it if curvature of the stroke is less than curvature of the ellipse
            if (scaledStroke.fX*(yRadius*yRadius) < (scaledStroke.fY*scaledStroke.fY)*xRadius ||
                scaledStroke.fY*(xRadius*xRadius) < (scaledStroke.fX*scaledStroke.fX)*yRadius) {
                if (useUV) {
                    GrDrawState::AutoLocalMatrixRestore almr;
                    almr.set(drawState, localMatrixInv);
                }
                return false;
            }

            // this is legit only if scale & translation (which should be the case at the moment)
            if (isStrokeOnly) {
                innerXRadius = xRadius - scaledStroke.fX;
                innerYRadius = yRadius - scaledStroke.fY;
            }

            xRadius += scaledStroke.fX;
            yRadius += scaledStroke.fY;
            xLocalRadius += SK_ScalarHalf;
            yLocalRadius += SK_ScalarHalf;
            bounds.outset(scaledStroke.fX, scaledStroke.fY);
            localBounds.outset(localHalfWidth, localHalfWidth);
        }

        isStrokeOnly = (isStrokeOnly && innerXRadius >= 0 && innerYRadius >= 0);

        GrDrawTarget::AutoReleaseGeometry geo(target, 16, 0);
        if (!geo.succeeded()) {
            GrPrintf("Failed to get space for vertices!\n");
            return false;
        }

        GrEffectRef* effect = EllipseEdgeEffect::Create(isStrokeOnly);
        static const int kEllipseOffsetAttrIndex = 1;
        static const int kEllipseRadiiAttrIndex = 2;
        drawState->addCoverageEffect(effect,
                                     kEllipseOffsetAttrIndex, kEllipseRadiiAttrIndex)->unref();

        // Compute the reciprocals of the radii here to save time in the shader
        SkScalar xRadRecip = SkScalarInvert(xRadius);
        SkScalar yRadRecip = SkScalarInvert(yRadius);
        SkScalar xInnerRadRecip = SkScalarInvert(innerXRadius);
        SkScalar yInnerRadRecip = SkScalarInvert(innerYRadius);

        // Extend the radii out half a pixel to antialias.
        SkScalar xOuterRadius = xRadius + SK_ScalarHalf;
        SkScalar yOuterRadius = yRadius + SK_ScalarHalf;
        SkScalar xLocalOuterRadius = xLocalRadius + SK_ScalarHalf;
        SkScalar yLocalOuterRadius = yLocalRadius + SK_ScalarHalf;

        // Expand the rect so all the pixels will be captured.
        bounds.outset(SK_ScalarHalf, SK_ScalarHalf);
        localBounds.outset(SK_ScalarHalf, SK_ScalarHalf);

        SkScalar yCoords[4] = {
            bounds.fTop,
            bounds.fTop + yOuterRadius,
            bounds.fBottom - yOuterRadius,
            bounds.fBottom
        };
        SkScalar yOuterOffsets[4] = {
            yOuterRadius,
            SK_ScalarNearlyZero, // we're using inversesqrt() in the shader, so can't be exactly 0
            SK_ScalarNearlyZero,
            yOuterRadius
        };

        SkScalar yLocalCoords[4] = {
            localBounds.fTop,
            localBounds.fTop + yLocalOuterRadius,
            localBounds.fBottom - yLocalOuterRadius,
            localBounds.fBottom
        };

        if (!useUV) {
            EllipseVertex* verts = reinterpret_cast<EllipseVertex*>(geo.vertices());
            for (int i = 0; i < 4; ++i) {
                verts->fPos = SkPoint::Make(bounds.fLeft, yCoords[i]);
                verts->fOffset = SkPoint::Make(xOuterRadius, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fLeft + xOuterRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(SK_ScalarNearlyZero, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight - xOuterRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(SK_ScalarNearlyZero, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight, yCoords[i]);
                verts->fOffset = SkPoint::Make(xOuterRadius, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                verts++;
            }
        }
        else {
            EllipseUVVertex* verts = reinterpret_cast<EllipseUVVertex*>(geo.vertices());

            SkPoint point;
            SkPoint mappedPoint;

            for (int i = 0; i < 4; ++i) {
                verts->fPos = SkPoint::Make(bounds.fLeft, yCoords[i]);
                verts->fOffset = SkPoint::Make(xOuterRadius, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                point.fX = localBounds.fLeft;
                point.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &point, 1);
                verts->fLocalPos = mappedPoint;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fLeft + xOuterRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(SK_ScalarNearlyZero, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                point.fX = localBounds.fLeft + xLocalOuterRadius;
                point.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &point, 1);
                verts->fLocalPos = mappedPoint;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight - xOuterRadius, yCoords[i]);
                verts->fOffset = SkPoint::Make(SK_ScalarNearlyZero, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                point.fX = localBounds.fRight - xLocalOuterRadius;
                point.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &point, 1);
                verts->fLocalPos = mappedPoint;
                verts++;

                verts->fPos = SkPoint::Make(bounds.fRight, yCoords[i]);
                verts->fOffset = SkPoint::Make(xOuterRadius, yOuterOffsets[i]);
                verts->fOuterRadii = SkPoint::Make(xRadRecip, yRadRecip);
                verts->fInnerRadii = SkPoint::Make(xInnerRadRecip, yInnerRadRecip);
                verts->fColor = color;
                point.fX = localBounds.fRight;
                point.fY = yLocalCoords[i];
                localMatrixInv.mapPoints(&mappedPoint, &point, 1);
                verts->fLocalPos = mappedPoint;
                verts++;
            }
        }

        // drop out the middle quad if we're stroked
        int indexCnt = isStrokeOnly ? SK_ARRAY_COUNT(gRRectStrokeIndices) :
                                      SK_ARRAY_COUNT(gRRectIndices);
        target->setIndexSourceToBuffer(indexBuffer);
        target->drawIndexedInstances(kTriangles_GrPrimitiveType, 1, 16, indexCnt, &bounds);
    }

    return true;
}
