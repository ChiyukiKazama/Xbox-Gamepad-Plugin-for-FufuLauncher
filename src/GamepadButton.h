#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <string_view>

inline bool SameName(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto x = a[i] >= L'a' && a[i] <= L'z' ? a[i] - L'a' + L'A' : a[i];
        const auto y = b[i] >= L'a' && b[i] <= L'z' ? b[i] - L'a' + L'A' : b[i];
        if (x != y) return false;
    }
    return true;
}

// Standard XInput buttons only. Logo/back paddles are not exposed by this API.
class GamepadButton {
public:
    static GamepadButton Parse(std::wstring_view name) {
        // Config text is parsed once at startup, not in the input hook.
        const auto isSpace = [](wchar_t c) {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        };
        while (!name.empty() && isSpace(name.front())) name.remove_prefix(1);
        while (!name.empty() && isSpace(name.back())) name.remove_suffix(1);
        struct Definition { const wchar_t* name; WORD mask; };
        constexpr Definition buttons[] = {
            {L"View", XINPUT_GAMEPAD_BACK}, {L"Back", XINPUT_GAMEPAD_BACK},
            {L"Menu", XINPUT_GAMEPAD_START}, {L"Start", XINPUT_GAMEPAD_START},
            {L"A", XINPUT_GAMEPAD_A}, {L"B", XINPUT_GAMEPAD_B},
            {L"X", XINPUT_GAMEPAD_X}, {L"Y", XINPUT_GAMEPAD_Y},
            {L"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {L"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
            {L"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {L"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
            {L"DPadUp", XINPUT_GAMEPAD_DPAD_UP}, {L"DPadDown", XINPUT_GAMEPAD_DPAD_DOWN},
            {L"DPadLeft", XINPUT_GAMEPAD_DPAD_LEFT}, {L"DPadRight", XINPUT_GAMEPAD_DPAD_RIGHT},
            {L"Up", XINPUT_GAMEPAD_DPAD_UP}, {L"Down", XINPUT_GAMEPAD_DPAD_DOWN},
            {L"Left", XINPUT_GAMEPAD_DPAD_LEFT}, {L"Right", XINPUT_GAMEPAD_DPAD_RIGHT}
        };
        for (const auto& button : buttons) if (SameName(name, button.name))
            return GamepadButton(button.mask, 0);
        if (SameName(name, L"LT")) return GamepadButton(0, 1);
        if (SameName(name, L"RT")) return GamepadButton(0, 2);
        return GamepadButton(0, 0); // None or invalid: no silent default binding.
    }
    bool IsValid() const { return mask_ || trigger_; }
    bool SameBinding(const GamepadButton& other) const {
        return mask_ == other.mask_ && trigger_ == other.trigger_;
    }
    bool IsDown(const XINPUT_GAMEPAD& pad) const {
        if (trigger_ == 1) return pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        if (trigger_ == 2) return pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        return mask_ && (pad.wButtons & mask_) != 0;
    }
    bool CanStart(const XINPUT_GAMEPAD& pad) const {
        // Another physical View hold cannot generate a new carrier edge.
        return mask_ == XINPUT_GAMEPAD_BACK || !(pad.wButtons & XINPUT_GAMEPAD_BACK);
    }
    void EmitCarrier(XINPUT_GAMEPAD& pad) const {
        pad.wButtons &= static_cast<WORD>(~mask_);
        if (trigger_ == 1) pad.bLeftTrigger = 0;
        if (trigger_ == 2) pad.bRightTrigger = 0;
        pad.wButtons |= XINPUT_GAMEPAD_BACK;
    }
private:
    GamepadButton(WORD mask, unsigned trigger) : mask_(mask), trigger_(trigger) {}
    WORD mask_;
    unsigned trigger_;
};
