# Mandelbrot Set & Fractal Explorer (WinUI 3 + C++/WinRT)

A high-performance Windows desktop application built with native **C++/WinRT** and **WinUI 3** (Windows App SDK) designed for interactive exploration and custom visualization of fractals.

---

## Features

### 1. Multi-Fractal Computation Engine
- **Supported Fractal Formulas**:
  - **Mandelbrot Set**: $z_{n+1} = z_n^2 + c$ with fast cardioid and period-2 bulb checks.
  - **Burning Ship**: $z_{n+1} = (|\text{Re}(z_n)| + i|\text{Im}(z_n)|)^2 + c$, oriented upright with masts and rigging rising into view.
  - **Tricorn (Mandelbar)**: $z_{n+1} = (\bar{z}_n)^2 + c$, anti-holomorphic 3-cornered fractal.
  - **Newton's Fractal ($z^3 - 1 = 0$)**: Multi-basin Newton-Raphson iteration with smooth quadratic convergence rate potential.
- **Native C++ with OpenMP**: Multithreaded row-by-row scanning using dynamic scheduling.
- **Continuous (Smooth) Potential Coloring**: Eliminates discrete step color banding across all fractals.
- **Double-Precision Coordinate Space**: Supports deep zooming up to $10^{12}\times$.

### 2. Live Modifiable Color Theme Engine
- **Decoupled Iteration & Coloring Pipeline**:
  - The escape computation caches fractional iteration values in a dedicated 2D buffer.
  - Color palette adjustments (frequency, phase, presets, custom gradient stops) run as a separate $O(1)$ lookup table recoloring pass in **~1 to 2 milliseconds**, achieving a silky-smooth **60+ FPS** live interactive experience without recalculating the fractal math.
- **Multi-Basin Newton Coloring**:
  - Automatically maps each of the three root attraction basins to distinct palette partitions, modulated by iteration ripple contours and boundary depth shading.
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
- **Window Modes**:
  - **Collapsible Controls Panel**: Close panel to give visualization 100% of window space (`Esc` or top toggle button).
  - **Chrome-Free Full Screen**: Press `F11` to hide all window chrome, title bars, and borders.
- **Mouse Navigation**:
  - **Mouse Wheel**: Cursor-centered zoom in / zoom out.
  - **Click & Drag**: Smooth panning across the complex plane.
  - **Double-Click**: Re-center and 2x zoom on cursor location.
- **Curated Landmark Bookmarks**:
  - Over 25 curated, validated landmarks across Mandelbrot, Burning Ship, Tricorn, and Newton sets.
- **Telemetry HUD**:
  - Displays active formula, center coordinates $(Re, Im)$, zoom factor, and separate calculation vs. recoloring timings in milliseconds.
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
  .\ARM64\Release\MandelbrotViewer.exe
  ```

- **Debug Build** (ARM64):
  ```powershell
  .\ARM64\Debug\MandelbrotViewer.exe
  ```

### Rebuilding from Source
Using the Visual Studio Developer Command Prompt:
```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" arm64
msbuild MandelbrotViewer.sln /p:Configuration=Release /p:Platform=ARM64
```

---

## Unit Tests

A comprehensive native C++ unit test suite is included in `MandelbrotViewer.Tests`:

### Running Unit Tests
```cmd
.\ARM64\Release\MandelbrotViewer.Tests.exe
```

Or with options:
```cmd
.\ARM64\Release\MandelbrotViewer.Tests.exe --filter MandelbrotEngine
.\ARM64\Release\MandelbrotViewer.Tests.exe --list
```

### Test Coverage
- **Coordinate Transforms**: Center mapping, zoom scaling, aspect ratios, and round-trip coordinate accuracy.
- **Calculation Engines**: Escape-time calculation, fast cardioid checks, smooth vs discrete modes, and cancellation tokens.
- **Fractal Formulas**: Mandelbrot, Burning Ship upright masts, Tricorn conjugate symmetry, and Newton root convergence.
- **Landmark Integrity**: Validates all landmark bookmarks across all 4 fractals to guarantee non-blank, non-flat rendering.
- **Color Theme & LUT**: LUT 4096-entry generation, gradient stop manipulation, interior point coloring, cyclic palette mapping, and multi-basin Newton coloring.
