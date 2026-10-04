#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "LiveConfig.h"
#include "SensitivityFactor.h"
#include <charconv>
#include <cstring>
#include <string>

namespace {
    std::string_view Trim(std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }
    bool Same(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto c = a[i] >= 'A' && a[i] <= 'Z' ? a[i] - 'A' + 'a' : a[i];
            if (c != b[i]) return false;
        }
        return true;
    }
}

bool ParseLiveSettings(std::string_view text, LiveSettings& output, bool requireComplete) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    LiveSettings next;
    next.mapEnabled = true;
    std::string_view section;
    unsigned seen = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string_view::npos || !Same(Trim(line.substr(0, equal)), "value")) continue;
        const auto value = Trim(line.substr(equal + 1));
        if (Same(section, "mapbinding") || (!(seen & 1) && Same(section, "viewopensmap"))) {
            if (value == "1" || Same(value, "true")) next.mapEnabled = true;
            else if (value == "0" || Same(value, "false")) next.mapEnabled = false;
            else return false;
            seen |= 1;
        } else if (Same(section, "mapbutton")) {
            next.mapButton = GamepadButton::Parse(std::wstring(value.begin(), value.end()));
            seen |= 2;
        } else if (Same(section, "gamepadsensitivitymultiplier")) {
            auto number = value;
            if (!number.empty() && number.front() == '+') number.remove_prefix(1);
            if (number.empty()) return false;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), next.sensitivity);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
                !ValidSensitivityFactor(next.sensitivity)) return false;
            seen |= 4;
        }
    }
    if (requireComplete && seen != 7) return false;
    output = next;
    return true;
}

bool ReadLiveSettings(const std::filesystem::path& path, LiveSettings& output, bool requireComplete) {
    // Deny concurrent writers: never apply an INI snapshot mid-save.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 128 * 1024;
    std::string bytes(validSize ? static_cast<std::size_t>(size.QuadPart) : 0, '\0');
    DWORD read = 0;
    const bool complete = validSize && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
        && read == bytes.size();
    CloseHandle(file);
    if (!complete) return false;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        if ((bytes.size() - 2) % sizeof(wchar_t)) return false;
        std::wstring wide((bytes.size() - 2) / sizeof(wchar_t), L'\0');
        std::memcpy(wide.data(), bytes.data() + 2, bytes.size() - 2);
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (length <= 0) return false;
        bytes.resize(length);
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            bytes.data(), length, nullptr, nullptr);
    }
    return ParseLiveSettings(bytes, output, requireComplete);
}

bool ConfigWatcher::Start(const std::filesystem::path& directory) {
    directory_ = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory_ == INVALID_HANDLE_VALUE) return false;
    overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped_.hEvent && Arm()) return true;
    CloseHandle(directory_);
    directory_ = INVALID_HANDLE_VALUE;
    if (overlapped_.hEvent) CloseHandle(overlapped_.hEvent);
    overlapped_.hEvent = nullptr;
    return false;
}

bool ConfigWatcher::Arm() {
    const HANDLE event = overlapped_.hEvent;
    overlapped_ = {};
    overlapped_.hEvent = event;
    ResetEvent(overlapped_.hEvent);
    pending_ = ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME,
        nullptr, &overlapped_, nullptr) != FALSE;
    return pending_;
}

bool ConfigWatcher::Consume() {
    DWORD size = 0;
    const bool completed = GetOverlappedResult(directory_, &overlapped_, &size, FALSE) != FALSE;
    const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    pending_ = false;
    bool changed = (completed && size == 0) || error == ERROR_NOTIFY_ENUM_DIR;
    // Overflow: validate one snapshot, not a scan/retry loop.
    for (DWORD offset = 0; completed && offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= size;) {
        const auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data() + offset);
        if (info->FileNameLength % sizeof(wchar_t) ||
            info->FileNameLength > size - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) break;
        if (SameName(std::wstring_view(info->FileName, info->FileNameLength / sizeof(wchar_t)), L"config.ini"))
            changed = true;
        if (!info->NextEntryOffset || info->NextEntryOffset > size - offset) break;
        offset += info->NextEntryOffset;
    }
    Arm();
    return changed;
}

ConfigWatcher::~ConfigWatcher() {
    if (directory_ != INVALID_HANDLE_VALUE) {
        if (pending_) {
            CancelIoEx(directory_, &overlapped_);
            DWORD ignored = 0;
            GetOverlappedResult(directory_, &overlapped_, &ignored, TRUE);
        }
        CloseHandle(directory_);
    }
    if (overlapped_.hEvent) CloseHandle(overlapped_.hEvent);
}
