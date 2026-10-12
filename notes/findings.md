# Findings

Game build: `Grim Dawn.exe` FileVersion 0.3.0.0, x64 (`G:\Games\Steam\steamapps\common\Grim Dawn\x64`).
Export dumps: `exports_*.txt` (`ordinal  RVA  mangled-name`), made with `tools/pe_dump.py`.

## Big result: the engine exports its C++ API by name

| DLL | Named exports |
|---|---|
| Engine.dll | 6,283 |
| Game.dll | 25,113 |
| Direct3D11.dll | 164 |

`Grim Dawn.exe` imports ~3,800 of these. So we can hook engine functions by name with
`GetProcAddress`: no signature scanning, and hooks survive most patches.

## Rendering

- `GraphicsMTRenderer::Render(const Camera&, Region*, const Viewport&, const Frustum&, uint, bool, const WorldFrustum*)`:
  **public**, takes the camera by reference. Main stereo candidate: call it once per eye.
- Internals: `RenderCamera`, `RenderScene`, `RenderFinalScene`, `RenderPostEffects`, `RenderDepth`,
  `BuildScene(bool, const WorldFrustum*, GraphicsScene&, vector<Region*>&)` (culling/scene build),
  `GetDepthBuffer`, `GetDepthTexture`, `CreateRenderTargets`.
- UI is drawn separately via `GraphicsCanvas` (`Display::Render`, `OverlayActivityManager::Render`, ...),
  so the 3D scene and UI should be separable.
- `GraphicsEngine::PresentSurface()`, `Engine::Render()` (top level), global `GAME::gEngine`.
- Direct3D11.dll wraps D3D in class `Direct3DDevice11`.

## Camera

- `Camera`: `GetCoords/SetCoords` (`Coords` = probably 3 axes + origin, 12 floats; to confirm),
  `GetFOV`, `GetNearPlane`, `GetFarPlane`, `GetCameraAspect`, `GetCameraToScreenMatrix(Mat4&, const Viewport&)`,
  `GetSubCameraToScreenMatrix` (off-centre projection? possibly usable for per-eye asymmetric frusta),
  `GetFrustum(const Viewport&[, float])`, `GetRayThroughImagePoint` (mouse picking).
- `WorldCamera`: `GetCameraPitch/Yaw/Distance`, `Set...`, `CalculateViewPosition`, `Update`, `GetRegionFrustum`.
- `GameCamera` (Game.dll): follows the player; `Update`, `UpdateTarget`, `UpdatePitch`, `Shake`, `Zoom`.
- `GraphicsScene`: `GetWorldToCameraMatrix`, `GetCameraToScreenMatrix`, `SetViewer`, `SetFrustum`.

## Week 1 results (2026-10-12, 3840x2160, Devil's Crossing area)

- **1 `Render` call per frame**, viewport = full window. No extra passes to filter out.
- **Camera**: FOV 0.5236 rad (30° vertical), near 1.0, far 120.0, aspect 1.0 (hmm: check what this field means).
- **`Coords` layout confirmed**: floats 0-2, 3-5, 6-8 = three unit axes (rotation); **9-11 = position** (Y is up).
  Floats 12-15 = 0.524 1 1 1 (FOV again + something; not part of Coords probably).
  Axis 0 = (-0.643, 0, 0.766) is horizontal → likely camera right; use it for the eye offset.
- **Offset experiment works**: 3 units sideways shifts the view cleanly, no artifacts or crash.
- **Culling confirmed**: 12 units sideways shows black holes where geometry outside the original frustum is skipped.
  Fix: widen the frustum passed to `Render` / `BuildScene`.
- **Render twice works**: stable; CPU cost of the 2nd pass ≈ 1.5 ms (same as the 1st). GPU cost not yet measured.
- **Hot reload works** while the game runs.
- **Input**: the game reads the mouse directly, so ImGui doesn't get clicks reliably. Experiments can be driven
  from `%LOCALAPPDATA%\gdvr\control.ini` (`[exp]` offset_enabled, origin_index, offset_x/y/z, target_call, render_twice).

## Week 2 results (2026-10-12): stereo in the headset works — GO

- OpenXR (SteamVR, Lighthouse HMD, RTX 5080) sharing the game's D3D11 device; eye images captured with
  CopyResource from the back buffer right after each `Render` call (scene is in the back buffer, pre-UI).
- `Coords` axis 0 points screen-**left** (projection X scale is negative). Mapping it as right mirrored yaw/roll
  and swapped the eyes; fixed by using -axis0 as right.
- `SetFOV` sets the **vertical** FOV. `SetCameraAspect` multiplies the horizontal extent (tanX grows with it),
  so the frustum can be fitted to the lens: Index-class lens needs tan 1.605 x 1.466 (~116° x 111°).
- Recomputing the frustum per eye (`Camera::GetFrustum`) is used for culling; user reported it working well.
- Performance at 2560x1440, both eyes: scene ~20 ms avg (7.5–43 ms) → ~50 fps before the rest of the frame.
  Performance is the main remaining work.
- Game must be windowed (fullscreen minimises when SteamVR takes focus) with vsync off.

## Week 1 questions (answer with the debug overlay)

1. How many `Render` calls per frame, and which is the main view? (viewport size)
2. Which `Coords` floats are the origin? Walk around; the changing triple is the position.
3. Does the offset experiment move the view cleanly, and what breaks (culling, shadows, fog)?
4. Does calling `Render` twice per frame work, and what does it cost?
5. In RenderDoc: which render target holds the finished scene before UI? Is depth reverse-Z?
