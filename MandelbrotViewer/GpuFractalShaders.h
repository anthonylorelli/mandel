#pragma once

namespace Mandelbrot::detail
{
    const char* GetShallowShaderSource() noexcept;
    const char* GetPerturbLdsShaderSource() noexcept;
    const char* GetPerturbLdsBlaShaderSource() noexcept;
    const char* GetPerturbDeepExpShaderSource() noexcept;
    const char* GetMovieDetectShaderSource() noexcept;
    const char* GetMovieResolveShaderSource() noexcept;
    const char* GetMovieShutterShaderSource() noexcept;
}

#define MANDEL_HLSL_ORBIT_COLOR_HELPERS \
"\nfloat EncodeTrapPotential(float smoothPhase)\n" \
"{\n" \
"    float clamped = clamp(smoothPhase, 0.0f, 12.0f);\n" \
"    return 10.0f * (exp(clamped * 0.625f) - 1.0f);\n" \
"}\n\n" \
"void SampleOrbitColor(\n" \
"    float zr, float zi, float mod2,\n" \
"    inout float stripeSum, inout float prevStripeSum, inout uint stripeCount, inout float minDist)\n" \
"{\n" \
"    if (coloringMode == 1u)\n" \
"    {\n" \
"        prevStripeSum = stripeSum;\n" \
"        float angle = (zr == 0.0f && zi == 0.0f) ? 0.0f : atan2(zi, zr);\n" \
"        stripeSum += 0.5f + 0.5f * sin(3.0f * angle);\n" \
"        ++stripeCount;\n" \
"    }\n" \
"    else if (coloringMode == 2u)\n" \
"    {\n" \
"        minDist = min(minDist, mod2);\n" \
"    }\n" \
"    else if (coloringMode == 3u)\n" \
"    {\n" \
"        minDist = min(minDist, min(abs(zr), abs(zi)));\n" \
"    }\n" \
"    else if (coloringMode == 4u)\n" \
"    {\n" \
"        minDist = min(minDist, abs(sqrt(max(mod2, 0.0f)) - 1.0f));\n" \
"    }\n" \
"}\n\n" \
"float FinishOrbitColor(\n" \
"    uint fType, uint iter, float escapeMod2, float effBailout2,\n" \
"    float stripeSum, float prevStripeSum, uint stripeCount, float minDist)\n" \
"{\n" \
"    if (coloringMode == 1u)\n" \
"    {\n" \
"        if (stripeCount == 0u) return 0.0f;\n" \
"        float curAvg = stripeSum / float(stripeCount);\n" \
"        float avg = curAvg;\n" \
"        float t = 1.0f;\n" \
"        if (smoothFlag != 0u && stripeCount >= 2u && escapeMod2 > 1.0f && effBailout2 > 1.0f)\n" \
"        {\n" \
"            float logZn = 0.5f * log(escapeMod2);\n" \
"            float logR = 0.5f * log(effBailout2);\n" \
"            if (logZn > 0.0f && logR > 0.0f)\n" \
"            {\n" \
"                float invLnDeg = (fType == 5u) ? 0.9102392266f : 1.4426950409f;\n" \
"                t = saturate(1.0f - log(max(logZn / logR, 1e-12f)) * invLnDeg);\n" \
"                float prevAvg = prevStripeSum / float(stripeCount - 1u);\n" \
"                avg = lerp(prevAvg, curAvg, t);\n" \
"            }\n" \
"        }\n" \
"        float contIter = max(0.0f, float(iter - 1u) + t);\n" \
"        float smoothPhase = avg * 2.4f + 0.18f * log(1.0f + 0.1f * contIter);\n" \
"        return EncodeTrapPotential(smoothPhase);\n" \
"    }\n" \
"    if (minDist >= 1e29f) return 0.0f;\n" \
"    float d = (coloringMode == 2u) ? sqrt(max(minDist, 0.0f)) : minDist;\n" \
"    float smoothPhase = 0.9f * log(1.0f + 1.0f / max(d, 1e-4f));\n" \
"    return EncodeTrapPotential(smoothPhase);\n" \
"}\n"
