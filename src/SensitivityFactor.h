#pragma once
#include <cmath>

inline float ApplySensitivityFactor(float original, float factor, int inputMode, bool savingSettings) {
    if (inputMode != 3 || savingSettings || factor == 1.0f) return original;
    const float result = original * factor;
    return std::isfinite(result) ? result : original;
}

inline bool ValidSensitivityFactor(float value) {
    return std::isfinite(value) && value > 0.0f;
}
