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

#include <atomic>
#include <mutex>
#include <string>

#include "openvr_driver.h"

#include "ctrl_config.h"
#include "ctrl_math.h"
#include "ctrl_plugin_host.h"

namespace vrto3d::ctrl {

// The HMD pose a hand is placed against.
struct HeadFrame {
    bool valid = false;
    Vec3 pos;
    Quat rot;
};

// One emulated hand controller. Impersonates a Valve Index controller
// ("knuckles") or an HTC Vive wand by pointing at the input profile and
// render models that ship with every SteamVR install, so games pick up their
// existing bindings for that controller.
class CtrlControllerDevice : public vr::ITrackedDeviceServerDriver {
public:
    CtrlControllerDevice(int hand, EmulatedType type, std::string serial);

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t unObjectId) override;
    void Deactivate() override;
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* buf, uint32_t size) override
    {
        if (size) buf[0] = '\0';
    }
    vr::DriverPose_t GetPose() override;

    const std::string& Serial() const { return serial_; }
    bool IsActive() const { return device_index_.load() != vr::k_unTrackedDeviceIndexInvalid; }
    bool OwnsHaptic(vr::VRInputComponentHandle_t h) const
    {
        return h != vr::k_ulInvalidInputComponentHandle && h == h_.haptic;
    }

    // Input thread: publish a merged plugin state (or "disconnected" when
    // !m.tracked / !enabled). `head` is the HMD pose the plugins were given.
    // With `head_hook` set, VRto3D delivers HMD poses through FollowHead and
    // head-anchored hands are placed against the newest of those instead.
    void Update(const MergedHand& m, const CtrlConfig& cfg, bool enabled, double now,
                const HeadFrame& head, bool head_hook);

    // VRto3D's pose thread, right after it published a new HMD pose:
    // re-place a head-anchored hand against it in the same instant.
    void FollowHead(const HeadFrame& head, double now);

private:
    struct Handles {
        vr::VRInputComponentHandle_t system_click, system_touch;
        vr::VRInputComponentHandle_t a_click, a_touch, b_click, b_touch;
        vr::VRInputComponentHandle_t trigger_value, trigger_click, trigger_touch;
        vr::VRInputComponentHandle_t grip_value, grip_force, grip_touch, grip_click;
        vr::VRInputComponentHandle_t thumb_x, thumb_y, thumb_click, thumb_touch;
        vr::VRInputComponentHandle_t pad_x, pad_y, pad_click, pad_touch, pad_force;
        vr::VRInputComponentHandle_t finger[4];  // index, middle, ring, pinky
        vr::VRInputComponentHandle_t menu_click;
        vr::VRInputComponentHandle_t haptic;
    };

    void SetProperties(vr::PropertyContainerHandle_t c);
    void CreateComponents(vr::PropertyContainerHandle_t c);
    void PublishPose(const vr::DriverPose_t& pose);
    // Caller holds state_mutex_.
    void PublishAimLocked(const Vec3& aim_pos, const Quat& aim_rot, double now);
    HeadFrame AnchorFrame(const HeadFrame& head) const;
    void PublishInputs(const Vrto3dCtrlControllerState& s, bool fingers_reported,
                       float click_threshold, bool force);

    void Bool(vr::VRInputComponentHandle_t h, bool v, bool& last, bool force);
    void Scalar(vr::VRInputComponentHandle_t h, float v, float& last, bool force);

    const int          hand_;  // 0 left, 1 right
    const EmulatedType type_;
    const std::string  serial_;

    std::atomic<uint32_t> device_index_{vr::k_unTrackedDeviceIndexInvalid};
    Handles h_{};

    std::mutex       pose_mutex_;
    vr::DriverPose_t curr_pose_{};

    // Pose state shared by the input thread (Update) and VRto3D's pose thread
    // (FollowHead). Publishing happens under the lock, so whichever call runs
    // last always publishes against the newest HMD pose.
    std::mutex state_mutex_;
    HeadFrame  head_;                       // newest HMD pose known
    bool       connected_ = false;          // a valid pose is being published
    uint32_t   anchor_ = VRTO3D_ANCHOR_WORLD;
    Vec3       rel_pos_;                    // aim pose in the anchor frame
    Quat       rel_rot_;
    uint32_t   tracking_ = VRTO3D_TRACKING_OK;
    bool       derive_velocity_ = false;
    bool       have_plugin_velocity_ = false;
    Vec3       plugin_vel_, plugin_ang_vel_;
    double     pose_time_offset_ = 0.0;     // world-anchored poses only
    double     last_publish_ = 0.0;
    Quat       prev_rot_{};
    Vec3       prev_pos_{};
    Vec3       vel_{}, ang_vel_{};

    // Input-thread state.
    bool   was_tracked_ = false;
    bool   inputs_published_ = false;
    double last_disconnected_publish_ = 0.0;
    bool   trigger_click_latched_ = false;
    bool   grip_click_latched_ = false;

    struct Last {
        bool b[32] = {};
        float f[32] = {};
    } last_;
};

}  // namespace vrto3d::ctrl
