#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

enum class InputSource : std::uint64_t { None = 0, Keyboard = 1, Mouse = 2, Gamepad = 3 };

// Producers only report real activity. The game thread consumes the newest
// token; controller connection, idle polling and releases are not activity.
class DeviceActivity {
public:
    void Notify(InputSource source, std::uint64_t now) {
        if (source == InputSource::None) return;
        const auto token = ((now ? now : 1) << 2) | static_cast<std::uint64_t>(source);
        auto previous = latest_.load(std::memory_order_relaxed);
        while (token > previous && !latest_.compare_exchange_weak(previous, token,
            std::memory_order_release, std::memory_order_relaxed)) {}
    }
    std::uint64_t Peek(std::uint64_t now) const {
        const auto token = latest_.load(std::memory_order_acquire);
        const auto time = token >> 2;
        return time && now >= time && now - time <= 1000 ? token : 0;
    }
    void Clear() { latest_.store(0, std::memory_order_release); }
    static InputSource Source(std::uint64_t token) { return static_cast<InputSource>(token & 3); }
    static int NativeMode(std::uint64_t token) { return Source(token) == InputSource::Gamepad ? 3 : 2; }
private:
    std::atomic<std::uint64_t> latest_{ 0 };
};

// Called under the plugin's existing input mutex, including its low-rate poll.
class GamepadActivity {
public:
    bool Sample(unsigned index, const XINPUT_GAMEPAD& pad) {
        if (index >= pads_.size()) return false;
        auto& sample = pads_[index];
        if (!sample.connected) { sample = { true, pad }; return false; }
        const auto& old = sample.pad;
        const bool active = (pad.wButtons & ~old.wButtons) != 0 ||
            Trigger(pad.bLeftTrigger, old.bLeftTrigger) || Trigger(pad.bRightTrigger, old.bRightTrigger) ||
            Stick(pad.sThumbLX, pad.sThumbLY, old.sThumbLX, old.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
            Stick(pad.sThumbRX, pad.sThumbRY, old.sThumbRX, old.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        sample.pad = pad;
        return active;
    }
    void Disconnect(unsigned index) { if (index < pads_.size()) pads_[index] = {}; }
private:
    struct SampleState { bool connected = false; XINPUT_GAMEPAD pad{}; };
    std::array<SampleState, 4> pads_{};
    static bool Trigger(int value, int old) {
        return value > XINPUT_GAMEPAD_TRIGGER_THRESHOLD &&
            (old <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD || std::abs(value - old) >= 15);
    }
    static bool Stick(int x, int y, int oldX, int oldY, int deadzone) {
        const auto squared = [](int a, int b) { return std::int64_t(a) * a + std::int64_t(b) * b; };
        return squared(x, y) > std::int64_t(deadzone) * deadzone &&
            (squared(oldX, oldY) <= std::int64_t(deadzone) * deadzone ||
                std::abs(x - oldX) >= 1500 || std::abs(y - oldY) >= 1500);
    }
};
