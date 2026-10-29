#include "mountControl.h"
#include "startupMount.h"
#include <sddl.h>

namespace webmount {
namespace {
struct ReadyMessage {
    DWORD magic = 0x414d4e54;
    DWORD code = 0;
    wchar_t letter = 0;
    wchar_t message[512]{};
};
Result PipeWait(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout, DWORD& bytes) {
    HANDLE handles[]{operation.hEvent, stop};
    DWORD wait = WaitForMultipleObjects(stop ? 2 : 1, handles, FALSE, timeout);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        return Fail(wait == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT, L"Instance control request cancelled or timed out");
    }
    if (!GetOverlappedResult(pipe, &operation, &bytes, FALSE)) return LastError(L"Instance control I/O failed");
    return {};
}
Result PipeRead(HANDLE pipe, void* buffer, DWORD length, HANDLE stop, DWORD timeout, DWORD& bytes) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return LastError(L"Cannot allocate control event");
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    if (ReadFile(pipe, buffer, length, &bytes, &operation)) return {};
    if (GetLastError() != ERROR_IO_PENDING) return LastError(L"Cannot read instance control response");
    return PipeWait(pipe, operation, stop, timeout, bytes);
}
Result PipeWrite(HANDLE pipe, const void* buffer, DWORD length, DWORD timeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return LastError(L"Cannot allocate control event");
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD bytes = 0;
    if (!WriteFile(pipe, buffer, length, &bytes, &operation)) {
        if (GetLastError() != ERROR_IO_PENDING) return LastError(L"Cannot send instance control command");
        auto result = PipeWait(pipe, operation, nullptr, timeout, bytes);
        if (!result) return result;
    }
    return bytes == length ? Result{} : Fail(ERROR_WRITE_FAULT, L"Incomplete instance control message");
}
std::wstring PipeName(const std::wstring& sid, wchar_t letter) {
    return L"\\\\.\\pipe\\webmount-" + sid + L"-" + letter;
}
Result EjectPipe(const std::wstring& name) {
    Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe && GetLastError() == ERROR_PIPE_BUSY) {
        if (!WaitNamedPipeW(name.c_str(), 3000)) return LastError(L"Mount control pipe is busy");
        pipe.Reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    }
    if (!pipe) return LastError(L"Cannot contact mount instance");
    const DWORD command = 0x454a4543;
    auto result = PipeWrite(pipe.value, &command, sizeof(command), 5000);
    if (!result) return result;
    DWORD reply = ERROR_GEN_FAILURE, bytes = 0;
    result = PipeRead(pipe.value, &reply, sizeof(reply), nullptr, 60000, bytes);
    if (!result) return result;
    if (bytes != sizeof(reply)) return Fail(ERROR_BAD_NET_RESP, L"Invalid eject response");
    return reply ? Fail(reply, L"Mount instance could not eject") : Result{};
}
struct StartupGuard {
    Handle lock;
    ~StartupGuard() { UnlockStartup(lock); }
};
}
Result UserSid(std::wstring& sid) {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return LastError(L"Cannot identify current user");
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size) return LastError(L"Cannot query user SID");
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) return LastError(L"Cannot read user SID");
    PWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text)) return LastError(L"Cannot format user SID");
    sid = text;
    LocalFree(text);
    return {};
}
MountControl::MountControl() : stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)), finished(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
MountControl::~MountControl() { Finish(); }
Result MountControl::Reserve(wchar_t letter) {
    if (!stop || !finished) return LastError(L"Cannot allocate mount lifecycle events");
    std::wstring sid;
    auto result = UserSid(sid);
    if (!result) return result;
    auto acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return LastError(L"Cannot prepare instance permissions");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    auto name = PipeName(sid, letter);
    pipe.Reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 5000, &security));
    DWORD error = GetLastError();
    LocalFree(descriptor);
    return pipe ? Result{} : Fail(error, L"Cannot reserve mount control endpoint");
}
void MountControl::Start() { worker = std::thread(&MountControl::Serve, this); }
void MountControl::Finish() {
    if (stop) SetEvent(stop.value);
    if (finished) SetEvent(finished.value);
    if (worker.joinable()) worker.join();
    pipe.Reset();
}
void MountControl::Serve() {
    while (WaitForSingleObject(stop.value, 0) == WAIT_TIMEOUT) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) { RequestStop(); return; }
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        DWORD bytes = 0;
        BOOL connected = ConnectNamedPipe(pipe.value, &operation);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        Result result;
        switch (error) {
        case ERROR_SUCCESS: case ERROR_PIPE_CONNECTED: break;
        case ERROR_IO_PENDING: result = PipeWait(pipe.value, operation, stop.value, INFINITE, bytes); break;
        default: result = Fail(error, L"Cannot accept instance control connection"); break;
        }
        if (!result) return;
        DWORD command = 0;
        result = PipeRead(pipe.value, &command, sizeof(command), stop.value, 5000, bytes);
        if (result && bytes == sizeof(command) && command == 0x454a4543) {
            Log(L"Eject requested by another webmount instance");
            RequestStop();
            DWORD waited = WaitForSingleObject(finished.value, 60000);
            DWORD reply = waited == WAIT_OBJECT_0 ? ERROR_SUCCESS : ERROR_TIMEOUT;
            PipeWrite(pipe.value, &reply, sizeof(reply), 5000);
            return;
        }
        DisconnectNamedPipe(pipe.value);
    }
}
Result EjectMounts(wchar_t letter, bool allUsers) {
    StartupGuard guard;
    size_t removed = 0;
    Result failure;
    if (!allUsers) {
        auto result = LockStartup(guard.lock);
        if (!result) return result;
        failure = RemoveStartup(letter, removed);
        if (!failure) LogError(failure);
    }
    std::vector<std::wstring> names;
    if (allUsers) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\*", &entry);
        if (find == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND ? Result{} : Fail(error, L"Cannot enumerate webmount instances");
        }
        do {
            std::wstring name = entry.cFileName;
            if (name.starts_with(L"webmount-S-") && name.size() > 2 && name[name.size() - 2] == L'-' && name.back() >= L'A' && name.back() <= L'Z') names.push_back(L"\\\\.\\pipe\\" + name);
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    } else {
        std::wstring sid;
        auto result = UserSid(sid);
        if (!result) return result;
        for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) if (!letter || drive == letter) names.push_back(PipeName(sid, drive));
    }
    size_t count = 0;
    for (const auto& name : names) {
        auto result = EjectPipe(name);
        if (!result) {
            if (result.code == ERROR_FILE_NOT_FOUND || result.code == ERROR_PIPE_NOT_CONNECTED) continue;
            LogError(result);
            if (failure) failure = result;
            continue;
        }
        ++count;
        Log(std::wstring(L"Ejected ") + name.back() + L":");
    }
    if (!failure) return failure;
    if (letter && !count && !removed) return Fail(ERROR_NOT_FOUND, std::wstring(L"No WebMount drive or startup registration owned by this user at ") + letter + L":");
    if (!count && !removed) Log(L"No WebMount drives or startup registrations found");
    return {};
}
void ReportReady(HANDLE pipe, DWORD code, wchar_t letter, const std::wstring& detail) {
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return;
    ReadyMessage message;
    message.code = code;
    message.letter = letter;
    wcsncpy_s(message.message, detail.c_str(), _TRUNCATE);
    DWORD written = 0;
    WriteFile(pipe, &message, sizeof(message), &written, nullptr);
    CloseHandle(pipe);
}
Result LaunchBackground(const std::wstring& url, wchar_t letter, bool persist, bool restore) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle read, write;
    if (!CreatePipe(&read.value, &write.value, &security, 0) || !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) return LastError(L"Cannot create background readiness channel");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return LastError(L"Cannot prepare background process attributes");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
    HANDLE inherited[]{write.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) return LastError(L"Cannot restrict inherited process handles");
    auto executable = ExePath();
    auto command = QuoteArgument(executable) + L" ";
    if (letter) command += std::wstring{letter, L':'} + L" ";
    command += QuoteArgument(url);
    if (persist) command += L" --persist";
    if (restore) command += L" --restore";
    command += L" --ready " + std::to_wstring(reinterpret_cast<uintptr_t>(write.value));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process)) return LastError(L"Cannot launch detached mount process");
    Handle child(process.hProcess), thread(process.hThread);
    write.Reset();
    ULONGLONG deadline = GetTickCount64() + (restore ? 360000 : 120000);
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available >= sizeof(ReadyMessage)) {
            ReadyMessage message;
            DWORD received = 0;
            if (!ReadFile(read.value, &message, sizeof(message), &received, nullptr) || received != sizeof(message) || message.magic != 0x414d4e54) return Fail(ERROR_BAD_NET_RESP, L"Invalid background startup response");
            message.message[std::size(message.message) - 1] = L'\0';
            if (message.code) return Fail(message.code, message.message[0] ? message.message : L"Detached mount failed during startup");
            Log(std::wstring(L"Mounted ") + message.letter + L": in background, PID " + std::to_wstring(process.dwProcessId) + (persist ? L"; startup enabled" : L""));
            return {};
        }
        if (WaitForSingleObject(child.value, 50) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available >= sizeof(ReadyMessage)) continue;
            DWORD code = 0;
            GetExitCodeProcess(child.value, &code);
            return Fail(code, L"Background process exited before mounting");
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(child.value, ERROR_TIMEOUT);
            return Fail(ERROR_TIMEOUT, L"Background mount startup timed out and was terminated");
        }
    }
}
}
