#pragma once
#include <Windows.h>
#include <Xinput.h>

namespace HotSwitch {
    using Logger = void (*)(const char*);
    using Resolver = void* (*)(const char*);
    void Configure(Logger log, bool enabled, bool diagnostics);
    void Prepare(Resolver resolve); // Startup-only, unique native matches required.
    void Install(); // Worker, after main-plugin hooks finish initialization.
    bool IsEnabled();
    bool IsReady();
    void ObserveGamepad(unsigned index, const XINPUT_GAMEPAD& pad); // Existing input mutex held.
    void Disconnect(unsigned index);
}
