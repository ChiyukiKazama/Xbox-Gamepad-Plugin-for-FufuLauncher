#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "MinHook.h"
#include "HotSwitch.h"
#include "DeviceActivity.h"

namespace {
    // Current native input-manager UI update. Its original finishes walking
    // input handlers before we request a device change, including menu pages.
    constexpr const char* UpdateSignature =
        "56 57 53 48 83 EC 20 48 89 CE 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "80 BE ? ? ? ? 00 74 08 48 83 C4 20 5B 5F 5E C3 "
        "80 3D ? ? ? ? 00 0F 85 ? ? ? ? 80 BE ? ? ? ? 00 74 ? "
        "48 89 F1 E8 ? ? ? ? 48 85 C0 74 ? 83 BE ? ? ? ? 00 75 ? "
        "8B 86 ? ? ? ? 83 F8 23 74 ? 83 F8 0B 75 ? B9 1B 00 00 00 E8 ? ? ? ? "
        "84 C0 74 ? 48 8B 05 ? ? ? ? 48 8B B8 ? ? ? ? 48 85 FF";
    constexpr const char* UiModeSignature =
        "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF "
        "44 89 C3 41 89 D0 48 89 CE 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "44 39 86 ? ? ? ? 0F 84 ? ? ? ? 44 89 86 ? ? ? ? 48 89 F1 E8 ? ? ? ?";
    constexpr const char* InputModeSignature =
        "56 57 53 48 83 EC 70 44 89 C3 89 D7 48 89 CE 80 3D ? ? ? ? 00 "
        "0F 85 ? ? ? ? 8B 86 ? ? ? ? 39 F8 75 08 48 83 C4 70 5B 5F 5E C3";
    constexpr const char* CursorSignature =
        "48 83 EC 28 48 89 CA 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "8B 82 ? ? ? ? 85 C0 74 ? 83 F8 03 74 ? 83 F8 02 75 ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9";
    // The native settings-page keyboard switch. Only decode its shared
    // callees/metadata; never invoke the wrapper with a fabricated receiver.
    // Its settings child distinguishes it from the second native wrapper.
    constexpr const char* SwitchSequenceSignature =
        "48 8B 15 ? ? ? ? E8 ? ? ? ? 48 89 C7 48 8B 05 ? ? ? ? "
        "48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? BA 01 00 00 00 41 B0 01 E8 ? ? ? ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? "
        "BA 02 00 00 00 45 31 C0 E8 ? ? ? ? 48 85 FF 74 ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? "
        "80 3D ? ? ? ? 00 0F 85 ? ? ? ? 48 89 F9 89 C2 E8 ? ? ? ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? "
        "48 8B 8E 98 04 00 00 48 85 C9";
    // Native UI-manager device transition, including registered child pages.
    // These are shared functions, not a settings-page wrapper or a page reopen.
    constexpr const char* BeginMenusSignature =
        "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 45 89 C4 41 89 D5 48 89 CE "
        "80 3D ? ? ? ? 00 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "48 8B 8E ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? E8 ? ? ? ? "
        "48 85 C0 0F 84 ? ? ? ? 48 89 C7 8B 58 18 FF CB";
    constexpr const char* FinishMenusSignature =
        "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 45 89 C4 41 89 D5 48 89 CB "
        "80 3D ? ? ? ? 00 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "48 8B 8B ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? E8 ? ? ? ? "
        "48 85 C0 0F 84 ? ? ? ? 48 89 C6 48 8B 40 18 85 C0";
    using Update = void (WINAPI*)(void*, void*);
    using SetMode = void (WINAPI*)(void*, int, bool, void*);
    using GetPage = void* (WINAPI*)(void*, void*);
    using GetKind = int (WINAPI*)(void*, void*);
    using RefreshHud = void (WINAPI*)(void*, int, void*);
    using TransitionMenus = void (WINAPI*)(void*, int, int, void*);
    Update g_OriginalUpdate = nullptr, g_Cursor = nullptr;
    SetMode g_SetUiMode = nullptr, g_SetInputMode = nullptr;
    GetPage g_GetHud = nullptr;
    GetKind g_GetUiKind = nullptr;
    RefreshHud g_RefreshHud = nullptr;
    TransitionMenus g_BeginMenus = nullptr, g_FinishMenus = nullptr;
    void** g_HudMethodSlot = nullptr;
    void* g_UpdateEntry = nullptr;
    void** g_ManagerSlot = nullptr;
    unsigned g_UiManagerOffset = 0;
    unsigned g_InputModeOffset = 0, g_UiModeOffset = 0;
    unsigned g_UiKindOffset = 0, g_PagesOffset = 0, g_PageRefreshOffset = 0, g_ReferenceOffset = 0;
    unsigned g_PageReadyOffset = 0, g_PageDirtyOffset = 0, g_PageModeStateOffset = 0;
    unsigned g_PreviousUiOffset = 0, g_PreviousInputOffset = 0, g_TargetUiOffset = 0, g_TargetInputOffset = 0;
    HotSwitch::Logger g_Log = nullptr;
    bool g_Diagnostics = false;
    unsigned g_LayoutTraces = 0;
    std::atomic<bool> g_Enabled{ false }, g_Ready{ false };
    bool g_Prepared = false, g_InstallAttempted = false;
    DeviceActivity g_Activity;
    GamepadActivity g_Pads;
    HWND g_Window = nullptr;
    bool g_WindowAttempted = false, g_MouseSeen = false, g_FirstTick = true;
    LPARAM g_MousePosition = 0;
    ULONGLONG g_IgnoreMouseUntil = 0, g_LastSwitchTime = 0;
    std::uint64_t g_Handled = 0;
    thread_local bool g_Updating = false;

    void Log(const char* text) { if (g_Log) g_Log(text); }
    const unsigned char* Relative(const unsigned char* displacement) {
        return displacement + 4 + *reinterpret_cast<const int*>(displacement);
    }
    const unsigned char* CallTarget(const unsigned char* call) {
        return call[0] == 0xE8 ? Relative(call + 1) : nullptr;
    }
    bool Focused() {
        DWORD pid = 0;
        const HWND window = GetForegroundWindow();
        return window && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId();
    }
    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM detail,
        UINT_PTR id, DWORD_PTR) {
        if (g_Enabled.load(std::memory_order_relaxed)) {
            const auto now = GetTickCount64();
            switch (message) {
            case WM_KILLFOCUS:
                g_Activity.Clear();
                g_MouseSeen = false;
                break;
            case WM_SETFOCUS:
                g_Activity.Clear();
                g_IgnoreMouseUntil = now + 500;
                break;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (!(detail & (1LL << 30)) && Focused())
                    g_Activity.Notify(InputSource::Keyboard, now);
                break;
            case WM_MOUSEMOVE:
                if (g_MouseSeen && detail != g_MousePosition && now >= g_IgnoreMouseUntil && Focused())
                    g_Activity.Notify(InputSource::Mouse, now);
                g_MouseSeen = true;
                g_MousePosition = detail;
                break;
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                if (Focused()) g_Activity.Notify(InputSource::Mouse, now);
                break;
            }
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, WindowProc, id);
            g_Window = nullptr;
            g_Enabled.store(false, std::memory_order_release);
        }
        return DefSubclassProc(window, message, key, detail);
    }
    BOOL CALLBACK FindWindow(HWND window, LPARAM data) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
        wchar_t name[64]{};
        GetClassNameW(window, name, ARRAYSIZE(name));
        if (wcscmp(name, L"UnityWndClass")) return TRUE;
        *reinterpret_cast<HWND*>(data) = window;
        return FALSE;
    }
    bool InstallWindowMonitor() {
        if (g_Window) return true;
        if (g_WindowAttempted) return false;
        g_WindowAttempted = true;
        HWND window = nullptr;
        EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&window));
        // SetWindowSubclass must execute on the window's owning thread.
        if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
            !SetWindowSubclass(window, WindowProc, reinterpret_cast<UINT_PTR>(&g_Activity), 0)) {
            Log("Hot switch window/thread check failed; feature disabled, no cross-thread subclass");
            g_Enabled.store(false, std::memory_order_release);
            return false;
        }
        g_Window = window;
        Log("Hot switch window input monitor installed on game UI thread");
        return true;
    }
    bool ReadManagers(void* instance, void*& ui, int& inputMode, int& uiMode) {
        __try {
            const auto root = static_cast<unsigned char*>(*g_ManagerSlot);
            if (!root || !instance) return false;
            ui = *reinterpret_cast<void**>(root + g_UiManagerOffset);
            if (!ui) return false;
            inputMode = *reinterpret_cast<int*>(static_cast<unsigned char*>(instance) + g_InputModeOffset);
            uiMode = *reinterpret_cast<int*>(static_cast<unsigned char*>(ui) + g_UiModeOffset);
            return (inputMode == 2 || inputMode == 3) && (uiMode == 1 || uiMode == 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void TraceLayout(void* ui, bool hadHud) {
        // Read only the native open-page collection on the first eight device
        // changes. No timer, scene search or object-name lookup.
        if (!g_Diagnostics || g_LayoutTraces >= 8) return;
        ++g_LayoutTraces;
        __try {
            const auto bytes = static_cast<const unsigned char*>(ui);
            const auto reference = reinterpret_cast<const float*>(bytes + g_ReferenceOffset);
            const auto pages = *reinterpret_cast<const unsigned char* const*>(bytes + g_PagesOffset);
            const int count = pages ? *reinterpret_cast<const int*>(pages + 0x20) : 0;
            char text[4096]{};
            const int written = sprintf_s(text, "Hot switch layout: HUD refreshed=%u; kind=%d; reference=%.0fx%.0f; open pages=%d",
                hadHud ? 1u : 0u, *reinterpret_cast<const int*>(bytes + g_UiKindOffset),
                static_cast<double>(reference[0]), static_cast<double>(reference[1]), count);
            if (written < 0) return;
            if (count < 0 || count > 32) { Log(text); return; }
            const auto array = pages ? *reinterpret_cast<const unsigned char* const*>(pages + 0x10) : nullptr;
            if (!array || *reinterpret_cast<const std::size_t*>(array + 0x18) < static_cast<unsigned>(count)) { Log(text); return; }
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::size_t used = static_cast<std::size_t>(written);
            for (int index = count - 1; index >= 0; --index) {
                const auto page = *reinterpret_cast<const unsigned char* const*>(array + 0x20 + index * sizeof(void*));
                if (!page) continue;
                const auto klass = *reinterpret_cast<const unsigned char* const*>(page);
                if (!klass) continue;
                const auto callback = *reinterpret_cast<const std::uintptr_t*>(klass + g_PageRefreshOffset);
                const auto state = *reinterpret_cast<const unsigned char* const*>(page + g_PageModeStateOffset);
                if (used + 128 >= sizeof(text)) break;
                const int appended = sprintf_s(text + used, sizeof(text) - used,
                    "; page[%d] callback=0x%llX ready=%d dirty=%u previous=%d/%d target=%d/%d", index,
                    static_cast<unsigned long long>(callback >= base ? callback - base : 0),
                    *reinterpret_cast<const int*>(page + g_PageReadyOffset), static_cast<unsigned>(page[g_PageDirtyOffset]),
                    state ? *reinterpret_cast<const int*>(state + g_PreviousUiOffset) : -1,
                    state ? *reinterpret_cast<const int*>(state + g_PreviousInputOffset) : -1,
                    state ? *reinterpret_cast<const int*>(state + g_TargetUiOffset) : -1,
                    state ? *reinterpret_cast<const int*>(state + g_TargetInputOffset) : -1);
                if (appended < 0) break;
                used += static_cast<std::size_t>(appended);
            }
            Log(text); // One file write per captured device change, not per page.
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("Hot switch layout trace unavailable; no retry/search"); }
    }
    bool Apply(void* instance, void* ui, int mode, bool& hadHud) {
        const char* stage = "HUD lookup";
        __try {
            // Match the native switch order. It captures the HUD before changing
            // modes and refreshes its layout/button group afterwards explicitly.
            // The UI setter's generic page notification does not replace this.
            if (!*g_HudMethodSlot) { Log("Hot switch native HUD MethodInfo unavailable; switch skipped"); return false; }
            void* hud = g_GetHud(ui, *g_HudMethodSlot);
            const int uiMode = mode == 3 ? 2 : 1;
            // Capture old device state before either global mode is changed.
            // Generic resize notifications alone do not prepare menu controls.
            stage = "menu transition begin";
            g_BeginMenus(ui, uiMode, mode, nullptr);
            stage = "UI mode";
            // The native global transition passes false here: it uses the
            // menu begin/finish pair, not generic callbacks while enumerating
            // the live page collection. Resolution/kind still update normally.
            g_SetUiMode(ui, uiMode, false, nullptr);
            stage = "input mode";
            g_SetInputMode(instance, mode, false, nullptr);
            if (hud) {
                stage = "HUD refresh";
                g_RefreshHud(hud, g_GetUiKind(ui, nullptr), nullptr);
                hadHud = true;
            }
            stage = "cursor refresh";
            g_Cursor(instance, nullptr);
            stage = "menu transition finish";
            g_FinishMenus(ui, uiMode, mode, nullptr);
            return *reinterpret_cast<int*>(static_cast<unsigned char*>(instance) + g_InputModeOffset) == mode &&
                *reinterpret_cast<int*>(static_cast<unsigned char*>(ui) + g_UiModeOffset) == uiMode;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            char text[160]{};
            sprintf_s(text, "Hot switch native exception at %s; code=0x%08lX", stage, GetExceptionCode());
            Log(text);
            return false;
        }
    }
    void WINAPI HookUpdate(void* instance, void* methodInfo) {
        if (g_Updating) { g_OriginalUpdate(instance, methodInfo); return; }
        g_Updating = true;
        g_OriginalUpdate(instance, methodInfo);
        g_Updating = false;
        if (!g_Enabled.load(std::memory_order_relaxed) || !InstallWindowMonitor()) return;
        void* ui = nullptr;
        int current = 0, uiMode = 0;
        if (!ReadManagers(instance, ui, current, uiMode)) return;
        if (g_FirstTick) {
            char text[160]{};
            sprintf_s(text, "Hot switch native UI update observed; thread=%lu; input mode=%d; UI mode=%d",
                GetCurrentThreadId(), current, uiMode);
            Log(text);
            g_FirstTick = false;
        }
        const auto now = GetTickCount64();
        const auto token = g_Activity.Peek(now);
        if (!token || token == g_Handled) return;
        const int requested = DeviceActivity::NativeMode(token);
        if (current == requested && uiMode == (requested == 3 ? 2 : 1)) { g_Handled = token; return; }
        if (now - g_LastSwitchTime < 500) return; // Preserve held input; avoid mode ping-pong.
        g_Handled = token;
        if (!Focused()) return;
        // Cursor recentering during the native switch must not count as mouse use.
        g_IgnoreMouseUntil = now + 500;
        g_Updating = true;
        bool hadHud = false;
        const bool success = Apply(instance, ui, requested, hadHud);
        g_Updating = false;
        if (!success) {
            Log("Hot switch native call failed or modes mismatched; feature disabled, no repeated retry");
            g_Enabled.store(false, std::memory_order_release);
            return;
        }
        g_LastSwitchTime = now;
        char text[160]{};
        sprintf_s(text, "Hot switch applied %d -> %d; source=%u; UI thread=%lu", current, requested,
            static_cast<unsigned>(DeviceActivity::Source(token)), GetCurrentThreadId());
        Log(text);
        TraceLayout(ui, hadHud);
    }
}

void HotSwitch::Configure(Logger log, bool enabled, bool diagnostics) {
    g_Log = log; g_Enabled.store(enabled); g_Diagnostics = diagnostics;
}
bool HotSwitch::IsEnabled() { return g_Enabled.load(std::memory_order_relaxed); }
bool HotSwitch::IsReady() { return g_Ready.load(std::memory_order_acquire); }
void HotSwitch::ObserveGamepad(unsigned index, const XINPUT_GAMEPAD& pad) {
    if (IsEnabled() && g_Pads.Sample(index, pad) && Focused())
        g_Activity.Notify(InputSource::Gamepad, GetTickCount64());
}
void HotSwitch::Disconnect(unsigned index) { g_Pads.Disconnect(index); }

void HotSwitch::Prepare(Resolver resolve) {
    if (!IsEnabled() || g_Prepared) return;
    g_Prepared = true;
    g_UpdateEntry = resolve(UpdateSignature);
    g_SetUiMode = reinterpret_cast<SetMode>(resolve(UiModeSignature));
    g_SetInputMode = reinterpret_cast<SetMode>(resolve(InputModeSignature));
    g_Cursor = reinterpret_cast<Update>(resolve(CursorSignature));
    g_BeginMenus = reinterpret_cast<TransitionMenus>(resolve(BeginMenusSignature));
    g_FinishMenus = reinterpret_cast<TransitionMenus>(resolve(FinishMenusSignature));
    const auto sequence = static_cast<const unsigned char*>(resolve(SwitchSequenceSignature));
    if (!g_UpdateEntry || !g_SetUiMode || !g_SetInputMode || !g_Cursor || !sequence || !g_BeginMenus || !g_FinishMenus) {
        Log("Hot switch signature absent/ambiguous; feature disabled, no fallback scan");
        g_Enabled.store(false);
        return;
    }
    // Decode fields from the verified native bodies, not fixed game RVAs.
    const auto update = static_cast<const unsigned char*>(g_UpdateEntry);
    const auto cursor = reinterpret_cast<const unsigned char*>(g_Cursor);
    g_ManagerSlot = reinterpret_cast<void**>(const_cast<unsigned char*>(update + 0x79 +
        *reinterpret_cast<const int*>(update + 0x75)));
    g_UiManagerOffset = *reinterpret_cast<const unsigned*>(update + 0x7C);
    g_InputModeOffset = *reinterpret_cast<const unsigned*>(reinterpret_cast<const unsigned char*>(g_SetInputMode) + 0x1E);
    g_UiModeOffset = *reinterpret_cast<const unsigned*>(reinterpret_cast<const unsigned char*>(g_SetUiMode) + 0x2E);
    const auto uiSetter = reinterpret_cast<const unsigned char*>(g_SetUiMode);
    // Validate the native wrapper against all three already matched entries.
    // Its generic HUD getter requires its real MethodInfo, not a null pointer.
    g_GetHud = reinterpret_cast<GetPage>(const_cast<unsigned char*>(CallTarget(sequence + 0x07)));
    g_GetUiKind = reinterpret_cast<GetKind>(const_cast<unsigned char*>(CallTarget(sequence + 0x73)));
    g_RefreshHud = reinterpret_cast<RefreshHud>(const_cast<unsigned char*>(CallTarget(sequence + 0x8A)));
    g_HudMethodSlot = reinterpret_cast<void**>(const_cast<unsigned char*>(Relative(sequence + 0x03)));
    // The cursor body independently references the same root and UI slot.
    const auto cursorSlot = cursor + 0x2F + *reinterpret_cast<const int*>(cursor + 0x2B);
    const auto cursorUi = *reinterpret_cast<const unsigned*>(cursor + 0x32);
    if (cursorSlot != reinterpret_cast<const unsigned char*>(g_ManagerSlot) || cursorUi != g_UiManagerOffset ||
        g_InputModeOffset != *reinterpret_cast<const unsigned*>(cursor + 0x16) ||
        g_InputModeOffset != *reinterpret_cast<const unsigned*>(update + 0x4D) ||
        CallTarget(sequence + 0x2E) != uiSetter ||
        CallTarget(sequence + 0x52) != reinterpret_cast<const unsigned char*>(g_SetInputMode) ||
        CallTarget(sequence + 0xA6) != cursor ||
        Relative(sequence + 0x12) != reinterpret_cast<const unsigned char*>(g_ManagerSlot) ||
        *reinterpret_cast<const unsigned*>(sequence + 0x19) != g_UiManagerOffset ||
        !g_GetHud || !g_GetUiKind || !g_RefreshHud ||
        CallTarget(uiSetter + 0x58) != reinterpret_cast<const unsigned char*>(g_GetUiKind) ||
        uiSetter[0x5D] != 0x89 || uiSetter[0x5E] != 0x86 ||
        uiSetter[0x6B] != 0x48 || uiSetter[0x6C] != 0x8B || uiSetter[0x6D] != 0x86 ||
        uiSetter[0xC7] != 0xFF || uiSetter[0xC8] != 0x90) {
        Log("Hot switch native field cross-check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    const auto layout = CallTarget(uiSetter + 0x50);
    if (!layout || layout[0x88] != 0x48 || layout[0x89] != 0x89 || layout[0x8A] != 0xBE) {
        Log("Hot switch UI layout body check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    g_UiKindOffset = *reinterpret_cast<const unsigned*>(uiSetter + 0x5F);
    g_PagesOffset = *reinterpret_cast<const unsigned*>(uiSetter + 0x6E);
    g_PageRefreshOffset = *reinterpret_cast<const unsigned*>(uiSetter + 0xC9);
    g_ReferenceOffset = *reinterpret_cast<const unsigned*>(layout + 0x8B);
    const auto begin = reinterpret_cast<const unsigned char*>(g_BeginMenus);
    const auto finish = reinterpret_cast<const unsigned char*>(g_FinishMenus);
    const auto preparePage = CallTarget(begin + 0x8F);
    if (!preparePage || !CallTarget(begin + 0x4A) || CallTarget(begin + 0x4A) != CallTarget(finish + 0x4A) ||
        *reinterpret_cast<const unsigned*>(begin + 0xFB) != g_PagesOffset ||
        *reinterpret_cast<const unsigned*>(finish + 0x115) != g_PagesOffset ||
        Relative(preparePage + 0x2B) != reinterpret_cast<const unsigned char*>(g_ManagerSlot) ||
        *reinterpret_cast<const unsigned*>(preparePage + 0x51) != g_UiModeOffset ||
        *reinterpret_cast<const unsigned*>(preparePage + 0x57) != g_InputModeOffset ||
        preparePage[0x1F] != 0x83 || preparePage[0x20] != 0xBF ||
        preparePage[0x63] != 0x80 || preparePage[0x64] != 0xBF ||
        preparePage[0x6A] != 0x48 || preparePage[0x6B] != 0x8B || preparePage[0x6C] != 0x87 ||
        preparePage[0x95] != 0x89 || preparePage[0x96] != 0x68 ||
        preparePage[0x98] != 0x89 || preparePage[0x99] != 0x70 ||
        preparePage[0xA2] != 0x89 || preparePage[0xA3] != 0x58 ||
        preparePage[0xA5] != 0x44 || preparePage[0xA6] != 0x89 || preparePage[0xA7] != 0x70) {
        Log("Hot switch native menu transition cross-check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    // Only used by the bounded, read-only event diagnostic; never written.
    g_PageReadyOffset = *reinterpret_cast<const unsigned*>(preparePage + 0x21);
    g_PageDirtyOffset = *reinterpret_cast<const unsigned*>(preparePage + 0x65);
    g_PageModeStateOffset = *reinterpret_cast<const unsigned*>(preparePage + 0x6D);
    g_PreviousUiOffset = preparePage[0x97]; g_PreviousInputOffset = preparePage[0x9A];
    g_TargetUiOffset = preparePage[0xA4]; g_TargetInputOffset = preparePage[0xA8];
    // The input manager is the receiver supplied by its own native update;
    // no additional root-table offset or setting-page instance is guessed.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char text[256]{};
    sprintf_s(text, "Hot switch entries resolved once; update=0x%llX; UI setter=0x%llX; input setter=0x%llX; cursor=0x%llX",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_UpdateEntry) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_SetUiMode) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_SetInputMode) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_Cursor) - base));
    Log(text);
    sprintf_s(text, "Hot switch native menu transition resolved; begin=0x%llX; finish=0x%llX; device changes only",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_BeginMenus) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_FinishMenus) - base));
    Log(text);
    sprintf_s(text, "Hot switch native HUD chain resolved; getter=0x%llX; UI kind=0x%llX; HUD refresh=0x%llX",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_GetHud) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_GetUiKind) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_RefreshHud) - base));
    Log(text);
}
void HotSwitch::Install() {
    if (!IsEnabled() || !g_Prepared || g_InstallAttempted) return;
    g_InstallAttempted = true;
    if (MH_CreateHook(g_UpdateEntry, reinterpret_cast<void*>(&HookUpdate),
        reinterpret_cast<void**>(&g_OriginalUpdate)) != MH_OK || MH_EnableHook(g_UpdateEntry) != MH_OK) {
        if (g_OriginalUpdate) MH_RemoveHook(g_UpdateEntry);
        Log("Hot switch UI-update hook failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    g_Ready.store(true, std::memory_order_release);
    Log("Global hot switch candidate 0.1.9 ready; native menu transaction; generic live-page refresh disabled; no extra polling");
}
