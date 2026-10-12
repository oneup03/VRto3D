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

// Built-in "kbm" plugin: play VR-only games with a keyboard and mouse.
//
// The control scheme follows r57zone's Half-Life: Alyx NoVR driver
// (https://github.com/r57zone/Half-Life-Alyx-novr): the mouse turns the head,
// the right hand is held up in front of the face so its laser / gun follows
// the mouse, holding the right mouse button moves the left hand with the
// mouse instead, and gesture keys snap a hand to a body position (backpack
// over the shoulder, wrist, chest...). Only the behaviour is borrowed; the
// code is written from scratch for this driver.
//
// Head look is sent to VRto3D as OpenTrack packets, so VRto3D's OpenTrack
// input must be enabled. Mouse motion is read with Raw Input on a hidden
// window, which works no matter which window has focus and never fights the
// game over the cursor; the cursor can optionally be pinned to the middle of
// the game window so stray clicks never land outside it.
//
// Windows only.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "plugin_util.h"

namespace {

using namespace vrto3d::ctrl;
using namespace vrto3d::ctrl::plugin;

// --------------------------------------------------------------------------
// Raw Input mouse reader on its own thread with a message-only window.
// --------------------------------------------------------------------------
class RawMouse {
public:
    ~RawMouse() { Stop(); }

    bool Start()
    {
        if (thread_.joinable()) return true;
        ready_ = 0;
        thread_ = std::thread([this] { Run(); });
        while (ready_.load() == 0) std::this_thread::yield();
        return ready_.load() > 0;
    }

    void Stop()
    {
        if (!thread_.joinable()) return;
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        thread_.join();
        hwnd_ = nullptr;
    }

    // Called on the mouse thread every 100 ms (set before Start).
    std::function<void()> on_tick;

    // Motion accumulated since the previous call.
    void Take(long& dx, long& dy, int& wheel)
    {
        dx = dx_.exchange(0);
        dy = dy_.exchange(0);
        wheel = wheel_.exchange(0);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<RawMouse*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_INPUT:
            if (self) self->OnInput(reinterpret_cast<HRAWINPUT>(lp));
            break;
        case WM_TIMER:
            if (self && self->on_tick) self->on_tick();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void OnInput(HRAWINPUT h)
    {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
        if (ri.header.dwType != RIM_TYPEMOUSE) return;
        const RAWMOUSE& m = ri.data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {  // tablets / RDP report absolute; skip
            dx_ += m.lLastX;
            dy_ += m.lLastY;
        }
        if (m.usButtonFlags & RI_MOUSE_WHEEL) wheel_ += (short)m.usButtonData / WHEEL_DELTA;
    }

    void Run()
    {
        const wchar_t* cls = L"VRto3DCtrlRawMouse";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);  // fails harmlessly if already registered

        hwnd_ = CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;  // generic desktop
        rid.usUsage = 0x02;      // mouse
        rid.dwFlags = RIDEV_INPUTSINK;  // deliver even when another app has focus
        rid.hwndTarget = hwnd_;
        if (!hwnd_ || !RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            if (hwnd_) DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            ready_ = -1;
            return;
        }
        ready_ = 1;
        SetTimer(hwnd_, 1, 100, nullptr);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    std::thread thread_;
    std::atomic<int> ready_{0};
    HWND hwnd_ = nullptr;
    std::atomic<long> dx_{0}, dy_{0};
    std::atomic<int> wheel_{0};
};

// --------------------------------------------------------------------------
// Cursor capture while playing.
//
// Mouse motion is read with Raw Input, which still reports movement when the
// cursor cannot move. So instead of warping the cursor back to the centre
// every tick (which Windows draws as a trail of cursors jumping between the
// two spots), the cursor is clipped to a single pixel in the middle of the
// focused window and, optionally, every system cursor is swapped for a blank
// one - the same cross-process hide VRto3D's hide_cursor uses. Everything is
// undone the moment the game loses focus, the OSD opens, or the plugin stops
// being polled. Methods may be called from the input thread and the mouse
// thread's watchdog, hence the mutex.
// --------------------------------------------------------------------------
class CursorLock {
public:
    void Engage(bool hide)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (now < next_assert_) return;
        next_assert_ = now + std::chrono::milliseconds(10);

        HWND fg = GetForegroundWindow();
        if (!fg) return;
        RECT rc{};
        if (!GetClientRect(fg, &rc)) return;
        POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
        if (!ClientToScreen(fg, &c)) return;

        // Windows drops the clip on every focus change and rejects clips from
        // a process that doesn't share the foreground window's input queue.
        const DWORD my_tid = GetCurrentThreadId();
        const DWORD fg_tid = GetWindowThreadProcessId(fg, nullptr);
        const BOOL attached = (fg_tid && fg_tid != my_tid) ? AttachThreadInput(my_tid, fg_tid, TRUE) : FALSE;

        const RECT want{c.x, c.y, c.x + 1, c.y + 1};
        RECT have{};
        if (!GetClipCursor(&have) || have.left != want.left || have.top != want.top ||
            have.right != want.right || have.bottom != want.bottom) {
            ClipCursor(&want);
        }
        clipped_ = true;

        if (hide) {
            if (!blanked_) blanked_ = BlankSystemCursors();
            SetCursor(nullptr);
        } else if (blanked_) {
            RestoreCursorsLocked();
        }
        if (attached) AttachThreadInput(my_tid, fg_tid, FALSE);
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (clipped_) {
            ClipCursor(nullptr);
            clipped_ = false;
        }
        RestoreCursorsLocked();
        next_assert_ = {};
    }

    bool Engaged()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return clipped_ || blanked_;
    }

private:
    void RestoreCursorsLocked()
    {
        if (blanked_) {
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);  // reload the user's scheme
            blanked_ = false;
        }
    }

    static bool BlankSystemCursors()
    {
        static const DWORD kIds[] = {32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644,
                                     32645, 32646, 32648, 32649, 32650, 32651};
        const int w = GetSystemMetrics(SM_CXCURSOR);
        const int h = GetSystemMetrics(SM_CYCURSOR);
        if (w <= 0 || h <= 0) return false;
        const size_t bytes = static_cast<size_t>(((w + 15) / 16) * 2) * h;
        std::vector<BYTE> and_mask(bytes, 0xFF), xor_mask(bytes, 0x00);  // fully transparent
        bool any = false;
        for (DWORD id : kIds) {
            HCURSOR blank = CreateCursor(nullptr, 0, 0, w, h, and_mask.data(), xor_mask.data());
            if (!blank) continue;
            if (SetSystemCursor(blank, id)) any = true;  // takes ownership on success
            else DestroyCursor(blank);
        }
        return any;
    }

    std::mutex mutex_;
    bool clipped_ = false;
    bool blanked_ = false;
    std::chrono::steady_clock::time_point next_assert_{};
};

// --------------------------------------------------------------------------
// Key with edge detection.
// --------------------------------------------------------------------------
struct Key {
    int vk = 0;
    bool down = false, was = false;
    void Set(const std::string& name) { vk = KeyCode(name); }
    void Update(bool active)
    {
        was = down;
        down = active && vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    bool Pressed() const { return down && !was; }
};

struct Gesture {
    Key key;
    int hand = VRTO3D_HAND_RIGHT;
    Vec3 pos;                  // metres, head-heading frame (x right, y up, -z forward)
    double pitch = 0, yaw = 0, roll = 0;  // aim direction, degrees
    bool grip = false;         // hold grip while the gesture key is held
};

nlohmann::json DefaultGestures()
{
    // Positions in the head-heading frame. Ported behaviourally from the
    // Alyx NoVR presets: reach over the shoulder for the backpack, to the
    // opposite wrist for the wrist pocket, to the mouth for food / gas mask.
    using nlohmann::json;
    return json::array({
        json{{"name", "Backpack (over right shoulder)"}, {"key", "Key_1"}, {"hand", "right"},
             {"x", 0.15}, {"y", 0.05}, {"z", 0.20}, {"pitch", 70.0}, {"yaw", 0.0}, {"roll", 0.0},
             {"grip", true}},
        json{{"name", "Backpack (over left shoulder)"}, {"key", "Key_2"}, {"hand", "left"},
             {"x", -0.15}, {"y", 0.05}, {"z", 0.20}, {"pitch", 70.0}, {"yaw", 0.0}, {"roll", 0.0},
             {"grip", true}},
        json{{"name", "Left wrist pocket"}, {"key", "Key_3"}, {"hand", "right"},
             {"x", -0.10}, {"y", -0.35}, {"z", -0.30}, {"pitch", -30.0}, {"yaw", 30.0}, {"roll", 0.0},
             {"grip", true}},
        json{{"name", "Mouth (eat / gas mask)"}, {"key", "Key_4"}, {"hand", "right"},
             {"x", 0.0}, {"y", -0.08}, {"z", -0.08}, {"pitch", 0.0}, {"yaw", 0.0}, {"roll", 0.0},
             {"grip", false}},
        json{{"name", "Chest"}, {"key", "Key_5"}, {"hand", "right"},
             {"x", 0.0}, {"y", -0.35}, {"z", -0.15}, {"pitch", -20.0}, {"yaw", 0.0}, {"roll", 0.0},
             {"grip", false}},
        json{{"name", "Right hip"}, {"key", "Key_6"}, {"hand", "right"},
             {"x", 0.22}, {"y", -0.70}, {"z", 0.0}, {"pitch", -80.0}, {"yaw", 0.0}, {"roll", 0.0},
             {"grip", false}},
    });
}

class KbmPlugin {
public:
    KbmPlugin()
    {
        using nlohmann::json;
        json s = json::array();
        s.push_back(SHeader("Mouse"));
        s.push_back(SFloat("mouse_sens_yaw", "Yaw sensitivity (deg/count)", 0.08, 0.005, 1.0));
        s.push_back(SFloat("mouse_sens_pitch", "Pitch sensitivity (deg/count)", 0.06, 0.005, 1.0));
        s.push_back(SBool("invert_y", "Invert Y", false));
        s.push_back(SBool("head_look", "Mouse turns the head", true,
                          "Sends the view to VRto3D over OpenTrack. Enable OpenTrack in "
                          "VRto3D's Tracking tab, and turn off XInput pitch/yaw there."));
        s.push_back(SBool("require_focus", "Only while the game has focus", true));
        s.push_back(SFloat("motion_sens", "Hand move speed (m/count)", 0.0008, 0.0001, 0.01));

        s.push_back(SHeader("Aiming"));
        s.push_back(SBool("aim_right", "Right hand aims where you look", true));
        s.push_back(SBool("aim_left", "Left hand aims where you look", false));
        s.push_back(SFloat("aim_distance", "Aiming hand distance (m)", 0.35, 0.1, 1.0));
        s.push_back(SFloat("aim_side", "Aiming hand offset sideways (m)", 0.08, 0.0, 0.4));
        s.push_back(SFloat("aim_down", "Aiming hand below eyes (m)", 0.12, -0.2, 0.5));
        s.push_back(SFloat("converge_distance", "Aim converges at (m)", 5.0, 0.5, 50.0,
                           "The hand points at the spot this far straight ahead of your "
                           "eyes, so its laser meets the middle of the view."));

        s.push_back(SHeader("Height"));
        s.push_back(SFloat("crouch_depth", "Crouch depth (cm)", 60.0, 0.0, 120.0));
        s.push_back(SFloat("height_speed", "Height change speed (cm/s)", 50.0, 5.0, 200.0));

        s.push_back(SHeader("Right hand"));
        s.push_back(SKey("right_trigger", "Trigger", "Mouse_Left"));
        s.push_back(SKey("right_grip", "Grip", "Key_E"));
        s.push_back(SKey("right_a", "A", "Key_F"));
        s.push_back(SKey("right_b", "B", "Key_R"));
        s.push_back(SKey("right_stick_click", "Thumbstick click", "Mouse_Middle"));
        s.push_back(SKey("right_stick_up", "Thumbstick up", "Key_Up"));
        s.push_back(SKey("right_stick_down", "Thumbstick down", "Key_Down"));
        s.push_back(SKey("right_stick_left", "Thumbstick left", "Key_Left"));
        s.push_back(SKey("right_stick_right", "Thumbstick right", "Key_Right"));

        s.push_back(SHeader("Left hand"));
        s.push_back(SKey("left_trigger", "Trigger", "Key_Q"));
        s.push_back(SKey("left_grip", "Grip", "Key_Shift"));
        s.push_back(SKey("left_a", "A", "Key_X"));
        s.push_back(SKey("left_b", "B", "Key_Tab"));
        s.push_back(SKey("left_stick_click", "Thumbstick click", "Key_C"));
        s.push_back(SKey("left_stick_up", "Thumbstick up (move)", "Key_W"));
        s.push_back(SKey("left_stick_down", "Thumbstick down", "Key_S"));
        s.push_back(SKey("left_stick_left", "Thumbstick left", "Key_A"));
        s.push_back(SKey("left_stick_right", "Thumbstick right", "Key_D"));

        s.push_back(SHeader("Body"));
        s.push_back(SKey("motion_hold", "Hold: mouse moves a hand", "Mouse_Right"));
        s.push_back(SEnum("motion_hand", "Hand moved by the mouse", "left", {"left", "right"}));
        s.push_back(SBool("motion_sticky", "Hand stays where you moved it", false,
                          "Off: the hand springs back when you release the button."));
        s.push_back(SKey("crouch", "Crouch (hold)", "Key_Ctrl"));
        s.push_back(SKey("height_up", "Stand taller", "Key_PageUp"));
        s.push_back(SKey("height_down", "Stand lower", "Key_PageDown"));
        s.push_back(SKey("aim_toggle", "Toggle right-hand aiming", ""));
        s.push_back(SKey("recenter", "Recenter view and hands", "Key_End"));

        json gestures{{"key", "gestures"}, {"type", "table"}, {"label", "Gestures (hold key)"},
                      {"help", "Hold the key to move a hand to a spot around your body. "
                               "x right, y up, z back, in metres from your eyes."},
                      {"default", DefaultGestures()}};
        gestures["columns"] = json::array({
            json{{"key", "name"}, {"type", "string"}, {"label", "Name"}, {"default", ""}},
            json{{"key", "key"}, {"type", "keybind"}, {"label", "Key"}, {"default", ""}},
            json{{"key", "hand"}, {"type", "enum"}, {"label", "Hand"}, {"default", "right"},
                 {"values", {"left", "right"}}},
            json{{"key", "x"}, {"type", "float"}, {"label", "X"}, {"default", 0.0}, {"min", -1.0}, {"max", 1.0}},
            json{{"key", "y"}, {"type", "float"}, {"label", "Y"}, {"default", 0.0}, {"min", -1.5}, {"max", 1.0}},
            json{{"key", "z"}, {"type", "float"}, {"label", "Z"}, {"default", 0.0}, {"min", -1.0}, {"max", 1.0}},
            json{{"key", "pitch"}, {"type", "float"}, {"label", "Pitch"}, {"default", 0.0}, {"min", -180.0}, {"max", 180.0}},
            json{{"key", "yaw"}, {"type", "float"}, {"label", "Yaw"}, {"default", 0.0}, {"min", -180.0}, {"max", 180.0}},
            json{{"key", "roll"}, {"type", "float"}, {"label", "Roll"}, {"default", 0.0}, {"min", -180.0}, {"max", 180.0}},
            json{{"key", "grip"}, {"type", "bool"}, {"label", "Grip"}, {"default", false}},
        });
        s.push_back(gestures);
        AddRestLayout(s);
        schema_ = json{{"settings", s}}.dump();
    }

    int Init(const Vrto3dCtrlHostApi* host)
    {
        host_ = host;
        settings_.Bind(host);
        // Watchdog: if the host stops polling us (plugin or controllers
        // switched off, plugin faulted), give the cursor back.
        mouse_.on_tick = [this] {
            const double since = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - last_poll_s_.load();
            if (since > 0.3 && cursor_.Engaged()) cursor_.Release();
        };
        if (!mouse_.Start()) {
            settings_.Log(VRTO3D_LOG_ERROR, "could not register for raw mouse input");
            return -1;
        }
        Read();
        return 0;
    }

    void Shutdown()
    {
        mouse_.Stop();
        cursor_.Release();
    }

    void Read()
    {
        sens_yaw_ = settings_.Float("mouse_sens_yaw", 0.08);
        sens_pitch_ = settings_.Float("mouse_sens_pitch", 0.06);
        invert_y_ = settings_.Bool("invert_y", false);
        head_look_ = settings_.Bool("head_look", true);
        require_focus_ = settings_.Bool("require_focus", true);
        motion_sens_ = settings_.Float("motion_sens", 0.0008);
        aim_[VRTO3D_HAND_RIGHT] = settings_.Bool("aim_right", true);
        aim_[VRTO3D_HAND_LEFT] = settings_.Bool("aim_left", false);
        aim_distance_ = settings_.Float("aim_distance", 0.35);
        aim_side_ = settings_.Float("aim_side", 0.08);
        aim_down_ = settings_.Float("aim_down", 0.12);
        converge_ = settings_.Float("converge_distance", 5.0);
        crouch_cm_ = settings_.Float("crouch_depth", 60.0);
        height_speed_ = settings_.Float("height_speed", 50.0);
        motion_hand_ = settings_.String("motion_hand") == "right" ? VRTO3D_HAND_RIGHT : VRTO3D_HAND_LEFT;
        motion_sticky_ = settings_.Bool("motion_sticky", false);
        layout_.Read(settings_);

        const char* names[] = {"trigger", "grip", "a", "b", "stick_click",
                               "stick_up", "stick_down", "stick_left", "stick_right"};
        for (int h = 0; h < 2; ++h) {
            const std::string prefix = h == VRTO3D_HAND_LEFT ? "left_" : "right_";
            for (int k = 0; k < kHandKeys; ++k)
                hand_keys_[h][k].Set(settings_.String((prefix + names[k]).c_str()));
        }
        motion_key_.Set(settings_.String("motion_hold"));
        crouch_key_.Set(settings_.String("crouch"));
        up_key_.Set(settings_.String("height_up"));
        down_key_.Set(settings_.String("height_down"));
        aim_toggle_key_.Set(settings_.String("aim_toggle"));
        recenter_key_.Set(settings_.String("recenter"));

        gestures_.clear();
        const auto g = settings_.Json("gestures");
        if (g.is_array()) {
            for (const auto& row : g) {
                if (!row.is_object()) continue;
                Gesture ge;
                ge.key.Set(row.value("key", std::string()));
                if (ge.key.vk == 0) continue;
                ge.hand = row.value("hand", std::string("right")) == "left" ? VRTO3D_HAND_LEFT : VRTO3D_HAND_RIGHT;
                ge.pos = {row.value("x", 0.0), row.value("y", 0.0), row.value("z", 0.0)};
                ge.pitch = row.value("pitch", 0.0);
                ge.yaw = row.value("yaw", 0.0);
                ge.roll = row.value("roll", 0.0);
                ge.grip = row.value("grip", false);
                gestures_.push_back(ge);
            }
        }
    }

    void Poll(double dt, const Vrto3dCtrlHeadPose* head, Vrto3dCtrlControllerState out[2])
    {
        const bool suppressed = host_->is_input_suppressed(host_->ctx) != 0;
        const bool focused = !require_focus_ || host_->is_game_foreground(host_->ctx) != 0;
        const bool active = !suppressed && focused;

        long dx = 0, dy = 0;
        int wheel = 0;
        mouse_.Take(dx, dy, wheel);  // always drain so motion never piles up
        if (!active) dx = dy = wheel = 0;

        for (auto& hk : hand_keys_)
            for (auto& k : hk) k.Update(active);
        motion_key_.Update(active);
        crouch_key_.Update(active);
        up_key_.Update(active);
        down_key_.Update(active);
        aim_toggle_key_.Update(active);
        recenter_key_.Update(active);
        for (auto& g : gestures_) g.key.Update(active);

        if (recenter_key_.Pressed()) {
            yaw_ = pitch_ = height_cm_ = 0.0;
            motion_offset_[0] = motion_offset_[1] = Vec3{};
        }
        if (aim_toggle_key_.Pressed()) aim_[VRTO3D_HAND_RIGHT] = !aim_[VRTO3D_HAND_RIGHT];

        // Mouse: head look, or move a hand while the motion key is held.
        const bool motion = motion_key_.down;
        if (motion) {
            Vec3& o = motion_offset_[motion_hand_];
            o.x += dx * motion_sens_;
            o.y -= dy * motion_sens_ * (invert_y_ ? -1.0 : 1.0);
            o.z -= wheel * 0.03;
        } else {
            if (!motion_sticky_ && motion_was_) motion_offset_[motion_hand_] = Vec3{};
            yaw_ += dx * sens_yaw_;
            pitch_ -= dy * sens_pitch_ * (invert_y_ ? -1.0 : 1.0);
            if (pitch_ > 89.0) pitch_ = 89.0;
            if (pitch_ < -89.0) pitch_ = -89.0;
            yaw_ = std::fmod(yaw_, 360.0);
        }
        motion_was_ = motion;

        if (up_key_.down) height_cm_ += height_speed_ * dt;
        if (down_key_.down) height_cm_ -= height_speed_ * dt;
        if (height_cm_ > 100.0) height_cm_ = 100.0;
        if (height_cm_ < -150.0) height_cm_ = -150.0;

        if (head_look_) {
            // OpenTrack conventions as VRto3D maps them: +yaw turns right,
            // +pitch looks up, +Y moves the head down (centimetres).
            const double y_cm = (crouch_key_.down ? crouch_cm_ : 0.0) - height_cm_;
            const bool sent = host_->send_opentrack(host_->ctx, 0.0, y_cm, 0.0, yaw_, pitch_, 0.0) != 0;
            if (!sent && !warned_head_) {
                settings_.Log(VRTO3D_LOG_WARN, "head look not sent: another plugin owns head look");
                warned_head_ = true;
            }
        }

        last_poll_s_.store(std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        // The cursor is pinned and hidden while playing; mouse look reads
        // the hardware directly, so it keeps working.
        if (active) cursor_.Engage(true);
        else cursor_.Release();

        // Buttons and sticks.
        for (int h = 0; h < 2; ++h) {
            const auto& k = hand_keys_[h];
            Vrto3dCtrlControllerState& o = out[h];
            if (k[kTrigger].down) o.trigger = 1.0f;
            if (k[kGrip].down) o.grip = 1.0f;
            if (k[kA].down) o.buttons |= VRTO3D_BTN_A;
            if (k[kB].down) o.buttons |= VRTO3D_BTN_B;
            if (k[kStickClick].down) o.buttons |= VRTO3D_BTN_THUMBSTICK;
            o.thumbstick[0] = (k[kStickRight].down ? 1.f : 0.f) - (k[kStickLeft].down ? 1.f : 0.f);
            o.thumbstick[1] = (k[kStickUp].down ? 1.f : 0.f) - (k[kStickDown].down ? 1.f : 0.f);
            const float m = std::sqrt(o.thumbstick[0] * o.thumbstick[0] + o.thumbstick[1] * o.thumbstick[1]);
            if (m > 1.f) {
                o.thumbstick[0] /= m;
                o.thumbstick[1] /= m;
            }
        }

        // Poses.
        if (!head || !head->valid) return;
        const Pose hp = HeadPose(*head);
        const Quat heading = YawOnly(hp.rot);
        for (int h = 0; h < 2; ++h) {
            Pose p;
            // Every kbm hand is placed against the head, so the driver can
            // move it in lockstep with the HMD (see VRTO3D_ANCHOR_*).
            uint32_t anchor = VRTO3D_ANCHOR_HEADING;
            const Gesture* g = nullptr;
            for (const auto& ge : gestures_)
                if (ge.hand == h && ge.key.down) g = &ge;
            if (g) {
                p = {hp.pos + Rotate(heading, g->pos),
                     Normalize(heading * FromYawPitchRoll(g->yaw, g->pitch, g->roll))};
                if (g->grip) out[h].grip = 1.0f;
            } else if (aim_[h]) {
                p = AimPose(hp, h, aim_distance_, aim_side_, aim_down_, converge_);
                anchor = VRTO3D_ANCHOR_HEAD;
            } else {
                p = RestPose(hp, h, layout_);
            }
            p.pos = p.pos + Rotate(heading, motion_offset_[h]);
            WritePose(out[h], p);
            out[h].pose_anchor = anchor;
        }
    }

    const char* Schema() const { return schema_.c_str(); }

private:
    enum { kTrigger, kGrip, kA, kB, kStickClick, kStickUp, kStickDown, kStickLeft, kStickRight, kHandKeys };

    const Vrto3dCtrlHostApi* host_ = nullptr;
    Settings settings_;
    std::string schema_;
    RawMouse mouse_;
    CursorLock cursor_;
    std::atomic<double> last_poll_s_{0.0};
    RestLayout layout_;

    double sens_yaw_ = 0.08, sens_pitch_ = 0.06, motion_sens_ = 0.0008;
    bool invert_y_ = false, head_look_ = true;
    bool require_focus_ = true;
    bool aim_[2] = {false, true};
    double aim_distance_ = 0.35, aim_side_ = 0.08, aim_down_ = 0.12, converge_ = 5.0;
    double crouch_cm_ = 60.0, height_speed_ = 50.0;
    int motion_hand_ = VRTO3D_HAND_LEFT;
    bool motion_sticky_ = false;

    Key hand_keys_[2][kHandKeys];
    Key motion_key_, crouch_key_, up_key_, down_key_, aim_toggle_key_, recenter_key_;
    std::vector<Gesture> gestures_;

    double yaw_ = 0, pitch_ = 0, height_cm_ = 0;
    Vec3 motion_offset_[2];
    bool motion_was_ = false;
    bool warned_head_ = false;
};

KbmPlugin* Self(void* u) { return static_cast<KbmPlugin*>(u); }

}  // namespace

extern "C" int vrto3d_builtin_kbm_entry(uint32_t abi, Vrto3dCtrlPlugin* out)
{
    if (abi != VRTO3D_CTRL_ABI_VERSION || !out) return -1;
    static KbmPlugin instance;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(Vrto3dCtrlPlugin);
    out->abi_version = VRTO3D_CTRL_ABI_VERSION;
    out->name = "kbm";
    out->display_name = "Keyboard + Mouse";
    out->version = "1.0.0";
    out->capabilities = VRTO3D_CAP_POSE | VRTO3D_CAP_BUTTONS | VRTO3D_CAP_HEAD_LOOK;
    out->user = &instance;
    out->init = [](void* u, const Vrto3dCtrlHostApi* h) { return Self(u)->Init(h); };
    out->shutdown = [](void* u) { Self(u)->Shutdown(); };
    out->poll = [](void* u, double dt, const Vrto3dCtrlHeadPose* head, Vrto3dCtrlControllerState o[2]) {
        Self(u)->Poll(dt, head, o);
    };
    out->on_settings_changed = [](void* u) { Self(u)->Read(); };
    out->settings_schema = [](void* u) { return Self(u)->Schema(); };
    return 0;
}
