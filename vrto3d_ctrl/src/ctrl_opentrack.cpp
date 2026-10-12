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
#include "ctrl_opentrack.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vrto3d::ctrl {

namespace {
#pragma pack(push, 1)
struct OpenTrackPacket {
    double x, y, z, yaw, pitch, roll;
};
#pragma pack(pop)
}  // namespace

OpenTrackSender::~OpenTrackSender()
{
    std::lock_guard<std::mutex> lock(mutex_);
    CloseSocket();
}

void OpenTrackSender::SetDestinationPort(int port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    port_ = port;
}

bool OpenTrackSender::EnsureSocket()
{
    if (socket_ != -1) return true;
#ifdef _WIN32
    if (!wsa_started_) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_started_ = true;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    socket_ = (intptr_t)s;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    socket_ = s;
#endif
    return true;
}

void OpenTrackSender::CloseSocket()
{
    if (socket_ != -1) {
#ifdef _WIN32
        closesocket((SOCKET)socket_);
#else
        close((int)socket_);
#endif
        socket_ = -1;
    }
#ifdef _WIN32
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }
#endif
}

bool OpenTrackSender::Send(double x_cm, double y_cm, double z_cm, double yaw, double pitch, double roll)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureSocket()) return false;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port_);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const OpenTrackPacket pkt{x_cm, y_cm, z_cm, yaw, pitch, roll};
#ifdef _WIN32
    const int sent = sendto((SOCKET)socket_, reinterpret_cast<const char*>(&pkt), (int)sizeof(pkt), 0,
                            reinterpret_cast<const sockaddr*>(&to), (int)sizeof(to));
#else
    const ssize_t sent = sendto((int)socket_, &pkt, sizeof(pkt), 0,
                                reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
    return sent == (int)sizeof(pkt);
}

}  // namespace vrto3d::ctrl
