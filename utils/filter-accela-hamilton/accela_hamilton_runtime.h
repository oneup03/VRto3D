/* Copyright (c) 2012-2015 Stanislaw Halik
 * Copyright (c) 2023-2024 Michael Welter
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */
#pragma once

#include "openvr_driver.h"
#include "vrto3dlib/stereo_config.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

class AccelaHamiltonRuntimeFilter
{
public:
    void Reset()
    {
        initialized_ = false;
    }

    void FilterPose(vr::HmdQuaternion_t& rotation, double position[3], const StereoDisplayDriverConfiguration& config)
    {
        const auto now = std::chrono::steady_clock::now();

        const vr::HmdQuaternion_t current_rotation = Normalize(rotation);
        const std::array<double, 3> current_position = { position[0], position[1], position[2] };

        if (!initialized_)
        {
            initialized_ = true;
            last_timestamp_ = now;
            last_rotation_ = current_rotation;
            last_position_ = current_position;
            return;
        }

        double dt = std::chrono::duration<double>(now - last_timestamp_).count();
        last_timestamp_ = now;
        dt = std::clamp<double>(dt, kMinDeltaTimeSeconds, kMaxDeltaTimeSeconds);

        const double pos_threshold = std::max<double>(kEpsilon, static_cast<double>(config.track_filter_translation_sensitivity));
        const double rot_threshold_base = std::max<double>(kEpsilon, static_cast<double>(config.track_filter_rotation_sensitivity));

        const std::array<double, 3> delta = {
            current_position[0] - last_position_[0],
            current_position[1] - last_position_[1],
            current_position[2] - last_position_[2]
        };

        const double delta_len = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
        const std::array<double, 3> delta_norm = (delta_len > kEpsilon)
            ? std::array<double, 3>{ delta[0] / delta_len, delta[1] / delta_len, delta[2] / delta_len }
            : std::array<double, 3>{ 0.0, 0.0, 0.0 };

        const double pos_deadzone = std::clamp<double>(
            static_cast<double>(config.track_filter_translation_deadzone),
            0.0,
            1.0);
        const double normalized_pos = std::max<double>(0.0, delta_len - pos_deadzone) / pos_threshold;
        const double pos_gain = dt * EvaluateGain(normalized_pos, kPosGains);
        const std::array<double, 3> output_position = {
            last_position_[0] + delta_norm[0] * pos_gain,
            last_position_[1] + delta_norm[1] * pos_gain,
            last_position_[2] + delta_norm[2] * pos_gain
        };

        const double zoomed_smoothing = [&]() {
            const double max_zoom = std::max<double>(
                kEpsilon,
                static_cast<double>(config.track_filter_max_zoom));
            const double zoom_smoothing = std::max<double>(
                0.0,
                static_cast<double>(config.track_filter_zoom_smoothing));
            const double z = std::clamp<double>(-output_position[2], 0.0, max_zoom);
            return zoom_smoothing * z / (max_zoom + kEpsilon);
        }();

        const double rot_threshold = rot_threshold_base + zoomed_smoothing;
        const vr::HmdQuaternion_t current_contiguous = EnsureSignContinuity(current_rotation, last_rotation_);

        const double angle = AngleBetween(last_rotation_, current_contiguous);
        const double rot_deadzone = std::clamp<double>(
            static_cast<double>(config.track_filter_rotation_deadzone),
            0.0,
            0.2);
        const double normalized_angle = std::max<double>(0.0, angle - rot_deadzone) /
            std::max<double>(kEpsilon, rot_threshold);
        const double gain_angle = dt * EvaluateGain(std::abs(normalized_angle), kRotGains);
        const double alpha = std::min<double>(1.0, gain_angle / (angle + kEpsilon));

        const vr::HmdQuaternion_t output_rotation = Slerp(last_rotation_, current_contiguous, alpha);

        last_position_ = output_position;
        last_rotation_ = output_rotation;

        position[0] = output_position[0];
        position[1] = output_position[1];
        position[2] = output_position[2];
        rotation = output_rotation;
    }

private:
    struct GainPoint
    {
        double x;
        double y;
    };

    static constexpr double kEpsilon = 1e-9;
    static constexpr double kMinDeltaTimeSeconds = 1e-5;
    static constexpr double kMaxDeltaTimeSeconds = 0.25;

    static constexpr vr::HmdQuaternion_t kIdentity = { 1.0, 0.0, 0.0, 0.0 };

    static constexpr std::array<GainPoint, 8> kRotGains = {{
        { 0.0, 0.0 },
        { 0.5, 0.4 },
        { 1.0, 1.5 },
        { 1.5, 8.0 },
        { 2.5, 35.0 },
        { 5.0, 100.0 },
        { 8.0, 200.0 },
        { 9.0, 300.0 }
    }};

    static constexpr std::array<GainPoint, 11> kPosGains = {{
        { 0.0, 0.0 },
        { 0.33, 0.375 },
        { 0.66, 0.75 },
        { 1.33, 2.25 },
        { 1.66, 4.5 },
        { 2.0, 7.5 },
        { 3.0, 24.0 },
        { 5.0, 60.0 },
        { 7.0, 110.0 },
        { 8.0, 150.0 },
        { 9.0, 200.0 }
    }};

    template <size_t N>
    static double EvaluateGain(double x, const std::array<GainPoint, N>& gains)
    {
        if (x <= gains.front().x)
        {
            return gains.front().y;
        }

        if (x >= gains.back().x)
        {
            return gains.back().y;
        }

        for (size_t i = 1; i < gains.size(); ++i)
        {
            if (x <= gains[i].x)
            {
                const double x0 = gains[i - 1].x;
                const double y0 = gains[i - 1].y;
                const double x1 = gains[i].x;
                const double y1 = gains[i].y;
                const double t = (x - x0) / (x1 - x0 + kEpsilon);
                return y0 + (y1 - y0) * t;
            }
        }

        return gains.back().y;
    }

    static float Dot(const vr::HmdQuaternion_t& lhs, const vr::HmdQuaternion_t& rhs)
    {
        return static_cast<float>(lhs.w * rhs.w + lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z);
    }

    static vr::HmdQuaternion_t Negate(const vr::HmdQuaternion_t& q)
    {
        return { -q.w, -q.x, -q.y, -q.z };
    }

    static vr::HmdQuaternion_t Normalize(const vr::HmdQuaternion_t& q)
    {
        const double norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (norm <= kEpsilon)
        {
            return kIdentity;
        }

        return { q.w / norm, q.x / norm, q.y / norm, q.z / norm };
    }

    static vr::HmdQuaternion_t EnsureSignContinuity(const vr::HmdQuaternion_t& current, const vr::HmdQuaternion_t& reference)
    {
        return (Dot(current, reference) < 0.0f) ? Negate(current) : current;
    }

    static double AngleBetween(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b)
    {
        const vr::HmdQuaternion_t a_norm = Normalize(a);
        const vr::HmdQuaternion_t b_norm = Normalize(b);
        const float dot = std::clamp<float>(Dot(a_norm, b_norm), -1.0f, 1.0f);
        return 2.0 * std::acos(std::abs(static_cast<double>(dot)));
    }

    static vr::HmdQuaternion_t Slerp(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b, double t)
    {
        vr::HmdQuaternion_t qa = Normalize(a);
        vr::HmdQuaternion_t qb = Normalize(b);

        float dot = std::clamp<float>(Dot(qa, qb), -1.0f, 1.0f);
        if (dot < 0.0f)
        {
            qb = Negate(qb);
            dot = -dot;
        }

        if (dot > 0.9995f)
        {
            vr::HmdQuaternion_t lerped = {
                qa.w + (qb.w - qa.w) * t,
                qa.x + (qb.x - qa.x) * t,
                qa.y + (qb.y - qa.y) * t,
                qa.z + (qb.z - qa.z) * t
            };
            return Normalize(lerped);
        }

        const double theta0 = std::acos(std::clamp(static_cast<double>(dot), -1.0, 1.0));
        const double sin_theta0 = std::sin(theta0);
        if (sin_theta0 < kEpsilon)
        {
            return qa;
        }

        const double theta = theta0 * t;
        const double sin_theta = std::sin(theta);
        const double s0 = std::cos(theta) - dot * sin_theta / sin_theta0;
        const double s1 = sin_theta / sin_theta0;

        vr::HmdQuaternion_t result = {
            qa.w * s0 + qb.w * s1,
            qa.x * s0 + qb.x * s1,
            qa.y * s0 + qb.y * s1,
            qa.z * s0 + qb.z * s1
        };

        return Normalize(result);
    }

    bool initialized_ = false;
    std::chrono::steady_clock::time_point last_timestamp_{};
    vr::HmdQuaternion_t last_rotation_ = kIdentity;
    std::array<double, 3> last_position_ = { 0.0, 0.0, 0.0 };
};
