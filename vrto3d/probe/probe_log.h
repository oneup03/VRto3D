/*
 * VRto3D Linux port — Milestone 0 probe logger.
 *
 * Writes timestamped lines to $HOME/vrto3d_probe.log and tees important
 * lines into vrserver.txt via IVRDriverLog once the driver context is up.
 */
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

class ProbeLog
{
public:
    static ProbeLog &Get()
    {
        static ProbeLog instance;
        return instance;
    }

    void SetDriverLogReady(bool ready) { driver_log_ready_.store(ready); }

    void Log(const char *fmt, ...)
    {
        char body[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(mutex_);
        EnsureFile();
        if (file_) {
            fprintf(file_, "[%.6f] %s\n", NowSec(), body);
            fflush(file_);
        }
        if (driver_log_ready_.load() && vr::VRDriverLog()) {
            char line[2100];
            snprintf(line, sizeof(line), "[vrto3d_probe] %s", body);
            vr::VRDriverLog()->Log(line);
        }
    }

private:
    ProbeLog() = default;

    void EnsureFile()
    {
        if (file_)
            return;
        const char *home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof(path), "%s/vrto3d_probe.log", home ? home : "/tmp");
        file_ = fopen(path, "a");
        if (file_)
            fprintf(file_, "\n===== probe session start =====\n");
    }

    double NowSec()
    {
        using namespace std::chrono;
        static const steady_clock::time_point t0 = steady_clock::now();
        return duration<double>(steady_clock::now() - t0).count();
    }

    std::mutex mutex_;
    FILE *file_ = nullptr;
    std::atomic<bool> driver_log_ready_{false};
};

#define PLOG(...) ProbeLog::Get().Log(__VA_ARGS__)
