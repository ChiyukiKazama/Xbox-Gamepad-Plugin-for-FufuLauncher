#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include "MinHook.h"
#include "Sensitivity.h"
#include "SensitivityFactor.h"

namespace {
    // The existing input manager computes native camera sensitivity for both
    // axes and both sensitivity groups, with distinct keyboard/gamepad settings.
    constexpr const char* SensitivitySignature =
        "56 57 55 53 48 83 EC 48 0F 29 74 24 30 44 89 C5 89 D3 48 89 CF "
        "80 3D ? ? ? ? 00 0F 85 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "48 8B 47 18 48 85 C0 74 ? 8B 8F ? ? ? ? 83 F9 03";
    // This separate native path serializes settings. Its getter calls must
    // receive unmodified values, not the effective in-game multiplier.
    constexpr const char* SaveSettingsSignature =
        "56 57 48 83 EC 28 48 89 CE 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "48 8B 7E 18 E8 ? ? ? ? 48 85 FF 0F 84 ? ? ? ? 48 89 47 10 "
        "48 8B 7E 18 48 85 FF 0F 84 ? ? ? ? 8B 86 ? ? ? ? 89 47 1C";
    using GetSensitivity = float (WINAPI*)(void*, bool, bool, void*);
    GetSensitivity g_Original = nullptr;
    void* g_Entry = nullptr;
    std::uintptr_t g_SaveBegin = 0, g_SaveEnd = 0;
    unsigned g_ModeOffset = 0;
    std::atomic<float> g_Factor{ 1.0f };
    static_assert(std::atomic<float>::is_always_lock_free, "Sensitivity reads must not acquire a lock");
    Sensitivity::Logger g_Log = nullptr;
    bool g_Diagnostics = false, g_PrepareAttempted = false, g_InstallAttempted = false;
    std::atomic<unsigned> g_Traced{ 0 };

    void Log(const char* text) { if (g_Log) g_Log(text); }

    float WINAPI HookSensitivity(void* input, bool group, bool vertical, void* methodInfo) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const float original = g_Original(input, group, vertical, methodInfo);
        const float factor = g_Factor.load(std::memory_order_relaxed);
        if (factor == 1.0f) return original;
        const int mode = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(input) + g_ModeOffset);
        const bool saving = caller >= g_SaveBegin && caller < g_SaveEnd;
        const float result = ApplySensitivityFactor(original, factor, mode, saving);
        // At most four gamepad samples and one keyboard sample per process.
        // No per-frame logging or call-stack walks.
        if (g_Diagnostics && !saving && (mode == 2 || mode == 3)) {
            const unsigned bit = mode == 2 ? 16u : 1u << ((group ? 2u : 0u) + (vertical ? 1u : 0u));
            if (!(g_Traced.load(std::memory_order_relaxed) & bit) &&
                !(g_Traced.fetch_or(bit, std::memory_order_relaxed) & bit)) {
                char text[192]{};
                sprintf_s(text, "Camera sensitivity sample: mode=%d group=%u axis=%c native=%.4f effective=%.4f factor=%.3f",
                    mode, group ? 1u : 0u, vertical ? 'Y' : 'X', static_cast<double>(original),
                    static_cast<double>(result), static_cast<double>(factor));
                Log(text);
            }
        }
        return result;
    }
}

void Sensitivity::Configure(Logger log, float factor, bool diagnostics) {
    g_Log = log;
    SetFactor(factor);
    g_Diagnostics = diagnostics;
}

void Sensitivity::SetFactor(float factor) {
    g_Factor.store(ValidSensitivityFactor(factor) ? factor : 1.0f, std::memory_order_relaxed);
}

void Sensitivity::Prepare(Resolver find) {
    if (g_Factor.load(std::memory_order_relaxed) == 1.0f || g_PrepareAttempted) return;
    g_PrepareAttempted = true;
    const auto entry = static_cast<const unsigned char*>(find(SensitivitySignature));
    const auto save = static_cast<const unsigned char*>(find(SaveSettingsSignature));
    DWORD64 base = 0;
    const auto function = save ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(save), &base, nullptr) : nullptr;
    if (!entry || !function || base + function->BeginAddress != reinterpret_cast<DWORD64>(save) ||
        function->EndAddress <= function->BeginAddress) {
        Log("Camera sensitivity signatures/function boundary absent or ambiguous; disabled, no fallback");
        return;
    }
    const unsigned mode = *reinterpret_cast<const unsigned*>(entry + 0x3A);
    // Cross-check the mode field against the settings serialization branch.
    if (mode >= 0x1000 || save[0xC9] != 0x8B || save[0xCA] != 0x86 ||
        *reinterpret_cast<const unsigned*>(save + 0xCB) != mode) {
        Log("Camera sensitivity mode field cross-check failed; disabled");
        return;
    }
    g_ModeOffset = mode;
    g_SaveBegin = base + function->BeginAddress;
    g_SaveEnd = base + function->EndAddress;
    g_Entry = const_cast<unsigned char*>(entry);
    char text[192]{};
    const auto game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    sprintf_s(text, "Camera sensitivity resolved once: getter=0x%llX save=0x%llX-0x%llX mode=0x%X factor=%.3f",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_Entry) - game),
        static_cast<unsigned long long>(g_SaveBegin - game), static_cast<unsigned long long>(g_SaveEnd - game),
        mode, static_cast<double>(g_Factor.load(std::memory_order_relaxed)));
    Log(text);
}

void Sensitivity::Install() {
    if (!g_Entry || g_InstallAttempted) return;
    g_InstallAttempted = true;
    if (MH_CreateHook(g_Entry, reinterpret_cast<void*>(&HookSensitivity),
        reinterpret_cast<void**>(&g_Original)) != MH_OK || MH_EnableHook(g_Entry) != MH_OK) {
        if (g_Original) MH_RemoveHook(g_Entry);
        Log("Camera sensitivity hook failed; native sensitivity unchanged");
        return;
    }
    Log("Gamepad camera sensitivity multiplier ready; keyboard and settings-save calls unchanged; no polling");
}
