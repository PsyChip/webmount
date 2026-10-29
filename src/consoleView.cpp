#include "app.h"
#include <condition_variable>
#include <set>

namespace webmount {
namespace {
struct ConsoleRow {
    std::wstring key;
    std::wstring name;
    std::wstring detail;
    std::wstring rendered;
    ItemState state = ItemState::Listed;
    bool event = false;
};
class ConsoleView {
public:
    ~ConsoleView() { Stop(); }
    void Start(const std::wstring& base) {
        std::lock_guard lock(mutex);
        root = base;
        if (started) return;
        started = true;
        stopping = false;
        output = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        CONSOLE_SCREEN_BUFFER_INFO info{};
        interactive = GetConsoleMode(output, &mode) && GetConsoleScreenBufferInfo(output, &info);
        if (!interactive) return;
        origin = info.dwCursorPosition.Y;
        attributes = info.wAttributes;
        savedCursor = GetConsoleCursorInfo(output, &cursor) != FALSE;
        if (savedCursor) {
            auto hidden = cursor;
            hidden.bVisible = FALSE;
            SetConsoleCursorInfo(output, &hidden);
        }
        worker = std::thread(&ConsoleView::Run, this);
    }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        if (interactive && savedCursor) SetConsoleCursorInfo(output, &cursor);
        interactive = false;
        started = false;
    }
    void Message(const std::wstring& text) {
        std::lock_guard lock(mutex);
        if (!interactive) { std::wcerr << text << std::endl; return; }
        ConsoleRow row;
        row.name = text;
        row.event = true;
        rows.push_back(std::move(row));
        Trim();
    }
    void Item(const std::wstring& key, const std::wstring& name, ItemState state, const std::wstring& detail) {
        std::lock_guard lock(mutex);
        if (!interactive) {
            bool first = printed.insert(key).second;
            if (first || state == ItemState::Error) {
                auto label = name.empty() ? Label(key) : name;
                auto line = label + L"  " + StateName(state);
                if (!detail.empty()) line += L"  " + detail;
                if (first || lastError[key] != line) std::wcerr << line << std::endl;
                if (state == ItemState::Error) lastError[key] = std::move(line);
            }
            return;
        }
        auto found = indices.find(key);
        if (found == indices.end()) {
            ConsoleRow row;
            row.key = key;
            row.name = name.empty() ? Label(key) : name;
            row.state = state;
            row.detail = detail;
            indices[key] = rows.size();
            rows.push_back(std::move(row));
            Trim();
            return;
        }
        auto& row = rows[found->second];
        if (!name.empty()) row.name = name;
        if (state == ItemState::Listed || (state == ItemState::Queued && row.state == ItemState::Cached)) return;
        row.state = state;
        row.detail = detail;
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
    void Trim() {
        if (rows.size() <= 4096) return;
        rows.erase(rows.begin(), rows.begin() + 1024);
        indices.clear();
        for (size_t index = 0; index < rows.size(); ++index) {
            rows[index].rendered.clear();
            if (!rows[index].event) indices[rows[index].key] = index;
        }
    }
    std::wstring Text(const ConsoleRow& row, size_t width) const {
        auto text = row.event ? row.name : std::wstring(L"[") + StateName(row.state) + L"] " + row.name;
        if (!row.event && !row.detail.empty()) text += L"  " + row.detail;
        for (auto& ch : text) if (ch < 32 || ch == 127) ch = L' ';
        if (text.size() > width) {
            text.resize(width);
            if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.back() = L' ';
        }
        text.resize(width, L' ');
        return text;
    }
    WORD Color(const ConsoleRow& row) const {
        if (row.event) return attributes;
        switch (row.state) {
        case ItemState::Error: return (attributes & 0xfff0) | FOREGROUND_RED | FOREGROUND_INTENSITY;
        case ItemState::Fetching: case ItemState::Reading: return (attributes & 0xfff0) | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        default: return attributes;
        }
    }
    void Draw() {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info) || info.dwSize.X < 2) return;
        size_t count = std::max(rows.size(), drawnRows);
        if (!count) return;
        int needed = static_cast<int>(origin) + static_cast<int>(count) + 1;
        if (needed > 32760) {
            origin = 0;
            needed = static_cast<int>(count) + 1;
            for (auto& row : rows) row.rendered.clear();
        }
        if (needed > info.dwSize.Y) {
            COORD size{info.dwSize.X, static_cast<SHORT>(needed)};
            if (!SetConsoleScreenBufferSize(output, size)) {
                origin = 0;
                count = std::min(count, static_cast<size_t>(info.dwSize.Y - 1));
            }
        }
        size_t width = static_cast<size_t>(info.dwSize.X);
        size_t first = rows.size() > count ? rows.size() - count : 0;
        if (first != firstRow || origin != lastOrigin) {
            for (auto& row : rows) row.rendered.clear();
            firstRow = first;
            lastOrigin = origin;
        }
        size_t start = count;
        size_t end = 0;
        std::vector<std::wstring> lines(count);
        for (size_t index = 0; index < count; ++index) {
            size_t source = first + index;
            lines[index] = source < rows.size() ? Text(rows[source], width - 1) + L" " : std::wstring(width, L' ');
            if (source >= rows.size() || rows[source].rendered != lines[index]) {
                start = std::min(start, index);
                end = index + 1;
            }
        }
        if (start < end) {
            std::vector<CHAR_INFO> cells((end - start) * width);
            for (size_t index = start; index < end; ++index) {
                size_t source = first + index;
                WORD color = source < rows.size() ? Color(rows[source]) : attributes;
                for (size_t column = 0; column < width; ++column) {
                    auto& cell = cells[(index - start) * width + column];
                    cell.Char.UnicodeChar = lines[index][column];
                    cell.Attributes = color;
                }
            }
            COORD size{static_cast<SHORT>(width), static_cast<SHORT>(end - start)};
            SMALL_RECT area{0, static_cast<SHORT>(origin + start), static_cast<SHORT>(width - 1), static_cast<SHORT>(origin + end - 1)};
            if (WriteConsoleOutputW(output, cells.data(), size, COORD{0, 0}, &area)) {
                for (size_t index = start; index < end; ++index) if (first + index < rows.size()) rows[first + index].rendered = std::move(lines[index]);
            }
        }
        if (drawnRows != rows.size()) {
            SetConsoleCursorPosition(output, COORD{0, static_cast<SHORT>(origin + std::min(count, rows.size()))});
            drawnRows = rows.size();
        }
    }
    void Run() {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping; });
            Draw();
            if (stopping) return;
        }
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::vector<ConsoleRow> rows;
    std::map<std::wstring, size_t> indices;
    std::set<std::wstring> printed;
    std::map<std::wstring, std::wstring> lastError;
    std::wstring root;
    HANDLE output = nullptr;
    CONSOLE_CURSOR_INFO cursor{};
    SHORT origin = 0;
    WORD attributes = 7;
    size_t drawnRows = 0;
    size_t firstRow = 0;
    SHORT lastOrigin = -1;
    bool savedCursor = false;
    bool started = false;
    bool interactive = false;
    bool stopping = false;
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
