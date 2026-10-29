#pragma once
#include <algorithm>
#include <chrono>

namespace Mandelbrot
{
    inline std::chrono::milliseconds ZoomFrameDelay(std::chrono::steady_clock::duration elapsed)
    {
        constexpr auto frameInterval = std::chrono::milliseconds(16);
        constexpr auto minimumDelay = std::chrono::milliseconds(1);
        elapsed = std::max(elapsed, std::chrono::steady_clock::duration::zero());
        // Target the next frame's start time. Over-budget frames yield to the UI without
        // adding another full interval or queuing catch-up renders.
        return std::max(minimumDelay, std::chrono::ceil<std::chrono::milliseconds>(frameInterval - elapsed));
    }
}
