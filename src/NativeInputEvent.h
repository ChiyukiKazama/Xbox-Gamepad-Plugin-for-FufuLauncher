#pragma once
#include <cstddef>
#include <cstdint>

// Layout established by the actual virtual dispatcher caller: two 16-byte
// copies, with the action at +4. Other fields are intentionally opaque.
struct NativeInputEvent {
    std::uint32_t opaque0;
    std::uint32_t action;
    std::uint64_t opaque1;
    std::uint64_t opaque2;
    std::uint64_t opaque3;

    static constexpr std::uint32_t ChatAction = 113;
    static constexpr std::uint32_t MapAction = 3;
    static constexpr std::uint32_t AlternateMapAction = 272;

    NativeInputEvent ForMap(bool alternate) const {
        auto mapped = *this;
        mapped.action = alternate ? AlternateMapAction : MapAction;
        return mapped;
    }
};
static_assert(sizeof(NativeInputEvent) == 32 && offsetof(NativeInputEvent, action) == 4);
