#include "pch.h"
#include "TestHarness.h"
#include "MandelbrotEngine.h"
#include "MandelbrotBla.h"
#include "MoviePlan.h"
#include "MovieExport.h"
#include <bit>

using namespace Mandelbrot;

namespace
{
    using Point = MandelbrotEngine::ComplexOrbitPt;
    std::vector<Point> ReferenceAtI(int length)
    {
        std::vector<Point> orbit(static_cast<size_t>(length) + 1);
        double x = 0.0, y = 0.0;
        for (int m = 0; m <= length; ++m)
        {
            orbit[m] = {x, y};
            const double nextX = x * x - y * y;
            y = 2.0 * x * y + 1.0;
            x = nextX;
        }
        return orbit;
    }

    struct HighComplex { BigFloat x, y; };
    HighComplex Add(HighComplex a, HighComplex b) { return {a.x + b.x, a.y + b.y}; }
    HighComplex Multiply(HighComplex a, HighComplex b)
    { return {a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x}; }
    HighComplex ParameterRoot(double x, double y, int coefficient)
    {
        // c^3+2c^2+2c+2=0: preperiodic parameters; coefficient 1: period-three nuclei.
        HighComplex c{BigFloat(x), BigFloat(y)};
        const HighComplex two{BigFloat(2), {}}, three{BigFloat(3), {}}, four{BigFloat(4), {}};
        const HighComplex b{BigFloat(static_cast<double>(coefficient)), {}};
        for (int n = 0; n < 16; ++n)
        {
            const auto c2 = Multiply(c, c);
            const auto f = Add(Add(Multiply(c2, c), Multiply(two, c2)), Add(Multiply(b, c), b));
            const auto df = Add(Add(Multiply(three, c2), Multiply(four, c)), b);
            const BigFloat denominator = df.x * df.x + df.y * df.y;
            const BigFloat dx = (f.x * df.x + f.y * df.y) / denominator;
            const BigFloat dy = (f.y * df.x - f.x * df.y) / denominator;
            c.x = c.x - dx; c.y = c.y - dy;
        }
        return c;
    }

    std::vector<Point> HighReference(HighComplex c, int length)
    {
        std::vector<Point> orbit;
        BigFloat x, y;
        for (int m = 0; m <= length; ++m)
        {
            const double rx = x.ToDouble(), ry = y.ToDouble();
            orbit.push_back({rx, ry});
            if (rx * rx + ry * ry > 16.0 || m == length) break;
            // Same precision used by the engine at zoom 1e100.
            const BigFloat x2 = x.Sqr(14), y2 = y.Sqr(14), xy = x.Mul(y, 14);
            y = xy + xy + c.y;
            x = x2 - y2 + c.x;
        }
        return orbit;
    }

    void AssertSameFrame(MandelbrotEngine& scalar, MandelbrotEngine& automatic,
        const MandelbrotConfig& c, bool refine = false, bool directDeep = false)
    {
        std::vector<float> expected, actual;
        std::vector<SubpixelSamples> first, second;
        if (directDeep)
        {
            scalar.ComputeDeepZoom(c, c.hpCenterX, c.hpCenterY, expected, nullptr, refine ? &first : nullptr);
            automatic.ComputeDeepZoom(c, c.hpCenterX, c.hpCenterY, actual, nullptr, refine ? &second : nullptr);
        }
        else
        {
            scalar.Compute(c, expected, nullptr, refine ? &first : nullptr);
            automatic.Compute(c, actual, nullptr, refine ? &second : nullptr);
        }
        ASSERT_EQ(expected.size(), actual.size());
        for (size_t i = 0; i < expected.size(); ++i)
            ASSERT_EQ(std::bit_cast<uint32_t>(expected[i]), std::bit_cast<uint32_t>(actual[i]));
        ASSERT_EQ(first.size(), second.size());
        for (size_t i = 0; i < first.size(); ++i)
        {
            ASSERT_EQ(first[i].pixelIndex, second[i].pixelIndex);
            ASSERT_EQ(std::bit_cast<uint32_t>(first[i].weight), std::bit_cast<uint32_t>(second[i].weight));
            for (int s = 0; s < 4; ++s)
                ASSERT_EQ(std::bit_cast<uint32_t>(first[i].iterations[s]), std::bit_cast<uint32_t>(second[i].iterations[s]));
        }
    }
}

TEST_CASE(Bla, EligibilityAndStepBudgetsAreConservative)
{
    const auto orbit = ReferenceAtI(1000);
    detail::BlaTable table;
    table.Build(orbit.data(), 1000, 2e-100);
    ASSERT_TRUE(table.Enabled());
    double x = 1e-100, y = 0.0;
    ASSERT_EQ(0, table.Apply(0, 1000, x, y, x, y));
    ASSERT_EQ(0, table.Apply(1, 3, x, y, x, y));
    ASSERT_EQ(0, table.Apply(2, 999, x, y, x, y));
    ASSERT_EQ(0, table.Apply(999, 999, x, y, x, y));
    const int skipped = table.Apply(1, 999, x, y, 1e-100, 0.0);
    ASSERT_GE(skipped, detail::BlaTable::InitialSkip);
    ASSERT_LE(skipped, 999);
    ASSERT_TRUE(std::isfinite(x) && std::isfinite(y));
    x = 1e-100; y = 0.0;
    ASSERT_EQ(4, table.Apply(1, 7, x, y, 1e-100, 0.0));
    x = 1.0; y = 1.0;
    ASSERT_EQ(0, table.Apply(1, 999, x, y, 1e-100, 0.0));
}

TEST_CASE(Bla, RebuildAndCancellationCannotLeaveAnOldTableEnabled)
{
    const auto orbit = ReferenceAtI(1000);
    detail::BlaTable table;
    table.Build(orbit.data(), 1000, 2e-100); ASSERT_TRUE(table.Enabled());
    table.Build(orbit.data(), 1000, 2e-25); ASSERT_FALSE(table.Enabled());
    table.Build(orbit.data(), 1000, 2e-100); ASSERT_TRUE(table.Enabled());
    std::atomic<bool> cancel{true};
    table.Build(orbit.data(), 1000, 2e-100, &cancel); ASSERT_FALSE(table.Enabled());
    double x = 0.0, y = 0.0;
    ASSERT_EQ(0, table.Apply(1, 999, x, y, 0.0, 0.0));
    cancel = false;
    table.Build(orbit.data(), 64, 2e-100, &cancel); ASSERT_FALSE(table.Enabled());
    std::vector<Point> critical(1001);
    table.Build(critical.data(), 1000, 2e-100); ASSERT_FALSE(table.Enabled());
    table.Build<Point>(nullptr, 1000, 2e-100); ASSERT_FALSE(table.Enabled());
}

TEST_CASE(Bla, SensitiveOrbitsRequestExactReplay)
{
    const auto orbit = ReferenceAtI(1000);
    detail::BlaTable table;
    table.Build(orbit.data(), 1000, 4e-100);
    ASSERT_TRUE(table.Enabled());
    bool foundReplay = false;
    for (int y = -8; y <= 8 && !foundReplay; ++y)
        for (int x = -8; x <= 8 && !foundReplay; ++x)
        {
            const double dcx = (x + .125) * 1e-101, dcy = (y + .375) * 1e-101;
            const auto accelerated = detail::EvalMandelbrotPerturbation<true>(orbit.data(), 1000, 1000, dcx, dcy, table);
            if (accelerated.replay != detail::ReplayReason::Rebase) continue;
            foundReplay = true;
            const auto exact = detail::EvalMandelbrotPerturbation<false>(orbit.data(), 1000, 1000, dcx, dcy, table);
            ASSERT_TRUE(exact.replay == detail::ReplayReason::None);
            ASSERT_GE(exact.iterations, 1);
            ASSERT_LE(exact.iterations, 1000);
        }
    ASSERT_TRUE(foundReplay);
}

TEST_CASE(Bla, LongOrdinaryTailsAlsoRequestExactReplay)
{
    // This real preperiodic parameter lands near a weakly repelling fixed point.
    // A skipped orbit can have a long ordinary tail before any sensitive rebase.
    const auto root = ParameterRoot(-1.54, 0, 2);
    const auto orbit = HighReference(root, 1000);
    const int length = static_cast<int>(orbit.size()) - 1;
    detail::BlaTable table;
    table.Build(orbit.data(), length, 2e-100);
    ASSERT_TRUE(table.Enabled());
    const auto accelerated = detail::EvalMandelbrotPerturbation<true>(orbit.data(), length, 1000, 1e-100, 0.0, table);
    ASSERT_TRUE(accelerated.replay == detail::ReplayReason::LongTail);
    const auto exact = detail::EvalMandelbrotPerturbation<false>(orbit.data(), length, 1000, 1e-100, 0.0, table);
    ASSERT_TRUE(exact.replay == detail::ReplayReason::None);
    ASSERT_EQ(1000, exact.iterations);
}

TEST_CASE(Bla, DeepIterationValuesMatchTheUnacceleratedReference)
{
    MandelbrotEngine scalar(MandelbrotEngine::ExecutionMode::Scalar), automatic;
    for (double zoom : {1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e18, 1e25, 1e50, 1e100, 1e200, 1e250, 1e300})
        for (auto center : {std::pair{0.0, 1.0}, std::pair{-2.0, 0.0},
            std::pair{-.7436438870371587, .1318259042053119}, std::pair{0.0, 0.0}})
            for (bool smooth : {false, true})
            {
                MandelbrotConfig c; c.width = 65; c.height = 49; c.zoom = zoom;
                c.SetCenter(center.first, center.second); c.maxIterations = 1500; c.smooth = smooth;
                AssertSameFrame(scalar, automatic, c);
            }
    for (auto root : {ParameterRoot(-1.54, 0, 2), ParameterRoot(-.23, 1.115, 2), ParameterRoot(-.12256, .74486, 1)})
        for (double zoom : {1e11, 1e16, 1e25, 1e50, 1e100, 1e200, 1e300})
            for (bool smooth : {false, true})
            {
                MandelbrotConfig c; c.width = 65; c.height = 49; c.SetCenter(root.x, root.y);
                c.zoom = zoom; c.maxIterations = 2000; c.smooth = smooth;
                AssertSameFrame(scalar, automatic, c, true);
            }
}

TEST_CASE(Bla, SubpixelsPartialImagesAndIterationLimitsMatchReference)
{
    MandelbrotEngine scalar(MandelbrotEngine::ExecutionMode::Scalar), automatic;
    for (auto size : {std::pair{1, 1}, std::pair{1, 9}, std::pair{2, 3}, std::pair{3, 3}, std::pair{257, 145}})
        for (int stride : {1, 2, 4})
        {
            MandelbrotConfig c; c.width = size.first; c.height = size.second; c.SetCenter(0, 1);
            c.zoom = 1e100; c.maxIterations = 1000; c.renderStride = stride;
            AssertSameFrame(scalar, automatic, c, true);
        }
    for (int limit : {0, 1, 63, 64, 65, 66, 100, 1000})
    {
        MandelbrotConfig c; c.width = 33; c.height = 25; c.SetCenter(0, 1);
        c.zoom = 1e100; c.maxIterations = limit;
        AssertSameFrame(scalar, automatic, c, false, true);
    }
}

TEST_CASE(Bla, MovingDeepCamerasRetainIdenticalSamples)
{
    MandelbrotEngine scalar(MandelbrotEngine::ExecutionMode::Scalar), automatic;
    MoviePlan p; p.width = 257; p.height = 145; p.seconds = 1; p.fps = 24;
    const auto root = ParameterRoot(-.23, 1.115, 2);
    p.start.SetCenter(root.x, root.y); p.start.zoom = 1e100; p.start.maxIterations = 2000;
    p.endZoom = p.start.zoom * 16;
    p.targetX = root.x + BigFloat(.8 * 3.0 / p.start.zoom);
    p.targetY = root.y + BigFloat(.15 * 3.0 / p.start.zoom);
    for (double frame : {0.0, 1.0, 2.0, 3.25, 5.0, 3.25, 23.0, 0.0})
        AssertSameFrame(scalar, automatic, p.CameraAt(frame), true);
}

TEST_CASE(Bla, JuliaAndOtherFormulasKeepTheirReferenceArithmetic)
{
    MandelbrotEngine scalar(MandelbrotEngine::ExecutionMode::Scalar), automatic;
    for (auto type : GetAllFractalTypes())
    {
        MandelbrotConfig c; c.width = 33; c.height = 25; c.fractalType = type;
        c.SetCenter(0, 1); c.zoom = 1e100; c.maxIterations = 500;
        AssertSameFrame(scalar, automatic, c, true);
        if (type == FractalType::Mandelbrot)
        {
            c.juliaSet = true; c.juliaX = BigFloat(-.75); c.juliaY = BigFloat(.1);
            AssertSameFrame(scalar, automatic, c, true);
        }
    }
}

TEST_CASE(Bla, PairPerturbationAndPreparedBandSlicesMatchReference)
{
#if defined(_M_ARM64)
    const auto orbit = ReferenceAtI(1000);
    detail::BlaTable table;
    table.Build(orbit.data(), 1000, 4e-100);
    ASSERT_TRUE(table.Enabled());
    for (int y = -4; y <= 4; ++y)
        for (int x = -4; x <= 4; ++x)
        {
            const double dcx0 = (x + .125) * 1e-101, dcx1 = (x + .625) * 1e-101, dcy = (y + .375) * 1e-101;
            const auto s0 = detail::EvalMandelbrotPerturbation<false>(orbit.data(), 1000, 1000, dcx0, dcy, table);
            const auto s1 = detail::EvalMandelbrotPerturbation<false>(orbit.data(), 1000, 1000, dcx1, dcy, table);
            detail::PerturbationResult p0{}, p1{};
            detail::EvalMandelbrotPerturbationPair<false>(orbit.data(), 1000, 1000, dcx0, dcx1, dcy, table, p0, p1);
            ASSERT_EQ(s0.iterations, p0.iterations);
            ASSERT_EQ(s1.iterations, p1.iterations);
            ASSERT_EQ(std::bit_cast<uint64_t>(s0.x), std::bit_cast<uint64_t>(p0.x));
            ASSERT_EQ(std::bit_cast<uint64_t>(s0.y), std::bit_cast<uint64_t>(p0.y));
            ASSERT_EQ(std::bit_cast<uint64_t>(s1.x), std::bit_cast<uint64_t>(p1.x));
            ASSERT_EQ(std::bit_cast<uint64_t>(s1.y), std::bit_cast<uint64_t>(p1.y));
        }
#endif
    MoviePlan p; p.width = 64; p.height = 48; p.seconds = 1; p.fps = 24;
    const auto root = ParameterRoot(-.12256, .74486, 1);
    p.start.SetCenter(root.x, root.y); p.start.zoom = 1e100; p.start.maxIterations = 1200;
    p.endZoom = 2e100; p.targetX = root.x; p.targetY = root.y;
    std::atomic<bool> cancel{};
    MandelbrotEngine bandedEngine, wholeEngine;
    const auto banded = RenderMovieFrame(p, 0, cancel, {}, 8, &bandedEngine);
    const auto whole = RenderMovieFrame(p, 0, cancel, {}, p.height, &wholeEngine);
    ASSERT_EQ(banded.size(), whole.size());
    for (size_t i = 0; i < banded.size(); ++i)
        ASSERT_EQ(banded[i], whole[i]);
}

TEST_CASE(Bla, QuadPerturbationEarlierBlaAndInteriorStripCertifierMatchReference)
{
    const auto orbit = ReferenceAtI(1000);
    detail::BlaTable earlyTable;
    earlyTable.Build(orbit.data(), 1000, 2e-25, nullptr, 8);
    ASSERT_TRUE(earlyTable.Enabled());

#if defined(_M_ARM64)
    detail::BlaTable deepTable;
    deepTable.Build(orbit.data(), 1000, 4e-100);
    ASSERT_TRUE(deepTable.Enabled());
    for (int y = -3; y <= 3; ++y)
        for (int x = -3; x <= 3; ++x)
        {
            const double dcxCoords[4]{
                (x + .125) * 1e-101, (x + .375) * 1e-101,
                (x + .625) * 1e-101, (x + .875) * 1e-101
            };
            const double dcy01 = (y + .125) * 1e-101, dcy23 = (y + .625) * 1e-101;
            detail::PerturbationResult qPlain[4]{}, qBla[4]{};
            detail::EvalMandelbrotPerturbationQuad<false>(orbit.data(), 1000, 1000, dcxCoords, dcy01, dcy23, deepTable, qPlain);
            detail::EvalMandelbrotPerturbationQuad<true>(orbit.data(), 1000, 1000, dcxCoords, dcy01, dcy23, deepTable, qBla);
            for (int k = 0; k < 4; ++k)
            {
                const double dcy = k < 2 ? dcy01 : dcy23;
                const auto sPlain = detail::EvalMandelbrotPerturbation<false>(orbit.data(), 1000, 1000, dcxCoords[k], dcy, deepTable);
                ASSERT_EQ(sPlain.iterations, qPlain[k].iterations);
                ASSERT_EQ(std::bit_cast<uint64_t>(sPlain.x), std::bit_cast<uint64_t>(qPlain[k].x));
                ASSERT_EQ(std::bit_cast<uint64_t>(sPlain.y), std::bit_cast<uint64_t>(qPlain[k].y));
                if (qBla[k].replay != detail::ReplayReason::None)
                {
                    qBla[k] = detail::EvalMandelbrotPerturbation<false>(orbit.data(), 1000, 1000, dcxCoords[k], dcy, deepTable);
                }
                ASSERT_EQ(sPlain.iterations, qBla[k].iterations);
            }
        }
#endif

    // Period-3 nucleus interior strip certifies true; dendritic escaping strip certifies false.
    const auto nucleus = ParameterRoot(-.12256, .74486, 1);
    const auto nucleusOrbit = HighReference(nucleus, 600);
    const int nucleusLen = static_cast<int>(nucleusOrbit.size()) - 1;
    ASSERT_TRUE(detail::CertifyMandelbrotDeepInteriorStrip(nucleusOrbit.data(), nucleusLen, 600, 1e-20, -1e-20, 5e-21));
    ASSERT_FALSE(detail::CertifyMandelbrotDeepInteriorStrip(orbit.data(), 1000, 1000, 1e-16, 1e-16, 1e-15));
}
