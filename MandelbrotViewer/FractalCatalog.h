#pragma once
#include <string>
#include <vector>

namespace Mandelbrot
{
    enum class FractalType
    {
        Mandelbrot = 0,
        BurningShip,
        Tricorn,
        Celtic,
        Buffalo,
        CubicMultibrot,
        Phoenix,
        Newton
    };

    std::wstring GetFractalTypeName(FractalType type);
    const std::vector<FractalType>& GetAllFractalTypes();

    struct BookmarkLocation
    {
        std::wstring name;
        std::wstring description;
        double centerX;
        double centerY;
        double zoom;
        int maxIterations;
    };

    const std::vector<BookmarkLocation>& GetBookmarks(FractalType type = FractalType::Mandelbrot);
}
