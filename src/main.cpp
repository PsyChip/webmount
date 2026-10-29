#include "driveMount.h"
#include "startupMount.h"
#include <fcntl.h>
#include <io.h>
#include <objbase.h>
#include <shellapi.h>

namespace webmount {
namespace {
std::atomic<HANDLE> consoleStop = nullptr;
BOOL WINAPI ConsoleHandler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT: case CTRL_BREAK_EVENT: case CTRL_CLOSE_EVENT: case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT: {
        HANDLE stop = consoleStop.load();
        if (stop) SetEvent(stop);
        return stop ? TRUE : FALSE;
    }
    default: return FALSE;
    }
}
enum class Mode { Mount, Install, Uninstall, Eject, Help };
struct Arguments {
    Mode mode = Mode::Mount;
    std::wstring url;
    wchar_t letter = 0;
    bool background = false;
    bool persist = false;
    bool restore = false;
    HANDLE ready = nullptr;
};
bool ParseLetter(const std::wstring& value, wchar_t& letter) {
    if (value.size() != 2 || value[1] != L':') return false;
    auto upper = static_cast<wchar_t>(towupper(value[0]));
    if (upper < L'A' || upper > L'Z') return false;
    letter = upper;
    return true;
}
Result ParseArguments(int count, wchar_t** values, Arguments& arguments) {
    if (count == 1) { arguments.mode = Mode::Help; return {}; }
    bool selected = false;
    std::vector<std::wstring> positional;
    for (int index = 1; index < count; ++index) {
        std::wstring value = values[index];
        if (value == L"--detach" || value == L"--background") { arguments.background = true; continue; }
        if (value == L"--persist") { arguments.persist = true; continue; }
        if (value == L"--restore") { arguments.restore = true; continue; }
        if (value == L"--ready") {
            uint64_t handle = 0;
            if (arguments.ready || ++index >= count || !ParseNumber(values[index], handle) || !handle) return Fail(ERROR_INVALID_PARAMETER, L"Invalid internal readiness handle");
            arguments.ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
            if (GetFileType(arguments.ready) != FILE_TYPE_PIPE) return Fail(ERROR_INVALID_HANDLE, L"Readiness handle must be an inherited pipe");
            continue;
        }
        Mode mode = Mode::Mount;
        if (value == L"--install") mode = Mode::Install;
        else if (value == L"--uninstall") mode = Mode::Uninstall;
        else if (value == L"--eject") mode = Mode::Eject;
        else if (value == L"--help" || value == L"-h") mode = Mode::Help;
        else {
            if (value.starts_with(L'-') || positional.size() >= 2) return Fail(ERROR_INVALID_PARAMETER, L"Unexpected argument: " + value);
            positional.push_back(std::move(value));
            continue;
        }
        if (selected) return Fail(ERROR_INVALID_PARAMETER, L"Choose only one command");
        selected = true;
        arguments.mode = mode;
    }
    if (arguments.mode != Mode::Mount && (arguments.background || arguments.ready || arguments.persist || arguments.restore)) return Fail(ERROR_INVALID_PARAMETER, L"Background and persistence flags apply only to mounting");
    switch (arguments.mode) {
    case Mode::Mount:
        switch (positional.size()) {
        case 1:
            if (ParseLetter(positional[0], arguments.letter)) return Fail(ERROR_INVALID_PARAMETER, L"Supply a site URL after the drive letter");
            arguments.url = positional[0];
            break;
        case 2:
            if (!ParseLetter(positional[0], arguments.letter)) return Fail(ERROR_INVALID_PARAMETER, L"Use webmount D: <site url> when selecting a drive letter");
            arguments.url = positional[1];
            break;
        default: return Fail(ERROR_INVALID_PARAMETER, L"Supply a directory index URL; use webmount --help for syntax");
        }
        if (arguments.ready && arguments.background) return Fail(ERROR_INVALID_PARAMETER, L"Invalid background worker arguments");
        if (arguments.restore && (arguments.persist || !arguments.letter)) return Fail(ERROR_INVALID_PARAMETER, L"Invalid startup restoration arguments");
        if ((arguments.persist || arguments.restore) && !arguments.ready) arguments.background = true;
        return NormalizeUrl(arguments.url, arguments.url);
    case Mode::Eject:
        if (positional.empty()) return {};
        if (positional.size() != 1 || !ParseLetter(positional[0], arguments.letter)) return Fail(ERROR_INVALID_PARAMETER, L"Use webmount --eject D:");
        return {};
    default:
        if (!positional.empty()) return Fail(ERROR_INVALID_PARAMETER, L"This command does not take a URL or drive letter");
        return {};
    }
}
void Help() {
    std::wcout << L"webmount 0.2.0 - read-only HTTP directory drive mounter\n"
        L"Curated by Psychip root@psychip.net Oct 2026\n\n"
        L"Usage:\n"
        L"  webmount <site url>               Mount using the first free letter\n"
        L"  webmount D: <site url>            Mount specifically at D:\n"
        L"  webmount [D:] <site url> --detach  Mount in background\n"
        L"  webmount [D:] <site url> --persist Mount detached and restore at sign-in\n"
        L"  webmount --eject                  Eject all drives and remove saved mounts\n"
        L"  webmount --eject D:               Eject D: and remove its startup entry\n"
        L"  webmount --install                Install the bundled x64 WinFsp runtime\n"
        L"  webmount --uninstall              Clear saved mounts and remove the runtime\n"
        L"  webmount --help                   Show this help\n\n"
        L"No arguments prints help. --background is an alias for --detach.\n"
        L"Explicit letters A: through Z: must be free; automatic selection uses D: through Z:.\n"
        L"--persist does not need --detach. It remembers the actual assigned letter.\n"
        L"Startup entries belong to the current user and run after Windows sign-in.\n"
        L"Startup retries transient failures for up to five minutes without replacing busy drives.\n"
        L"Keep webmount.exe at its registered path, or register persistence again after moving it.\n"
        L"Eject also removes saved entries for offline drives.\n"
        L"Bare hosts use HTTP. HTTPS URLs are supported. Ctrl+C unmounts foreground drives.\n"
        L"The first mount installs the private runtime with a UAC prompt.\n"
        L"Read-only; immediate root mount; one-level directory prefetch.\n"
        L"Browsing a folder prefetches its child listings, never recursively.\n"
        L"Foreground activity appends to a timestamped scrolling log.\n"
        L"Drive labels use the supplied site hostname, limited to 32 characters.\n"
        L"Manual mounts open in Explorer when ready; sign-in restoration stays silent.\n"
        L"Listings expire after 60 seconds; file data cache is 64 MiB.\n"
        L"Servers without byte ranges support whole files up to 64 MiB.\n"
        L"Formats: Apache/nginx HTML, IIS, lighttpd, LiteSpeed, nginx fancyindex,\n"
        L"Caddy JSON, and nginx JSON/XML. Detection is automatic.\n"
        L"Listing sizes can be rounded until exact HTTP metadata is fetched.\n"
        L"WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\n"
        L"https://github.com/winfsp/winfsp\n";
}
struct MountLocks {
    Handle runtime;
    Handle startup;
    ~MountLocks() { Release(); }
    void Release() { UnlockStartup(startup); UnlockRuntime(runtime); }
};
Result OpenDrive(wchar_t letter) {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(status) && status != RPC_E_CHANGED_MODE) return Fail(static_cast<DWORD>(status), L"Drive mounted, but Explorer initialization failed");
    std::wstring path{letter, L':', L'\\'};
    auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(status)) CoUninitialize();
    return opened > 32 ? Result{} : Fail(static_cast<DWORD>(opened), L"Drive mounted, but Explorer could not open " + path);
}
Result MountOnce(Arguments& arguments, ULONGLONG deadline = 0) {
    auto result = CheckLetter(arguments.letter);
    if (!result) return result;
    MountCache cache;
    MountControl control;
    if (!control.StopEvent()) return LastError(L"Cannot allocate mount stop event");
    std::atomic_bool mounted = false;
    consoleStop = control.StopEvent();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    struct StopGuard {
        MountControl& control;
        std::thread watcher;
        ~StopGuard() {
            control.RequestStop();
            if (watcher.joinable()) watcher.join();
            consoleStop = nullptr;
            SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        }
    } guard{control, std::thread([&] {
        for (;;) {
            DWORD wait = WaitForSingleObject(control.StopEvent(), arguments.restore ? 1000 : INFINITE);
            if (wait != WAIT_TIMEOUT) break;
            if (!mounted.load() && (!CheckStartup(arguments.letter, arguments.url) || (deadline && GetTickCount64() >= deadline))) {
                control.RequestStop();
                break;
            }
        }
        cache.Cancel();
    })};
    result = cache.Initialize(arguments.url);
    if (!result) return result;
    result = arguments.restore ? LoadRuntime() : EnsureRuntime();
    if (!result) return result;
    DriveMount drive(cache, control);
    MountLocks locks;
    result = LockRuntime(locks.runtime);
    if (!result) return result;
    result = LockStartup(locks.startup);
    if (!result) return result;
    if (arguments.restore) {
        result = CheckStartup(arguments.letter, arguments.url);
        if (!result) return result;
    }
    if (WaitForSingleObject(control.StopEvent(), 0) != WAIT_TIMEOUT) return Fail(ERROR_OPERATION_ABORTED, L"Mount startup cancelled");
    result = drive.Start(arguments.letter, arguments.url);
    if (!result) { drive.Stop(); return result; }
    mounted = true;
    if (arguments.persist) {
        if (WaitForSingleObject(control.StopEvent(), 0) != WAIT_TIMEOUT) result = Fail(ERROR_OPERATION_ABORTED, L"Persistent mount startup cancelled");
        else result = SaveStartup(drive.Letter(), arguments.url);
        if (!result) { drive.Stop(); return result; }
    }
    locks.Release();
    if (!arguments.restore) {
        auto opened = OpenDrive(drive.Letter());
        if (!opened) LogError(opened);
    }
    cache.StartPrefetch();
    if (arguments.ready) {
        ReportReady(arguments.ready, ERROR_SUCCESS, drive.Letter());
        arguments.ready = nullptr;
    } else Log(L"Ctrl+C unmounts. Other instances can use webmount --eject.");
    WaitForSingleObject(control.StopEvent(), INFINITE);
    drive.Stop();
    return {};
}
bool RetryStartup(DWORD code) {
    switch (code) {
    case ERROR_ALREADY_ASSIGNED: case ERROR_BUSY: case ERROR_RETRY:
    case ERROR_TIMEOUT: case ERROR_SEM_TIMEOUT: case ERROR_BAD_NET_RESP:
    case ERROR_NETWORK_UNREACHABLE: case ERROR_HOST_UNREACHABLE: case ERROR_CONNECTION_REFUSED:
    case ERROR_WINHTTP_TIMEOUT: case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT: case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_RESEND_REQUEST: case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
    case static_cast<DWORD>(STATUS_OBJECT_NAME_COLLISION):
        return true;
    default: return false;
    }
}
Result RestoreMount(Arguments& arguments) {
    ULONGLONG deadline = GetTickCount64() + 300000;
    DWORD delay = 1000;
    for (;;) {
        auto result = CheckStartup(arguments.letter, arguments.url);
        if (!result) return result;
        result = MountOnce(arguments, deadline);
        if (!result && GetTickCount64() >= deadline) return Fail(ERROR_TIMEOUT, L"Startup restoration timed out; registration was retained");
        if (result || !RetryStartup(result.code)) return result;
        ULONGLONG until = std::min(deadline, GetTickCount64() + delay);
        while (GetTickCount64() < until) {
            auto saved = CheckStartup(arguments.letter, arguments.url);
            if (!saved) return saved;
            Sleep(250);
        }
        if (GetTickCount64() >= deadline) return Fail(ERROR_TIMEOUT, L"Startup restoration timed out; registration was retained");
        delay = std::min<DWORD>(delay * 2, 30000);
    }
}
Result RunMount(Arguments& arguments) {
    if (arguments.background) {
        Result result;
        if (arguments.restore) {
            HideStartupConsole();
            result = CheckStartup(arguments.letter, arguments.url);
        } else {
            result = CheckLetter(arguments.letter);
            if (result) result = EnsureRuntime();
        }
        return result ? LaunchBackground(arguments.url, arguments.letter, arguments.persist, arguments.restore) : result;
    }
    return arguments.restore ? RestoreMount(arguments) : MountOnce(arguments);
}
}
}
int wmain(int count, wchar_t** values) {
    using namespace webmount;
    if (_isatty(_fileno(stdout))) _setmode(_fileno(stdout), _O_U16TEXT);
    if (_isatty(_fileno(stderr))) _setmode(_fileno(stderr), _O_U16TEXT);
    Arguments arguments;
    auto result = ParseArguments(count, values, arguments);
    if (!result) { LogError(result); return static_cast<int>(result.code); }
    if (arguments.ready) quiet = true;
    struct ConsoleGuard { ~ConsoleGuard() { StopConsole(); } } consoleGuard;
    if (arguments.mode == Mode::Mount && !arguments.background && !arguments.ready) StartConsole(arguments.url);
    switch (arguments.mode) {
    case Mode::Help: Help(); break;
    case Mode::Install: result = InstallRuntime(); break;
    case Mode::Uninstall:
        result = EjectMounts();
        if (result) result = UninstallRuntime();
        break;
    case Mode::Eject: result = EjectMounts(arguments.letter); break;
    case Mode::Mount: result = RunMount(arguments); break;
    }
    if (arguments.ready) ReportReady(arguments.ready, result.code ? result.code : ERROR_GEN_FAILURE, 0, result.message);
    if (!result) LogError(result);
    return static_cast<int>(result.code);
}
