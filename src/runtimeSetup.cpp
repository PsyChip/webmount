#include "runtimeSetup.h"
#include "mountControl.h"
#include <shlobj.h>
#include <shellapi.h>
#include <sddl.h>
#include <aclapi.h>
#include <wintrust.h>
#include <softpub.h>

namespace webmount {
namespace {
constexpr wchar_t serviceName[] = L"WinFsp+amount-2.1.25156";
HMODULE runtimeModule = nullptr;
struct Service {
    SC_HANDLE value = nullptr;
    explicit Service(SC_HANDLE input) : value(input) {}
    ~Service() { if (value) CloseServiceHandle(value); }
};
std::wstring RootPath() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DEFAULT, nullptr, &folder))) return {};
    std::wstring path(folder);
    CoTaskMemFree(folder);
    return path + L"\\amount";
}
std::span<const unsigned char> Payload(int id) {
    HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!info) return {};
    HGLOBAL data = LoadResource(nullptr, info);
    return {static_cast<const unsigned char*>(LockResource(data)), SizeofResource(nullptr, info)};
}
bool FileMatches(const std::wstring& path, int id) {
    auto payload = Payload(id);
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file || payload.empty()) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || info.nFileSizeHigh || info.nFileSizeLow != payload.size()) return false;
    std::vector<unsigned char> bytes(payload.size());
    DWORD read = 0;
    return ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size() && std::equal(bytes.begin(), bytes.end(), payload.begin());
}
Result ServiceOwned(SC_HANDLE service) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (!needed) return LastError(L"Cannot inspect webmount driver registration");
    std::vector<unsigned char> bytes(needed);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    if (!QueryServiceConfigW(service, config, needed, &needed)) return LastError(L"Cannot read driver registration");
    std::wstring path(config->lpBinaryPathName);
    if (path.starts_with(L"\\??\\")) path.erase(0, 4);
    if (NameCompare(path, RuntimePath() + L"\\winfsp-x64.sys") || config->dwServiceType != SERVICE_FILE_SYSTEM_DRIVER)
        return Fail(ERROR_ALREADY_EXISTS, L"Driver service name belongs to a different installation");
    return {};
}
Result RuntimeReady() {
    Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) return LastError(L"Cannot open service manager");
    Service service(OpenServiceW(manager.value, serviceName, SERVICE_QUERY_CONFIG));
    if (!service.value) return LastError(L"webmount driver is not installed");
    auto result = ServiceOwned(service.value);
    if (!result) return result;
    if (!FileMatches(RuntimePath() + L"\\winfsp-x64.sys", 101) || !FileMatches(RuntimePath() + L"\\winfsp-x64.dll", 102))
        return Fail(ERROR_FILE_CORRUPT, L"Installed runtime differs from the bundled runtime; uninstall it before reinstalling");
    return {};
}
Result SecureDirectory(const std::wstring& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1, &descriptor, nullptr))
        return LastError(L"Cannot create runtime directory permissions");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    DWORD error = ERROR_SUCCESS;
    if (!CreateDirectoryW(path.c_str(), &attributes)) error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) error = ERROR_SUCCESS;
    DWORD flags = GetFileAttributesW(path.c_str());
    if (!error && (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT))) error = ERROR_ACCESS_DENIED;
    if (!error) {
        PACL acl = nullptr;
        BOOL present = FALSE, inherited = FALSE;
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &inherited);
        error = SetNamedSecurityInfoW(const_cast<PWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    }
    LocalFree(descriptor);
    return error ? Fail(error, L"Cannot secure runtime directory " + path) : Result{};
}
Result Extract(int id, const std::wstring& name) {
    auto path = RuntimePath() + L"\\" + name;
    if (FileMatches(path, id)) return {};
    auto payload = Payload(id);
    if (payload.empty()) return Fail(ERROR_RESOURCE_DATA_NOT_FOUND, L"Missing embedded runtime payload");
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return LastError(L"Cannot extract " + path + L"; existing files are not overwritten");
    DWORD written = 0;
    if (!WriteFile(file.value, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) || written != payload.size()) {
        auto result = LastError(L"Cannot write runtime payload");
        file.Reset();
        DeleteFileW(path.c_str());
        return result;
    }
    return {};
}
Result VerifySignature(const std::wstring& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{sizeof(trust)};
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    return status == ERROR_SUCCESS ? Result{} : Fail(static_cast<DWORD>(status), L"Runtime signature verification failed: " + path);
}
struct RuntimeGuard {
    Handle lock;
    ~RuntimeGuard() { UnlockRuntime(lock); }
};
}
std::wstring RuntimePath() {
    auto root = RootPath();
    return root.empty() ? root : root + L"\\SxS\\sxs.amount-2.1.25156\\bin";
}
bool IsElevated() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    TOKEN_ELEVATION value{};
    DWORD size = 0;
    return GetTokenInformation(token.value, TokenElevation, &value, sizeof(value), &size) && value.TokenIsElevated;
}
Result LockRuntime(Handle& lock) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) return LastError(L"Cannot prepare runtime lock");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    lock.Reset(CreateMutexExW(&attributes, L"Global\\amount-runtime-2.1.25156", 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
    DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!lock) return Fail(error, L"Cannot open runtime lock");
    DWORD wait = WaitForSingleObject(lock.value, 120000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return {};
    lock.Reset();
    return Fail(ERROR_TIMEOUT, L"Another webmount instance is installing or removing its driver");
}
void UnlockRuntime(Handle& lock) {
    if (lock) { ReleaseMutex(lock.value); lock.Reset(); }
}
Result ElevateSetup(bool uninstall) {
    auto path = ExePath();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = uninstall ? L"--uninstall" : L"--install";
    info.nShow = SW_SHOWNORMAL;
    Log(uninstall ? L"Requesting administrator permission to remove the webmount driver" : L"Requesting administrator permission to install the bundled driver");
    if (!ShellExecuteExW(&info)) return LastError(L"Administrator approval was not obtained");
    Handle process(info.hProcess);
    WaitForSingleObject(process.value, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(process.value, &code)) return LastError(L"Cannot read setup result");
    return code ? Fail(code, L"Elevated runtime setup failed") : Result{};
}
Result InstallRuntime() {
    if (!IsElevated()) return ElevateSetup(false);
    RuntimeGuard guard;
    auto result = LockRuntime(guard.lock);
    if (!result) return result;
    auto root = RootPath();
    if (root.empty()) return Fail(ERROR_PATH_NOT_FOUND, L"Cannot locate Program Files");
    for (const auto& path : {root, root + L"\\SxS", root + L"\\SxS\\sxs.amount-2.1.25156", RuntimePath()}) {
        result = SecureDirectory(path);
        if (!result) return result;
    }
    for (const auto& item : {std::pair{101, L"winfsp-x64.sys"}, {102, L"winfsp-x64.dll"}}) {
        result = Extract(item.first, item.second);
        if (!result) return result;
    }
    for (const auto& name : {L"winfsp-x64.sys", L"winfsp-x64.dll"}) {
        result = VerifySignature(RuntimePath() + L"\\" + name);
        if (!result) return result;
    }
    Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (!manager.value) return LastError(L"Cannot open service manager for installation");
    Service service(OpenServiceW(manager.value, serviceName, SERVICE_QUERY_CONFIG | WRITE_DAC));
    if (service.value) {
        result = ServiceOwned(service.value);
        if (!result) return result;
    } else {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) return Fail(error, L"Cannot inspect existing driver service");
        auto path = L"\\??\\" + RuntimePath() + L"\\winfsp-x64.sys";
        service.value = CreateServiceW(manager.value, serviceName, L"webmount WinFsp runtime", SERVICE_QUERY_CONFIG | WRITE_DAC, SERVICE_FILE_SYSTEM_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!service.value) return LastError(L"Cannot register webmount driver");
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;WP;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCRPRC;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) return LastError(L"Cannot prepare driver permissions");
    BOOL set = SetServiceObjectSecurity(service.value, DACL_SECURITY_INFORMATION, descriptor);
    DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!set) return Fail(error, L"Cannot set driver permissions");
    result = RuntimeReady();
    if (result) Log(L"Installed bundled WinFsp 2.1.25156 at " + RuntimePath());
    return result;
}
Result EnsureRuntime() {
    auto result = RuntimeReady();
    if (result) return result;
    if (result.code != ERROR_SERVICE_DOES_NOT_EXIST) return result;
    result = InstallRuntime();
    return result ? RuntimeReady() : result;
}
Result LoadRuntime() {
    if (runtimeModule) return {};
    auto result = RuntimeReady();
    if (!result) return result;
    auto path = RuntimePath() + L"\\winfsp-x64.dll";
    runtimeModule = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtimeModule) return LastError(L"Cannot load private WinFsp runtime");
    fsp.create = reinterpret_cast<decltype(fsp.create)>(GetProcAddress(runtimeModule, "FspFileSystemCreate"));
    fsp.remove = reinterpret_cast<decltype(fsp.remove)>(GetProcAddress(runtimeModule, "FspFileSystemDelete"));
    fsp.mount = reinterpret_cast<decltype(fsp.mount)>(GetProcAddress(runtimeModule, "FspFileSystemSetMountPoint"));
    fsp.unmount = reinterpret_cast<decltype(fsp.unmount)>(GetProcAddress(runtimeModule, "FspFileSystemRemoveMountPoint"));
    fsp.start = reinterpret_cast<decltype(fsp.start)>(GetProcAddress(runtimeModule, "FspFileSystemStartDispatcher"));
    fsp.stop = reinterpret_cast<decltype(fsp.stop)>(GetProcAddress(runtimeModule, "FspFileSystemStopDispatcher"));
    fsp.addDir = reinterpret_cast<decltype(fsp.addDir)>(GetProcAddress(runtimeModule, "FspFileSystemAddDirInfo"));
    fsp.status = reinterpret_cast<decltype(fsp.status)>(GetProcAddress(runtimeModule, "FspNtStatusFromWin32"));
    fsp.stopDriver = reinterpret_cast<decltype(fsp.stopDriver)>(GetProcAddress(runtimeModule, "FspFsctlStopService"));
    if (!fsp.create || !fsp.remove || !fsp.mount || !fsp.unmount || !fsp.start || !fsp.stop || !fsp.addDir || !fsp.status || !fsp.stopDriver) {
        FreeLibrary(runtimeModule);
        runtimeModule = nullptr;
        fsp = {};
        return Fail(ERROR_PROC_NOT_FOUND, L"Bundled WinFsp API is incomplete");
    }
    return {};
}
Result UninstallRuntime() {
    if (!IsElevated()) return ElevateSetup(true);
    RuntimeGuard guard;
    auto result = LockRuntime(guard.lock);
    if (!result) return result;
    Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) return LastError(L"Cannot open service manager");
    Service service(OpenServiceW(manager.value, serviceName, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | DELETE));
    if (!service.value && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) return LastError(L"Cannot open webmount driver service");
    if (service.value) {
        result = ServiceOwned(service.value);
        if (!result) return result;
        result = EjectMounts(0, true);
        if (!result) return result;
        SERVICE_STATUS state{};
        if (!QueryServiceStatus(service.value, &state)) return LastError(L"Cannot inspect driver state");
        if (state.dwCurrentState != SERVICE_STOPPED) {
            result = LoadRuntime();
            if (!result) return result;
            NTSTATUS status = fsp.stopDriver();
            FreeLibrary(runtimeModule);
            runtimeModule = nullptr;
            fsp = {};
            if (!NT_SUCCESS(status)) return Fail(static_cast<DWORD>(status), L"Driver could not unload; close applications using webmount drives and retry");
        }
        if (!DeleteService(service.value)) return LastError(L"Cannot remove webmount driver registration");
    }
    bool pending = false;
    for (const auto& name : {L"winfsp-x64.sys", L"winfsp-x64.dll", L"License.txt"}) {
        auto path = RuntimePath() + L"\\" + name;
        if (!DeleteFileW(path.c_str())) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) return Fail(error, L"Cannot remove " + path);
            if (!MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) return LastError(L"Cannot schedule runtime cleanup");
            pending = true;
        }
    }
    auto root = RootPath();
    for (const auto& path : {RuntimePath(), root + L"\\SxS\\sxs.amount-2.1.25156", root + L"\\SxS", root}) RemoveDirectoryW(path.c_str());
    Log(pending ? L"Driver unregistered; restart Windows to finish removing locked runtime files" : L"webmount driver and runtime removed; separately installed WinFsp was untouched");
    return pending ? Fail(ERROR_SUCCESS_REBOOT_REQUIRED, L"Runtime cleanup requires a restart") : Result{};
}
}
