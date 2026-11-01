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

### Animation Rendering
- A single background worker calculates fresh detail while Windows Composition animates the displayed image between completed frames.
- Animation requests replace the pending view; resizing, formula changes, and manual navigation cancel obsolete work.
- Animated renders include an 8% margin on each edge at the viewport's pixel spacing, allowing panning and zooming out without exposing blank borders.
- Image enlargement is limited to 12%. Zoom progression slows for expensive frames, preserving refinement instead of falling back to coarse blocks.
- Spatial sampling stays fixed during animation. Boundaries and detailed regions receive four exact quarter-pixel samples, colored separately and averaged in linear light to reduce fine-detail shimmer. Flat regions skip the extra evaluations, and refinement fades in smoothly with local variation.
- Subpixel samples are retained for live palette edits and final still frames. Extra detail work can slow zoom progression in expensive views; camera speed adapts while the compositor continues animating.
- Stopping zoom requests a full-resolution, per-pixel frame. PNG exports crop and sample the visible viewport, excluding the rendering margin.

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
- **Optional Julia Companion**:
  - With Mandelbrot selected, enable **Julia Companion** for a side-by-side quadratic Julia view.
  - Click the Mandelbrot image to select its fixed parameter `c`, or choose **Use Mandelbrot Center**. A marker follows that point as the Mandelbrot camera moves.
  - Pan, scroll, double-click, or use the Julia pane's zoom/reset buttons to explore independently. Its parameter stays pinned during Mandelbrot animation.
  - **Start Random Tour** (or the play button in the Julia pane) moves continuously around the main cardioid and adjoining period-two bulb, animating Julia while marking the current point in Mandelbrot. The tour starts from your selection in or near these regions, otherwise from their shared point at -0.75. Starting a tour fits both views to an overview.
  - Tours mix short runs near detailed edges with interior excursions in both regions. After one to three detail stops, the route explores farther inward, occasionally approaching the center. Random depths, angles, and direction changes let branching Julia shapes morph into broader forms before returning to the edges.
  - Occasional short detours extend beyond the Mandelbrot boundary, revealing disconnected Julia shapes before smoothly returning inward. Exterior destinations are confirmed by an escaping critical orbit, and detours stay close to the regions being explored.
  - Adjust **Tour Speed** from 0.2x to 2.0x. Each parameter step waits for its rendered frame, so expensive views slow the travel without skipping points. The Julia camera stays steady and can still be panned or zoomed. Stop the tour to hold the displayed parameter; selecting another point or hiding Julia also stops it.
  - Both views share palette, smooth coloring, iteration settings, and adaptive subpixel sampling. Julia scales its automatic iteration budget to its own zoom.
  - Export Julia PNGs with its save button; **Copy Julia Coordinates & Config** includes the exact parameter. Switching to another formula hides the companion.
- **Telemetry HUD**:
  - Displays active formula, center coordinates $(Re, Im)$, zoom factor, and separate calculation vs. recoloring timings in milliseconds.
- **Export & Share**:
  - **Export Zoom Movie** renders an offline zoom from the current view to a chosen target center and end magnification. Choose 720p, 1080p, 1440p, 4K UHD, or 8K UHD (7680 × 4320), at 24, 25, 30, 48, 50, or 60 frames per second, with a duration from 1 to 3600 seconds.
  - Progress reports the duration of completed footage and encoded frames. A recent footage frame appears after the first frame, approximately every 15 seconds while rendering, and at completion. The UI stays responsive and export can be canceled.
  - Movie frames use a fixed full-resolution sampling grid, linear-light supersampling, an optional short motion-blur shutter, a fixed iteration budget and palette, and camera coordinates evaluated directly from footage time. Band rendering keeps working memory bounded at 8K. These settings minimize sampling changes and accumulated camera error between frames.
  - Movies use high-quality 4:2:2 Photo-JPEG in an MP4 container, with explicit full-range color information, the HD color matrix, and 64-bit sample offsets for large files. Playback geometry and colors are checked through the Windows Media playback engine at resolutions through 8K. Movies can be substantially larger than interframe-compressed files. Output is staged and replaces the chosen destination only when the complete movie has been finalized; cancellation removes the staging file. AVI movies exported by earlier builds must be exported again to correct playback corruption.
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
- **Background Rendering**: Pending-request coalescing, cancellation, stale-frame rejection, buffer reuse, error recovery, and equality with synchronous calculation.
- **Animated Presentation**: High-precision viewport transforms, compositor anchor rebasing, guard-band coverage, delayed-frame and direction-change behavior, and viewport image export.
- **Subpixel Sampling**: Exact sample positions across all formulas and deep zooms, deterministic refinement, flat-region skipping, interior coverage and Newton basin colors, and reduced camera-aligned color variation.
- **Julia Views**: Quadratic orbits, the unit disk, deep parameter and critical-point precision, subpixel sampling, point selection and projection, viewport fitting, stale-parameter rejection, and complete shared state.
- **Julia Tours**: Seeded continuous routes around Mandelbrot regions, balanced boundary/interior/exterior travel, rendered detail, bounded frame steps, critical-orbit escape and return checks, and safe handling of unsupported selections and long frame delays.
- **Movie Export**: Exact and deep camera paths, fixed sampling and render-band agreement, 8K JPEG encoding, MP4 sample tables and timing, rendered playback geometry and colors through 8K, background progress and previews, and cancellation preserving an existing destination.
