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
