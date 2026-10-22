# Mandelbrot Set Explorer (WinUI 3 + C++/WinRT)

A high-performance Windows desktop application built with native **C++/WinRT** and **WinUI 3** (Windows App SDK) designed for interactive exploration and custom visualization of the Mandelbrot set.

---

## Features

### 1. High-Performance Computation Engine
- **Native C++ with OpenMP**: Multithreaded row-by-row scanning using dynamic scheduling.
- **Continuous (Smooth) Potential Coloring**: Eliminates discrete step color banding using Douady-Hubbard fractional escape normalization ($\nu = i + 1 - \frac{\ln(0.5 \ln(|z|^2))}{\ln 2}$).
- **Cardioid & Period-2 Bulb Fast Checks**: Instantly bypasses millions of iterations for interior coordinates.
- **Double-Precision Coordinate Space**: Supports deep zooming up to $10^{12}\times$.

### 2. Live Modifiable Color Theme Engine
- **Decoupled Iteration & Coloring Pipeline**:
  - The escape computation caches fractional iteration values in a dedicated 2D buffer.
  - Color palette adjustments (frequency, phase, presets, custom gradient stops) run as a separate $O(1)$ lookup table recoloring pass in **~1 to 2 milliseconds**, achieving a silky-smooth **60+ FPS** live interactive experience without recalculating the fractal math.
- **Preset Palettes**:
  - **Electric Blue**: Deep space blue, cyan, and electric white.
  - **Inferno / Fire**: Dark violet, hot red, vivid orange, and golden yellow.
  - **Psychedelic Rainbow**: Full spectrum vibrant cycling.
  - **Cyberpunk Neon**: Void black, neon violet, hot pink, and cyber cyan.
  - **Viridis**: Perceptually uniform scientific colormap.
  - **Twilight Ocean**: Deep marine blue, aqua teal, seafoam, and glacial white.
  - **Sunset Glow**: Midnight purple, amethyst, coral orange, and golden sky.
  - **Monochrome**: Elegant grayscale shading.
- **Live Tuning Controls**:
  - **Cycle Frequency Slider**: Tune the wavelength of the color bands from $0.05\times$ to $5.0\times$.
  - **Phase Offset Slider**: Continuous shift of palette phases ($0.00$ to $1.00$).
  - **Cycle Colors (Live Animation)**: Continuous 40 FPS animated palette flow.
  - **Invert Palette**: Instantly invert gradient direction.
- **Custom Gradient Stop Editor**:
  - Add and remove gradient stops.
  - Live position slider ($0\%$ to $100\%$).
  - Full RGB color controls with live color preview swatch.
  - Visual horizontal gradient preview bar.

### 3. Interactive Viewport & Fluent UI
- **Mouse Navigation**:
  - **Mouse Wheel**: Cursor-centered zoom in / zoom out.
  - **Click & Drag**: Smooth panning across the complex plane.
  - **Double-Click**: Re-center and 2x zoom on cursor location.
- **Navigation Controls**:
  - Zoom in (+25%), Zoom out (-20%), and Reset View buttons.
- **Landmark Bookmarks**:
  - Quick jump to famous regions: *Overview*, *Seahorse Valley*, *Elephant Valley*, *Triple Spiral*, *Mini Mandelbrot Satellite*, *Scepter Valley*, and *Deep Spiral Core*.
- **Telemetry HUD**:
  - Displays real-time complex center coordinates $(Re, Im)$, zoom factor, and separate calculation vs. recoloring timings in milliseconds.
- **Export & Share**:
  - **Export Viewport Image**: Saves current view directly to a PNG file using Windows Imaging Component (`BitmapEncoder`).
  - **Copy Coordinates & Config**: Copies current fractal view parameters and theme as JSON to the clipboard.

---

## Build & Run

### Prerequisites
- Windows 11 (ARM64 or x64)
- Visual Studio 2022 (with Desktop development with C++)
- Windows App SDK 1.6 Runtime (installed via NuGet package references)

### Running the Application
The executable binaries are pre-compiled and ready:

- **Release Build** (Optimized ARM64):
  ```powershell
  .\MandelbrotViewer\ARM64\Release\MandelbrotViewer.exe
  ```

- **Debug Build** (ARM64):
  ```powershell
  .\MandelbrotViewer\ARM64\Debug\MandelbrotViewer.exe
  ```

### Rebuilding from Source
Using the Visual Studio Developer Command Prompt:
```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" arm64
msbuild MandelbrotViewer\MandelbrotViewer.vcxproj /p:Configuration=Release /p:Platform=ARM64
```
