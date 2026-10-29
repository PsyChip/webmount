#pragma once
#include <string>

namespace webmount {
enum class ItemState { Listed, Queued, Fetching, Cached, Reading, Error };
void StartConsole(const std::wstring& root);
void StopConsole();
void ConsoleLog(const std::wstring& text);
void SetItem(const std::wstring& key, const std::wstring& name, ItemState state, const std::wstring& detail = L"");
}
