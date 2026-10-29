#pragma once
#include "app.h"

namespace webmount {
std::wstring QuoteArgument(const std::wstring& input);
Result SaveStartup(wchar_t letter, const std::wstring& url);
Result CheckStartup(wchar_t letter, const std::wstring& url);
Result RemoveStartup(wchar_t letter, size_t& removed);
Result LockStartup(Handle& lock);
void UnlockStartup(Handle& lock);
void HideStartupConsole();
}
