#pragma once
#include <atomic>
#include <cstdint>
#include <string_view>

enum class PageKind { Other, Chat, Map };
inline PageKind ClassifyPage(std::wstring_view name) {
    if (name == L"ChatPage" || name == L"InLevelChatPage" || name == L"ChatDialog") return PageKind::Chat;
    if (name == L"InLevelMapPage") return PageKind::Map;
    return PageKind::Other;
}

// A configured button edge authorizes one native chat-to-map action conversion.
// The input and native action callback may be on different threads.
class PageRedirectRequest {
public:
    static constexpr std::uint64_t LifetimeMs = 500;

    void Arm(std::uint64_t now) { timestamp_.store(now ? now : 1, std::memory_order_release); }
    void Cancel() { timestamp_.store(0, std::memory_order_release); }
    std::uint64_t Peek(std::uint64_t now) {
        auto token = timestamp_.load(std::memory_order_acquire);
        if (token && (now < token || now - token > LifetimeMs)) {
            timestamp_.compare_exchange_strong(token, 0, std::memory_order_acq_rel);
            return 0;
        }
        return token;
    }
    bool Consume(std::uint64_t token) {
        return token && timestamp_.compare_exchange_strong(token, 0, std::memory_order_acq_rel);
    }

private:
    std::atomic<std::uint64_t> timestamp_{ 0 };
};
