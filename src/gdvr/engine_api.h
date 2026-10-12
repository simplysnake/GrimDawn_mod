#pragma once
// Grim Dawn engine functions we call or hook, looked up by their exported (MSVC-mangled) names.
// Source: notes/exports_Engine.txt, dumped from game build 0.3.0.0 (x64).
// All are member functions: on x64 `this` arrives in rcx, so we model them as free
// functions whose first parameter is the object pointer.

namespace gd {

// Opaque engine types. We only ever hold pointers to them.
struct Camera;
struct Coords;
struct Region;
struct Viewport;
struct Frustum;
struct WorldFrustum;
struct GraphicsMTRenderer;

// void GraphicsMTRenderer::Render(const Camera&, Region*, const Viewport&, const Frustum&,
//                                 unsigned int, bool, const WorldFrustum*)
// Public entry point for rendering a scene from one camera. Our stereo candidate.
inline constexpr char kRendererRender[] =
    "?Render@GraphicsMTRenderer@GAME@@QEAAXAEBVCamera@2@PEAVRegion@2@AEBVViewport@2@AEBVFrustum@2@I_NPEBVWorldFrustum@2@@Z";
using RendererRenderFn = void (*)(GraphicsMTRenderer* self, const Camera* camera, Region* region,
                                  const Viewport* viewport, const Frustum* frustum,
                                  unsigned int flags, bool flag2, const WorldFrustum* worldFrustum);

// const Coords& Camera::GetCoords() const
inline constexpr char kCameraGetCoords[] = "?GetCoords@Camera@GAME@@QEBAAEBVCoords@2@XZ";
using CameraGetCoordsFn = const Coords* (*)(const Camera* self);

// void Camera::SetCoords(const Coords&)
inline constexpr char kCameraSetCoords[] = "?SetCoords@Camera@GAME@@QEAAXAEBVCoords@2@@Z";
using CameraSetCoordsFn = void (*)(Camera* self, const Coords* coords);

// float Camera::GetFOV/GetNearPlane/GetFarPlane/GetCameraAspect() const
inline constexpr char kCameraGetFOV[] = "?GetFOV@Camera@GAME@@QEBAMXZ";
inline constexpr char kCameraGetNear[] = "?GetNearPlane@Camera@GAME@@QEBAMXZ";
inline constexpr char kCameraGetFar[] = "?GetFarPlane@Camera@GAME@@QEBAMXZ";
inline constexpr char kCameraGetAspect[] = "?GetCameraAspect@Camera@GAME@@QEBAMXZ";
using CameraGetFloatFn = float (*)(const Camera* self);

// void Camera::SetFOV(float)
inline constexpr char kCameraSetFOV[] = "?SetFOV@Camera@GAME@@QEAAXM@Z";
using CameraSetFloatFn = void (*)(Camera* self, float value);

// void Camera::SetFarPlane(float)
inline constexpr char kCameraSetFar[] = "?SetFarPlane@Camera@GAME@@QEAAXM@Z";

// void Camera::SetCameraAspect(float)  (multiplier on the viewport aspect; 1.0 by default)
inline constexpr char kCameraSetAspect[] = "?SetCameraAspect@Camera@GAME@@QEAAXM@Z";

// void Camera::GetCameraToScreenMatrix(Mat4&, const Viewport&) const  (projection matrix, 16 floats)
struct Mat4;
inline constexpr char kCameraGetProjection[] = "?GetCameraToScreenMatrix@Camera@GAME@@QEBAXAEAVMat4@2@AEBVViewport@2@@Z";
using CameraGetProjectionFn = void (*)(const Camera* self, Mat4* out, const Viewport* viewport);

// Frustum Camera::GetFrustum(const Viewport&) const
// Returned by value: on x64 the caller passes a hidden result pointer right after `this`.
inline constexpr char kCameraGetFrustum[] = "?GetFrustum@Camera@GAME@@QEBA?AVFrustum@2@AEBVViewport@2@@Z";
using CameraGetFrustumFn = Frustum* (*)(const Camera* self, Frustum* result, const Viewport* viewport);

// const WorldVec3& GraphicsMTRenderer::GetCameraTarget() const  (what the camera follows: the player)
// WorldVec3 layout: Region* (8 bytes), then float x, y, z in the same space as Camera::GetCoords.
inline constexpr char kRendererGetCameraTarget[] = "?GetCameraTarget@GraphicsMTRenderer@GAME@@QEBAAEBVWorldVec3@2@XZ";
using RendererGetCameraTargetFn = const float* (*)(const GraphicsMTRenderer* self);

// int Viewport::GetWidth/GetHeight() const
inline constexpr char kViewportGetWidth[] = "?GetWidth@Viewport@GAME@@QEBAHXZ";
inline constexpr char kViewportGetHeight[] = "?GetHeight@Viewport@GAME@@QEBAHXZ";
using ViewportGetIntFn = int (*)(const Viewport* self);

}  // namespace gd
