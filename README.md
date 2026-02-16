# Mirror's Edge Camera Proxy for RTX Remix

A D3D9 proxy DLL that sits between Mirror's Edge (Unreal Engine 3) and NVIDIA RTX Remix, providing the View and Projection matrices that Remix needs to render path-traced lighting correctly.

## The Problem

Mirror's Edge uses UE3's shader-based rendering pipeline. The game never calls the fixed-function `SetTransform(D3DTS_VIEW, ...)` or `SetTransform(D3DTS_PROJECTION, ...)` APIs -- it uploads a combined ViewProjection matrix directly to vertex shader constant registers via `SetVertexShaderConstantF`. RTX Remix requires separate View and Projection matrices set through the fixed-function pipeline in order to understand the 3D scene. Without them, Remix has no camera information and cannot perform ray tracing.

## How It Works

The proxy DLL masquerades as `d3d9.dll` and forwards all calls to the real Remix runtime (`d3d9_remix.dll`). It wraps `IDirect3D9`, `IDirect3D9Ex`, and `IDirect3DDevice9`, intercepting two key methods:

### 1. SetVertexShaderConstantF -- VP Detection & Decomposition

Every time the game uploads shader constants, the proxy checks if the data at register `c0` (4 consecutive `float4` vectors = a 4x4 matrix) looks like a ViewProjection matrix.

**Scoring heuristic (`ScoreAsVP`):** UE3 stores matrices column-major in shader registers. The proxy reads cross-register "rows" and checks:

- **Perspective row** (`{f[3], f[7], f[11]}`) should be a unit vector (~1.0 magnitude) -- this is the camera's forward direction.
- **Projection scales** (magnitudes of rows 0 and 1) should correspond to a reasonable field of view (30-140 degrees).
- **f[15]** should be non-trivial -- it encodes `-dot(forward, eye_position)`, the camera's distance along the forward axis.

A candidate that passes all checks gets a score of 6-14. The best candidate per frame is tracked.

**State machine:**

| State | Behavior |
|-------|----------|
| `SCANNING` | Collects the highest-scoring VP candidate each frame. If `f[15]` (camera Z) changes across 3 consecutive frames, transitions to `LOCKED`. This confirms a real, moving 3D camera rather than a static UI matrix. |
| `LOCKED` | Decomposes every qualifying VP upload into separate View and Projection matrices and sends them to Remix via `SetTransform`. Also computes per-draw-call World matrices (see below). |

**Decomposition (`DecomposeVP_ColMajor`):** From the column-major UE3 ViewProjection:

1. Extracts the right, up, and forward camera axes by normalizing the appropriate cross-register rows.
2. Recovers the camera eye position from the dot-product translations embedded in the matrix.
3. Builds a standard D3D left-handed row-vector View matrix.
4. Builds a synthetic Projection matrix using the game's FOV (xScale, yScale) but with configurable zNear/zFar (default 10 - 100,000) to avoid depth clipping issues.

### 2. Per-Draw-Call World Matrix Extraction

When the proxy is locked and has a valid VP, it also computes per-object World matrices. Each time a new matrix is uploaded to `c0`, the proxy:

1. Transposes the column-major data to row-major (D3D convention).
2. Multiplies it by `VP^-1` (the inverse of the game's actual ViewProjection, using the game's real projection -- not the synthetic one).
3. Sets the result as `D3DTS_WORLD` so Remix knows each object's position in world space.

This distinguishes full-resolution geometry passes (where `f[14]` is non-zero, indicating a valid depth translation) from half-resolution or UI passes (where identity World is set instead).

### 3. Present -- Frame Boundary

At each `Present` call:
- Applies any pending camera update from the frame.
- Recomputes `VP^-1` for the next frame's World extraction.
- Resets per-frame scoring state.
- Logs periodic status every 300 frames.

## DLL Loading Chain

```
Game (MirrorsEdge.exe)
  -> loads d3d9.dll (this proxy)
       -> loads d3d9_remix.dll (NVIDIA RTX Remix runtime)
            -> renders via Vulkan ray tracing
```

The proxy looks for `d3d9_remix.dll` first in its own directory, then via standard search paths.

## Building

**Requirements:** Visual Studio 2022 with the "Desktop development with C++" workload (x86 / 32-bit target).

From a **Visual Studio x86 Developer Command Prompt**, or by running the build script:

```bat
build_here.bat
```

This calls `vcvars32.bat`, then compiles:

```
cl /LD /EHsc /O2 d3d9_proxy.cpp /link /DEF:d3d9.def /OUT:d3d9.dll
```

- `/LD` -- build a DLL
- `/EHsc` -- C++ exception handling
- `/O2` -- optimize for speed
- `d3d9.def` -- export table mapping `Direct3DCreate9` etc. to `Proxy_*` functions

Output: `d3d9.dll` (~150 KB)

## Installation

1. Rename RTX Remix's `d3d9.dll` to `d3d9_remix.dll` in the game directory.
2. Copy the built `d3d9.dll` (this proxy) into the game directory alongside `d3d9_remix.dll`.
3. (Optional) Place a `camera_proxy.ini` next to the game executable for configuration.

## Configuration

Create `camera_proxy.ini` next to the game `.exe`:

```ini
[CameraProxy]
EnableLogging=1
DiagnosticFrames=10
Aspect=1.7778
ZNear=10.0
ZFar=100000.0
```

| Key | Default | Description |
|-----|---------|-------------|
| `EnableLogging` | `1` | Write diagnostic log to `camera_proxy.log` |
| `DiagnosticFrames` | `10` | Number of frames to log all VP candidates after first detection |
| `Aspect` | `1.7778` (16:9) | Display aspect ratio |
| `ZNear` | `10.0` | Near clip plane for synthetic projection sent to Remix |
| `ZFar` | `100000.0` | Far clip plane for synthetic projection sent to Remix |

## Logging

When enabled, the proxy writes `camera_proxy.log` in the working directory. Key log entries:

- **`DIAGNOSTIC START`** -- First VP candidate detected, starts diagnostic window.
- **`LOCKED on c0-c3`** -- VP source confirmed and locked.
- **`FIRST CAMERA`** -- First successful View/Projection decomposition.
- **`GAME PROJ`** -- Game's actual projection parameters (A, B, zNear/zFar estimates).
- **`WORLD[n]`** -- First few per-draw World matrices for verification.
- **`Frame N Status`** -- Periodic status dump every 300 frames.

## File Overview

| File | Purpose |
|------|---------|
| `d3d9_proxy.cpp` | Main source -- proxy DLL with VP detection, decomposition, and D3D9 wrapping |
| `d3d9.def` | Module definition exporting D3D9 entry points to proxy functions |
| `build_here.bat` | Build script (compiles in the project directory) |
| `build.bat` / `do_build.bat` | Older build scripts (reference a different source path) |
| `d3d9.dll` | Compiled proxy DLL (output) |
| `camera_proxy.ini` | Optional runtime configuration (user-created) |
| `camera_proxy.log` | Runtime diagnostic log (generated) |
| `d3d9_proxy_backup_*.cpp` | Earlier iterations of the proxy (historical backups) |

## Technical Notes

- **Column-major vs row-major:** UE3 uploads matrices to shader constants in column-major order (column 0 in c0, etc.). D3D9 fixed-function uses row-major. The proxy handles the conversion during decomposition.
- **Synthetic vs game projection:** The game's projection has unusual depth parameters (A ~4.34) that would clip geometry at Remix's camera distance. The proxy substitutes a standard projection with configurable zNear/zFar while preserving the game's FOV.
- **Half-res pass filtering:** UE3 renders some passes at half resolution with stripped eye-position data (f[14] ~0). The proxy detects this and sets identity World for those draws to avoid corrupting the scene.
- **32-bit only:** Mirror's Edge is a 32-bit application, so the proxy must be compiled as x86.
