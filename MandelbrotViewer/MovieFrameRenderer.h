#pragma once
#include "GpuFractalSolver.h"
#include "MoviePlan.h"

namespace Mandelbrot::detail
{
    bool BuildShutterParams(
        const MoviePlan& plan,
        double frameIndex,
        const FractalConfig& cam0,
        GpuMovieShutterParams& outShutter);
}
