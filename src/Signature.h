#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

// Startup-only matching. Use the longest fixed span as a search anchor;
// do not scan byte-by-byte across the image for every wildcard pattern.
class Signature {
public:
    explicit Signature(const char* text, std::size_t skip = 0) {
        std::istringstream input(text);
        std::string token;
        while (input >> token) {
            bytes_.push_back(token.find('?') != std::string::npos
                ? -1 : std::stoi(token, nullptr, 16));
        }
        if (skip >= bytes_.size()) { bytes_.clear(); return; }
        bytes_.erase(bytes_.begin(), bytes_.begin() + skip);
        std::size_t best = 0;
        for (std::size_t start = 0; start < bytes_.size();) {
            if (bytes_[start] < 0) { ++start; continue; }
            std::size_t end = start;
            while (end < bytes_.size() && bytes_[end] >= 0) ++end;
            if (end - start > best) { offset_ = start; best = end - start; }
            start = end;
        }
        for (std::size_t i = 0; i < best; ++i)
            anchor_.push_back(static_cast<std::uint8_t>(bytes_[offset_ + i]));
    }

    // Returns 0, 1, or 2 (ambiguous). The caller accumulates across regions.
    // firstMatchOnly explicitly preserves an existing API's first-match policy.
    unsigned Find(const std::uint8_t* begin, std::size_t size,
        const std::uint8_t*& result, bool firstMatchOnly = false) const {
        result = nullptr;
        if (anchor_.empty() || size < bytes_.size()) return 0;
        const auto searcher = std::boyer_moore_horspool_searcher(anchor_.begin(), anchor_.end());
        const auto end = begin + size;
        auto cursor = begin;
        unsigned count = 0;
        while (cursor < end) {
            const auto hit = std::search(cursor, end, searcher);
            if (hit == end) break;
            cursor = hit + 1;
            if (static_cast<std::size_t>(hit - begin) < offset_) continue;
            const auto candidate = hit - offset_;
            if (static_cast<std::size_t>(end - candidate) < bytes_.size()) continue;
            bool matches = true;
            for (std::size_t i = 0; i < bytes_.size(); ++i) {
                if (bytes_[i] >= 0 && candidate[i] != bytes_[i]) { matches = false; break; }
            }
            if (!matches) continue;
            result = candidate;
            if (++count == (firstMatchOnly ? 1u : 2u)) return count;
        }
        return count;
    }

private:
    std::vector<int> bytes_;
    std::vector<std::uint8_t> anchor_;
    std::size_t offset_ = 0;
};
