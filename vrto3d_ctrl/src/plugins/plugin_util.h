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

// Helpers shared by the built-in plugins. They only use the public plugin
// ABI, exactly like a third-party plugin would.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../ctrl_math.h"
#include "vrto3d_ctrl_plugin.h"
#include "vrto3dlib/key_names.h"

namespace vrto3d::ctrl::plugin {

// Thin wrapper over the host's settings getters.
class Settings {
public:
    explicit Settings(const Vrto3dCtrlHostApi* host = nullptr) : host_(host) {}
    void Bind(const Vrto3dCtrlHostApi* host) { host_ = host; }

    bool Bool(const char* key, bool def) const
    {
        return host_ ? host_->get_bool(host_->ctx, key, def ? 1 : 0) != 0 : def;
    }
    double Float(const char* key, double def) const
    {
        return host_ ? host_->get_float(host_->ctx, key, def) : def;
    }
    int64_t Int(const char* key, int64_t def) const
    {
        return host_ ? host_->get_int(host_->ctx, key, def) : def;
    }
    std::string String(const char* key) const
    {
        if (!host_) return {};
        std::string s(64, '\0');
        uint32_t n = host_->get_string(host_->ctx, key, &s[0], (uint32_t)s.size());
        if (n >= s.size()) {
            s.assign(n + 1, '\0');
            n = host_->get_string(host_->ctx, key, &s[0], (uint32_t)s.size());
        }
        s.resize(n);
        return s;
    }
    nlohmann::json Json(const char* key) const
    {
        if (!host_) return nullptr;
        std::string s(256, '\0');
        uint32_t n = host_->get_json(host_->ctx, key, &s[0], (uint32_t)s.size());
        if (n >= s.size()) {
            s.assign(n + 1, '\0');
            n = host_->get_json(host_->ctx, key, &s[0], (uint32_t)s.size());
        }
        s.resize(n);
        try {
            return nlohmann::json::parse(s);
        } catch (const std::exception&) {
            return nullptr;
        }
    }
    void Log(int level, const std::string& msg) const
    {
        if (host_) host_->log(host_->ctx, level, msg.c_str());
    }

private:
    const Vrto3dCtrlHostApi* host_;
};

// Keyboard / mouse key name ("Key_W", "Mouse_Left") -> VK code; 0 = unbound.
inline int KeyCode(const std::string& name)
{
    if (name.empty()) return 0;
    const int vk = vrto3d::keys::KeyCodeFromName(name);
    return vk > 0 ? vk : 0;
}

struct Pose {
    Vec3 pos;
    Quat rot;
};

inline Pose HeadPose(const Vrto3dCtrlHeadPose& head)
{
    return {{head.position[0], head.position[1], head.position[2]},
            Normalize(Quat{head.rotation[0], head.rotation[1], head.rotation[2], head.rotation[3]})};
}

inline void WritePose(Vrto3dCtrlControllerState& s, const Pose& p)
{
    s.tracking = VRTO3D_TRACKING_OK;
    s.position[0] = p.pos.x;
    s.position[1] = p.pos.y;
    s.position[2] = p.pos.z;
    s.rotation[0] = p.rot.w;
    s.rotation[1] = p.rot.x;
    s.rotation[2] = p.rot.y;
    s.rotation[3] = p.rot.z;
}

// Hand placement shared by the keyboard/mouse and gamepad plugins: the
// hands hang in front of the body, following the head's heading (yaw) but
// not its pitch, pointing forward and slightly down.
struct RestLayout {
    double forward = 0.35;  // metres in front of the eyes
    double side = 0.18;     // metres left / right of centre
    double down = 0.30;     // metres below the eyes
    double pitch = -15.0;   // degrees, aim direction relative to level

    void Read(const Settings& s)
    {
        forward = s.Float("hand_forward", forward);
        side = s.Float("hand_side", side);
        down = s.Float("hand_down", down);
        pitch = s.Float("hand_pitch", pitch);
    }
};

inline Pose RestPose(const Pose& head, int hand, const RestLayout& l)
{
    const Quat yaw = YawOnly(head.rot);
    const double sx = hand == VRTO3D_HAND_LEFT ? -l.side : l.side;
    return {head.pos + Rotate(yaw, Vec3{sx, -l.down, -l.forward}),
            Normalize(yaw * RotX(DegToRad(l.pitch)))};
}

// Hand held up in front of the face, pointing at whatever is
// `converge_m` straight ahead of the eyes, so a game's laser or gun sight
// lands on the centre of the view.
inline Pose AimPose(const Pose& head, int hand, double distance, double side, double down,
                    double converge_m)
{
    const double sx = hand == VRTO3D_HAND_LEFT ? -side : side;
    const Vec3 pos = head.pos + Rotate(head.rot, Vec3{sx, -down, -distance});
    const Vec3 target = head.pos + Rotate(head.rot, Vec3{0, 0, -converge_m});
    return {pos, LookRotation(target - pos)};
}

// Schema-building helpers.
inline nlohmann::json SBool(const char* key, const char* label, bool def, const char* help = nullptr)
{
    nlohmann::json j{{"key", key}, {"type", "bool"}, {"label", label}, {"default", def}};
    if (help) j["help"] = help;
    return j;
}
inline nlohmann::json SFloat(const char* key, const char* label, double def, double mn, double mx,
                             const char* help = nullptr)
{
    nlohmann::json j{{"key", key}, {"type", "float"}, {"label", label},
                     {"default", def}, {"min", mn}, {"max", mx}};
    if (help) j["help"] = help;
    return j;
}
inline nlohmann::json SInt(const char* key, const char* label, int def, int mn, int mx,
                           const char* help = nullptr)
{
    nlohmann::json j{{"key", key}, {"type", "int"}, {"label", label},
                     {"default", def}, {"min", mn}, {"max", mx}};
    if (help) j["help"] = help;
    return j;
}
inline nlohmann::json SEnum(const char* key, const char* label, const char* def,
                            std::vector<std::string> values, const char* help = nullptr)
{
    nlohmann::json j{{"key", key}, {"type", "enum"}, {"label", label},
                     {"default", def}, {"values", values}};
    if (help) j["help"] = help;
    return j;
}
inline nlohmann::json SKey(const char* key, const char* label, const char* def)
{
    return {{"key", key}, {"type", "keybind"}, {"label", label}, {"default", def}};
}
inline nlohmann::json SHeader(const char* label)
{
    return {{"type", "header"}, {"label", label}};
}

inline void AddRestLayout(nlohmann::json& settings)
{
    settings.push_back(SHeader("Hand placement"));
    settings.push_back(SFloat("hand_forward", "Hands forward (m)", 0.35, 0.0, 1.0));
    settings.push_back(SFloat("hand_side", "Hands apart (m, each side)", 0.18, 0.0, 0.6));
    settings.push_back(SFloat("hand_down", "Hands below eyes (m)", 0.30, -0.3, 1.0));
    settings.push_back(SFloat("hand_pitch", "Hand pitch (deg)", -15.0, -90.0, 90.0));
}

}  // namespace vrto3d::ctrl::plugin
