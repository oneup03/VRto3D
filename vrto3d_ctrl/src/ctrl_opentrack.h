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

#include <cstdint>
#include <mutex>

namespace vrto3d::ctrl {

// Sends OpenTrack UDP packets ({double X,Y,Z,Yaw,Pitch,Roll}, centimetres and
// degrees) to VRto3D's OpenTrack receiver on 127.0.0.1. This is how the
// keyboard/mouse plugin turns the head: VRto3D owns the HMD pose, and its
// OpenTrack input is the one public way to steer it.
class OpenTrackSender {
public:
    OpenTrackSender() = default;
    ~OpenTrackSender();

    void SetDestinationPort(int port);
    bool Send(double x_cm, double y_cm, double z_cm, double yaw, double pitch, double roll);

private:
    bool EnsureSocket();
    void CloseSocket();

    std::mutex mutex_;
    int port_ = 4242;
    intptr_t socket_ = -1;
    bool wsa_started_ = false;
};

}  // namespace vrto3d::ctrl
