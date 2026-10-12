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

// Built-in "remote" plugin: receives Vrto3dCtrlRemotePacket datagrams
// (vrto3d_ctrl_remote.h) so out-of-process apps can drive the controllers
// without writing a DLL. Non-blocking socket drained from poll(), so it adds
// no thread of its own.

#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static const socket_t kBadSocket = INVALID_SOCKET;
static void CloseSock(socket_t s) { closesocket(s); }
static bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kBadSocket = -1;
static void CloseSock(socket_t s) { close(s); }
static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

#include "plugin_util.h"
#include "vrto3d_ctrl_remote.h"

namespace {

using namespace vrto3d::ctrl;
using namespace vrto3d::ctrl::plugin;

class RemotePlugin {
public:
    RemotePlugin()
    {
        nlohmann::json s = nlohmann::json::array();
        s.push_back(SInt("port", "UDP port", VRTO3D_REMOTE_DEFAULT_PORT, 1024, 65535,
                         "Packets use the layout in vrto3d_ctrl_remote.h."));
        s.push_back(SBool("listen_all", "Listen on all interfaces", false,
                          "Off: only this PC (127.0.0.1) can send. On: any device on your "
                          "network can, e.g. a phone app."));
        s.push_back(SInt("stale_ms", "Stale timeout (ms)", 500, 50, 5000,
                         "A hand disconnects this long after its last packet."));
        schema_ = nlohmann::json{{"settings", s}}.dump();
    }

    int Init(const Vrto3dCtrlHostApi* host)
    {
        settings_.Bind(host);
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        wsa_ = true;
#endif
        Read();
        return 0;
    }

    void Shutdown()
    {
        CloseSocket();
#ifdef _WIN32
        if (wsa_) WSACleanup();
        wsa_ = false;
#endif
    }

    void Read()
    {
        const int port = (int)settings_.Int("port", VRTO3D_REMOTE_DEFAULT_PORT);
        const bool all = settings_.Bool("listen_all", false);
        stale_s_ = settings_.Int("stale_ms", 500) / 1000.0;
        if (port != port_ || all != listen_all_ || sock_ == kBadSocket) {
            port_ = port;
            listen_all_ = all;
            OpenSocket();
        }
    }

    void Poll(double dt, const Vrto3dCtrlHeadPose* head, Vrto3dCtrlControllerState out[2])
    {
        clock_ += dt;
        Drain();
        for (int h = 0; h < 2; ++h) {
            if (!have_[h] || clock_ - last_rx_[h] > stale_s_) continue;
            const Vrto3dCtrlRemoteHand& in = hand_[h];
            Vrto3dCtrlControllerState& o = out[h];

            Pose p{{in.position[0], in.position[1], in.position[2]},
                   Normalize(Quat{in.rotation[0], in.rotation[1], in.rotation[2], in.rotation[3]})};
            if (flags_[h] & VRTO3D_REMOTE_FLAG_HEAD_RELATIVE) {
                if (!head || !head->valid) continue;
                const Pose hp = HeadPose(*head);
                const Quat frame = (flags_[h] & VRTO3D_REMOTE_FLAG_YAW_ONLY) ? YawOnly(hp.rot) : hp.rot;
                p = {hp.pos + Rotate(frame, p.pos), Normalize(frame * p.rot)};
            }
            if (in.tracking != VRTO3D_TRACKING_NONE) {
                WritePose(o, p);
                o.tracking = in.tracking;
                if (flags_[h] & VRTO3D_REMOTE_FLAG_HEAD_RELATIVE)
                    o.pose_anchor = (flags_[h] & VRTO3D_REMOTE_FLAG_YAW_ONLY) ? VRTO3D_ANCHOR_HEADING
                                                                              : VRTO3D_ANCHOR_HEAD;
                for (int i = 0; i < 3; ++i) {
                    o.velocity[i] = in.velocity[i];
                    o.angular_velocity[i] = in.angular_velocity[i];
                }
            }
            o.buttons = in.buttons;
            o.touches = in.touches;
            o.trigger = in.trigger;
            o.grip = in.grip;
            o.grip_force = in.grip_force;
            o.thumbstick[0] = in.thumbstick[0];
            o.thumbstick[1] = in.thumbstick[1];
            o.trackpad[0] = in.trackpad[0];
            o.trackpad[1] = in.trackpad[1];
            o.trackpad_force = in.trackpad_force;
            for (int f = 0; f < 5; ++f) o.finger_curl[f] = in.finger_curl[f];
        }
    }

    void Haptic(uint32_t hand, float duration, float freq, float amp)
    {
        if (sock_ == kBadSocket || !have_sender_) return;
        Vrto3dCtrlRemoteHaptic msg{};
        std::memcpy(msg.magic, VRTO3D_REMOTE_HAPTIC_MAGIC, 4);
        msg.version = VRTO3D_REMOTE_VERSION;
        msg.hand = (uint8_t)hand;
        msg.duration_s = duration;
        msg.frequency_hz = freq;
        msg.amplitude = amp;
        sendto(sock_, reinterpret_cast<const char*>(&msg), (int)sizeof(msg), 0,
               reinterpret_cast<const sockaddr*>(&sender_), (int)sizeof(sender_));
    }

    const char* Schema() const { return schema_.c_str(); }

private:
    void CloseSocket()
    {
        if (sock_ != kBadSocket) CloseSock(sock_);
        sock_ = kBadSocket;
    }

    void OpenSocket()
    {
        CloseSocket();
        socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kBadSocket) {
            settings_.Log(VRTO3D_LOG_ERROR, "socket() failed");
            return;
        }
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons((uint16_t)port_);
        local.sin_addr.s_addr = htonl(listen_all_ ? INADDR_ANY : INADDR_LOOPBACK);
        if (bind(s, reinterpret_cast<const sockaddr*>(&local), (int)sizeof(local)) != 0) {
            settings_.Log(VRTO3D_LOG_ERROR, "bind to UDP port " + std::to_string(port_) +
                                                " failed (already in use?)");
            CloseSock(s);
            return;
        }
        sock_ = s;
        settings_.Log(VRTO3D_LOG_INFO, "listening on UDP " + std::string(listen_all_ ? "0.0.0.0" : "127.0.0.1") +
                                           ":" + std::to_string(port_));
    }

    void Drain()
    {
        if (sock_ == kBadSocket) return;
        Vrto3dCtrlRemotePacket pkt;
        for (int i = 0; i < 64; ++i) {  // bounded so a flood can't stall the loop
            sockaddr_in from{};
#ifdef _WIN32
            int from_len = sizeof(from);
#else
            socklen_t from_len = sizeof(from);
#endif
            const auto n = recvfrom(sock_, reinterpret_cast<char*>(&pkt), (int)sizeof(pkt), 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 0) {
                if (!WouldBlock()) settings_.Log(VRTO3D_LOG_DEBUG, "recvfrom error");
                return;
            }
            if (n != (int)sizeof(pkt) || std::memcmp(pkt.magic, VRTO3D_REMOTE_MAGIC, 4) != 0 ||
                pkt.version != VRTO3D_REMOTE_VERSION) {
                if (!warned_bad_) {
                    settings_.Log(VRTO3D_LOG_WARN, "ignoring malformed packet (" + std::to_string((int)n) +
                                                       " bytes); expected " +
                                                       std::to_string(sizeof(pkt)));
                    warned_bad_ = true;
                }
                continue;
            }
            sender_ = from;
            have_sender_ = true;
            for (int h = 0; h < 2; ++h) {
                if (!pkt.hand[h].connected) continue;
                hand_[h] = pkt.hand[h];
                flags_[h] = pkt.flags;
                have_[h] = true;
                last_rx_[h] = clock_;
            }
        }
    }

    Settings settings_;
    std::string schema_;
    socket_t sock_ = kBadSocket;
#ifdef _WIN32
    bool wsa_ = false;
#endif
    int port_ = -1;
    bool listen_all_ = false;
    double stale_s_ = 0.5;
    double clock_ = 0.0;
    Vrto3dCtrlRemoteHand hand_[2] = {};
    uint16_t flags_[2] = {};
    bool have_[2] = {};
    double last_rx_[2] = {};
    sockaddr_in sender_{};
    bool have_sender_ = false;
    bool warned_bad_ = false;
};

RemotePlugin* Self(void* u) { return static_cast<RemotePlugin*>(u); }

}  // namespace

extern "C" int vrto3d_builtin_remote_entry(uint32_t abi, Vrto3dCtrlPlugin* out)
{
    if (abi != VRTO3D_CTRL_ABI_VERSION || !out) return -1;
    static RemotePlugin instance;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(Vrto3dCtrlPlugin);
    out->abi_version = VRTO3D_CTRL_ABI_VERSION;
    out->name = "remote";
    out->display_name = "Remote (UDP)";
    out->version = "1.0.0";
    out->capabilities = VRTO3D_CAP_POSE | VRTO3D_CAP_VELOCITY | VRTO3D_CAP_BUTTONS |
                        VRTO3D_CAP_FINGERS | VRTO3D_CAP_HAPTICS;
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
