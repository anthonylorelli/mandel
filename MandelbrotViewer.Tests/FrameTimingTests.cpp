#include "pch.h"
#include "TestHarness.h"
#include "FrameTiming.h"

using namespace std::chrono;
using Mandelbrot::ZoomFrameDelay;

TEST_CASE(FrameTiming, FastFramesWaitOnlyForTheirRemainingBudget)
{
    ASSERT_EQ(16, ZoomFrameDelay(0ms).count());
    ASSERT_EQ(11, ZoomFrameDelay(5ms).count());
    ASSERT_EQ(1, ZoomFrameDelay(15ms).count());
    // Round up fractional milliseconds so the next frame never starts before its deadline.
    ASSERT_EQ(11, ZoomFrameDelay(5500us).count());
}

TEST_CASE(FrameTiming, SlowFramesYieldWithoutAddingAnotherFrameInterval)
{
    for (auto elapsed : { 16ms, 17ms, 55ms, 300ms, 2000ms })
        ASSERT_EQ(1, ZoomFrameDelay(elapsed).count());
}

TEST_CASE(FrameTiming, FrameBudgetRecoversAfterASlowFrame)
{
    ASSERT_EQ(1, ZoomFrameDelay(80ms).count());
    ASSERT_EQ(12, ZoomFrameDelay(4ms).count());
    ASSERT_EQ(16, ZoomFrameDelay(0ms).count());
    ASSERT_EQ(16, ZoomFrameDelay(-1ms).count());
}
