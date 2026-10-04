#pragma once
#include <Windows.h>
#include <array>
#include <filesystem>
#include <string_view>
#include "GamepadButton.h"

struct LiveSettings {
    bool mapEnabled = false;
    GamepadButton mapButton = GamepadButton::Parse(L"View");
    float sensitivity = 1.0f;
};

// Only these three settings are reloadable. Output is unchanged on failure.
bool ParseLiveSettings(std::string_view text, LiveSettings& output, bool requireComplete = true);
bool ReadLiveSettings(const std::filesystem::path& path, LiveSettings& output, bool requireComplete = true);

class ConfigWatcher {
public:
    ConfigWatcher() = default;
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;
    ~ConfigWatcher();
    bool Start(const std::filesystem::path& directory);
    HANDLE Event() const { return overlapped_.hEvent; }
    bool Consume(); // Filters config.ini; re-arms before returning.

private:
    bool Arm();
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
    alignas(DWORD) std::array<unsigned char, 4096> buffer_{};
    bool pending_ = false;
};
