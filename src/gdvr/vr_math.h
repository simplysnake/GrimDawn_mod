#pragma once
// Minimal vector / quaternion helpers for mapping headset poses onto the game camera.

#include <cmath>

namespace gdvr {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Normalize(Vec3 a) {
  float len = std::sqrt(Dot(a, a));
  return len > 1e-6f ? a * (1.0f / len) : a;
}

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

inline Quat operator*(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Vec3 Rotate(Quat q, Vec3 v) {
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 t = Cross(u, v) * 2.0f;
  return v + t * q.w + Cross(u, t);
}
// Rotation about +Y by `angle` radians.
inline Quat YawQuat(float angle) { return {0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f)}; }
// Yaw angle of an orientation (OpenXR convention: looking down -Z).
inline float YawOf(Quat q) {
  const Vec3 fwd = Rotate(q, {0, 0, -1});
  return std::atan2(-fwd.x, -fwd.z);
}

struct Pose {
  Quat orientation;
  Vec3 position;
};

// Pose of `p` expressed relative to `ref`.
inline Pose Relative(const Pose& ref, const Pose& p) {
  const Quat inv = Conjugate(ref.orientation);
  return {inv * p.orientation, Rotate(inv, p.position - ref.position)};
}

}  // namespace gdvr
