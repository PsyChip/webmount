#pragma once
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
using PNTSTATUS = NTSTATUS*;
#include <winfsp/winfsp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "consoleView.h"

namespace webmount {
struct Result {
    DWORD code = ERROR_SUCCESS;
    std::wstring message;
    explicit operator bool() const { return code == ERROR_SUCCESS; }
};
inline Result Fail(DWORD code, std::wstring message) {
    return {code ? code : ERROR_GEN_FAILURE, std::move(message)};
}
inline Result LastError(std::wstring message) {
    return Fail(GetLastError(), std::move(message));
}
class Handle {
public:
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE input) : value(input) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept { Reset(other.Release()); return *this; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    HANDLE Release() { auto old = value; value = nullptr; return old; }
    void Reset(HANDLE input = nullptr) { if (*this) CloseHandle(value); value = input; }
};
inline std::atomic_bool quiet = false;
inline std::mutex logMutex;
inline void Log(const std::wstring& text) {
    if (quiet.load()) return;
    std::lock_guard lock(logMutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"[%02u:%02u:%02u] ", time.wHour, time.wMinute, time.wSecond);
    ConsoleLog(std::wstring(stamp) + text);
}
inline void LogError(const Result& result) {
    Log(result.message + L" [error " + std::to_wstring(result.code) + L"]");
}
inline std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}
inline std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
inline std::wstring Wide(std::string_view value) {
    if (value.empty() || value.size() > INT_MAX) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
inline std::string Utf8(std::wstring_view value) {
    if (value.empty() || value.size() > INT_MAX) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
inline uint64_t NowFileTime() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
inline std::wstring ExePath() {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size);
    return path;
}
inline bool ParseNumber(std::wstring_view value, uint64_t& number) {
    if (value.empty()) return false;
    number = 0;
    for (wchar_t digit : value) {
        if (digit < L'0' || digit > L'9' || number > (UINT64_MAX - (digit - L'0')) / 10) return false;
        number = number * 10 + digit - L'0';
    }
    return true;
}
inline uint64_t HashName(std::wstring_view text) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : text) { hash ^= static_cast<uint16_t>(ch); hash *= 1099511628211ull; }
    return hash;
}
inline int NameCompare(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) - CSTR_EQUAL;
}
}
