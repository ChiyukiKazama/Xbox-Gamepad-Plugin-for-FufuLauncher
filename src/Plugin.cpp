#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>
#include <winternl.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "ButtonRouter.h"
#include "NativePages.h"
#include "GamepadButton.h"
#include "HotSwitch.h"
#include "Sensitivity.h"
#include "SensitivityFactor.h"
#include "LiveConfig.h"

namespace {
    using GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    struct Backend {
        const wchar_t* name;
        void* target = nullptr;
        GetState original = nullptr;
        std::atomic<bool> observed{ false };
    };
    std::array<Backend, 3> g_Backends{{
        {L"xinput1_3.dll"}, {L"xinput9_1_0.dll"}, {L"xinput1_4.dll"}
    }};
    HMODULE g_Module = nullptr;
    HANDLE g_ModuleEvent = nullptr;
    std::filesystem::path g_Directory;
    bool g_Diagnostics = false;
    std::atomic<bool> g_MapEnabled{ false };
    GamepadButton g_MapButton = GamepadButton::Parse(L"View");
    bool g_LogEnabled = false;
    std::mutex g_LogMutex;
    std::mutex g_InputMutex;
    ButtonRouter g_Router;
    thread_local unsigned g_InputDepth = 0;

    void Log(const char* message) {
        if (!g_LogEnabled) return;
        std::lock_guard<std::mutex> lock(g_LogMutex);
        std::ofstream file(g_Directory / L"XboxGamepadPlugin.log", std::ios::app);
        file << GetTickCount64() << " " << message << '\n';
    }

    bool RequestMap(void* context) {
        (void)context;
        if (g_Diagnostics) Log("Configured map button pressed");
        return NativePages::RequestMap(g_MapEnabled.load(std::memory_order_relaxed));
    }

    bool ApplyLiveSettings(const LiveSettings& next, LiveSettings& current) {
        const bool rebind = next.mapEnabled != current.mapEnabled || !next.mapButton.SameBinding(current.mapButton);
        const bool factorChanged = next.sensitivity != current.sensitivity;
        if (rebind) {
            std::lock_guard<std::mutex> lock(g_InputMutex);
            g_MapEnabled.store(next.mapEnabled && next.mapButton.IsValid(), std::memory_order_relaxed);
            g_MapButton = next.mapButton;
            g_Router.Rebind();
            NativePages::CancelMapRequest();
        }
        if (factorChanged) Sensitivity::SetFactor(next.sensitivity);
        current = next;
        return rebind || factorChanged;
    }

    template<std::size_t Index>
    DWORD WINAPI HookGetState(DWORD controller, XINPUT_STATE* state) {
        auto& backend = g_Backends[Index];
        ++g_InputDepth;
        const DWORD result = backend.original(controller, state);
        const bool outermost = --g_InputDepth == 0;
        if (!outermost || controller >= 4) return result;
        if (NativePages::RequestActivation()) SetEvent(g_ModuleEvent);

        if (!backend.observed.exchange(true, std::memory_order_relaxed)) {
            Log(Index == 0 ? "Game reads XInput 1.3" :
                Index == 1 ? "Game reads XInput 9.1.0" : "Game reads XInput 1.4");
        }
        if (!g_MapEnabled.load(std::memory_order_relaxed) && !g_Diagnostics && !HotSwitch::IsEnabled())
            return result;
        std::lock_guard<std::mutex> lock(g_InputMutex);
        if (result != ERROR_SUCCESS) {
            g_Router.Disconnect(controller);
            HotSwitch::Disconnect(controller);
            return result;
        }
        if (!state) return result;
        HotSwitch::ObserveGamepad(controller, state->Gamepad);
        const bool enabled = g_MapEnabled.load(std::memory_order_relaxed);
        const bool down = g_MapButton.IsDown(state->Gamepad);
        const bool canStart = !enabled || g_MapButton.CanStart(state->Gamepad);
        const bool captured = g_Router.Sample(controller, down,
            (enabled || g_Diagnostics) && canStart, RequestMap, nullptr);
        if (captured) g_MapButton.EmitCarrier(state->Gamepad);
        return result;
    }

    const std::array<void*, 3> g_Detours{{
        reinterpret_cast<void*>(&HookGetState<0>),
        reinterpret_cast<void*>(&HookGetState<1>),
        reinterpret_cast<void*>(&HookGetState<2>)
    }};

    void InstallAvailableBackends() {
        NativePages::Prepare(); NativePages::Install();
        if (NativePages::IsReady()) HotSwitch::Install();
        for (std::size_t index = 0; index < g_Backends.size(); ++index) {
            auto& backend = g_Backends[index];
            if (backend.target) continue;
            const HMODULE module = GetModuleHandleW(backend.name);
            if (!module) continue; // Never loads extra input DLLs into the game.
            void* target = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
            if (!target) continue;
            bool duplicate = false;
            for (const auto& other : g_Backends) {
                if (other.target == target) duplicate = true;
            }
            if (duplicate) {
                backend.target = target;
                continue;
            }
            const auto created = MH_CreateHook(target, g_Detours[index],
                reinterpret_cast<void**>(&backend.original));
            if (created != MH_OK) {
                Log("XInputGetState hook creation failed; backend unchanged");
                continue;
            }
            if (MH_EnableHook(target) != MH_OK) {
                MH_RemoveHook(target);
                Log("XInputGetState hook activation failed; backend unchanged");
                continue;
            }
            backend.target = target;
            Log("XInputGetState hook installed");
        }
    }

    void PollGamepads() {
        // Keyboard mode may stop the game's own controller reads. Reuse one
        // already-installed backend at 20 Hz; do not load or scan input DLLs.
        if (!HotSwitch::IsReady() || !HotSwitch::IsEnabled()) return;
        GetState read = nullptr;
        for (const auto& backend : g_Backends) {
            if (backend.original) { read = backend.original; break; }
        }
        if (!read) return;
        for (unsigned controller = 0; controller < 4; ++controller) {
            XINPUT_STATE state{};
            ++g_InputDepth; // Alias backends must not treat our poll as game input.
            const DWORD result = read(controller, &state);
            --g_InputDepth;
            std::lock_guard<std::mutex> lock(g_InputMutex);
            if (result == ERROR_SUCCESS) HotSwitch::ObserveGamepad(controller, state.Gamepad);
            else HotSwitch::Disconnect(controller);
        }
    }

    // Loader notifications run under the loader lock. Only signal here;
    // resolve exports/install hooks on our worker after the callback returns.
    struct NotificationEntry {
        ULONG flags;
        const UNICODE_STRING* fullName;
        const UNICODE_STRING* baseName;
        PVOID base;
        ULONG imageSize;
    };
    union NotificationData { NotificationEntry loaded; NotificationEntry unloaded; };
    using NotifyCallback = void (CALLBACK*)(ULONG, const NotificationData*, PVOID);
    using RegisterNotification = NTSTATUS (NTAPI*)(ULONG, NotifyCallback, PVOID, PVOID*);

    bool NameEquals(const UNICODE_STRING* name, const wchar_t* expected) {
        const std::size_t length = wcslen(expected);
        return name && name->Buffer && name->Length == length * sizeof(wchar_t) &&
            _wcsnicmp(name->Buffer, expected, length) == 0;
    }

    void CALLBACK OnModuleLoaded(ULONG reason, const NotificationData* data, PVOID) {
        if (reason != 1 || !data) return;
        const auto name = data->loaded.baseName;
        const auto full = data->loaded.fullName;
        if ((full && NativePages::MatchesMainPath(full->Buffer, full->Length / sizeof(wchar_t))) ||
            NameEquals(name, L"xinput1_3.dll") || NameEquals(name, L"xinput9_1_0.dll") ||
            NameEquals(name, L"xinput1_4.dll")) {
            SetEvent(g_ModuleEvent);
        }
    }

    DWORD WINAPI Worker(void*) {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(g_Module, path, ARRAYSIZE(path))) return 0;
        g_Directory = std::filesystem::path(path).parent_path();
        const auto config = g_Directory / L"config.ini";
        g_LogEnabled = GetPrivateProfileIntW(L"DebugLog", L"Value", 0, config.c_str()) != 0;
        g_Diagnostics = GetPrivateProfileIntW(L"InputDiagnostics", L"Value", 0, config.c_str()) != 0;
        const bool hotSwitch = GetPrivateProfileIntW(L"GlobalInputSwitch", L"Value", 0, config.c_str()) != 0;
        ConfigWatcher watcher;
        const bool watching = watcher.Start(g_Directory);
        LiveSettings current, initial;
        if (!ReadLiveSettings(config, initial, false))
            Log("Initial config unreadable/invalid; live features remain disabled until a valid save");
        Sensitivity::Configure(Log, initial.sensitivity, g_Diagnostics);
        ApplyLiveSettings(initial, current);
        HotSwitch::Configure(Log, hotSwitch, g_Diagnostics);
        // Keep the verified map route available for later live enabling. Resolve
        // it once; changing config never repeats native map signature searches.
        NativePages::Configure(Log, true, g_Diagnostics, hotSwitch, true, g_Directory.c_str());
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        const auto executable = std::filesystem::path(path).filename().wstring();
        if (_wcsicmp(executable.c_str(), L"YuanShen.exe") &&
            _wcsicmp(executable.c_str(), L"GenshinImpact.exe")) {
            Log("Unsupported process; plugin left inactive");
            return 0;
        }
        Log("Xbox plugin 1.0.0 started; three live settings; diagnostics opt-in; global switching experimental");
        if (!watching) Log("Config notifications unavailable; settings require game restart (no polling fallback)");
        if (MH_Initialize() != MH_OK) {
            Log("MinHook initialization failed; input unchanged");
            return 0;
        }
        g_ModuleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_ModuleEvent) return 0;
        auto registerNotification = reinterpret_cast<RegisterNotification>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        PVOID cookie = nullptr;
        const bool registered = registerNotification &&
            registerNotification(0, OnModuleLoaded, nullptr, &cookie) >= 0;
        InstallAvailableBackends();
        if (!registered) {
            Log("DLL notification unavailable; only already-loaded backends were inspected");
        }
        HANDLE events[] = {g_ModuleEvent, watcher.Event()};
        ULONGLONG reloadAfter = 0;
        // Block while idle. Only a config event starts a 100 ms save-settling
        // delay; the existing experimental switch alone adds 50 ms sampling.
        for (;;) {
            DWORD timeout = HotSwitch::IsEnabled() ? 50 : INFINITE;
            if (reloadAfter) {
                const auto now = GetTickCount64();
                timeout = (std::min)(timeout, static_cast<DWORD>(now < reloadAfter ? reloadAfter - now : 0));
            }
            const DWORD result = WaitForMultipleObjects(watching ? 2 : 1, events, FALSE, timeout);
            if (result == WAIT_OBJECT_0) InstallAvailableBackends();
            else if (watching && result == WAIT_OBJECT_0 + 1) {
                if (watcher.Consume()) reloadAfter = GetTickCount64() + 100;
            }
            else if (result != WAIT_TIMEOUT) break;
            if (reloadAfter && GetTickCount64() >= reloadAfter) {
                reloadAfter = 0;
                LiveSettings next;
                if (!ReadLiveSettings(config, next)) {
                    Log("Config save unreadable/incomplete/invalid; keeping last valid live settings");
                } else if (ApplyLiveSettings(next, current)) {
                    InstallAvailableBackends();
                    Log("Map switch/button and sensitivity reloaded; startup-only options unchanged");
                }
            }
            if (HotSwitch::IsEnabled()) {
                NativePages::Install();
                if (NativePages::IsReady()) HotSwitch::Install();
                PollGamepads();
            }
        }
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_Module = module;
        DisableThreadLibraryCalls(module);
        // A game plugin remains loaded until process exit; live FreeLibrary is unsupported.
        if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
