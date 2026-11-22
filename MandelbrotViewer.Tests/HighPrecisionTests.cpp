#include "pch.h"
#include "TestHarness.h"
#include "HighPrecision.h"

using namespace Mandelbrot;

TEST_CASE(HighPrecision, BigFloat_ArithmeticAndNoCancellation)
{
    // Verify basic arithmetic
    BigFloat a(1.5);
    BigFloat b(-2.25);
    ASSERT_NEAR(-0.75, (a + b).ToDouble(), 1e-12);
    ASSERT_NEAR(3.75, (a - b).ToDouble(), 1e-12);
    ASSERT_NEAR(-3.375, (a * b).ToDouble(), 1e-12);
    ASSERT_NEAR(2.25, a.Sqr().ToDouble(), 1e-12);
    ASSERT_NEAR(1.0 / 3.0, (BigFloat(1.0) / BigFloat(3.0)).ToDouble(), 1e-12);

    // Crucial test: adding a 1e-40 offset to -0.75 and subtracting -0.75 must preserve 1e-40 without cancellation!
    BigFloat base(-0.75);
    BigFloat tiny(1e-40);
    BigFloat sum = base + tiny;
    BigFloat deltaResult = sum - base;
    ASSERT_NEAR(1e-40, deltaResult.ToDouble(), 1e-52);

    // String round-trip
    BigFloat parsed("-0.743643887037158704752191506114774");
    ASSERT_NEAR(-0.7436438870371587, parsed.ToDouble(), 1e-15);
    std::string str = parsed.ToString(20);
    ASSERT_TRUE(str.find("-0.743643887037158") == 0);

    // Regression test for 64-bit limb product carry overflow when squaring values with large 32-bit limbs (> 2^31)
    const double testVal = -0.7436438870371587;
    const BigFloat bfVal(testVal);
    ASSERT_NEAR(testVal * testVal, bfVal.Sqr().ToDouble(), 1e-15);
    ASSERT_NEAR(testVal * testVal, (bfVal * bfVal).ToDouble(), 1e-15);
    ASSERT_NEAR(testVal * testVal, bfVal.Sqr(6).ToDouble(), 1e-15);
}

TEST_CASE(HighPrecision, BigFloat_TryParse_StringsAndScientific)
{
    BigFloat out;
    ASSERT_TRUE(BigFloat::TryParse(L" -0.743643887037158704752191506114774 ", out));
    ASSERT_NEAR(-0.7436438870371587, out.ToDouble(), 1e-15);

    ASSERT_TRUE(BigFloat::TryParse(L"+1.25e-4", out));
    ASSERT_NEAR(1.25e-4, out.ToDouble(), 1e-18);

    ASSERT_TRUE(BigFloat::TryParse("-2.5E+2", out));
    ASSERT_NEAR(-250.0, out.ToDouble(), 1e-12);

    ASSERT_FALSE(BigFloat::TryParse(L"", out));
    ASSERT_FALSE(BigFloat::TryParse(L"   ", out));
    ASSERT_FALSE(BigFloat::TryParse(L"not_a_number", out));
    ASSERT_FALSE(BigFloat::TryParse(L"1.23e", out));
}

TEST_CASE(HighPrecision, BigFloat_EqualityOperator)
{
    BigFloat a("-0.743643887037158704752191506114774");
    BigFloat b("-0.743643887037158704752191506114774");
    BigFloat c("-0.743643887037158704752191506114775");
    BigFloat z1(0.0);
    BigFloat z2(-0.0);

    ASSERT_TRUE(a == b);
    ASSERT_FALSE(a != b);
    ASSERT_TRUE(a != c);
    ASSERT_TRUE(z1 == z2);
}

TEST_CASE(HighPrecision, BigFloat_64BitLimbSqrAndPrecision)
{
    // Verify 1024+ bits preserved even when limbs[0] == 1 (63 leading zero bits)
    const BigFloat one(1.0);
    const BigFloat tiny(1e-290);
    const BigFloat recovered = (one + tiny) - one;
    ASSERT_NEAR(1e-290, recovered.ToDouble(), 1e-302);

    // Verify Reciprocal reaches full 1024-bit accuracy
    const BigFloat inv7 = BigFloat(1.0) / BigFloat(7.0);
    ASSERT_LT(std::abs((inv7 * BigFloat(7.0) - one).ToDouble()), 1e-290);

    // Verify dedicated Sqr matches Mul bit-for-bit across all-ones 64-bit limbs and partial activeLimbs
    BigFloat allOnes;
    allOnes.isZero = false;
    allOnes.sign = true;
    allOnes.exp = 0;
    for (int w = 0; w < BigFloat::W; ++w) allOnes.limbs[w] = UINT64_MAX;
    for (int limbs : { 4, 6, 14, 24, 32 })
    {
        ASSERT_TRUE(allOnes.Sqr(limbs) == allOnes.Mul(allOnes, limbs));
    }

    // Verify activeLimbs-aware Add/Sub preserve precision at deep zoom
    const BigFloat x("-1.749759145130366516130633058415226389506262666801809093836100185");
    const BigFloat y("0.000000003685137949292318500001859506262666801809093836100185");
    ASSERT_NEAR((x + y).ToDouble(), x.Add(y, 14).ToDouble(), 1e-15);
    ASSERT_NEAR((x - y).ToDouble(), x.Sub(y, 14).ToDouble(), 1e-15);
    ASSERT_TRUE((x.Add(y, 24).Sub(x, 24) - y).Abs().ToDouble() < 1e-150);
}

TEST_CASE(HighPrecision, BigFloat_ToStringAndTryParseEdgeCases)
{
    // Zero honors requested maxDecimals (including 0).
    const BigFloat zero(0.0);
    ASSERT_TRUE(zero.ToString(0) == "+0");
    ASSERT_TRUE(zero.ToString(4) == "+0.0000");
    ASSERT_TRUE(zero.ToString(12) == "+0.000000000000");

    // maxDecimals == 0 omits trailing decimal point.
    ASSERT_TRUE(BigFloat(42.75).ToString(0) == "+42");
    ASSERT_TRUE(BigFloat(-42.75).ToString(0) == "-42");

    // Values >= 2^64 (exp >= 2 in 64-bit limbs) format exact integer digits without UB.
    const BigFloat exactHuge = BigFloat(18446744073709551616.0) + BigFloat(0.5); // 2^64 + 0.5
    ASSERT_TRUE(exactHuge.ToString(2) == "+18446744073709551616.50");

    BigFloat parsedHuge;
    ASSERT_TRUE(BigFloat::TryParse("18446744073709551616.5", parsedHuge));
    ASSERT_TRUE(parsedHuge.ToString(0) == "+18446744073709551616");
    ASSERT_TRUE((parsedHuge - exactHuge).Abs().ToDouble() < 1e-100);

    BigFloat negHuge;
    ASSERT_TRUE(BigFloat::TryParse("-1000000000000000000000000.25", negHuge)); // -10^24 - 0.25
    ASSERT_TRUE(negHuge.ToString(0) == "-1000000000000000000000000");
    const BigFloat exactNegHuge = -(BigFloat("1000000000000000000000000") + BigFloat(0.25));
    ASSERT_TRUE(exactNegHuge.ToString(2) == "-1000000000000000000000000.25");
    ASSERT_TRUE((negHuge - exactNegHuge).Abs().ToDouble() < 1e-100);

    // Exponents beyond +/-308 are parsed accurately rather than silently clamped to 308.
    BigFloat tiny350, tiny308;
    ASSERT_TRUE(BigFloat::TryParse("1e-350", tiny350));
    ASSERT_TRUE(BigFloat::TryParse("1e-308", tiny308));
    ASSERT_FALSE(tiny350.isZero);
    ASSERT_TRUE(tiny350 != tiny308);
    const BigFloat scaledBack = tiny350 * BigFloat("1e350");
    ASSERT_NEAR(1.0, scaledBack.ToDouble(), 1e-12);

    // Exponents past the supported +/-10000 bound fail cleanly instead of silently clamping.
    BigFloat overflowed;
    ASSERT_FALSE(BigFloat::TryParse("1e50000", overflowed));
    ASSERT_FALSE(BigFloat::TryParse("1e-50000", overflowed));
}
