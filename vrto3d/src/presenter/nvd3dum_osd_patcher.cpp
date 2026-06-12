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
#include "presenter/nvd3dum_osd_patcher.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <MinHook.h>

#include "vrto3dlib/debug_log.hpp"

namespace platform {

namespace {

// ---------------------------------------------------------------------------
// Signature for the OSD dispatcher (FUN_1802c4850) in nvd3dumx.dll.
//
// Derived from Ghidra of the current driver build by walking the function's
// prologue and identifying the bytes that the linker rewrites between
// builds (RIP-relative loads + IAT calls) vs. the bytes baked into the
// source (pushes, register moves, stack-relative literals, the literal
// stack frame size, the literal divide-by-10000 reciprocal constant).
//
// Anchor bytes worth highlighting:
//   - 0x488 stack frame (literal)
//   - 0x346DC5D63886594B = fast-divide-by-10000 magic constant (literal)
// These together are essentially unique within nvd3dumx.dll.
//
// Wildcards (kSigMask[i] == false):
//   - 4 bytes: RIP-relative displacement of MOV RAX, [security_cookie]
//   - 4 bytes: RIP-relative displacement of CALL [GetSystemTimeAsFileTime]
// ---------------------------------------------------------------------------
constexpr uint8_t kSig[] = {
    0x40, 0x55,                                     // PUSH RBP
    0x56,                                           // PUSH RSI
    0x48, 0x8D, 0xAC, 0x24, 0x78, 0xFC, 0xFF, 0xFF, // LEA RBP, [RSP-0x388]
    0x48, 0x81, 0xEC, 0x88, 0x04, 0x00, 0x00,       // SUB RSP, 0x488
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,       // MOV RAX, [security_cookie]  (wildcard last 4)
    0x48, 0x33, 0xC4,                               // XOR RAX, RSP
    0x48, 0x89, 0x85, 0x50, 0x03, 0x00, 0x00,       // MOV [RBP+0x350], RAX
    0x48, 0x8B, 0xF1,                               // MOV RSI, RCX
    0x48, 0x8D, 0x4C, 0x24, 0x38,                   // LEA RCX, [RSP+0x38]
    0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,             // CALL [GetSystemTimeAsFileTime]  (wildcard last 4)
    0x48, 0xB8, 0x4B, 0x59, 0x86, 0x38,
    0xD6, 0xC5, 0x6D, 0x34,                         // MOV RAX, 0x346DC5D63886594B
};
constexpr bool kSigMask[] = {
    true, true,
    true,
    true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true,
    true, true, true,
    false, false, false, false,                     // RIP-rel security cookie load
    true, true, true,
    true, true, true, true, true, true, true,
    true, true, true,
    true, true, true, true, true,
    true, true,
    false, false, false, false,                     // RIP-rel IAT call
    true, true, true, true, true, true,
    true, true, true, true,
};
static_assert(sizeof(kSig) == sizeof(kSigMask) / sizeof(bool),
              "kSig and kSigMask length mismatch");

constexpr size_t kSigLen = sizeof(kSig);

// MinHook is process-global, so refcount Initialize / Uninitialize so that
// multiple patchers (current and any future ones) can coexist without
// stomping on each other.
std::atomic<int> g_minhook_refs{0};

bool EnsureMinHookInit() {
    int prev = g_minhook_refs.fetch_add(1, std::memory_order_acq_rel);
    if (prev > 0) return true;
    MH_STATUS s = MH_Initialize();
    if (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) return true;
    g_minhook_refs.fetch_sub(1, std::memory_order_acq_rel);
    LOG() << "Nvd3dumOsdPatcher: MH_Initialize failed status=" << s;
    return false;
}

void ReleaseMinHook() {
    int prev = g_minhook_refs.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) MH_Uninitialize();
}

// Walk the PE headers of an already-loaded module to find the .text
// section's base and size in the current process's address space.
bool GetTextSection(HMODULE mod, uint8_t** out_base, size_t* out_size) {
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        // Section names are zero-padded 8-byte fields, ".text" is the
        // canonical code section name MSVC's linker emits.
        if (std::memcmp(sec->Name, ".text\0\0\0", 8) == 0) {
            *out_base = base + sec->VirtualAddress;
            *out_size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Linear byte/mask scan for the dispatcher signature. The signature is
// dense enough that a naive O(n*m) scan over a 5.5 MB .text completes in
// well under a millisecond, no need for KMP/Boyer-Moore.
uint8_t* ScanSignature(uint8_t* start, size_t size,
                        const uint8_t* sig, const bool* mask, size_t sig_len) {
    if (size < sig_len) return nullptr;
    const size_t end = size - sig_len;
    for (size_t i = 0; i <= end; ++i) {
        bool match = true;
        for (size_t j = 0; j < sig_len; ++j) {
            if (mask[j] && start[i + j] != sig[j]) { match = false; break; }
        }
        if (match) return start + i;
    }
    return nullptr;
}

// __fastcall on Windows x64 = standard calling convention (RCX, RDX, R8, R9).
using NvOsdDispatch_t = void(__fastcall*)(LONGLONG);
NvOsdDispatch_t g_original_dispatcher = nullptr;

// The detour: clear bit 10 of the warnings bitmask and pass through.
//
// Bit 10 = the "Warning: attempt to run Stereoscopic 3D in a non-stereo
// display mode" OSD slot. The dispatcher iterates 23 slots per call; bits
// other than 10 are left alone so legitimate driver warnings (e.g. the
// VESA-emitter-disconnected, MSAA-downgraded, refresh-rate-not-available
// warnings) still appear normally.
//
// param_1 is a pointer to NVIDIA's internal stereo-OSD state struct.
// Defensive null-check in case the driver ever calls in with 0.
void __fastcall NvOsdDispatchDetour(LONGLONG param_1) {
    if (param_1) {
        auto& warning_bits = *reinterpret_cast<uint32_t*>(param_1 + 600);
        warning_bits &= ~(1u << 10);
    }
    if (g_original_dispatcher) {
        g_original_dispatcher(param_1);
    }
}

}  // namespace

Nvd3dumOsdPatcher::~Nvd3dumOsdPatcher() {
    Uninstall();
}

bool Nvd3dumOsdPatcher::Install() {
    if (installed_) return true;

    HMODULE mod = GetModuleHandleW(L"nvd3dumx.dll");
    if (!mod) {
        LOG() << "Nvd3dumOsdPatcher: nvd3dumx.dll is not loaded — "
                 "OSD patch skipped (D3D9Ex device not yet created?).";
        return false;
    }

    uint8_t* text_base = nullptr;
    size_t   text_size = 0;
    if (!GetTextSection(mod, &text_base, &text_size)) {
        LOG() << "Nvd3dumOsdPatcher: could not locate .text section in nvd3dumx.dll.";
        return false;
    }

    uint8_t* hit = ScanSignature(text_base, text_size,
                                  kSig, kSigMask, kSigLen);
    if (!hit) {
        LOG() << "Nvd3dumOsdPatcher: dispatcher signature not found in nvd3dumx.dll — "
                 "driver build may have changed. 3D Vision \"non-stereo display "
                 "mode\" OSD will fire normally.";
        return false;
    }
    {
        std::ostringstream addr;
        addr << std::hex << std::showbase << reinterpret_cast<uintptr_t>(hit)
             << " (.text offset " << (hit - text_base) << ")";
        LOG() << "Nvd3dumOsdPatcher: signature matched at " << addr.str();
    }

    if (!EnsureMinHookInit()) return false;

    MH_STATUS s = MH_CreateHook(
        hit, reinterpret_cast<LPVOID>(NvOsdDispatchDetour),
        reinterpret_cast<LPVOID*>(&g_original_dispatcher));
    if (s != MH_OK) {
        LOG() << "Nvd3dumOsdPatcher: MH_CreateHook failed status=" << s;
        ReleaseMinHook();
        return false;
    }
    s = MH_EnableHook(hit);
    if (s != MH_OK) {
        LOG() << "Nvd3dumOsdPatcher: MH_EnableHook failed status=" << s;
        MH_RemoveHook(hit);
        g_original_dispatcher = nullptr;
        ReleaseMinHook();
        return false;
    }

    target_    = hit;
    installed_ = true;
    LOG() << "Nvd3dumOsdPatcher: \"non-stereo display mode\" OSD suppressed.";
    return true;
}

void Nvd3dumOsdPatcher::Uninstall() {
    if (!installed_) return;
    if (target_) {
        MH_DisableHook(target_);
        MH_RemoveHook(target_);
        target_ = nullptr;
    }
    g_original_dispatcher = nullptr;
    installed_ = false;
    ReleaseMinHook();
    LOG() << "Nvd3dumOsdPatcher: OSD suppression hook removed.";
}

}  // namespace platform
