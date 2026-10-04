#pragma once
namespace NativePages {
    using Logger = void (*)(const char*);
    void Configure(Logger log, bool mapBinding, bool diagnostics, bool hotSwitch, bool sensitivity, const wchar_t* pluginDirectory);
    void* FindNative(const char* pattern); // Worker-only unique-match resolver.
    bool IsReady();
    bool MatchesMainPath(const wchar_t* path, unsigned characters);
    void Prepare(); // Once, after the existing main-plugin module is loaded.
    bool RequestActivation(); // Signals the worker only once when main hooks are active.
    void Install(); // Worker only; never installs hooks inside XInput or a loader callback.
    bool RequestMap(bool enabled); // Arms one native action conversion; false leaves input unchanged.
    void CancelMapRequest();
}
