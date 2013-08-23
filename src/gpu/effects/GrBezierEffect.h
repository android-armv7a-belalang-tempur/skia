/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrBezierEffect_DEFINED
#define GrBezierEffect_DEFINED

#include "GrEffect.h"
#include "GrDrawTargetCaps.h"

enum GrBezierEdgeType {
    kFillAA_GrBezierEdgeType,
    kHairAA_GrBezierEdgeType,
    kFillNoAA_GrBezierEdgeType,
};

static inline bool GrBezierEdgeTypeIsFill(const GrBezierEdgeType edgeType) {
    return (kHairAA_GrBezierEdgeType != edgeType);
}

static inline bool GrBezierEdgeTypeIsAA(const GrBezierEdgeType edgeType) {
    return (kFillNoAA_GrBezierEdgeType != edgeType);
}

/**
 * Shader is based off of Loop-Blinn Quadratic GPU Rendering
 * The output of this effect is a hairline edge for conics.
 * Conics specified by implicit equation K^2 - LM.
 * K, L, and M, are the first three values of the vertex attribute,
 * the fourth value is not used. Distance is calculated using a
 * first order approximation from the taylor series.
 * Coverage for AA is max(0, 1-distance).
 *
 * Test were also run using a second order distance approximation.
 * There were two versions of the second order approx. The first version
 * is of roughly the form:
 * f(q) = |f(p)| - ||f'(p)||*||q-p|| - ||f''(p)||*||q-p||^2.
 * The second is similar:
 * f(q) = |f(p)| + ||f'(p)||*||q-p|| + ||f''(p)||*||q-p||^2.
 * The exact version of the equations can be found in the paper
 * "Distance Approximations for Rasterizing Implicit Curves" by Gabriel Taubin
 *
 * In both versions we solve the quadratic for ||q-p||.
 * Version 1:
 * gFM is magnitude of first partials and gFM2 is magnitude of 2nd partials (as derived from paper)
 * builder->fsCodeAppend("\t\tedgeAlpha = (sqrt(gFM*gFM+4.0*func*gF2M) - gFM)/(2.0*gF2M);\n");
 * Version 2:
 * builder->fsCodeAppend("\t\tedgeAlpha = (gFM - sqrt(gFM*gFM-4.0*func*gF2M))/(2.0*gF2M);\n");
 *
 * Also note that 2nd partials of k,l,m are zero
 *
 * When comparing the two second order approximations to the first order approximations,
 * the following results were found. Version 1 tends to underestimate the distances, thus it
 * basically increases all the error that we were already seeing in the first order
 * approx. So this version is not the one to use. Version 2 has the opposite effect
 * and tends to overestimate the distances. This is much closer to what we are
 * looking for. It is able to render ellipses (even thin ones) without the need to chop.
 * However, it can not handle thin hyperbolas well and thus would still rely on
 * chopping to tighten the clipping. Another side effect of the overestimating is
 * that the curves become much thinner and "ropey". If all that was ever rendered
 * were "not too thin" curves and ellipses then 2nd order may have an advantage since
 * only one geometry would need to be rendered. However no benches were run comparing
 * chopped first order and non chopped 2nd order.
 */
class GrGLConicEffect;

class GrConicEffect : public GrEffect {
public:
    static GrEffectRef* Create(const GrBezierEdgeType edgeType, const GrDrawTargetCaps& caps) {
        GR_CREATE_STATIC_EFFECT(gConicFillAA, GrConicEffect, (edgeType));
        GR_CREATE_STATIC_EFFECT(gConicHairAA, GrConicEffect, (edgeType));
        GR_CREATE_STATIC_EFFECT(gConicFillNoAA, GrConicEffect, (edgeType));
        if (kFillAA_GrBezierEdgeType == edgeType) {
            if (!caps.shaderDerivativeSupport()) {
                return NULL;
            }
            gConicFillAA->ref();
            return gConicFillAA;
        } else if (kHairAA_GrBezierEdgeType == edgeType) {
            if (!caps.shaderDerivativeSupport()) {
                return NULL;
            }
            gConicHairAA->ref();
            return gConicHairAA;
        } else {
            gConicFillNoAA->ref();
            return gConicFillNoAA;
        }
    }

    virtual ~GrConicEffect();

    static const char* Name() { return "Conic"; }

    inline bool isAntiAliased() const { return GrBezierEdgeTypeIsAA(fEdgeType); }
    inline bool isFilled() const { return GrBezierEdgeTypeIsFill(fEdgeType); }
    inline GrBezierEdgeType getEdgeType() const { return fEdgeType; }

    typedef GrGLConicEffect GLEffect;

    virtual void getConstantColorComponents(GrColor* color,
                                            uint32_t* validFlags) const SK_OVERRIDE {
        *validFlags = 0;
    }

    virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE;

private:
    GrConicEffect(GrBezierEdgeType);

    virtual bool onIsEqual(const GrEffect& other) const SK_OVERRIDE;

    GrBezierEdgeType fEdgeType;

    GR_DECLARE_EFFECT_TEST;

    typedef GrEffect INHERITED;
};

///////////////////////////////////////////////////////////////////////////////
/**
 * The output of this effect is a hairline edge for quadratics.
 * Quadratic specified by 0=u^2-v canonical coords. u and v are the first
 * two components of the vertex attribute. At the three control points that define
 * the Quadratic, u, v have the values {0,0}, {1/2, 0}, and {1, 1} respectively.
 * Coverage for AA is min(0, 1-distance). 3rd & 4th cimponent unused.
 * Requires shader derivative instruction support.
 */
class GrGLQuadEffect;

class GrQuadEffect : public GrEffect {
public:
    static GrEffectRef* Create(const GrBezierEdgeType edgeType, const GrDrawTargetCaps& caps) {
        GR_CREATE_STATIC_EFFECT(gQuadFillAA, GrQuadEffect, (kFillAA_GrBezierEdgeType));
        GR_CREATE_STATIC_EFFECT(gQuadHairAA, GrQuadEffect, (kHairAA_GrBezierEdgeType));
        GR_CREATE_STATIC_EFFECT(gQuadFillNoAA, GrQuadEffect, (kFillNoAA_GrBezierEdgeType));
        if (kFillAA_GrBezierEdgeType == edgeType) {
            if (!caps.shaderDerivativeSupport()) {
                return NULL;
            }
            gQuadFillAA->ref();
            return gQuadFillAA;
        } else if (kHairAA_GrBezierEdgeType == edgeType) {
            if (!caps.shaderDerivativeSupport()) {
                return NULL;
            }
            gQuadHairAA->ref();
            return gQuadHairAA;
        } else {
            gQuadFillNoAA->ref();
            return gQuadFillNoAA;
        }
    }

    virtual ~GrQuadEffect();

    static const char* Name() { return "Quad"; }

    inline bool isAntiAliased() const { return GrBezierEdgeTypeIsAA(fEdgeType); }
    inline bool isFilled() const { return GrBezierEdgeTypeIsFill(fEdgeType); }
    inline GrBezierEdgeType getEdgeType() const { return fEdgeType; }

    typedef GrGLQuadEffect GLEffect;

    virtual void getConstantColorComponents(GrColor* color,
                                            uint32_t* validFlags) const SK_OVERRIDE {
        *validFlags = 0;
    }

    virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE;

private:
    GrQuadEffect(GrBezierEdgeType);

    virtual bool onIsEqual(const GrEffect& other) const SK_OVERRIDE;

    GrBezierEdgeType fEdgeType;

    GR_DECLARE_EFFECT_TEST;

    typedef GrEffect INHERITED;
};

//////////////////////////////////////////////////////////////////////////////
/**
 * Shader is based off of "Resolution Independent Curve Rendering using
 * Programmable Graphics Hardware" by Loop and Blinn.
 * The output of this effect is a hairline edge for non rational cubics.
 * Cubics are specified by implicit equation K^3 - LM.
 * K, L, and M, are the first three values of the vertex attribute,
 * the fourth value is not used. Distance is calculated using a
 * first order approximation from the taylor series.
 * Coverage for AA is max(0, 1-distance).
 */
class GrGLCubicEffect;

class GrCubicEffect : public GrEffect {
public:
    static GrEffectRef* Create(const GrBezierEdgeType edgeType, const GrDrawTargetCaps& caps) {
        GR_CREATE_STATIC_EFFECT(gCubicFillAA, GrCubicEffect, (kFillAA_GrBezierEdgeType));
        GR_CREATE_STATIC_EFFECT(gCubicHairAA, GrCubicEffect, (kHairAA_GrBezierEdgeType));
        GR_CREATE_STATIC_EFFECT(gCubicFillNoAA, GrCubicEffect, (kFillNoAA_GrBezierEdgeType));
        if (kFillAA_GrBezierEdgeType == edgeType) {
            if (!caps.shaderDerivativeSupport()) {
                return NULL;
            }
            gCubicFillAA->ref();
            return gCubicFillAA;
        } else if (kHairAA_GrBezierEdgeType == edgeType) {
            if (!caps.shaderDerivativeSupport()) {
                return NULL;
            }
            gCubicHairAA->ref();
            return gCubicHairAA;
        } else {
            gCubicFillNoAA->ref();
            return gCubicFillNoAA;
        }
    }

    virtual ~GrCubicEffect();

    static const char* Name() { return "Cubic"; }

    inline bool isAntiAliased() const { return GrBezierEdgeTypeIsAA(fEdgeType); }
    inline bool isFilled() const { return GrBezierEdgeTypeIsFill(fEdgeType); }
    inline GrBezierEdgeType getEdgeType() const { return fEdgeType; }

    typedef GrGLCubicEffect GLEffect;

    virtual void getConstantColorComponents(GrColor* color,
                                            uint32_t* validFlags) const SK_OVERRIDE {
        *validFlags = 0;
    }

    virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE;

private:
    GrCubicEffect(GrBezierEdgeType);

    virtual bool onIsEqual(const GrEffect& other) const SK_OVERRIDE;

    GrBezierEdgeType fEdgeType;

    GR_DECLARE_EFFECT_TEST;

    typedef GrEffect INHERITED;
};

#endif
