#pragma once
#include <array>
#include <cstddef>

// Pure input state machine. The caller serializes access; no OS/game dependency.
class ButtonRouter {
public:
    using RequestAction = bool (*)(void* context);

    bool Sample(std::size_t controller, bool down, bool enabled,
        RequestAction request, void* context) {
        if (controller >= states_.size()) return false;
        auto& state = states_[controller];
        if (!down) {
            state = {};
            return false;
        }
        if (state.waitRelease) return false;
        if (!state.down) {
            state.down = true;
            state.captured = enabled && request && request(context);
        }
        // Preserve capture through release, even if focus/config changes mid-press.
        // An unavailable action never steals the original chat action.
        return state.captured;
    }

    void Disconnect(std::size_t controller) {
        if (controller < states_.size()) states_[controller] = {};
    }

    void Rebind() {
        // Do not turn a key already held during a config change into a new press.
        for (auto& state : states_) state = {false, false, true};
    }

private:
    struct State { bool down = false; bool captured = false; bool waitRelease = false; };
    std::array<State, 4> states_{};
};
