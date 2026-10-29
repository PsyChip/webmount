#include "startupMount.h"
#include "mountControl.h"
#include <shellapi.h>
#include <sddl.h>

namespace webmount {
namespace {
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};
std::wstring ValueName(wchar_t letter) { return std::wstring(L"WebMount-") + letter; }
Result ReadCommand(wchar_t letter, std::wstring& command) {
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.value);
    if (status != ERROR_SUCCESS) return Fail(status, L"Cannot read WebMount startup registrations");
    auto name = ValueName(letter);
    DWORD type = 0, bytes = 0;
    status = RegQueryValueExW(key.value, name.c_str(), nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS) return Fail(status, L"Cannot read the saved startup mount");
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return Fail(ERROR_INVALID_DATA, L"Invalid startup command");
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    status = RegQueryValueExW(key.value, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (status != ERROR_SUCCESS) return Fail(status, L"Cannot read the saved startup command");
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t)) return Fail(ERROR_INVALID_DATA, L"Invalid startup command");
    buffer.resize(bytes / sizeof(wchar_t));
    if (buffer.back() != L'\0') return Fail(ERROR_INVALID_DATA, L"Unterminated startup command");
    buffer.pop_back();
    if (buffer.find(L'\0') != std::wstring::npos) return Fail(ERROR_INVALID_DATA, L"Invalid startup command");
    command = std::move(buffer);
    return {};
}
}
std::wstring QuoteArgument(const std::wstring& input) {
    std::wstring output = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : input) {
        if (ch == L'\\') { ++slashes; continue; }
        output.append(ch == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        output += ch;
    }
    output.append(slashes * 2, L'\\');
    return output + L'\"';
}
Result SaveStartup(wchar_t letter, const std::wstring& url) {
    if (letter < L'A' || letter > L'Z') return Fail(ERROR_INVALID_PARAMETER, L"Invalid persistent drive letter");
    auto executable = ExePath();
    if (executable.empty()) return LastError(L"Cannot locate webmount.exe for startup registration");
    auto command = QuoteArgument(executable) + L" " + letter + L": " + QuoteArgument(url) + L" --restore";
    if (command.size() >= 260) return Fail(ERROR_FILENAME_EXCED_RANGE, L"Startup command exceeds the Windows Run limit; use a shorter executable path or URL");
    RegistryKey key;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key.value, nullptr);
    if (status != ERROR_SUCCESS) return Fail(status, L"Cannot create the per-user startup registration");
    auto name = ValueName(letter);
    status = RegSetValueExW(key.value, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) return Fail(status, L"Cannot save the persistent mount");
    Log(std::wstring(L"Registered ") + letter + L": for restoration at sign-in");
    return {};
}
Result CheckStartup(wchar_t letter, const std::wstring& url) {
    std::wstring command;
    auto result = ReadCommand(letter, command);
    if (!result) {
        if (result.code == ERROR_FILE_NOT_FOUND || result.code == ERROR_PATH_NOT_FOUND) return Fail(ERROR_OPERATION_ABORTED, L"Startup registration was removed");
        return result;
    }
    int count = 0;
    PWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
    if (!arguments) return LastError(L"Cannot parse startup registration");
    std::wstring drive{letter, L':'};
    bool matches = count == 4 && NameCompare(arguments[0], ExePath()) == 0 && NameCompare(arguments[1], drive) == 0 && arguments[2] == url && std::wstring_view(arguments[3]) == L"--restore";
    LocalFree(arguments);
    return matches ? Result{} : Fail(ERROR_OPERATION_ABORTED, L"Startup registration was changed or belongs to a different executable");
}
Result RemoveStartup(wchar_t letter, size_t& removed) {
    removed = 0;
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status != ERROR_SUCCESS) return Fail(status, L"Cannot remove startup registrations");
    Result failure;
    for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
        if (letter && drive != letter) continue;
        auto name = ValueName(drive);
        status = RegDeleteValueW(key.value, name.c_str());
        switch (status) {
        case ERROR_SUCCESS:
            ++removed;
            Log(std::wstring(L"Removed startup registration for ") + drive + L":");
            break;
        case ERROR_FILE_NOT_FOUND: break;
        default:
            if (failure) failure = Fail(status, std::wstring(L"Cannot remove startup registration for ") + drive + L":");
            break;
        }
    }
    return failure;
}
Result LockStartup(Handle& lock) {
    std::wstring sid;
    auto result = UserSid(sid);
    if (!result) return result;
    auto acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return LastError(L"Cannot prepare startup registration lock");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    auto name = L"Global\\webmount-startup-" + sid;
    lock.Reset(CreateMutexExW(&attributes, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
    DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!lock) return Fail(error, L"Cannot open startup registration lock");
    DWORD wait = WaitForSingleObject(lock.value, 60000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return {};
    lock.Reset();
    return Fail(ERROR_TIMEOUT, L"Another webmount instance is updating startup registrations");
}
void UnlockStartup(Handle& lock) {
    if (lock) { ReleaseMutex(lock.value); lock.Reset(); }
}
void HideStartupConsole() {
    DWORD processes[2]{};
    if (GetConsoleProcessList(processes, 2) == 1 && processes[0] == GetCurrentProcessId()) FreeConsole();
}
}
