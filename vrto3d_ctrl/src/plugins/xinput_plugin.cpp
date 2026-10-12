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

// Built-in "xinput" plugin: an Xbox-style gamepad split across two emulated
// hands. Left half of the pad -> left controller, right half -> right.
//
//   LT / RT          trigger            LB / RB      grip
//   Left stick       left thumbstick    Right stick  right thumbstick
//                                                    (or aims the right hand)
//   X / Y            left A / B         A / B        right A / B
//   LS / RS click    thumbstick click   D-pad        left trackpad
//
// Hands hang in front of the body and follow the head's heading, so a
// gamepad alone is enough for pointer-style VR games.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")
#else
#include "vrto3dlib/input_state.h"
#include "vrto3dlib/key_codes.h"
#endif

#include "plugin_util.h"

namespace {

using namespace vrto3d::ctrl;
using namespace vrto3d::ctrl::plugin;

struct PadState {
    bool connected = false;
    uint16_t buttons = 0;
    float lt = 0, rt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

class XInputPlugin {
public:
    XInputPlugin()
    {
        nlohmann::json s = nlohmann::json::array();
        s.push_back(SInt("controller_index", "Controller slot", 0, 0, 3,
                         "XInput user index (0 = first pad)."));
        s.push_back(SFloat("deadzone", "Stick deadzone", 0.15, 0.0, 0.9));
        s.push_back(SFloat("trigger_deadzone", "Trigger deadzone", 0.05, 0.0, 0.9));
        s.push_back(SEnum("right_stick", "Right stick", "thumbstick", {"thumbstick", "aim_right_hand"},
                          "thumbstick: right controller thumbstick. aim_right_hand: the stick "
                          "tilts the right hand so you can point without moving the head."));
        s.push_back(SFloat("aim_range", "Aim range (deg)", 45.0, 5.0, 90.0));
        s.push_back(SBool("dpad_trackpad", "D-pad as left trackpad", true));
        s.push_back(SBool("provide_pose", "Place hands in front of the body", true,
                          "Turn off when another plugin (e.g. remote) supplies the poses "
                          "and the gamepad should only add buttons."));
        s.push_back(SBool("rumble", "Rumble on haptics", true));
        s.push_back(SFloat("rumble_strength", "Rumble strength", 1.0, 0.0, 1.0));
        AddRestLayout(s);
        schema_ = nlohmann::json{{"settings", s}}.dump();
    }

    int Init(const Vrto3dCtrlHostApi* host)
    {
        host_ = host;
        settings_.Bind(host);
        Read();
#ifndef _WIN32
        vrto3d::input::Start();
#endif
        return 0;
    }

    void Shutdown()
    {
        SetRumble(0, 0);
    }

    void Read()
    {
        index_ = (int)settings_.Int("controller_index", 0);
        if (index_ < 0 || index_ > 3) index_ = 0;
        deadzone_ = (float)settings_.Float("deadzone", 0.15);
        trigger_deadzone_ = (float)settings_.Float("trigger_deadzone", 0.05);
        aim_stick_ = settings_.String("right_stick") == "aim_right_hand";
        aim_range_ = settings_.Float("aim_range", 45.0);
        dpad_trackpad_ = settings_.Bool("dpad_trackpad", true);
        provide_pose_ = settings_.Bool("provide_pose", true);
        rumble_ = settings_.Bool("rumble", true);
        rumble_strength_ = (float)settings_.Float("rumble_strength", 1.0);
        layout_.Read(settings_);
        if (!rumble_) SetRumble(0, 0);
    }

    void Poll(double dt, const Vrto3dCtrlHeadPose* head, Vrto3dCtrlControllerState out[2])
    {
        UpdateRumble(dt);
        PadState pad = ReadPad();
        // While the VRto3D OSD is open the pad navigates the menu; keep the
        // hands where they are but don't press anything in the game.
        if (host_ && host_->is_input_suppressed(host_->ctx)) pad = PadState{pad.connected};
        if (!pad.connected) return;

        auto stick = [&](float x, float y, float o[2]) {
            const float mag = std::sqrt(x * x + y * y);
            if (mag <= deadzone_) {
                o[0] = o[1] = 0.f;
                return;
            }
            const float scaled = (std::min)(1.0f, (mag - deadzone_) / (1.0f - deadzone_));
            o[0] = x / mag * scaled;
            o[1] = y / mag * scaled;
        };
        auto trig = [&](float v) {
            return v <= trigger_deadzone_ ? 0.f : (v - trigger_deadzone_) / (1.f - trigger_deadzone_);
        };

        Vrto3dCtrlControllerState& L = out[VRTO3D_HAND_LEFT];
        Vrto3dCtrlControllerState& R = out[VRTO3D_HAND_RIGHT];

        L.trigger = trig(pad.lt);
        R.trigger = trig(pad.rt);
        if (pad.buttons & kLB) L.buttons |= VRTO3D_BTN_GRIP;
        if (pad.buttons & kRB) R.buttons |= VRTO3D_BTN_GRIP;
        if (pad.buttons & kA) R.buttons |= VRTO3D_BTN_A;
        if (pad.buttons & kB) R.buttons |= VRTO3D_BTN_B;
        if (pad.buttons & kX) L.buttons |= VRTO3D_BTN_A;
        if (pad.buttons & kY) L.buttons |= VRTO3D_BTN_B;
        if (pad.buttons & kLS) L.buttons |= VRTO3D_BTN_THUMBSTICK;
        if (pad.buttons & kRS) R.buttons |= VRTO3D_BTN_THUMBSTICK;

        stick(pad.lx, pad.ly, L.thumbstick);
        float rs[2];
        stick(pad.rx, pad.ry, rs);
        if (!aim_stick_) {
            R.thumbstick[0] = rs[0];
            R.thumbstick[1] = rs[1];
        }

        if (dpad_trackpad_) {
            const float dx = ((pad.buttons & kRight) ? 1.f : 0.f) - ((pad.buttons & kLeft) ? 1.f : 0.f);
            const float dy = ((pad.buttons & kUp) ? 1.f : 0.f) - ((pad.buttons & kDown) ? 1.f : 0.f);
            if (dx != 0.f || dy != 0.f) {
                L.trackpad[0] = dx;
                L.trackpad[1] = dy;
                L.touches |= VRTO3D_BTN_TRACKPAD;
            }
        }

        if (!provide_pose_ || !head || !head->valid) return;
        const Pose h = HeadPose(*head);
        WritePose(L, RestPose(h, VRTO3D_HAND_LEFT, layout_));
        L.pose_anchor = VRTO3D_ANCHOR_HEADING;
        Pose rp = RestPose(h, VRTO3D_HAND_RIGHT, layout_);
        if (aim_stick_) {
            // Stick deflection tilts the right hand around its rest pose.
            rp.rot = Normalize(rp.rot * FromYawPitchRoll(-rs[0] * aim_range_, rs[1] * aim_range_, 0.0));
        }
        WritePose(R, rp);
        R.pose_anchor = VRTO3D_ANCHOR_HEADING;
    }

    void Haptic(uint32_t hand, float duration, float /*freq*/, float amplitude)
    {
        if (!rumble_) return;
        const double d = (std::max)(0.03, (double)duration);
        const float a = (std::max)(0.f, (std::min)(1.f, amplitude)) * rumble_strength_;
        if (hand == VRTO3D_HAND_LEFT) {
            left_amp_ = a;
            left_left_ = d;
        } else {
            right_amp_ = a;
            right_left_ = d;
        }
    }

    const char* Schema() const { return schema_.c_str(); }
    Settings& settings() { return settings_; }

private:
    // key_codes.h provides the XINPUT_GAMEPAD_* values on Linux.
    static constexpr uint16_t kA = XINPUT_GAMEPAD_A, kB = XINPUT_GAMEPAD_B, kX = XINPUT_GAMEPAD_X,
                              kY = XINPUT_GAMEPAD_Y, kLB = XINPUT_GAMEPAD_LEFT_SHOULDER,
                              kRB = XINPUT_GAMEPAD_RIGHT_SHOULDER, kLS = XINPUT_GAMEPAD_LEFT_THUMB,
                              kRS = XINPUT_GAMEPAD_RIGHT_THUMB, kUp = XINPUT_GAMEPAD_DPAD_UP,
                              kDown = XINPUT_GAMEPAD_DPAD_DOWN, kLeft = XINPUT_GAMEPAD_DPAD_LEFT,
                              kRight = XINPUT_GAMEPAD_DPAD_RIGHT;

    PadState ReadPad()
    {
        PadState p;
#ifdef _WIN32
        XINPUT_STATE st{};
        if (XInputGetState((DWORD)index_, &st) != ERROR_SUCCESS) return p;
        const auto& g = st.Gamepad;
        p.buttons = g.wButtons;
        p.lt = g.bLeftTrigger / 255.f;
        p.rt = g.bRightTrigger / 255.f;
        p.lx = (std::max)(-1.f, g.sThumbLX / 32767.f);
        p.ly = (std::max)(-1.f, g.sThumbLY / 32767.f);
        p.rx = (std::max)(-1.f, g.sThumbRX / 32767.f);
        p.ry = (std::max)(-1.f, g.sThumbRY / 32767.f);
        p.connected = true;
#else
        const auto g = vrto3d::input::GetGamepadState();
        if (!g.connected) return p;
        p.buttons = g.wButtons;
        p.lt = g.bLeftTrigger / 255.f;
        p.rt = g.bRightTrigger / 255.f;
        p.lx = (std::max)(-1.f, g.sThumbLX / 32767.f);
        p.ly = (std::max)(-1.f, g.sThumbLY / 32767.f);
        p.rx = (std::max)(-1.f, g.sThumbRX / 32767.f);
        p.ry = (std::max)(-1.f, g.sThumbRY / 32767.f);
        p.connected = true;
#endif
        return p;
    }

    void UpdateRumble(double dt)
    {
        left_left_ -= dt;
        right_left_ -= dt;
        const float l = left_left_ > 0 ? left_amp_ : 0.f;
        const float r = right_left_ > 0 ? right_amp_ : 0.f;
        if (l != applied_l_ || r != applied_r_) SetRumble(l, r);
    }

    void SetRumble(float l, float r)
    {
        applied_l_ = l;
        applied_r_ = r;
#ifdef _WIN32
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = (WORD)(l * 65535.f);   // low-frequency motor
        v.wRightMotorSpeed = (WORD)(r * 65535.f);  // high-frequency motor
        XInputSetState((DWORD)index_, &v);
#endif
    }

    const Vrto3dCtrlHostApi* host_ = nullptr;
    Settings settings_;
    std::string schema_;
    RestLayout layout_;
    int index_ = 0;
    float deadzone_ = 0.15f, trigger_deadzone_ = 0.05f;
    bool aim_stick_ = false;
    double aim_range_ = 45.0;
    bool dpad_trackpad_ = true;
    bool provide_pose_ = true;
    bool rumble_ = true;
    float rumble_strength_ = 1.0f;
    float left_amp_ = 0, right_amp_ = 0;
    double left_left_ = 0, right_left_ = 0;
    float applied_l_ = -1, applied_r_ = -1;
};

XInputPlugin* Self(void* u) { return static_cast<XInputPlugin*>(u); }

}  // namespace

extern "C" int vrto3d_builtin_xinput_entry(uint32_t abi, Vrto3dCtrlPlugin* out)
{
    if (abi != VRTO3D_CTRL_ABI_VERSION || !out) return -1;
    static XInputPlugin instance;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(Vrto3dCtrlPlugin);
    out->abi_version = VRTO3D_CTRL_ABI_VERSION;
    out->name = "xinput";
    out->display_name = "Gamepad (XInput)";
    out->version = "1.0.0";
    out->capabilities = VRTO3D_CAP_POSE | VRTO3D_CAP_BUTTONS | VRTO3D_CAP_HAPTICS;
    out->user = &instance;
    out->init = [](void* u, const Vrto3dCtrlHostApi* h) { return Self(u)->Init(h); };
    out->shutdown = [](void* u) { Self(u)->Shutdown(); };
    out->poll = [](void* u, double dt, const Vrto3dCtrlHeadPose* head, Vrto3dCtrlControllerState o[2]) {
        Self(u)->Poll(dt, head, o);
    };
    out->on_haptic = [](void* u, uint32_t hand, float d, float f, float a) { Self(u)->Haptic(hand, d, f, a); };
    out->on_settings_changed = [](void* u) { Self(u)->Read(); };
    out->settings_schema = [](void* u) { return Self(u)->Schema(); };
    return 0;
}
