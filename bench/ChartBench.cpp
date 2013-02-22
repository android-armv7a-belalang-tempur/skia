
/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkBenchmark.h"
#include "SkCanvas.h"
#include "SkPaint.h"
#include "SkRandom.h"

/**
 * This is a conversion of samplecode/SampleChart.cpp into a bench. It sure would be nice to be able
 * to write one subclass that can be a GM, bench, and/or Sample.
 */

namespace {

// Generates y values for the chart plots.
void gen_data(SkScalar yAvg, SkScalar ySpread, int count, SkTDArray<SkScalar>* dataPts) {
    dataPts->setCount(count);
    static SkMWCRandom gRandom;
    for (int i = 0; i < count; ++i) {
        (*dataPts)[i] = gRandom.nextRangeScalar(yAvg - SkScalarHalf(ySpread),
                                                yAvg + SkScalarHalf(ySpread));
    }
}

// Generates a path to stroke along the top of each plot and a fill path for the area below each
// plot. The fill path is bounded below by the bottomData plot points or a horizontal line at
// yBase if bottomData == NULL.
// The plots are animated by rotating the data points by leftShift.
void gen_paths(const SkTDArray<SkScalar>& topData,
               const SkTDArray<SkScalar>* bottomData,
               SkScalar yBase,
               SkScalar xLeft, SkScalar xDelta,
               int leftShift,
               SkPath* plot, SkPath* fill) {
    plot->rewind();
    fill->rewind();
    plot->incReserve(topData.count());
    if (NULL == bottomData) {
        fill->incReserve(topData.count() + 2);
    } else {
        fill->incReserve(2 * topData.count());
    }

    leftShift %= topData.count();
    SkScalar x = xLeft;

    // Account for the leftShift using two loops
    int shiftToEndCount = topData.count() - leftShift;
    plot->moveTo(x, topData[leftShift]);
    fill->moveTo(x, topData[leftShift]);

    for (int i = 1; i < shiftToEndCount; ++i) {
        plot->lineTo(x, topData[i + leftShift]);
        fill->lineTo(x, topData[i + leftShift]);
        x += xDelta;
    }

    for (int i = 0; i < leftShift; ++i) {
        plot->lineTo(x, topData[i]);
        fill->lineTo(x, topData[i]);
        x += xDelta;
    }

    if (NULL != bottomData) {
        SkASSERT(bottomData->count() == topData.count());
        // iterate backwards over the previous graph's data to generate the bottom of the filled
        // area (and account for leftShift).
        for (int i = 0; i < leftShift; ++i) {
            x -= xDelta;
            fill->lineTo(x, (*bottomData)[leftShift - 1 - i]);
        }
        for (int i = 0; i < shiftToEndCount; ++i) {
            x -= xDelta;
            fill->lineTo(x, (*bottomData)[bottomData->count() - 1 - i]);
        }
    } else {
        fill->lineTo(x - xDelta, yBase);
        fill->lineTo(xLeft, yBase);
    }
}

}

// A set of scrolling line plots with the area between each plot filled. Stresses out GPU path
// filling
class ChartBench : public SkBenchmark {
public:
    ChartBench(void* param, bool aa) : SkBenchmark(param) {
        fShift = 0;
        fAA = aa;
    }

protected:
    virtual const char* onGetName() SK_OVERRIDE {
        if (fAA) {
            return "chart_aa";
        } else {
            return "chart_bw";
        }
    }

    virtual void onDraw(SkCanvas* canvas) SK_OVERRIDE {
        bool sizeChanged = false;
        if (canvas->getDeviceSize() != fSize) {
            fSize = canvas->getDeviceSize();
            sizeChanged = true;
        }

        SkScalar ySpread = SkIntToScalar(fSize.fHeight / 20);

        SkScalar height = SkIntToScalar(fSize.fHeight);

        for (int frame = 0; frame < kFramesPerRun; ++frame) {
            if (sizeChanged) {
                int dataPointCount = SkMax32(fSize.fWidth / kPixelsPerTick + 1, 2);

                for (int i = 0; i < kNumGraphs; ++i) {
                    SkScalar y = (kNumGraphs - i) * (height - ySpread) / (kNumGraphs + 1);
                    fData[i].reset();
                    gen_data(y, ySpread, dataPointCount, fData + i);
                }
                sizeChanged = false;
            }

            canvas->clear(0xFFE0F0E0);

            static SkMWCRandom colorRand;
            static SkColor gColors[kNumGraphs] = { 0x0 };
            if (0 == gColors[0]) {
                for (int i = 0; i < kNumGraphs; ++i) {
                    gColors[i] = colorRand.nextU() | 0xff000000;
                }
            }

            SkPath plotPath;
            SkPath fillPath;

            static const SkScalar kStrokeWidth = SkIntToScalar(2);
            SkPaint plotPaint;
            SkPaint fillPaint;
            plotPaint.setAntiAlias(fAA);
            plotPaint.setStyle(SkPaint::kStroke_Style);
            plotPaint.setStrokeWidth(kStrokeWidth);
            plotPaint.setStrokeCap(SkPaint::kRound_Cap);
            plotPaint.setStrokeJoin(SkPaint::kRound_Join);
            fillPaint.setAntiAlias(fAA);
            fillPaint.setStyle(SkPaint::kFill_Style);

            SkTDArray<SkScalar>* prevData = NULL;
            for (int i = 0; i < kNumGraphs; ++i) {
                gen_paths(fData[i],
                          prevData,
                          height,
                          0,
                          SkIntToScalar(kPixelsPerTick),
                          fShift,
                          &plotPath,
                          &fillPath);

                // Make the fills partially transparent
                fillPaint.setColor((gColors[i] & 0x00ffffff) | 0x80000000);
                canvas->drawPath(fillPath, fillPaint);

                plotPaint.setColor(gColors[i]);
                canvas->drawPath(plotPath, plotPaint);

                prevData = fData + i;
            }

            fShift += kShiftPerFrame;
        }
    }

private:
    enum {
        kNumGraphs = 5,
        kPixelsPerTick = 3,
        kShiftPerFrame = 1,

        kFramesPerRun = SkBENCHLOOP(5),
    };
    int                 fShift;
    SkISize             fSize;
    SkTDArray<SkScalar> fData[kNumGraphs];
    bool                fAA;

    typedef SkBenchmark INHERITED;
};

//////////////////////////////////////////////////////////////////////////////

static SkBenchmark* Fact0(void* p) { return new ChartBench(p, true); }
static SkBenchmark* Fact1(void* p) { return new ChartBench(p, false); }

static BenchRegistry gReg0(Fact0);
static BenchRegistry gReg1(Fact1);
