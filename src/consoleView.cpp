#include "app.h"

namespace webmount {
namespace {
class ConsoleView {
public:
    void Start(const std::wstring& base) {
        std::lock_guard lock(mutex);
        root = base;
    }
    void Stop() {
        std::lock_guard lock(mutex);
        std::wcerr.flush();
        root.clear();
    }
    void Message(const std::wstring& text) {
        std::lock_guard lock(mutex);
        Write(text);
    }
    void Item(const std::wstring& key, const std::wstring& name, ItemState state, const std::wstring& detail) {
        std::lock_guard lock(mutex);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t stamp[32]{};
        swprintf_s(stamp, L"[%02u:%02u:%02u] ", time.wHour, time.wMinute, time.wSecond);
        auto line = std::wstring(stamp) + L"[" + StateName(state) + L"] " + (name.empty() ? Label(key) : name);
        if (!detail.empty()) line += L"  " + detail;
        Write(std::move(line));
    }
private:
    static const wchar_t* StateName(ItemState state) {
        switch (state) {
        case ItemState::Listed: return L"listed";
        case ItemState::Queued: return L"queued";
        case ItemState::Fetching: return L"fetching";
        case ItemState::Cached: return L"cached";
        case ItemState::Reading: return L"reading";
        case ItemState::Error: return L"ERROR";
        }
        return L"";
    }
    std::wstring Label(const std::wstring& key) const {
        if (key == root) return L"/";
        return !root.empty() && key.starts_with(root) ? key.substr(root.size()) : key;
    }
    void Write(std::wstring text) {
        for (auto& ch : text) if (ch < 32 || ch == 127) ch = L' ';
        std::wcerr << text << std::endl;
    }
    std::mutex mutex;
    std::wstring root;
};
ConsoleView& View() { static ConsoleView view; return view; }
}
void StartConsole(const std::wstring& root) { if (!quiet.load()) View().Start(root); }
void StopConsole() { View().Stop(); }
void ConsoleLog(const std::wstring& text) { if (!quiet.load()) View().Message(text); }
void SetItem(const std::wstring& key, const std::wstring& name, ItemState state, const std::wstring& detail) {
    if (!quiet.load()) View().Item(key, name, state, detail);
}
}
