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
#include "ctrl_controller_device.h"

#include <cmath>
#include <cstring>

#include "vrto3dlib/debug_log.hpp"

namespace vrto3d::ctrl {

namespace {

// The "tip" component of each emulated render model, i.e. where the model's
// pointing pose sits relative to its raw pose. Plugins hand us aim poses;
// subtracting this transform yields the raw pose SteamVR expects, so the
// game's laser lands where the plugin aimed. Values are copied from the
// render-model JSON files that ship with SteamVR (component "tip").
struct TipTransform {
    Vec3 origin;
    double rot_xyz_deg[3];
};

TipTransform TipFor(EmulatedType type, int hand)
{
    if (type == EmulatedType::ViveController)
        return {{0.0, -0.01, -0.007}, {1.282, 0.0, 0.0}};
    if (hand == 0)
        return {{0.006, -0.015, 0.02}, {-40.0, -5.0, 0.0}};
    return {{-0.006, -0.015, 0.02}, {-40.0, 5.0, 0.0}};
}

Quat TipRotation(const TipTransform& t)
{
    return Normalize(RotX(DegToRad(t.rot_xyz_deg[0])) *
                     RotY(DegToRad(t.rot_xyz_deg[1])) *
                     RotZ(DegToRad(t.rot_xyz_deg[2])));
}

// Slots in Last::b / Last::f.
enum BoolSlot {
    kSysClick, kSysTouch, kAClick, kATouch, kBClick, kBTouch,
    kTrigClick, kTrigTouch, kGripTouch, kGripClick,
    kThumbClick, kThumbTouch, kPadClick, kPadTouch, kMenuClick,
};
enum FloatSlot {
    kTrigValue, kGripValue, kGripForce, kThumbX, kThumbY,
    kPadX, kPadY, kPadForce, kFinger0,  // + 0..3
};

vr::HmdQuaternion_t ToVr(const Quat& q) { return {q.w, q.x, q.y, q.z}; }

}  // namespace

CtrlControllerDevice::CtrlControllerDevice(int hand, EmulatedType type, std::string serial)
    : hand_(hand), type_(type), serial_(std::move(serial))
{
    vr::VRInputComponentHandle_t* all = reinterpret_cast<vr::VRInputComponentHandle_t*>(&h_);
    for (size_t i = 0; i < sizeof(h_) / sizeof(vr::VRInputComponentHandle_t); ++i)
        all[i] = vr::k_ulInvalidInputComponentHandle;

    curr_pose_.qWorldFromDriverRotation = {1, 0, 0, 0};
    curr_pose_.qDriverFromHeadRotation = {1, 0, 0, 0};
    curr_pose_.qRotation = {1, 0, 0, 0};
    curr_pose_.result = vr::TrackingResult_Uninitialized;
    curr_pose_.poseIsValid = false;
    curr_pose_.deviceIsConnected = false;
}

void CtrlControllerDevice::SetProperties(vr::PropertyContainerHandle_t c)
{
    auto* p = vr::VRProperties();
    const bool left = hand_ == 0;

    p->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
    p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                        left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    p->SetStringProperty(c, vr::Prop_SerialNumber_String, serial_.c_str());
    p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "lighthouse");
    p->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, 64);  // matches VRto3D's HMD
    // Real controllers, when present, win the hand assignment.
    p->SetInt32Property(c, vr::Prop_ControllerHandSelectionPriority_Int32, -1);
    p->SetBoolProperty(c, vr::Prop_DeviceIsWireless_Bool, true);
    p->SetBoolProperty(c, vr::Prop_DeviceIsCharging_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DeviceCanPowerOff_Bool, false);
    p->SetBoolProperty(c, vr::Prop_Identifiable_Bool, false);
    p->SetBoolProperty(c, vr::Prop_Firmware_UpdateAvailable_Bool, false);
    p->SetBoolProperty(c, vr::Prop_NeverTracked_Bool, false);
    p->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, false);
    p->SetStringProperty(c, vr::Prop_HardwareRevision_String, "1.0");

    if (type_ == EmulatedType::ViveController) {
        p->SetStringProperty(c, vr::Prop_ModelNumber_String, "VIVE Controller Pro MV");
        p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "HTC");
        p->SetStringProperty(c, vr::Prop_ControllerType_String, "vive_controller");
        p->SetStringProperty(c, vr::Prop_InputProfilePath_String,
                             "{htc}/input/vive_controller_profile.json");
        p->SetStringProperty(c, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5");
        p->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String,
                             ("htc/vive_controller" + serial_).c_str());
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, "{htc}/icons/controller_status_off.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, "{htc}/icons/controller_status_searching.gif");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{htc}/icons/controller_status_searching_alert.gif");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, "{htc}/icons/controller_status_ready.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{htc}/icons/controller_status_ready_alert.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String, "{htc}/icons/controller_status_error.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, "{htc}/icons/controller_status_off.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceAlertLow_String, "{htc}/icons/controller_status_ready_low.png");
        return;
    }

    const std::string side = left ? "left" : "right";
    p->SetStringProperty(c, vr::Prop_ModelNumber_String, left ? "Knuckles Left" : "Knuckles Right");
    p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Valve");
    p->SetStringProperty(c, vr::Prop_ControllerType_String, "knuckles");
    p->SetStringProperty(c, vr::Prop_InputProfilePath_String,
                         "{indexcontroller}/input/index_controller_profile.json");
    p->SetStringProperty(c, vr::Prop_RenderModelName_String,
                         ("{indexcontroller}valve_controller_knu_1_0_" + side).c_str());
    p->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String,
                         ("valve/index_controller" + serial_).c_str());
    const std::string icon = "{indexcontroller}/icons/" + side + "_controller_status_";
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, (icon + "off.png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, (icon + "searching.gif").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearchingAlert_String, (icon + "searching_alert.gif").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, (icon + "ready.png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReadyAlert_String, (icon + "ready_alert.png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String, (icon + "error.png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, (icon + "off.png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceAlertLow_String, (icon + "ready_low.png").c_str());
}

void CtrlControllerDevice::CreateComponents(vr::PropertyContainerHandle_t c)
{
    auto* in = vr::VRDriverInput();
    auto b = [&](const char* path, vr::VRInputComponentHandle_t& h) {
        in->CreateBooleanComponent(c, path, &h);
    };
    auto one = [&](const char* path, vr::VRInputComponentHandle_t& h) {
        in->CreateScalarComponent(c, path, &h, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
    };
    auto two = [&](const char* path, vr::VRInputComponentHandle_t& h) {
        in->CreateScalarComponent(c, path, &h, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
    };

    if (type_ == EmulatedType::ViveController) {
        b("/input/system/click", h_.system_click);
        b("/input/application_menu/click", h_.menu_click);
        b("/input/grip/click", h_.grip_click);
        one("/input/trigger/value", h_.trigger_value);
        b("/input/trigger/click", h_.trigger_click);
        two("/input/trackpad/x", h_.pad_x);
        two("/input/trackpad/y", h_.pad_y);
        b("/input/trackpad/click", h_.pad_click);
        b("/input/trackpad/touch", h_.pad_touch);
    } else {
        b("/input/system/click", h_.system_click);
        b("/input/system/touch", h_.system_touch);
        b("/input/a/click", h_.a_click);
        b("/input/a/touch", h_.a_touch);
        b("/input/b/click", h_.b_click);
        b("/input/b/touch", h_.b_touch);
        one("/input/trigger/value", h_.trigger_value);
        b("/input/trigger/click", h_.trigger_click);
        b("/input/trigger/touch", h_.trigger_touch);
        one("/input/grip/value", h_.grip_value);
        one("/input/grip/force", h_.grip_force);
        b("/input/grip/touch", h_.grip_touch);
        two("/input/thumbstick/x", h_.thumb_x);
        two("/input/thumbstick/y", h_.thumb_y);
        b("/input/thumbstick/click", h_.thumb_click);
        b("/input/thumbstick/touch", h_.thumb_touch);
        two("/input/trackpad/x", h_.pad_x);
        two("/input/trackpad/y", h_.pad_y);
        b("/input/trackpad/touch", h_.pad_touch);
        one("/input/trackpad/force", h_.pad_force);
        one("/input/finger/index", h_.finger[0]);
        one("/input/finger/middle", h_.finger[1]);
        one("/input/finger/ring", h_.finger[2]);
        one("/input/finger/pinky", h_.finger[3]);
    }
    in->CreateHapticComponent(c, "/output/haptic", &h_.haptic);
}

vr::EVRInitError CtrlControllerDevice::Activate(uint32_t unObjectId)
{
    const vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);
    SetProperties(c);
    CreateComponents(c);
    device_index_.store(unObjectId);
    LOG() << "Controller " << serial_ << " activated as device " << unObjectId << " ("
          << EmulatedTypeToString(type_) << ", " << (hand_ == 0 ? "left" : "right") << ")";
    return vr::VRInitError_None;
}

void CtrlControllerDevice::Deactivate()
{
    device_index_.store(vr::k_unTrackedDeviceIndexInvalid);
}

vr::DriverPose_t CtrlControllerDevice::GetPose()
{
    std::lock_guard<std::mutex> lock(pose_mutex_);
    return curr_pose_;
}

void CtrlControllerDevice::PublishPose(const vr::DriverPose_t& pose)
{
    {
        std::lock_guard<std::mutex> lock(pose_mutex_);
        curr_pose_ = pose;
    }
    const uint32_t idx = device_index_.load();
    if (idx != vr::k_unTrackedDeviceIndexInvalid)
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(idx, pose, sizeof(vr::DriverPose_t));
}

void CtrlControllerDevice::Bool(vr::VRInputComponentHandle_t h, bool v, bool& last, bool force)
{
    if (h == vr::k_ulInvalidInputComponentHandle) return;
    if (force || v != last) {
        vr::VRDriverInput()->UpdateBooleanComponent(h, v, 0.0);
        last = v;
    }
}

void CtrlControllerDevice::Scalar(vr::VRInputComponentHandle_t h, float v, float& last, bool force)
{
    if (h == vr::k_ulInvalidInputComponentHandle) return;
    if (force || std::fabs(v - last) > 0.0005f) {
        vr::VRDriverInput()->UpdateScalarComponent(h, v, 0.0);
        last = v;
    }
}

void CtrlControllerDevice::PublishInputs(const Vrto3dCtrlControllerState& s, bool fingers_reported,
                                         float click_threshold, bool force)
{
    auto clamp01 = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    auto clamp11 = [](float v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
    const uint32_t btn = s.buttons, tch = s.touches | s.buttons;

    // Analog -> click with hysteresis, OR'd with an explicit click bit. The
    // emulated hardware has real switches that game bindings expect, so the
    // click has to come from somewhere when the source is purely analog.
    const float hyst = 0.05f;
    const bool grip_bit = (btn & VRTO3D_BTN_GRIP) != 0;
    const float grip = clamp01((std::max)(s.grip, grip_bit ? 1.0f : 0.0f));
    const bool trig_bit = (btn & VRTO3D_BTN_TRIGGER) != 0;
    const float trigger = clamp01((std::max)(s.trigger, trig_bit ? 1.0f : 0.0f));

    trigger_click_latched_ = trigger_click_latched_ ? (trigger > click_threshold - hyst)
                                                    : (trigger >= click_threshold + hyst);
    grip_click_latched_ = grip_click_latched_ ? (grip > click_threshold - hyst)
                                              : (grip >= click_threshold + hyst);
    const bool trigger_click = trig_bit || trigger_click_latched_;
    const bool grip_click = grip_bit || grip_click_latched_;

    const float tx = clamp11(s.thumbstick[0]), ty = clamp11(s.thumbstick[1]);
    const float px = clamp11(s.trackpad[0]), py = clamp11(s.trackpad[1]);
    const bool thumb_moved = (tx * tx + ty * ty) > 0.01f;
    const bool pad_moved = (px * px + py * py) > 0.0025f;

    if (type_ == EmulatedType::ViveController) {
        // Vive wand: trackpad doubles as the thumbstick; A / trackpad bit /
        // stick click press the pad; B or MENU is the application menu.
        const bool use_stick = !pad_moved && thumb_moved;
        const float vx = use_stick ? tx : px, vy = use_stick ? ty : py;
        const bool pad_click = (btn & (VRTO3D_BTN_TRACKPAD | VRTO3D_BTN_THUMBSTICK | VRTO3D_BTN_A)) != 0;
        Bool(h_.system_click, (btn & VRTO3D_BTN_SYSTEM) != 0, last_.b[kSysClick], force);
        Bool(h_.menu_click, (btn & (VRTO3D_BTN_MENU | VRTO3D_BTN_B)) != 0, last_.b[kMenuClick], force);
        Bool(h_.grip_click, grip_click, last_.b[kGripClick], force);
        Scalar(h_.trigger_value, trigger, last_.f[kTrigValue], force);
        Bool(h_.trigger_click, trigger_click, last_.b[kTrigClick], force);
        Scalar(h_.pad_x, vx, last_.f[kPadX], force);
        Scalar(h_.pad_y, vy, last_.f[kPadY], force);
        Bool(h_.pad_click, pad_click, last_.b[kPadClick], force);
        Bool(h_.pad_touch, pad_click || pad_moved || thumb_moved ||
                               (tch & (VRTO3D_BTN_TRACKPAD | VRTO3D_BTN_THUMBSTICK)) != 0,
             last_.b[kPadTouch], force);
        return;
    }

    Bool(h_.system_click, (btn & VRTO3D_BTN_SYSTEM) != 0, last_.b[kSysClick], force);
    Bool(h_.system_touch, (tch & VRTO3D_BTN_SYSTEM) != 0, last_.b[kSysTouch], force);
    Bool(h_.a_click, (btn & VRTO3D_BTN_A) != 0, last_.b[kAClick], force);
    Bool(h_.a_touch, (tch & VRTO3D_BTN_A) != 0, last_.b[kATouch], force);
    Bool(h_.b_click, (btn & (VRTO3D_BTN_B | VRTO3D_BTN_MENU)) != 0, last_.b[kBClick], force);
    Bool(h_.b_touch, (tch & (VRTO3D_BTN_B | VRTO3D_BTN_MENU)) != 0, last_.b[kBTouch], force);

    Scalar(h_.trigger_value, trigger, last_.f[kTrigValue], force);
    Bool(h_.trigger_click, trigger_click, last_.b[kTrigClick], force);
    Bool(h_.trigger_touch, trigger_click || trigger > 0.02f || (tch & VRTO3D_BTN_TRIGGER) != 0,
         last_.b[kTrigTouch], force);

    Scalar(h_.grip_value, grip, last_.f[kGripValue], force);
    Scalar(h_.grip_force, clamp01(s.grip_force), last_.f[kGripForce], force);
    Bool(h_.grip_touch, grip > 0.02f || (tch & VRTO3D_BTN_GRIP) != 0, last_.b[kGripTouch], force);

    const bool thumb_click = (btn & VRTO3D_BTN_THUMBSTICK) != 0;
    Scalar(h_.thumb_x, tx, last_.f[kThumbX], force);
    Scalar(h_.thumb_y, ty, last_.f[kThumbY], force);
    Bool(h_.thumb_click, thumb_click, last_.b[kThumbClick], force);
    Bool(h_.thumb_touch, thumb_click || thumb_moved || (tch & VRTO3D_BTN_THUMBSTICK) != 0,
         last_.b[kThumbTouch], force);

    const bool pad_click = (btn & VRTO3D_BTN_TRACKPAD) != 0;
    Scalar(h_.pad_x, px, last_.f[kPadX], force);
    Scalar(h_.pad_y, py, last_.f[kPadY], force);
    Bool(h_.pad_touch, pad_click || pad_moved || (tch & VRTO3D_BTN_TRACKPAD) != 0,
         last_.b[kPadTouch], force);
    Scalar(h_.pad_force, clamp01((std::max)(s.trackpad_force, pad_click ? 1.0f : 0.0f)),
           last_.f[kPadForce], force);

    // Finger curls: reported values when a plugin supplies them, otherwise
    // index follows the trigger and the other three follow the grip so
    // games that read finger curl still see a closing hand.
    float curl[4] = {trigger, grip, grip, grip};
    if (fingers_reported) {
        for (int i = 0; i < 4; ++i) curl[i] = (std::max)(curl[i], clamp01(s.finger_curl[i + 1]));
    }
    for (int i = 0; i < 4; ++i) Scalar(h_.finger[i], curl[i], last_.f[kFinger0 + i], force);
}

HeadFrame CtrlControllerDevice::AnchorFrame(const HeadFrame& head) const
{
    HeadFrame f = head;
    if (anchor_ == VRTO3D_ANCHOR_HEADING) f.rot = YawOnly(head.rot);
    return f;
}

void CtrlControllerDevice::PublishAimLocked(const Vec3& aim_pos, const Quat& aim_rot, double now)
{
    // Aim -> raw: undo the render model's tip transform.
    const TipTransform tip = TipFor(type_, hand_);
    const Quat raw_rot = Normalize(aim_rot * Conjugate(TipRotation(tip)));
    const Vec3 raw_pos = aim_pos - Rotate(raw_rot, tip.origin);

    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation = {1, 0, 0, 0};
    pose.qDriverFromHeadRotation = {1, 0, 0, 0};
    pose.qRotation = ToVr(raw_rot);
    pose.vecPosition[0] = raw_pos.x;
    pose.vecPosition[1] = raw_pos.y;
    pose.vecPosition[2] = raw_pos.z;

    const double dt = last_publish_ > 0.0 ? now - last_publish_ : 0.0;
    if (have_plugin_velocity_ && anchor_ == VRTO3D_ANCHOR_WORLD) {
        pose.vecVelocity[0] = plugin_vel_.x;
        pose.vecVelocity[1] = plugin_vel_.y;
        pose.vecVelocity[2] = plugin_vel_.z;
        pose.vecAngularVelocity[0] = plugin_ang_vel_.x;
        pose.vecAngularVelocity[1] = plugin_ang_vel_.y;
        pose.vecAngularVelocity[2] = plugin_ang_vel_.z;
    } else if (derive_velocity_ && was_tracked_ && dt > 1e-4) {
        // Smoothed finite difference; raw differences of keyboard/mouse
        // driven poses are too spiky to feed SteamVR's pose prediction.
        const Vec3 v = (raw_pos - prev_pos_) * (1.0 / dt);
        const Vec3 w = AngularVelocity(prev_rot_, raw_rot, dt);
        const double a = 0.25;
        vel_ = vel_ * (1.0 - a) + v * a;
        ang_vel_ = ang_vel_ * (1.0 - a) + w * a;
        pose.vecVelocity[0] = vel_.x;
        pose.vecVelocity[1] = vel_.y;
        pose.vecVelocity[2] = vel_.z;
        pose.vecAngularVelocity[0] = ang_vel_.x;
        pose.vecAngularVelocity[1] = ang_vel_.y;
        pose.vecAngularVelocity[2] = ang_vel_.z;
    } else {
        vel_ = {};
        ang_vel_ = {};
    }
    prev_pos_ = raw_pos;
    prev_rot_ = raw_rot;
    last_publish_ = now;

    // Head-anchored poses are exact for the HMD pose just published, so they
    // carry no time offset of their own (the HMD has no velocity either).
    pose.poseTimeOffset = anchor_ == VRTO3D_ANCHOR_WORLD ? pose_time_offset_ : 0.0;
    pose.poseIsValid = true;
    pose.deviceIsConnected = true;
    pose.result = tracking_ == VRTO3D_TRACKING_OUT_OF_RANGE ? vr::TrackingResult_Running_OutOfRange
                                                            : vr::TrackingResult_Running_OK;
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    PublishPose(pose);
    was_tracked_ = true;
}

void CtrlControllerDevice::FollowHead(const HeadFrame& head, double now)
{
    if (!IsActive() || !head.valid) return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    head_ = head;
    if (!connected_ || anchor_ == VRTO3D_ANCHOR_WORLD) return;
    const HeadFrame f = AnchorFrame(head_);
    PublishAimLocked(f.pos + Rotate(f.rot, rel_pos_), Normalize(f.rot * rel_rot_), now);
}

void CtrlControllerDevice::Update(const MergedHand& m, const CtrlConfig& cfg, bool enabled, double now,
                                  const HeadFrame& head, bool head_hook)
{
    if (!IsActive()) return;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!head_hook && head.valid) head_ = head;
        derive_velocity_ = cfg.derive_velocity;

        if (!enabled || !m.tracked) {
            connected_ = false;
            // Report "disconnected" right away on the transition, then once a
            // second so SteamVR keeps the device greyed rather than frozen.
            if (was_tracked_ || now - last_disconnected_publish_ > 1.0) {
                vr::DriverPose_t pose{};
                pose.qWorldFromDriverRotation = {1, 0, 0, 0};
                pose.qDriverFromHeadRotation = {1, 0, 0, 0};
                pose.qRotation = {1, 0, 0, 0};
                pose.poseIsValid = false;
                pose.deviceIsConnected = false;
                pose.result = vr::TrackingResult_Uninitialized;
                PublishPose(pose);
                last_disconnected_publish_ = now;
                was_tracked_ = false;
            }
        } else {
            // Aim pose from the plugin, plus the user's per-hand offset.
            const Quat plugin_rot =
                Normalize(Quat{m.s.rotation[0], m.s.rotation[1], m.s.rotation[2], m.s.rotation[3]});
            Vec3 aim_pos{m.s.position[0], m.s.position[1], m.s.position[2]};
            const HandOffset& off = cfg.offset[hand_];
            aim_pos = aim_pos + Rotate(plugin_rot, Vec3{off.pos[0], off.pos[1], off.pos[2]});
            const Quat aim_rot = Normalize(plugin_rot * FromYawPitchRoll(off.rot[1], off.rot[0], off.rot[2]));

            tracking_ = m.s.tracking;
            have_plugin_velocity_ = m.velocity_reported;
            plugin_vel_ = {m.s.velocity[0], m.s.velocity[1], m.s.velocity[2]};
            plugin_ang_vel_ = {m.s.angular_velocity[0], m.s.angular_velocity[1], m.s.angular_velocity[2]};
            pose_time_offset_ = m.s.sample_time_s > 0.0 ? (m.s.sample_time_s - now) : 0.0;
            if (pose_time_offset_ < -0.25 || pose_time_offset_ > 0.05) pose_time_offset_ = 0.0;

            const uint32_t anchor = m.s.pose_anchor;
            anchor_ = (head.valid && (anchor == VRTO3D_ANCHOR_HEAD || anchor == VRTO3D_ANCHOR_HEADING))
                          ? anchor : (uint32_t)VRTO3D_ANCHOR_WORLD;
            connected_ = true;

            if (anchor_ == VRTO3D_ANCHOR_WORLD) {
                PublishAimLocked(aim_pos, aim_rot, now);
            } else {
                // Store the hand relative to the HMD pose the plugin used,
                // then place it against the newest HMD pose we know.
                const HeadFrame used = AnchorFrame(head);
                const Quat inv = Conjugate(used.rot);
                rel_pos_ = Rotate(inv, aim_pos - used.pos);
                rel_rot_ = Normalize(inv * aim_rot);
                const HeadFrame f = AnchorFrame(head_.valid ? head_ : head);
                PublishAimLocked(f.pos + Rotate(f.rot, rel_pos_), Normalize(f.rot * rel_rot_), now);
            }
        }
    }

    Vrto3dCtrlControllerState idle{};
    const Vrto3dCtrlControllerState& in = enabled ? m.s : idle;
    PublishInputs(in, enabled && m.fingers_reported, cfg.click_threshold, !inputs_published_);
    inputs_published_ = true;
}

}  // namespace vrto3d::ctrl
