/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

// Minimal double-precision vector / quaternion helpers shared by the
// controller driver and its built-in plugins. Kept free of OpenVR types so
// plugins can use it without the SteamVR headers.
//
// Conventions: SteamVR raw space, +X right, +Y up, -Z forward. Quaternions
// are (w, x, y, z) and rotate column vectors (v' = q v q*). Positive yaw
// turns left (counter-clockwise seen from above), positive pitch looks up.

#include <cmath>

namespace vrto3d::ctrl {

constexpr double kPi = 3.14159265358979323846;
inline double DegToRad(double d) { return d * kPi / 180.0; }
inline double RadToDeg(double r) { return r * 180.0 / kPi; }

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(const Vec3& a)
{
    const double l = Length(a);
    return l > 1e-12 ? a * (1.0 / l) : Vec3{0, 0, -1};
}

struct Quat {
    double w = 1, x = 0, y = 0, z = 0;
};

inline Quat operator*(const Quat& a, const Quat& b)
{
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

inline Quat Conjugate(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }

inline Quat Normalize(const Quat& q)
{
    const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-12) return {};
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}

inline Quat AxisAngle(double ax, double ay, double az, double rad)
{
    const double s = std::sin(rad * 0.5);
    return {std::cos(rad * 0.5), ax * s, ay * s, az * s};
}

inline Quat RotX(double rad) { return AxisAngle(1, 0, 0, rad); }
inline Quat RotY(double rad) { return AxisAngle(0, 1, 0, rad); }
inline Quat RotZ(double rad) { return AxisAngle(0, 0, 1, rad); }

// Yaw (about +Y), then pitch (about the yawed +X), then roll (about the
// resulting -Z). Angles in degrees.
inline Quat FromYawPitchRoll(double yaw_deg, double pitch_deg, double roll_deg)
{
    return Normalize(RotY(DegToRad(yaw_deg)) * RotX(DegToRad(pitch_deg)) * RotZ(DegToRad(roll_deg)));
}

inline Vec3 Rotate(const Quat& q, const Vec3& v)
{
    const Quat p{0, v.x, v.y, v.z};
    const Quat r = q * p * Conjugate(q);
    return {r.x, r.y, r.z};
}

inline Vec3 Forward(const Quat& q) { return Rotate(q, {0, 0, -1}); }

// Yaw / pitch (degrees) of a forward direction, matching FromYawPitchRoll.
inline void DirectionToYawPitch(const Vec3& dir, double& yaw_deg, double& pitch_deg)
{
    const Vec3 d = Normalize(dir);
    yaw_deg = RadToDeg(std::atan2(-d.x, -d.z));
    const double s = d.y < -1.0 ? -1.0 : (d.y > 1.0 ? 1.0 : d.y);
    pitch_deg = RadToDeg(std::asin(s));
}

// Yaw-only part of an orientation (heading), as a quaternion.
inline Quat YawOnly(const Quat& q)
{
    double yaw = 0, pitch = 0;
    DirectionToYawPitch(Forward(q), yaw, pitch);
    return RotY(DegToRad(yaw));
}

// Rotation that points -Z along `dir` with no roll.
inline Quat LookRotation(const Vec3& dir)
{
    double yaw = 0, pitch = 0;
    DirectionToYawPitch(dir, yaw, pitch);
    return FromYawPitchRoll(yaw, pitch, 0.0);
}

// Angular velocity (rad/s, world frame) that takes `from` to `to` in dt.
inline Vec3 AngularVelocity(const Quat& from, const Quat& to, double dt)
{
    if (dt <= 1e-6) return {};
    Quat d = to * Conjugate(from);
    if (d.w < 0) d = {-d.w, -d.x, -d.y, -d.z};
    const double w = d.w > 1.0 ? 1.0 : d.w;
    const double angle = 2.0 * std::acos(w);
    const double s = std::sqrt(1.0 - w * w);
    if (s < 1e-9) return {};
    const double k = angle / (s * dt);
    return {d.x * k, d.y * k, d.z * k};
}

}  // namespace vrto3d::ctrl
