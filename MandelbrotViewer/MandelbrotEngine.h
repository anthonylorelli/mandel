#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>

namespace Mandelbrot
{
    enum class FractalType
    {
        Mandelbrot = 0,
        BurningShip,
        Tricorn
    };

    std::wstring GetFractalTypeName(FractalType type);
    const std::vector<FractalType>& GetAllFractalTypes();

    struct MandelbrotConfig
    {
        FractalType fractalType{ FractalType::Mandelbrot };
        double centerX{ -0.75 };
        double centerY{ 0.0 };
        double zoom{ 1.0 }; // Viewport span = 3.0 / zoom
        int width{ 1024 };
        int height{ 768 };
        int maxIterations{ 500 };
        bool smooth{ true };
    };

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

    class MandelbrotEngine
    {
    public:
        MandelbrotEngine() = default;

        // Computes fractional iteration values into outIterations (size: width * height)
        // Returns elapsed time in milliseconds
        double Compute(
            const MandelbrotConfig& config,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr);

        static void ScreenToComplex(
            double px, double py,
            int width, int height,
            double centerX, double centerY, double zoom,
            double& outCx, double& outCy);

        static void ComplexToScreen(
            double cx, double cy,
            int width, int height,
            double centerX, double centerY, double zoom,
            double& outPx, double& outPy);
    };
}
