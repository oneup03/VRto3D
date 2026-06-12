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
#include "presenter/nv_3dvision_suppressor.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include "vrto3dlib/debug_log.hpp"

#pragma intrinsic(_ReturnAddress)

namespace platform {

namespace {

// Walk the PE headers of an already-loaded module to find the .text section's
// base and size in the current process's address space.
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

// Linear byte/mask scan for a function-prologue signature. The signatures we
// use are dense enough that a naive O(n*m) scan over a ~5.5 MB .text completes
// in well under a millisecond, no need for KMP/Boyer-Moore.
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

// Captured at Install() time, consumed by the GetAsyncKeyState detour to scope
// hotkey blocking to NVIDIA-driver callers only.
const uint8_t* g_nvd3dumx_text_start = nullptr;
const uint8_t* g_nvd3dumx_text_end   = nullptr;

// ---------------------------------------------------------------------------
// Hook 1: OSD warnings dispatcher (FUN_1802C4850 in current driver builds).
//
// Anchor bytes that make this signature essentially unique inside nvd3dumx.dll:
//   - 0x488 literal stack frame
//   - 0x346DC5D63886594B = fast-divide-by-10000 magic constant
//
// Wildcards (mask[i] == false):
//   - 4 bytes: RIP-relative displacement of MOV RAX, [security_cookie]
//   - 4 bytes: RIP-relative displacement of CALL [GetSystemTimeAsFileTime]
// ---------------------------------------------------------------------------
constexpr uint8_t kSigDispatcher[] = {
    0x40, 0x55,                                     // PUSH RBP
    0x56,                                           // PUSH RSI
    0x48, 0x8D, 0xAC, 0x24, 0x78, 0xFC, 0xFF, 0xFF, // LEA RBP, [RSP-0x388]
    0x48, 0x81, 0xEC, 0x88, 0x04, 0x00, 0x00,       // SUB RSP, 0x488
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,       // MOV RAX, [security_cookie]
    0x48, 0x33, 0xC4,                               // XOR RAX, RSP
    0x48, 0x89, 0x85, 0x50, 0x03, 0x00, 0x00,       // MOV [RBP+0x350], RAX
    0x48, 0x8B, 0xF1,                               // MOV RSI, RCX
    0x48, 0x8D, 0x4C, 0x24, 0x38,                   // LEA RCX, [RSP+0x38]
    0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,             // CALL [GetSystemTimeAsFileTime]
    0x48, 0xB8, 0x4B, 0x59, 0x86, 0x38,
    0xD6, 0xC5, 0x6D, 0x34,                         // MOV RAX, 0x346DC5D63886594B
};
constexpr bool kMaskDispatcher[] = {
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
static_assert(sizeof(kSigDispatcher) == sizeof(kMaskDispatcher) / sizeof(bool),
              "Dispatcher sig/mask length mismatch");

// __fastcall on Windows x64 = standard calling convention (RCX, RDX, R8, R9).
using NvOsdDispatch_t = void(__fastcall*)(LONGLONG);
NvOsdDispatch_t g_original_dispatcher = nullptr;

// Warnings bitmask layout in the dispatcher (FUN_1802C4850 in current driver
// builds, 23 slots). Each set bit triggers its OSD when the dispatcher's
// iteration counter reaches that index. Quoted strings below are the literals
// the case body passes to the overlay renderer (FUN_1802C66E0); entries with
// no string are graphical overlays identified by their case-body signature.
//
//   Bit  Element
//   ---  -------
//    0   Depth Amount slider widget (FUN_1802C5630)               *** SUPPRESSED ***
//    1   "Only anaglyph Stereoscopic 3D is allowed for this display."
//    2   Stereo test pattern / grid (FUN_1802C5CD0 + FUN_1802C5920)
//    3   Unidentified overlay (FUN_1802C6A10, no associated string)
//    4   (no case in dispatcher — unused slot)
//    5   "Out of memory."
//    6   Unidentified overlay (FUN_1802C5F90, no associated string)
//    7   "VESA emitter cable is disconnected / or Stereoscopic 3D is off on TV."
//    8   "Stereo signal is not detected. / Enable Stereoscopic 3D in TV menu."
//    9   Multi-line green OSD; 8 strings stored XOR-obfuscated with 0xAAAAAAAA
//        (decoded into a local stack buffer before rendering)
//   10   "Warning: attempt to run Stereoscopic 3D in a non-stereo display     *** SUPPRESSED ***
//        mode, please change to an acceptable mode. / See documentation for
//        acceptable Stereoscopic 3D modes."
//   11   "Prototype Test Only. NVIDIA Confidential. Not For Production. Not For Release."
//   12   "Warning - %dHz refresh for %dx%dx%d is not available. / Stereoscopic 3D
//        is temporarily disabled. / Change the override value in the registry."
//   13   "Warning - high VBI failed to set. / Stereoscopic 3D is temporarily disabled."
//   14   "Warning - attempt to run windowed page flipped mode with DCE on. /
//        Stereoscopic 3D is temporarily disabled. / Turn off AERO and launch an
//        application again."
//   15   "All vertex shaders are processed"
//   16   White rectangular overlay sized to the back buffer (FUN_1802C05A0)
//   17   "Warning: MSAA,CSAA and SSAA has been disabled due to resource constraints..."
//   18   "Warning: your requested AA setting has been downgraded to 2X AA..."
//   19   "Warning: your requested AA setting has been downgraded to 4X AA..."
//   20   "Please try 1280x720@60Hz or 1920x1080@24Hz."
//   21   "Please try 1920x1080." (or formatted "Please try %dx%d.")
//   22   "Please select a valid stereoscopic 3D mode."
//
// To suppress more bits, OR them into kSuppressedOsdBits below. Every bit not
// listed there is left alone so legitimate driver warnings still fire.
constexpr uint32_t kSuppressedOsdBits = (1u << 0) | (1u << 10);

// Detour body: clear the suppressed bits from the warnings bitmask at the
// well-known offset 600 of the dispatcher's argument struct, then pass through
// to the original. param_1 is NVIDIA's internal stereo-OSD state struct;
// defensive null-check in case the driver ever calls in with 0.
void __fastcall NvOsdDispatchDetour(LONGLONG param_1) {
    if (param_1) {
        auto& warning_bits = *reinterpret_cast<uint32_t*>(param_1 + 600);
        warning_bits &= ~kSuppressedOsdBits;
    }
    if (g_original_dispatcher) {
        g_original_dispatcher(param_1);
    }
}

// ---------------------------------------------------------------------------
// Hook 2: rating / info overlay (FUN_180284160 in current driver builds).
//
// This is the function NVIDIA's DX9 UMD calls to composite the per-application
// compatibility-info overlay: the "Rating: Excellent/Good/Fair/Not Recommended"
// header, the "This application is not rated by NVIDIA Corp." red text when no
// profile matches, the "3D Compatibility mode on/off" notice, the A-J
// known-issue list, and the "Press X to toggle this info." hint. For exes
// without an NVCP compatibility profile (vrserver.exe), every code path inside
// this function ends up at the "not rated" branch, so a flat no-op detour is
// functionally equivalent to suppressing just the "not rated" overlay.
//
// Anchor bytes:
//   - 0x12A0 literal stack size in LEA RBP, [RSP-0x12A0]
//   - 0x13A0 literal in MOV EAX, 0x13A0 (the chkstk request)
//   - Full PUSH RBX/RBP/RSI/RDI/R12/R13/R14/R15 prologue (uncommon to save all)
//   - RBP-relative store at [RBP+0x1290]
//
// Wildcards:
//   - 4 bytes: rel32 displacement of CALL __chkstk
//   - 4 bytes: RIP-relative displacement of MOV RAX, [security_cookie]
//   - 4 bytes: RIP-relative displacement of CMP byte ptr [DAT_*], 0
// ---------------------------------------------------------------------------
constexpr uint8_t kSigRating[] = {
    0x48, 0x89, 0x5C, 0x24, 0x20,                   // MOV [RSP+0x20], RBX
    0x55,                                           // PUSH RBP
    0x56,                                           // PUSH RSI
    0x57,                                           // PUSH RDI
    0x41, 0x54,                                     // PUSH R12
    0x41, 0x55,                                     // PUSH R13
    0x41, 0x56,                                     // PUSH R14
    0x41, 0x57,                                     // PUSH R15
    0x48, 0x8D, 0xAC, 0x24, 0x60, 0xED, 0xFF, 0xFF, // LEA RBP, [RSP-0x12A0]
    0xB8, 0xA0, 0x13, 0x00, 0x00,                   // MOV EAX, 0x13A0
    0xE8, 0x00, 0x00, 0x00, 0x00,                   // CALL __chkstk
    0x48, 0x2B, 0xE0,                               // SUB RSP, RAX
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,       // MOV RAX, [security_cookie]
    0x48, 0x33, 0xC4,                               // XOR RAX, RSP
    0x48, 0x89, 0x85, 0x90, 0x12, 0x00, 0x00,       // MOV [RBP+0x1290], RAX
    0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00,       // CMP byte [DAT_*], 0
};
constexpr bool kMaskRating[] = {
    true, true, true, true, true,
    true,
    true,
    true,
    true, true,
    true, true,
    true, true,
    true, true,
    true, true, true, true, true, true, true, true,
    true, true, true, true, true,
    true, false, false, false, false,               // rel32 CALL __chkstk
    true, true, true,
    true, true, true, false, false, false, false,   // RIP-rel security cookie load
    true, true, true,
    true, true, true, true, true, true, true,
    true, true, false, false, false, false, true,   // RIP-rel disp + imm8(0) stays stable
};
static_assert(sizeof(kSigRating) == sizeof(kMaskRating) / sizeof(bool),
              "Rating sig/mask length mismatch");

// Three-arg __fastcall: RCX, RDX, R8 carry param_1/param_2/param_3.
using NvRatingOverlay_t = void(__fastcall*)(void*, void*, unsigned int);
NvRatingOverlay_t g_original_rating = nullptr;

// Detour: drop the call on the floor. We deliberately do NOT chain to the
// original — calling through would just render the OSD we're trying to hide.
void __fastcall NvRatingOverlayDetour(void* /*p1*/, void* /*p2*/,
                                       unsigned int /*p3*/) {
    // Intentionally empty.
}

// ---------------------------------------------------------------------------
// Hook 3: user32!GetAsyncKeyState hotkey blocker.
//
// NVIDIA's per-feature hotkey dispatchers (FUN_1802A9300, FUN_1802A9840, and
// siblings) all share the same shape: poll a configured VK code via
// GetAsyncKeyState, check a separate modifier byte against DAT_1826E4FB4
// (the live Ctrl/Shift/Alt state), dispatch to the action. We don't need to
// find every one of those dispatchers — we cut them all off at the source by
// hooking GetAsyncKeyState in user32 and lying when:
//   1. the caller's return address falls inside nvd3dumx.dll's .text, AND
//   2. the polled VK is in our blocklist, AND
//   3. Ctrl is currently held.
//
// Calls from anywhere else in the process and presses without Ctrl pass
// through unmodified.
//
// The blocklist VKs map 1:1 to the registry actions documented in NVIDIA's
// Stereo3D hotkey schema (FUN_1800249F0 enumerates them):
//   F3 / F4   -> StereoSeparationAdjustLess / More
//   F5 / F6   -> StereoConvergenceAdjustLess / More
//   F7        -> WriteConfig
//   F10       -> RHWAtScreenMore
//   F11       -> CycleFrustumAdjust
// ---------------------------------------------------------------------------
constexpr int kHotkeyBlocklist[] = {
    VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F10, VK_F11,
};

using GetAsyncKeyState_t = SHORT (WINAPI*)(int);
GetAsyncKeyState_t g_original_GetAsyncKeyState = nullptr;

SHORT WINAPI NvGetAsyncKeyStateDetour(int vKey) {
    GetAsyncKeyState_t orig = g_original_GetAsyncKeyState;
    if (!orig) return 0;

    auto* ret = static_cast<const uint8_t*>(_ReturnAddress());
    const bool from_nvd3dumx =
        g_nvd3dumx_text_start && g_nvd3dumx_text_end &&
        ret >= g_nvd3dumx_text_start && ret < g_nvd3dumx_text_end;
    if (from_nvd3dumx) {
        for (int v : kHotkeyBlocklist) {
            if (vKey != v) continue;
            // Use the original directly — don't recurse through our own detour
            // (we're already past the return-address check for this frame).
            SHORT ctrl = orig(VK_CONTROL);
            if (ctrl & 0x8000) return 0;
            break;
        }
    }
    return orig(vKey);
}

// ---------------------------------------------------------------------------
// One spec per hook. Each spec uses EITHER signature-scan resolution (sig
// non-null, scans nvd3dumx.dll's .text) OR API resolution (api_proc non-null,
// resolves via GetProcAddress). Add new hooks by appending a row AND bumping
// Nv3DVisionSuppressor::kHookCount in the header. Index order matches
// installed_targets_[] in the class.
// ---------------------------------------------------------------------------
struct HookSpec {
    const char*    name;              // human-readable, used only in logs

    // Mode A — signature scan in nvd3dumx.dll's .text. Used when sig != null.
    const uint8_t* sig;
    const bool*    mask;
    size_t         sig_len;

    // Mode B — GetProcAddress on `api_module!api_proc`. Used when api_proc !=
    // null (and sig must be null). api_module is a wide module name e.g.
    // L"user32.dll".
    const wchar_t* api_module;
    const char*    api_proc;

    void*  detour;
    void** original_out;              // where MinHook stores the trampoline ptr
};

const HookSpec kHooks[] = {
    {
        "OSD warnings dispatcher",
        kSigDispatcher, kMaskDispatcher, sizeof(kSigDispatcher),
        nullptr, nullptr,
        reinterpret_cast<void*>(NvOsdDispatchDetour),
        reinterpret_cast<void**>(&g_original_dispatcher),
    },
    {
        "rating / info overlay",
        kSigRating, kMaskRating, sizeof(kSigRating),
        nullptr, nullptr,
        reinterpret_cast<void*>(NvRatingOverlayDetour),
        reinterpret_cast<void**>(&g_original_rating),
    },
    {
        "GetAsyncKeyState hotkey blocker",
        nullptr, nullptr, 0,
        L"user32.dll", "GetAsyncKeyState",
        reinterpret_cast<void*>(NvGetAsyncKeyStateDetour),
        reinterpret_cast<void**>(&g_original_GetAsyncKeyState),
    },
};
static_assert(sizeof(kHooks) / sizeof(kHooks[0])
                  == Nv3DVisionSuppressor::kHookCount,
              "Hook spec count must match kHookCount in the header.");

}  // namespace

Nv3DVisionSuppressor::~Nv3DVisionSuppressor() {
    Uninstall();
}

bool Nv3DVisionSuppressor::Install() {
    if (installed_) return true;

    // Resolve nvd3dumx.dll up front — both the signature-scan hooks and the
    // GetAsyncKeyState detour's caller-filter need its .text range.
    HMODULE nv_mod = GetModuleHandleW(L"nvd3dumx.dll");
    if (!nv_mod) {
        LOG() << "Nv3DVisionSuppressor: nvd3dumx.dll is not loaded — "
                 "hooks skipped (D3D9Ex device not yet created?).";
        return false;
    }

    uint8_t* nv_text_base = nullptr;
    size_t   nv_text_size = 0;
    if (!GetTextSection(nv_mod, &nv_text_base, &nv_text_size)) {
        LOG() << "Nv3DVisionSuppressor: could not locate .text section in nvd3dumx.dll.";
        return false;
    }
    // Set BEFORE any MH_EnableHook so the GetAsyncKeyState detour, which may
    // fire as soon as it's enabled, can safely consult the range.
    g_nvd3dumx_text_start = nv_text_base;
    g_nvd3dumx_text_end   = nv_text_base + nv_text_size;

    // MinHook is a process-global singleton. We're the only patcher in this
    // process, so a flat Init/Uninit pair on the class lifecycle is enough.
    // ALREADY_INITIALIZED is treated as success in case some other component
    // initialised it first.
    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            LOG() << "Nv3DVisionSuppressor: MH_Initialize failed status=" << s;
            return false;
        }
    }

    // Walk every spec. Each hook is independent — a missed signature on one
    // doesn't block the others. We track how many succeeded and only release
    // the MinHook ref if none did.
    int installed_count = 0;
    for (int i = 0; i < kHookCount; ++i) {
        const HookSpec& spec = kHooks[i];

        // Resolve the target address. Mode A: sig-scan within nvd3dumx's
        // .text. Mode B: GetProcAddress on the named module/function.
        void* target = nullptr;
        if (spec.sig) {
            target = ScanSignature(nv_text_base, nv_text_size,
                                    spec.sig, spec.mask, spec.sig_len);
        } else if (spec.api_proc) {
            HMODULE m = GetModuleHandleW(spec.api_module);
            if (m) target = reinterpret_cast<void*>(GetProcAddress(m, spec.api_proc));
        }
        if (!target) {
            LOG() << "Nv3DVisionSuppressor: " << spec.name
                  << " target could not be resolved — that suppression will not apply.";
            continue;
        }

        MH_STATUS s = MH_CreateHook(target, spec.detour,
                                     reinterpret_cast<LPVOID*>(spec.original_out));
        if (s != MH_OK) {
            LOG() << "Nv3DVisionSuppressor: MH_CreateHook(" << spec.name
                  << ") failed status=" << s;
            continue;
        }
        s = MH_EnableHook(target);
        if (s != MH_OK) {
            LOG() << "Nv3DVisionSuppressor: MH_EnableHook(" << spec.name
                  << ") failed status=" << s;
            MH_RemoveHook(target);
            *spec.original_out = nullptr;
            continue;
        }

        installed_targets_[i] = target;
        ++installed_count;

        std::ostringstream addr;
        addr << std::hex << std::showbase << reinterpret_cast<uintptr_t>(target);
        if (spec.sig) {
            addr << " (.text offset "
                 << (static_cast<uint8_t*>(target) - nv_text_base) << ")";
        }
        LOG() << "Nv3DVisionSuppressor: hooked " << spec.name << " at " << addr.str();
    }

    if (installed_count == 0) {
        // Nothing took — tear MinHook back down and clear the range globals
        // so a future Install() retry starts from a clean state.
        g_nvd3dumx_text_start = nullptr;
        g_nvd3dumx_text_end   = nullptr;
        MH_Uninitialize();
        return false;
    }

    installed_ = true;
    LOG() << "Nv3DVisionSuppressor: " << installed_count << " of " << kHookCount
          << " suppression hooks active.";
    return true;
}

void Nv3DVisionSuppressor::Uninstall() {
    if (!installed_) return;
    for (int i = 0; i < kHookCount; ++i) {
        if (installed_targets_[i]) {
            MH_DisableHook(installed_targets_[i]);
            MH_RemoveHook(installed_targets_[i]);
            installed_targets_[i] = nullptr;
        }
        *kHooks[i].original_out = nullptr;
    }
    g_nvd3dumx_text_start = nullptr;
    g_nvd3dumx_text_end   = nullptr;
    installed_ = false;
    MH_Uninitialize();
    LOG() << "Nv3DVisionSuppressor: suppression hooks removed.";
}

}  // namespace platform
