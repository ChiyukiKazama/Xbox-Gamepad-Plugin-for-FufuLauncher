#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include <filesystem>
#include <string>

#include "MinHook.h"
#include "Patterns/Patterns.h"
#include "NativePages.h"
#include "NativeInputEvent.h"
#include "PageRedirectRequest.h"
#include "Signature.h"
#include "GamepadButton.h"
#include "HotSwitch.h"
#include "Sensitivity.h"

namespace {
    // Same native signatures/calling convention used by the existing plugin.
    struct GameString { void* klass; void* monitor; int length; wchar_t chars[1]; };
    static_assert(offsetof(GameString, length) == 16 && offsetof(GameString, chars) == 20);
    // The actual View/chat stack reaches this virtual input-action dispatcher.
    // Its caller passes (this, pointer to a 32-byte event, MethodInfo).
    // A map request changes only the action in a local copy; never the caller's
    // event, instance, MethodInfo, or the game-owned map availability checks.
    using Dispatch = bool (WINAPI*)(void*, const void*, void*);
    constexpr const char* InputDispatchSignature =
        "56 57 55 53 48 81 EC ? ? ? ? 48 89 D3 48 89 CE C6 44 24 ? 00 "
        "48 C7 44 24 ? 00 00 00 00 48 C7 44 24 ? 00 00 00 00 "
        "48 C7 44 24 ? 00 00 00 00 48 C7 44 24 ? 00 00 00 00 "
        "80 3D ? ? ? ? 00 0F 85 ? ? ? ? 8B 53 04";
    // The observed map action 3 tests this game-state predicate. Action 272
    // tests the same predicate with the opposite condition and joins the same
    // handler. Select the appropriate existing branch, not both or a bypass.
    constexpr const char* MapBranchSignature =
        "48 8B 0D ? ? ? ? 80 B9 C7 00 00 00 00 0F 84 ? ? ? ? E8 ? ? ? ? "
        "84 C0 0F 85 ? ? ? ? 48 8B 0D ? ? ? ? 80 B9 C7 00 00 00 00 "
        "0F 84 ? ? ? ? 8B 15 ? ? ? ? 48 89 F1 45 31 C0 E8 ? ? ? ? E9 ? ? ? ?";
    using AlternateMapState = bool (WINAPI*)();
    AlternateMapState g_AlternateMapState = nullptr;
    PageRedirectRequest g_MapRequest;
    using SetActive = void (WINAPI*)(void*, bool);
    using GetName = GameString* (WINAPI*)(void*);
    Dispatch g_Original = nullptr;
    SetActive g_OriginalSetActive = nullptr;
    GetName g_GetName = nullptr;
    void* g_PageEntry = nullptr;
    void* g_SetActiveEntry = nullptr;
    const unsigned char* g_FovEntry = nullptr;
    std::uintptr_t g_MainBegin = 0, g_MainEnd = 0;
    std::uintptr_t g_GameBegin = 0, g_GameEnd = 0;
    std::wstring g_MainPath, g_ShortMainPath;
    bool g_MainWaitLogged = false;
    NativePages::Logger g_Log = nullptr;
    bool g_MapBinding = false, g_Diagnostics = false, g_HotSwitch = false, g_Sensitivity = false, g_PrepareAttempted = false;
    std::atomic<bool> g_Prepared{ false }, g_ActivationQueued{ false };
    std::atomic<bool> g_InstallAttempted{ false }, g_Ready{ false };
    // Diagnostic capture is armed by a button edge, not during game loading.
    // Stop after both root pages are observed, or a bounded number of lookups.
    constexpr unsigned TraceLookupLimit = 2048;
    std::atomic<bool> g_TraceActive{ false };
    std::atomic<unsigned> g_TraceLookups{ 0 }, g_SeenPages{ 0 }, g_DispatchTraces{ 0 };
    // At most one trace per action ID and 64 action traces per capture. This
    // prevents held/analog input from filling the log before the map test.
    std::array<std::atomic<unsigned long long>, 64> g_SeenActions{};
    std::atomic<unsigned long long> g_DispatchCalls{ 0 };
    thread_local bool g_InTrace = false;

    void Log(const char* message) { if (g_Log) g_Log(message); }

    bool Focused() {
        DWORD pid = 0;
        const HWND window = GetForegroundWindow();
        return window && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId();
    }

    void* FindEntry(const Signature& pattern, bool firstMatchOnly = false) {
        const auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        const auto sections = IMAGE_FIRST_SECTION(nt);
        const unsigned char* result = nullptr;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const auto start = base + section.VirtualAddress;
            const auto finish = start + section.Misc.VirtualSize;
            for (auto cursor = start; cursor < finish;) {
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery(cursor, &memory, sizeof(memory))) return nullptr;
                const auto regionEnd = (std::min)(finish,
                    static_cast<const unsigned char*>(memory.BaseAddress) + memory.RegionSize);
                if (regionEnd <= cursor) return nullptr;
                if (memory.State == MEM_COMMIT && !(memory.Protect & PAGE_GUARD) &&
                    (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                    const unsigned char* match = nullptr;
                    const unsigned count = pattern.Find(cursor, regionEnd - cursor, match, firstMatchOnly);
                    if (count && firstMatchOnly) return const_cast<unsigned char*>(match);
                    if (count > 1 || (count && result)) return nullptr;
                    if (count) result = match;
                }
                cursor = regionEnd;
            }
        }
        return const_cast<unsigned char*>(result);
    }

    // Main hooks are enabled only after its signature resolution has finished.
    // Wait for its existing FOV detour before patching the page entry, otherwise
    // our hooks must not precede the main plugin's signature resolution.
    bool MainHooksActive() {
        __try {
            if (!g_FovEntry || g_FovEntry[0] != 0xE9) return false;
            const auto target = g_FovEntry + 5 + *reinterpret_cast<const int*>(g_FovEntry + 1);
            std::uintptr_t address = reinterpret_cast<std::uintptr_t>(target);
            if (target[0] == 0xFF && target[1] == 0x25) {
                const auto slot = target + 6 + *reinterpret_cast<const int*>(target + 2);
                address = *reinterpret_cast<const std::uintptr_t*>(slot);
            }
            return address >= g_MainBegin && address < g_MainEnd;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadName(GameString* page, wchar_t (&name)[128]) {
        __try {
            if (!page || page->length < 1 || page->length >= 128) return false;
            std::memcpy(name, page->chars, page->length * sizeof(wchar_t));
            name[page->length] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadCode(void* entry, unsigned char (&code)[64]) {
        __try { std::memcpy(code, entry, sizeof(code)); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadEvent(const void* event, unsigned (&words)[8]) {
        __try { if (!event) return false; std::memcpy(words, event, sizeof(words)); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadObjectName(void* object, wchar_t (&name)[128]) {
        __try {
            return object && g_GetName && ReadName(g_GetName(object), name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void Trace(const char* source, const wchar_t* name, void* caller, bool stack) {
        char text[1024]{}, utf8[384]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
        const auto address = reinterpret_cast<std::uintptr_t>(caller);
        const bool inGame = address >= g_GameBegin && address < g_GameEnd;
        const int written = sprintf_s(text, "%s: %s; thread=%lu; game caller=0x%llX",
            source, utf8, GetCurrentThreadId(),
            static_cast<unsigned long long>(inGame ? address - g_GameBegin : 0));
        if (written < 0) return;
        std::size_t used = static_cast<std::size_t>(written);
        if (stack) {
            void* frames[48]{};
            const USHORT count = CaptureStackBackTrace(0, ARRAYSIZE(frames), frames, nullptr);
            for (USHORT index = 0; index < count && used + 24 < sizeof(text); ++index) {
                const auto frame = reinterpret_cast<std::uintptr_t>(frames[index]);
                if (frame < g_GameBegin || frame >= g_GameEnd) continue;
                const int appended = sprintf_s(text + used, sizeof(text) - used, " ->0x%llX",
                    static_cast<unsigned long long>(frame - g_GameBegin));
                if (appended < 0) break;
                used += static_cast<std::size_t>(appended);
            }
        }
        Log(text);
    }

    bool WINAPI HookDispatch(void* instance, const void* event, void* methodInfo) {
        if (g_Diagnostics) g_DispatchCalls.fetch_add(1, std::memory_order_relaxed);
        NativeInputEvent input{};
        // The verified ABI supplies a readable 32-byte event. ReadCode/ReadEvent
        // remain guarded for diagnostics; only a pending chat event is mapped.
        if (g_MapBinding && event) {
            std::memcpy(&input, event, sizeof(input));
            if (input.action == NativeInputEvent::ChatAction) {
                const auto token = g_MapRequest.Peek(GetTickCount64());
                if (token && g_MapRequest.Consume(token)) {
                    const auto mapped = input.ForMap(g_AlternateMapState());
                    if (g_Diagnostics) {
                        char text[128]{};
                        sprintf_s(text, "Native chat action 113 mapped to %u; UI thread=%lu",
                            mapped.action, GetCurrentThreadId());
                        Log(text);
                    }
                    return g_Original(instance, &mapped, methodInfo);
                }
            }
        }
        unsigned words[8]{};
        if (!g_InTrace && g_TraceActive.load(std::memory_order_acquire) &&
            ReadEvent(event, words) && words[1] < 4096) {
            const auto bit = 1ull << (words[1] % 64);
            const auto seen = g_SeenActions[words[1] / 64].fetch_or(bit, std::memory_order_relaxed);
            if ((seen & bit) || g_DispatchTraces.fetch_add(1, std::memory_order_relaxed) >= 64)
                return g_Original(instance, event, methodInfo);
            g_InTrace = true;
            wchar_t description[128]{};
            // Only the eight event words; no dereference of event payloads,
            // chat text, account fields, or speculative method calls.
            swprintf_s(description,
                L"action=%u; words=%08X %08X %08X %08X %08X %08X %08X %08X",
                words[1], words[0], words[1], words[2], words[3],
                words[4], words[5], words[6], words[7]);
            Trace("Native input event", description, _ReturnAddress(), true);
            g_InTrace = false;
        }
        return g_Original(instance, event, methodInfo);
    }

    void WINAPI HookSetActive(void* object, bool active) {
        // Retain the main plugin's existing SetActive hook and its side effects.
        g_OriginalSetActive(object, active);
        if (!active || g_InTrace || !g_TraceActive.load(std::memory_order_acquire)) return;
        const unsigned lookup = g_TraceLookups.fetch_add(1, std::memory_order_relaxed);
        if (lookup >= TraceLookupLimit) {
            if (g_TraceActive.exchange(false)) Log("UI trace lookup limit reached; capture stopped");
            return;
        }
        g_InTrace = true;
        wchar_t name[128]{};
        if (ReadObjectName(object, name)) {
            const auto kind = ClassifyPage(name);
            const unsigned bit = kind == PageKind::Chat ? 1u : kind == PageKind::Map ? 2u : 0u;
            if (bit) {
                const unsigned previous = g_SeenPages.fetch_or(bit, std::memory_order_relaxed);
                if (!(previous & bit)) Trace("UI root activated", name, _ReturnAddress(), true);
                if ((previous | bit) == 3 || (g_MapBinding && bit == 2)) {
                    g_TraceActive.store(false, std::memory_order_release);
                    Log("Expected root traces captured; further UI name lookups stopped");
                }
            } else if (lookup < 12) {
                Trace("UI activation sample", name, _ReturnAddress(), lookup < 4);
            }
        } else if (lookup == 0) {
            Log("First UI activation name unreadable; bounded capture continues");
        }
        g_InTrace = false;
    }
}

void NativePages::Configure(Logger log, bool mapBinding, bool diagnostics, bool hotSwitch, bool sensitivity, const wchar_t* pluginDirectory) {
    g_Log = log;
    g_Diagnostics = diagnostics;
    g_MapBinding = mapBinding;
    g_HotSwitch = hotSwitch;
    g_Sensitivity = sensitivity;
    const auto mainDirectory = std::filesystem::path(pluginDirectory).parent_path() / L"FuFuPlugin";
    const auto config = mainDirectory / L"config.ini";
    wchar_t file[32768]{}, path[32768]{};
    GetPrivateProfileStringW(L"General", L"File", L"FufuLauncher.UnlockerIsland.dll",
        file, ARRAYSIZE(file), config.c_str());
    g_MainPath = (mainDirectory / file).wstring();
    DWORD length = GetLongPathNameW(g_MainPath.c_str(), path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path)) g_MainPath = path;
    length = GetShortPathNameW(g_MainPath.c_str(), path, ARRAYSIZE(path));
    g_ShortMainPath = length && length < ARRAYSIZE(path) ? path : g_MainPath;
}

void* NativePages::FindNative(const char* pattern) { return FindEntry(Signature(pattern)); }
bool NativePages::IsReady() { return g_Ready.load(std::memory_order_acquire); }

bool NativePages::MatchesMainPath(const wchar_t* path, unsigned characters) {
    if (!path) return false;
    const std::wstring_view name(path, characters);
    return SameName(name, g_MainPath) || SameName(name, g_ShortMainPath);
}

void NativePages::Prepare() {
    if ((!g_MapBinding && !g_Diagnostics && !g_HotSwitch && !g_Sensitivity) || g_PrepareAttempted) return;
    // The injector uses GetShortPathNameW. Long basename lookup misses the
    // resulting FUFULA~1.DLL loader entry; full path lookup identifies the file.
    const auto main = reinterpret_cast<unsigned char*>(GetModuleHandleW(g_MainPath.c_str()));
    if (!main) {
        if (!g_MainWaitLogged) {
            Log("Waiting for main plugin at configured full path; no basename lookup");
            g_MainWaitLogged = true;
        }
        return;
    }
    Log("Main plugin found by full path; resolving native entries once");
    g_PrepareAttempted = true;
    const ULONGLONG start = GetTickCount64();
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(main);
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(main + dos->e_lfanew);
    g_MainBegin = reinterpret_cast<std::uintptr_t>(main);
    g_MainEnd = g_MainBegin + nt->OptionalHeader.SizeOfImage;
    const auto game = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto gameDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto gameNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game + gameDos->e_lfanew);
    g_GameBegin = reinterpret_cast<std::uintptr_t>(game);
    g_GameEnd = g_GameBegin + gameNt->OptionalHeader.SizeOfImage;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const bool isOS = SameName(std::filesystem::path(executable).filename().wstring(), L"GenshinImpact.exe");
    // One primary signature, unique match required. No fixed new RVA or
    // fallback patterns. InnerDispatcher is not on the observed input route.
    if (g_MapBinding || g_Diagnostics) g_PageEntry = FindEntry(Signature(InputDispatchSignature));
    if (g_MapBinding) {
        const auto branch = static_cast<const unsigned char*>(FindEntry(Signature(MapBranchSignature)));
        if (branch) {
            // E8 at +20 is the state predicate used by the map action branch.
            const auto target = branch + 25 + *reinterpret_cast<const int*>(branch + 21);
            const auto address = reinterpret_cast<std::uintptr_t>(target);
            if (address >= g_GameBegin && address < g_GameEnd)
                g_AlternateMapState = reinterpret_cast<AlternateMapState>(const_cast<unsigned char*>(target));
        }
        if (!g_AlternateMapState) {
            Log("Map branch signature absent/ambiguous; binding disabled, no fallback scan");
            g_MapBinding = false;
        }
    }
    g_SetActiveEntry = game + std::strtoull(isOS ? Patterns::OS::SetActiveOffset :
        Patterns::CN::SetActiveOffset, nullptr, 16);
    // Both bodies are checked before hooking. SetActive's first six bytes may
    // already contain the main plugin's detour; the remaining body is unchanged.
    unsigned char activationCode[64]{};
    const unsigned char* verified = nullptr;
    if (((g_MapBinding || g_Diagnostics) && !g_PageEntry) || (g_Diagnostics && (!ReadCode(g_SetActiveEntry, activationCode) ||
        Signature("48 83 EC 20 0F B6 FA 48 8B D9 48 85 C9 74 ? E8 ? ? ? ? 48 85 C0 74 ?")
        .Find(activationCode + 6, 25, verified) != 1))) {
        Log("Input signature absent/ambiguous or existing SetActive body mismatched; no hook or fallback search");
        return;
    }
    // The existing main plugin deliberately takes the first GetName match.
    // Current CN has two equivalent getter wrappers: same success path/vtable
    // slot and string creation target, differing only in diagnostic line number.
    // Match the main plugin's selection; this is not an alternate-pattern retry.
    if (g_Diagnostics)
        g_GetName = reinterpret_cast<GetName>(FindEntry(Signature(Patterns::GetName), true));
    // Only the unchanged tail is scanned; entry bytes may already be detoured.
    const auto fovTail = static_cast<const unsigned char*>(FindEntry(Signature(Patterns::ChangeFOV, 6)));
    g_FovEntry = fovTail ? fovTail - 6 : nullptr;
    if ((g_Diagnostics && !g_GetName) || !g_FovEntry) {
        Log("Required native signatures absent or ambiguous; input remains unchanged (no fallback scan)");
        return;
    }
    char text[256]{};
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    sprintf_s(text, "Native trace entries resolved in %llu ms; GetName=0x%llX; InputDispatcher=0x%llX; SetActive=0x%llX",
        static_cast<unsigned long long>(GetTickCount64() - start),
        static_cast<unsigned long long>(g_GetName ? reinterpret_cast<std::uintptr_t>(g_GetName) - base : 0),
        static_cast<unsigned long long>(g_PageEntry ? reinterpret_cast<std::uintptr_t>(g_PageEntry) - base : 0),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_SetActiveEntry) - base));
    Log(text);
    if (g_HotSwitch) HotSwitch::Prepare(FindNative);
    if (g_Sensitivity) Sensitivity::Prepare(FindNative);
    g_Prepared.store(true, std::memory_order_release);
}

bool NativePages::RequestActivation() {
    return g_Prepared.load(std::memory_order_acquire) &&
        !g_InstallAttempted.load(std::memory_order_relaxed) && MainHooksActive() &&
        !g_ActivationQueued.exchange(true);
}

void NativePages::Install() {
    if (!g_MapBinding && !g_Diagnostics && !g_HotSwitch && !g_Sensitivity) return;
    if (!g_Prepared.load(std::memory_order_acquire) ||
        !MainHooksActive()) return;
    // A neutral startup coefficient needs no sensitivity hook. The first live
    // non-neutral value resolves/installs it once, after the main plugin is ready.
    if (g_Sensitivity) { Sensitivity::Prepare(FindNative); Sensitivity::Install(); }
    if (g_InstallAttempted.exchange(true, std::memory_order_relaxed)) return;
    if (!g_MapBinding && !g_Diagnostics) {
        g_Ready.store(true, std::memory_order_release);
        Log("Main initialization complete; no map/diagnostic hooks requested");
        return;
    }
    if (MH_CreateHook(g_PageEntry, reinterpret_cast<void*>(&HookDispatch),
        reinterpret_cast<void**>(&g_Original)) != MH_OK) {
        Log("Dispatcher trace hook creation failed; input unchanged");
        return;
    }
    if (g_Diagnostics && MH_CreateHook(g_SetActiveEntry, reinterpret_cast<void*>(&HookSetActive),
        reinterpret_cast<void**>(&g_OriginalSetActive)) != MH_OK) {
        MH_RemoveHook(g_PageEntry);
        Log("UI trace hook creation failed; input unchanged");
        return;
    }
    // Enable individually. Never enable hooks owned by another plugin.
    if (MH_EnableHook(g_PageEntry) != MH_OK || (g_Diagnostics && MH_EnableHook(g_SetActiveEntry) != MH_OK)) {
        MH_DisableHook(g_PageEntry);
        MH_DisableHook(g_SetActiveEntry);
        MH_RemoveHook(g_PageEntry);
        MH_RemoveHook(g_SetActiveEntry);
        Log("Native trace hook activation failed; input unchanged");
        return;
    }
    g_Ready.store(true, std::memory_order_release);
    Log(g_MapBinding ? "Native map action binding candidate ready after main initialization" :
        "Native action tracing active after main initialization; map binding disabled");
}

void NativePages::CancelMapRequest() { g_MapRequest.Cancel(); }

bool NativePages::RequestMap(bool enabled) {
    if (!g_Ready.load(std::memory_order_acquire) || !Focused()) {
        Log("Map button observed; native tracing not ready or game unfocused; input unchanged");
        return false;
    }
    if (g_Diagnostics && g_SeenPages.load(std::memory_order_relaxed) != 3) {
        g_TraceLookups.store(0, std::memory_order_relaxed);
        g_DispatchTraces.store(0, std::memory_order_relaxed);
        for (auto& seen : g_SeenActions) seen.store(0, std::memory_order_relaxed);
        g_TraceActive.store(true, std::memory_order_release);
    }
    if (!enabled || !g_MapBinding) return false;
    g_MapRequest.Arm(GetTickCount64());
    if (g_Diagnostics) Log("Map request armed; waiting for native chat action (500 ms)");
    return true;
}
