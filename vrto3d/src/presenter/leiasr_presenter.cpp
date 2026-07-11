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


#define NOMINMAX
#include <winsock2.h>   // must precede windows.h
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "leiasr_presenter.h"

#include <algorithm>
#include <chrono>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include "dx11_renderer.h"
#include "hmd_device_driver.h"
#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"
#include "one_euro_filter.h"

// LeiaSR SDK headers
#include "sr/management/srcontext.h"
#include "sr/utility/exception.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/sense/headtracker/headposetracker.h"
#include "sr/sense/core/inputstream.h"

using Microsoft::WRL::ComPtr;

namespace {

// Helper: invokes SRContext::create inside an SEH __try block so a missing
// LeiaSR / OpenCV DLL (delay-load resolution failure) returns nullptr instead
// of propagating a structured exception that would crash vrserver. We also
// catch SR's C++ ServerNotAvailableException at the C++ layer above.
//
// Two-tier protection:
//   1. SEH __except: catches VcppException(MOD_NOT_FOUND / PROC_NOT_FOUND)
//      raised by the delay-load helper if the DLL or import isn't resolvable.
//   2. C++ try/catch (in caller): catches SR's runtime exceptions like
//      ServerNotAvailableException when DLLs are present but service isn't.
SR::SRContext* TryCreateSRContextSEH(bool* dll_failure)
{
    *dll_failure = false;
    __try {
        return SR::SRContext::create();
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH
    ) {
        *dll_failure = true;
        return nullptr;
    }
}

}  // namespace

namespace vrto3d {

// ---------------------------------------------------------------------------
// Head-pose listener: receives SR_headPose frames on an SDK-owned thread,
// stores the latest sample under a mutex for the tracking thread to consume.
// ---------------------------------------------------------------------------
class LeiaSrHeadPoseListener : public SR::HeadPoseListener {
    std::mutex mutex_;
    float pos_[3]    = { 0.0f, 0.0f, 600.0f };
    float orient_[3] = { 0.0f, 0.0f, 0.0f };
    uint64_t frame_time_ = 0;
    bool has_data_ = false;

public:
    SR::InputStream<SR::HeadPoseStream> stream;

    void accept(const SR_headPose& frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pos_[0] = static_cast<float>(frame.position.x);
        pos_[1] = static_cast<float>(frame.position.y);
        pos_[2] = static_cast<float>(frame.position.z);
        orient_[0] = static_cast<float>(frame.orientation.x);
        orient_[1] = static_cast<float>(frame.orientation.y);
        orient_[2] = static_cast<float>(frame.orientation.z);
        frame_time_ = frame.time;
        has_data_ = true;
    }

    bool get(float pos[3], float orient[3], uint64_t& time_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_data_) return false;
        pos[0] = pos_[0]; pos[1] = pos_[1]; pos[2] = pos_[2];
        orient[0] = orient_[0]; orient[1] = orient_[1]; orient[2] = orient_[2];
        time_us = frame_time_;
        return true;
    }
};

// ---------------------------------------------------------------------------
// OpenTrack UDP sender: 48-byte (6 doubles) little-endian XYZ + YPR packet
// to localhost:open_track_port. Non-blocking, fire-and-forget.
// ---------------------------------------------------------------------------
class LeiaSrOpenTrackSender {
public:
#pragma pack(push, 1)
    struct Packet {
        double X;
        double Y;
        double Z;
        double Yaw;
        double Pitch;
        double Roll;
    };
#pragma pack(pop)

    bool init(int port, const char* host = "127.0.0.1") {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_started_ = true;

        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return false;

        u_long nonblock = 1;
        ioctlsocket(sock_, FIONBIO, &nonblock);

        dest_.sin_family = AF_INET;
        dest_.sin_port   = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, host, &dest_.sin_addr);

        initialized_ = true;
        return true;
    }

    bool send(double x, double y, double z, double yaw, double pitch, double roll) {
        if (!initialized_) return false;
        Packet pkt = { x, y, z, yaw, pitch, roll };
        int sent = sendto(sock_, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
                          reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));
        return sent == sizeof(pkt);
    }

    bool sendIdentity() { return send(0.0, 0.0, 0.0, 0.0, 0.0, 0.0); }

    void shutdown() {
        if (sock_ != INVALID_SOCKET) {
            sendIdentity();    // park consumer at neutral pose so it doesn't freeze
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (wsa_started_) {
            WSACleanup();
            wsa_started_ = false;
        }
        initialized_ = false;
    }

    ~LeiaSrOpenTrackSender() { shutdown(); }

private:
    SOCKET       sock_ = INVALID_SOCKET;
    sockaddr_in  dest_ = {};
    bool         initialized_  = false;
    bool         wsa_started_  = false;
};

// ---------------------------------------------------------------------------
// Track pipeline: SR head pose -> One-Euro filter -> sensitivity/clamp/deadzone
// -> mode-specific axis masking. Mirrors the external bridge's TrackPipeline,
// configured from StereoDisplayDriverConfiguration.sr_* fields.
// ---------------------------------------------------------------------------
class LeiaSrTrackPipeline {
public:
    enum class Mode { XYZ_YawPitch = 1, XYZ = 2, YawPitch = 3, Full6DOF = 4, YawPitchRoll = 5 };

    struct Result {
        float yaw_deg = 0, pitch_deg = 0, roll_deg = 0;
        float pos_x_cm = 0, pos_y_cm = 0, pos_z_cm = 0;
        bool  valid = false;
    };

    static Mode ParseMode(const std::string& s) {
        if (s == "XYZ")           return Mode::XYZ;
        if (s == "YawPitch")      return Mode::YawPitch;
        if (s == "Full6DOF")      return Mode::Full6DOF;
        if (s == "YawPitchRoll")  return Mode::YawPitchRoll;
        return Mode::XYZ_YawPitch;  // default + "XYZ_YawPitch"
    }

    explicit LeiaSrTrackPipeline(const StereoDisplayDriverConfiguration& cfg) {
        const float freq = 60.0f;
        f_yaw_   = OneEuroFilter(freq, cfg.sr_filter_rot_mincutoff, cfg.sr_filter_rot_beta);
        f_pitch_ = OneEuroFilter(freq, cfg.sr_filter_rot_mincutoff, cfg.sr_filter_rot_beta);
        f_roll_  = OneEuroFilter(freq, cfg.sr_filter_rot_mincutoff, cfg.sr_filter_rot_beta);
        f_x_     = OneEuroFilter(freq, cfg.sr_filter_pos_mincutoff, cfg.sr_filter_pos_beta);
        f_y_     = OneEuroFilter(freq, cfg.sr_filter_pos_mincutoff, cfg.sr_filter_pos_beta);
        f_z_     = OneEuroFilter(freq, cfg.sr_filter_pos_mincutoff, cfg.sr_filter_pos_beta);
        Apply(cfg);
    }

    // Push live cfg into the pipeline. Cheap — no allocation or reset of the
    // One Euro filter state. Safe to call every iteration so OSD-tuned
    // sr_filter_* / sr_sens_* / sr_max_* changes propagate without rebuild.
    void Apply(const StereoDisplayDriverConfiguration& cfg) {
        mode_           = ParseMode(cfg.sr_track_mode);
        deadzone_deg_   = cfg.sr_angle_deadzone_deg;
        sens_yaw_       = cfg.sr_sens_yaw;
        sens_pitch_     = cfg.sr_sens_pitch;
        sens_roll_      = cfg.sr_sens_roll;
        max_yaw_        = cfg.sr_max_yaw;
        max_pitch_      = cfg.sr_max_pitch;
        max_roll_       = cfg.sr_max_roll;

        f_yaw_.setMinCutoff(cfg.sr_filter_rot_mincutoff);
        f_yaw_.setBeta(cfg.sr_filter_rot_beta);
        f_pitch_.setMinCutoff(cfg.sr_filter_rot_mincutoff);
        f_pitch_.setBeta(cfg.sr_filter_rot_beta);
        f_roll_.setMinCutoff(cfg.sr_filter_rot_mincutoff);
        f_roll_.setBeta(cfg.sr_filter_rot_beta);
        f_x_.setMinCutoff(cfg.sr_filter_pos_mincutoff);
        f_x_.setBeta(cfg.sr_filter_pos_beta);
        f_y_.setMinCutoff(cfg.sr_filter_pos_mincutoff);
        f_y_.setBeta(cfg.sr_filter_pos_beta);
        f_z_.setMinCutoff(cfg.sr_filter_pos_mincutoff);
        f_z_.setBeta(cfg.sr_filter_pos_beta);
    }

    // Drop accumulated filter state — the next process() call re-initializes
    // both LP filters from the new sample. Use on a use_open_track edge so
    // resumption doesn't see a giant dx/dt spike from the long idle gap.
    void ResetFilters() {
        f_yaw_.reset(); f_pitch_.reset(); f_roll_.reset();
        f_x_.reset();   f_y_.reset();    f_z_.reset();
    }

    // Set the current head orientation as the neutral zero. Stores per-axis
    // degree offsets that process() then ADDS to the filtered values before
    // sensitivity scaling — mirrors the Simulated-Reality-OpenTrack-Bridge
    // Ctrl+X calibrate behavior. Position offsets are not adjusted.
    void Calibrate(float orient_x_rad, float orient_y_rad, float orient_z_rad) {
        const float to_deg = 180.0f / static_cast<float>(M_PI);
        // Apply the same axis sign conventions process() uses (only yaw is
        // pre-inverted to match the OpenTrack receiver).
        const float pitch = orient_x_rad * to_deg;
        const float yaw   = -(orient_y_rad * to_deg);
        const float roll  = orient_z_rad * to_deg;
        yaw_offset_   = -yaw;
        pitch_offset_ = -pitch;
        roll_offset_  = -roll;
    }

    Result process(float pos_x_mm, float pos_y_mm, float pos_z_mm,
                   float orient_x_rad, float orient_y_rad, float orient_z_rad,
                   float timestamp_sec)
    {
        Result r;
        if (!std::isfinite(orient_x_rad) || !std::isfinite(orient_y_rad) || !std::isfinite(orient_z_rad)) {
            return r;
        }

        const float to_deg = 180.0f / static_cast<float>(M_PI);
        float pitch_raw = orient_x_rad * to_deg;
        float yaw_raw   = orient_y_rad * to_deg;
        float roll_raw  = orient_z_rad * to_deg;

        float yaw_f   = f_yaw_.filter(yaw_raw,    timestamp_sec);
        float pitch_f = f_pitch_.filter(pitch_raw, timestamp_sec);
        float roll_f  = f_roll_.filter(roll_raw,   timestamp_sec);

        // OpenTrack convention to match the consumer (MockControllerDeviceDriver
        // ::OpenTrackThread negates X and Yaw on receive): pre-invert yaw here
        // so the round-trip matches an external bridge running in OpenTrack
        // mode. Roll is NOT negated — the receiver passes it straight into the
        // quaternion, so negating here would mirror the roll direction.
        yaw_f  = -yaw_f;

        r.yaw_deg   = std::clamp((yaw_f   + yaw_offset_)   * sens_yaw_,   -max_yaw_,   max_yaw_);
        r.pitch_deg = std::clamp((pitch_f + pitch_offset_) * sens_pitch_, -max_pitch_, max_pitch_);
        r.roll_deg  = std::clamp((roll_f  + roll_offset_)  * sens_roll_,  -max_roll_,  max_roll_);

        if (std::fabs(r.yaw_deg)   < deadzone_deg_) r.yaw_deg   = 0.0f;
        if (std::fabs(r.pitch_deg) < deadzone_deg_) r.pitch_deg = 0.0f;
        if (std::fabs(r.roll_deg)  < deadzone_deg_) r.roll_deg  = 0.0f;

        // Position passthrough (mm -> cm), X inverted to match OpenTrack.
        float x_cm = -pos_x_mm / 10.0f;
        float y_cm =  pos_y_mm / 10.0f;
        float z_cm =  pos_z_mm / 10.0f;
        // Run position through filter for stability.
        r.pos_x_cm = f_x_.filter(x_cm, timestamp_sec);
        r.pos_y_cm = f_y_.filter(y_cm, timestamp_sec);
        r.pos_z_cm = f_z_.filter(z_cm, timestamp_sec);

        switch (mode_) {
            case Mode::XYZ_YawPitch:
                r.roll_deg = 0.0f;
                break;
            case Mode::XYZ:
                r.yaw_deg = r.pitch_deg = r.roll_deg = 0.0f;
                break;
            case Mode::YawPitch:
                r.pos_x_cm = r.pos_y_cm = r.pos_z_cm = 0.0f;
                r.roll_deg = 0.0f;
                break;
            case Mode::Full6DOF:
                break;  // everything passes through
            case Mode::YawPitchRoll:
                r.pos_x_cm = r.pos_y_cm = r.pos_z_cm = 0.0f;
                break;
        }

        r.valid = true;
        return r;
    }

private:
    Mode  mode_         = Mode::XYZ_YawPitch;
    float deadzone_deg_ = 0.2f;
    float sens_yaw_ = 1, sens_pitch_ = 1, sens_roll_ = 1;
    float max_yaw_  = 70, max_pitch_  = 70, max_roll_  = 70;
    OneEuroFilter f_yaw_, f_pitch_, f_roll_;
    OneEuroFilter f_x_, f_y_, f_z_;
    float yaw_offset_   = 0.0f;
    float pitch_offset_ = 0.0f;
    float roll_offset_  = 0.0f;
};


// Out-of-line ctor/dtor: the unique_ptr<> members above wrap forward-declared
// types in the header. Defining them here, where the full types are visible,
// keeps presenter_factory.cpp (and any other includer) free of the SR/winsock
// headers.
LeiaSrPresenter::LeiaSrPresenter() = default;
LeiaSrPresenter::~LeiaSrPresenter() { Shutdown(); }


bool LeiaSrPresenter::CreateSwapChain(Dx11Renderer& renderer)
{
    ID3D11Device* dev = renderer.Device();

    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    // Flip-model + waitable swap chain. The waitable handle gates pacing on
    // the window thread, so Present(0, 0) returns immediately — tear-free
    // without blocking inside Present (which would stall the compositor via
    // the shared context_mutex_).
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = window_->Width();
    scd.Height      = window_->Height();
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc  = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HWND hwnd = static_cast<HWND>(window_->NativeHandle());
    HRESULT hr = factory->CreateSwapChainForHwnd(dev, hwnd, &scd, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        LOG() << "LeiaSrPresenter: CreateSwapChainForHwnd failed hr=0x" << std::hex << hr;
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    if (FAILED(swapchain_.As(&swapchain2_)) || !swapchain2_) {
        LOG() << "LeiaSrPresenter: QueryInterface(IDXGISwapChain2) failed";
        return false;
    }
    swapchain2_->SetMaximumFrameLatency(1);
    frame_latency_wait_ = swapchain2_->GetFrameLatencyWaitableObject();
    if (!frame_latency_wait_) {
        LOG() << "LeiaSrPresenter: GetFrameLatencyWaitableObject returned null";
    }

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &swapchain_rtv_))) return false;

    swap_width_  = window_->Width();
    swap_height_ = window_->Height();
    LOG() << "LeiaSrPresenter: swapchain " << swap_width_ << "x" << swap_height_
          << " (FLIP_DISCARD, 2 buffers, waitable="
          << (frame_latency_wait_ ? "yes" : "no") << ")";
    return true;
}


bool LeiaSrPresenter::Init(Dx11Renderer& renderer,
                            const StereoDisplayDriverConfiguration& cfg,
                            const FocusContext& focus)
{
    renderer_      = &renderer;
    eye_swap_      = cfg.eye_swap;
    auto_focus_    = cfg.auto_focus;
    focus_         = focus;

    // Cache tracking config so the head-tracking thread can read it without
    // touching the caller's StereoDisplayDriverConfiguration after Init returns.
    tracking_enabled_ = cfg.use_open_track;
    tracking_port_    = cfg.open_track_port;
    tracking_cfg_     = cfg;

    // LeiaSR weaver targets a single SR display — never spans two monitors.
    platform::MonitorInfo primary{}, secondary{};
    if (!platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
        LOG() << "LeiaSrPresenter::Init: ResolveTargetMonitors failed";
        return false;
    }

    window_stop_.store(false);
    window_ready_.store(false);
    window_failed_.store(false);
    window_thread_ = std::thread(&LeiaSrPresenter::WindowThreadLoop, this, &renderer,
                                  primary, secondary);

    for (int i = 0; i < 500; ++i) {
        if (window_ready_.load() || window_failed_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (window_failed_.load() || !window_ready_.load()) {
        LOG() << "LeiaSrPresenter::Init: window thread failed to come up";
        Shutdown();
        return false;
    }

    LOG() << "LeiaSrPresenter: ready, target_monitor=" << primary.device_name
          << " " << (window_ ? window_->Width() : 0) << "x" << (window_ ? window_->Height() : 0);

    focus_stop_.store(false);
    focus_thread_ = std::thread(&LeiaSrPresenter::FocusThreadLoop, this);
    return true;
}


void LeiaSrPresenter::WindowThreadLoop(Dx11Renderer* renderer,
                                        platform::MonitorInfo primary,
                                        platform::MonitorInfo secondary)
{
    platform::EnablePerMonitorV2DpiAwareness();

    window_ = platform::CreatePresentWindow(
        primary,
        (secondary.width > 0 ? &secondary : nullptr),
        "VRto3D-LeiaSR");
    if (!window_) {
        LOG() << "LeiaSrPresenter: CreatePresentWindow failed";
        window_failed_.store(true);
        return;
    }

    if (!CreateSwapChain(*renderer)) {
        LOG() << "LeiaSrPresenter: swapchain init failed";
        window_failed_.store(true);
        window_.reset();
        return;
    }

    // Initial black so DWM has something to composite.
    {
        ID3D11DeviceContext* ctx = renderer->Context();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(swapchain_rtv_.Get(), clear);
        ID3D11RenderTargetView* null_rtv[] = { nullptr };
        ctx->OMSetRenderTargets(1, null_rtv, nullptr);
        swapchain_rtv_.Reset();   // release ref to back buffer before flip
        swapchain_->Present(0, 0);
    }

    // Build SR context + weaver. Block briefly waiting for SRService.
    HWND hwnd = static_cast<HWND>(window_->NativeHandle());
    {
        const ULONGLONG t0 = GetTickCount64();
        const ULONGLONG max_wait_ms = 5000;
        bool dll_missing = false;
        while (!sr_context_) {
            try {
                sr_context_ = TryCreateSRContextSEH(&dll_missing);
                if (dll_missing) break;       // delay-load failure — no point retrying
                if (sr_context_) break;       // success
                // Shouldn't reach here without exception, but bail out if we do.
                break;
            } catch (const SR::ServerNotAvailableException&) {
                if (GetTickCount64() - t0 > max_wait_ms) break;
                Sleep(100);
            } catch (...) {
                break;
            }
        }
        if (dll_missing) {
            LOG() << "LeiaSrPresenter: LeiaSR runtime DLL(s) not resolvable. "
                  << "Install the LeiaSR / SR Platform runtime, or pick a different output_mode.";
            window_failed_.store(true);
            window_.reset();
            return;
        }
        if (!sr_context_) {
            LOG() << "LeiaSrPresenter: SRContext::create failed (SRService not running?)";
            window_failed_.store(true);
            window_.reset();
            return;
        }

        WeaverErrorCode wec = SR::CreateDX11Weaver(sr_context_, renderer->Context(), hwnd, &sr_weaver_);
        if (wec != WeaverErrorCode::WeaverSuccess || !sr_weaver_) {
            LOG() << "LeiaSrPresenter: CreateDX11Weaver failed code=" << static_cast<int>(wec);
            window_failed_.store(true);
            window_.reset();
            return;
        }
        // Configure weaver. Input is sRGB R8G8B8A8 from compositor; backbuffer
        // is _UNORM (linear). Tell the weaver to convert sRGB→Linear on read
        // and Linear→sRGB on write so colors come out right.
        sr_weaver_->setShaderSRGBConversion(true, true);
        sr_weaver_->setLatencyInFrames(1);
        sr_weaver_->setContext(renderer->Context());

        // Always register the SR head tracker as a Sense before initialize().
        // The use_open_track flag is checked at consumption time (see
        // HeadTrackingThreadLoop) so the OSD can toggle it live without
        // needing a presenter restart.
        try {
            sr_head_tracker_ = SR::HeadPoseTracker::create(*sr_context_);
            head_listener_   = std::make_unique<LeiaSrHeadPoseListener>();
            head_listener_->stream.set(sr_head_tracker_->openHeadPoseStream(head_listener_.get()));
        } catch (const std::exception& e) {
            LOG() << "LeiaSrPresenter: HeadPoseTracker::create failed: " << e.what();
            sr_head_tracker_ = nullptr;
            head_listener_.reset();
        }

        // Activate sense streams.
        sr_context_->initialize();
        lens_hint_ = SR::SwitchableLensHint::create(*sr_context_);
        sr_initialized_ = true;
        LOG() << "LeiaSrPresenter: SRContext initialized; weaver ready";
    }

    window_ready_.store(true);

    // Always spawn the head-tracking sender thread once SR is up. The
    // per-frame use_open_track flag is checked inside the loop so toggling
    // it via the OSD takes effect immediately.
    if (head_listener_) {
        ot_sender_ = std::make_unique<LeiaSrOpenTrackSender>();
        if (!ot_sender_->init(tracking_port_)) {
            LOG() << "LeiaSrPresenter: OpenTrack UDP sender init failed; head tracking disabled";
            ot_sender_.reset();
        } else {
            track_pipeline_ = std::make_unique<LeiaSrTrackPipeline>(tracking_cfg_);
            tracking_stop_.store(false);
            tracking_thread_ = std::thread(&LeiaSrPresenter::HeadTrackingThreadLoop, this);
            LOG() << "LeiaSrPresenter: head tracking sender ready (UDP -> 127.0.0.1:" << tracking_port_
                  << ", mode=" << tracking_cfg_.sr_track_mode
                  << ", initial use_open_track=" << (tracking_enabled_ ? "true" : "false") << ")";
        }
    }

    while (!window_stop_.load(std::memory_order_relaxed)) {
        // DWM signals frame_latency_wait_ once per display refresh; gates
        // pacing without blocking inside Present.
        if (frame_latency_wait_) {
            WaitForSingleObjectEx(frame_latency_wait_, 100, TRUE);
        }
        renderer->WaitAndDrawPending(33);
        if (window_) window_->PollEvents();
    }

    // Stop head tracking before tearing down SR. Stream must be closed before
    // the tracker / context is destroyed.
    tracking_stop_.store(true);
    if (tracking_thread_.joinable()) tracking_thread_.join();
    head_listener_.reset();          // ~InputStream calls stopListening for us
    sr_head_tracker_ = nullptr;      // managed by SRContext; do not delete
    if (ot_sender_) { ot_sender_->shutdown(); ot_sender_.reset(); }
    track_pipeline_.reset();

    // Tear down SR resources on this thread (they were created here).
    if (sr_weaver_) {
        sr_weaver_->destroy();
        sr_weaver_ = nullptr;
    }
    lens_hint_ = nullptr;  // managed by SRContext; do not delete
    if (sr_context_) {
        delete sr_context_;
        sr_context_ = nullptr;
    }
    sr_initialized_ = false;

    input_srv_.Reset();
    if (frame_latency_wait_) {
        CloseHandle(frame_latency_wait_);
        frame_latency_wait_ = nullptr;
    }
    swapchain_rtv_.Reset();
    swapchain2_.Reset();
    swapchain_.Reset();
    window_.reset();
    LOG() << "LeiaSrPresenter: window thread exited";
}


void LeiaSrPresenter::RecordComposite(ID3D11Texture2D* sbs_input)
{
    if (!swapchain_ || !sbs_input || !sr_weaver_ || !renderer_) return;

    ID3D11Device*        dev = renderer_->Device();
    ID3D11DeviceContext* ctx = renderer_->Context();

    // FLIP_DISCARD rotates buffer 0 every Present, so refresh the RTV each
    // frame against the current back buffer. Must happen BEFORE weave() so
    // the weaver writes to the live buffer.
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return;
    swapchain_rtv_.Reset();
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &swapchain_rtv_))) return;

    D3D11_TEXTURE2D_DESC td{};
    sbs_input->GetDesc(&td);

    // Rebuild SRV + retell weaver only when input changes.
    if (sbs_input != cached_input_ptr_
        || td.Width  != cached_input_w_
        || td.Height != cached_input_h_
        || td.Format != cached_input_fmt_) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                    = td.Format;
        sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels       = static_cast<UINT>(-1);
        sv.Texture2D.MostDetailedMip = 0;
        ComPtr<ID3D11ShaderResourceView> new_srv;
        if (FAILED(dev->CreateShaderResourceView(sbs_input, &sv, &new_srv))) return;
        input_srv_         = new_srv;
        cached_input_ptr_  = sbs_input;
        cached_input_w_    = td.Width;
        cached_input_h_    = td.Height;
        cached_input_fmt_  = td.Format;

        // Weaver expects per-eye dimensions (input is treated as side-by-side).
        sr_weaver_->setInputViewTexture(input_srv_.Get(),
                                         static_cast<int>(td.Width / 2),
                                         static_cast<int>(td.Height),
                                         td.Format);
        LOG() << "LeiaSrPresenter: input view texture (re)bound "
              << td.Width << "x" << td.Height << " fmt=" << td.Format;
    }

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(swap_width_);
    vp.Height   = static_cast<float>(swap_height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv = swapchain_rtv_.Get();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);

    sr_weaver_->weave();

    // Release RTV + back-buffer ref before Present (FLIP requirement).
    ID3D11RenderTargetView* null_rtv[] = { nullptr };
    ctx->OMSetRenderTargets(1, null_rtv, nullptr);
    swapchain_rtv_.Reset();
}


void LeiaSrPresenter::Present()
{
    if (!swapchain_) return;

    // Flip-model + waitable: SyncInterval=0 queues the flip and returns
    // immediately; DWM consumes it at the next vblank. Pacing comes from the
    // waitable handle on the window thread.
    HRESULT hr = swapchain_->Present(0, 0);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true)) {
            LOG() << "LeiaSrPresenter: Present failed hr=0x" << std::hex << hr;
        }
    }
}


void LeiaSrPresenter::FocusThreadLoop()
{
    using namespace std::chrono_literals;
    bool was_on_top = false;
    int  reassert_counter = 0;
    uint32_t last_auto_focused_pid = 0;

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!window_) break;

        const bool is_on_top   = focus_.is_on_top   && focus_.is_on_top->load();
        const bool man_on_top  = focus_.man_on_top  && focus_.man_on_top->load();
        const uint32_t pid     = focus_.app_pid ? focus_.app_pid->load() : 0;
        const bool auto_focus  = focus_.auto_focus  ? focus_.auto_focus->load() : auto_focus_;

        // LeiaSR runs on a single SR display — no multi-display nudge.

        const bool app_running = platform::IsProcessRunning(pid);
        if (pid == 0 || !app_running) {
            last_auto_focused_pid = 0;
        }

        bool want_on_top = false;
        if (man_on_top) {
            want_on_top = true;
        } else if (is_on_top && app_running) {
            want_on_top = true;
        } else if (auto_focus && !is_on_top
                   && app_running && pid != 0
                   && pid != last_auto_focused_pid) {
            if (focus_.is_on_top)  focus_.is_on_top->store(true);
            if (focus_.man_on_top) focus_.man_on_top->store(true);
            last_auto_focused_pid = pid;
            want_on_top = true;
        }

        if (want_on_top != was_on_top) {
            HWND vr_hwnd = static_cast<HWND>(window_->NativeHandle());
            if (want_on_top) {
                window_->BringToTop();
                if (lens_hint_) lens_hint_->enable();
                if (vr_hwnd) {
                    // WS_EX_LAYERED stays on for the window's lifetime once
                    // set; only WS_EX_TRANSPARENT is toggled. Tearing down the
                    // layered surface on a DPI-scaled display can leave it
                    // recreated at virtualized dimensions.
                    LONG_PTR ex = GetWindowLongPtrW(vr_hwnd, GWL_EXSTYLE);
                    SetWindowLongPtrW(vr_hwnd, GWL_EXSTYLE,
                                      ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
                    SetLayeredWindowAttributes(vr_hwnd, 0, 255, LWA_ALPHA);
                }
                // When SteamVR is launched by the app, the game window may
                // not exist yet at the +8s mark, or SteamVR's bring-up
                // (vrmonitor / status window) may grab foreground after
                // our first ForceFocus. Run a watch loop that re-asserts
                // focus whenever the foreground drifts off the game.
                std::thread([pid, man_on_top = focus_.man_on_top]() {
                    for (int i = 0; i < 15; ++i) {
                        if (man_on_top && !man_on_top->load()) return;
                        HWND game_hwnd = GetHWNDFromPID(pid);
                        if (game_hwnd && GetForegroundWindow() != game_hwnd) {
                            ForceFocus(game_hwnd,
                                       GetCurrentThreadId(),
                                       GetWindowThreadProcessId(game_hwnd, nullptr));
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }).detach();
            } else {
                window_->ReleaseTopmost();
                if (lens_hint_) lens_hint_->disable();
                if (vr_hwnd) {
                    LONG_PTR ex = GetWindowLongPtrW(vr_hwnd, GWL_EXSTYLE);
                    SetWindowLongPtrW(vr_hwnd, GWL_EXSTYLE,
                                      ex & ~WS_EX_TRANSPARENT);
                }
            }
            was_on_top = want_on_top;
            reassert_counter = 0;
        } else if (want_on_top && ++reassert_counter >= 20) {
            reassert_counter = 0;
            window_->BringToTop();
        }

        std::this_thread::sleep_for(50ms);
    }
}


void LeiaSrPresenter::HeadTrackingThreadLoop()
{
    using namespace std::chrono;
    if (!head_listener_ || !ot_sender_ || !track_pipeline_) return;

    const auto t0 = steady_clock::now();
    float pos[3], orient[3];
    uint64_t time_us = 0;

    bool prev_use_ot = false;
    while (!tracking_stop_.load(std::memory_order_relaxed)) {
        // Live-poll use_open_track + the SR filter cfg so the OSD's
        // "Enable OpenTrack" checkbox and the sr_filter_*/sens_*/max_*
        // sliders take effect without a presenter restart. We drain the
        // SR head-pose stream regardless to keep its queue from backing
        // up — but skip the UDP send and reset the One Euro filters on
        // the disabled→enabled edge so resumption doesn't see a giant
        // dx/dt spike from the idle gap. sr_tracking_enabled mutes this
        // built-in sender while leaving the OpenTrack receiver running, so
        // an external source (OpenTrack app, VertoXR) can feed the port.
        bool use_ot = true;
        StereoDisplayDriverConfiguration live_cfg{};
        if (renderer_ && renderer_->Component()) {
            live_cfg = renderer_->Component()->GetConfig();
            use_ot   = live_cfg.use_open_track && live_cfg.sr_tracking_enabled;
        }

        if (use_ot && !prev_use_ot && track_pipeline_) {
            track_pipeline_->ResetFilters();
        }
        prev_use_ot = use_ot;

        // Push the latest cfg into the pipeline every tick (cheap).
        if (track_pipeline_) track_pipeline_->Apply(live_cfg);

        if (head_listener_->get(pos, orient, time_us)) {
            // Apply pending calibrate using the most recent raw orientation.
            if (calibrate_request_.exchange(false) && track_pipeline_) {
                track_pipeline_->Calibrate(orient[0], orient[1], orient[2]);
                LOG() << "LeiaSrPresenter: head pose calibrated to neutral";
            }
            if (use_ot) {
                const float ts = duration<float>(steady_clock::now() - t0).count();
                auto r = track_pipeline_->process(pos[0], pos[1], pos[2],
                                                  orient[0], orient[1], orient[2], ts);
                if (r.valid) {
                    ot_sender_->send(r.pos_x_cm, r.pos_y_cm, r.pos_z_cm,
                                     r.yaw_deg, r.pitch_deg, r.roll_deg);
                }
            }
        }
        // ~120Hz polling — SR head pose typically arrives at 60Hz; this keeps
        // jitter low without burning a core.
        std::this_thread::sleep_for(milliseconds(8));
    }
}


void LeiaSrPresenter::RequestCalibrate()
{
    calibrate_request_.store(true);
}

void LeiaSrPresenter::Shutdown()
{
    if (!window_thread_.joinable() && !window_) return;
    LOG() << "LeiaSrPresenter: Shutdown called";

    focus_stop_.store(true);
    if (focus_thread_.joinable()) focus_thread_.join();

    window_stop_.store(true);
    if (window_thread_.joinable()) window_thread_.join();

    renderer_ = nullptr;
}

}  // namespace vrto3d

