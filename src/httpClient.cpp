#include "httpClient.h"
#include <shlwapi.h>

namespace webmount {
namespace {
struct Internet {
    HINTERNET value = nullptr;
    explicit Internet(HINTERNET input) : value(input) {}
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct Operation {
    Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle closed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HINTERNET request = nullptr;
    DWORD error = 0;
    DWORD received = 0;
    std::vector<unsigned char> buffer = std::vector<unsigned char>(65536);
    std::wstring headers;
    ~Operation() {
        if (request) {
            WinHttpCloseHandle(request);
            WaitForSingleObject(closed.value, INFINITE);
        }
    }
    void Reset() { error = 0; received = 0; ResetEvent(event.value); }
    Result Wait(HANDLE cancelled, ULONGLONG deadline) {
        HANDLE handles[]{cancelled, event.value};
        ULONGLONG now = GetTickCount64();
        DWORD timeout = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        DWORD value = WaitForMultipleObjects(2, handles, FALSE, timeout);
        switch (value) {
        case WAIT_OBJECT_0: return Fail(ERROR_OPERATION_ABORTED, L"HTTP request cancelled");
        case WAIT_OBJECT_0 + 1: return error ? Fail(error, L"HTTP transport failed") : Result{};
        case WAIT_TIMEOUT: return Fail(ERROR_TIMEOUT, L"HTTP request exceeded its deadline");
        default: return LastError(L"Cannot wait for HTTP response");
        }
    }
    static void CALLBACK Notify(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size) {
        auto self = reinterpret_cast<Operation*>(context);
        if (!self) return;
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            SetEvent(self->event.value);
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            self->received = size;
            SetEvent(self->event.value);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            self->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            SetEvent(self->event.value);
            break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            SetEvent(self->closed.value);
            break;
        }
    }
};
std::wstring Header(HINTERNET request, DWORD query) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 65536) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}
}
Result SplitUrl(const std::wstring& url, UrlParts& parts) {
    if (url.size() > 16384 || url.find(L'\\') != std::wstring::npos || std::any_of(url.begin(), url.end(), [](wchar_t ch) { return ch < 32; }))
        return Fail(ERROR_INVALID_NAME, L"Invalid URL");
    URL_COMPONENTS value{sizeof(value)};
    value.dwSchemeLength = value.dwHostNameLength = value.dwUrlPathLength = value.dwExtraInfoLength = value.dwUserNameLength = value.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &value)) return LastError(L"Invalid HTTP URL");
    if ((value.nScheme != INTERNET_SCHEME_HTTP && value.nScheme != INTERNET_SCHEME_HTTPS) || !value.dwHostNameLength || value.dwUserNameLength || value.dwPasswordLength)
        return Fail(ERROR_INVALID_NAME, L"Use an HTTP or HTTPS URL without embedded credentials");
    parts.host.assign(value.lpszHostName, value.dwHostNameLength);
    parts.path.clear();
    if (value.dwUrlPathLength) parts.path.assign(value.lpszUrlPath, value.dwUrlPathLength);
    if (value.dwExtraInfoLength) parts.path.append(value.lpszExtraInfo, value.dwExtraInfoLength);
    if (parts.path.empty()) parts.path = L"/";
    if (parts.path.find(L'#') != std::wstring::npos) return Fail(ERROR_INVALID_NAME, L"URL fragments are not supported");
    parts.port = value.nPort;
    parts.secure = value.nScheme == INTERNET_SCHEME_HTTPS;
    return {};
}
Result NormalizeUrl(std::wstring input, std::wstring& output) {
    if (input.find(L"://") == std::wstring::npos) input = L"http://" + input;
    UrlParts parts;
    auto result = SplitUrl(input, parts);
    if (!result) return result;
    if (parts.path.find(L'?') != std::wstring::npos) return Fail(ERROR_INVALID_NAME, L"Supply the directory URL without sorting or filter query parameters");
    if (input.back() != L'/') input += L'/';
    std::wstring canonical(32768, L'\0');
    DWORD length = static_cast<DWORD>(canonical.size());
    HRESULT status = UrlCanonicalizeW(input.c_str(), canonical.data(), &length, URL_ESCAPE_UNSAFE);
    if (FAILED(status)) return Fail(static_cast<DWORD>(status), L"Cannot normalize directory URL");
    canonical.resize(length);
    output = std::move(canonical);
    return {};
}
Result CombineUrl(const std::wstring& base, const std::wstring& link, std::wstring& output) {
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    HRESULT result = UrlCombineW(base.c_str(), link.c_str(), buffer.data(), &size, URL_ESCAPE_UNSAFE);
    if (FAILED(result)) return Fail(static_cast<DWORD>(result), L"Cannot resolve URL");
    buffer.resize(size);
    UrlParts parts;
    auto checked = SplitUrl(buffer, parts);
    if (!checked) return checked;
    output = std::move(buffer);
    return {};
}
bool WithinRoot(const std::wstring& root, const std::wstring& url) {
    UrlParts base, child;
    if (!SplitUrl(root, base) || !SplitUrl(url, child)) return false;
    if (base.secure != child.secure || base.port != child.port || NameCompare(base.host, child.host) || !child.path.starts_with(base.path)) return false;
    auto path = child.path.substr(0, child.path.find(L'?'));
    std::wstring decoded;
    for (size_t index = 0; index < path.size(); ++index) {
        wchar_t ch = path[index];
        if (ch == L'%') {
            auto hex = [](wchar_t digit) -> int {
                if (digit >= L'0' && digit <= L'9') return digit - L'0';
                if (digit >= L'a' && digit <= L'f') return digit - L'a' + 10;
                if (digit >= L'A' && digit <= L'F') return digit - L'A' + 10;
                return -1;
            };
            if (index + 2 >= path.size()) return false;
            int high = hex(path[index + 1]), low = hex(path[index + 2]);
            if (high < 0 || low < 0) return false;
            ch = static_cast<wchar_t>((high << 4) | low);
            if (ch == L'/' || ch == L'\\' || ch == 0) return false;
            index += 2;
        }
        decoded += ch;
    }
    size_t position = 0;
    while (position < decoded.size()) {
        size_t end = decoded.find(L'/', position);
        if (end == std::wstring::npos) end = decoded.size();
        auto segment = std::wstring_view(decoded).substr(position, end - position);
        if (segment == L"." || segment == L"..") return false;
        position = end + 1;
    }
    return true;
}
Result HttpStatus(DWORD status) {
    switch (status) {
    case 200: case 206: return {};
    case 401: case 403: return Fail(ERROR_ACCESS_DENIED, L"HTTP access denied: " + std::to_wstring(status));
    case 404: case 410: return Fail(ERROR_FILE_NOT_FOUND, L"Remote entry no longer exists");
    case 412: return Fail(ERROR_FILE_INVALID, L"Remote file changed while it was open; reopen the file");
    case 416: return Fail(ERROR_HANDLE_EOF, L"Remote file range is no longer available");
    default: return Fail(ERROR_BAD_NET_RESP, L"Unexpected HTTP status " + std::to_wstring(status));
    }
}
HttpClient::HttpClient() : cancelled(CreateEventW(nullptr, TRUE, FALSE, nullptr)), slots(CreateSemaphoreW(nullptr, 4, 4, nullptr)) {}
HttpClient::~HttpClient() { if (session) WinHttpCloseHandle(session); }
Result HttpClient::Initialize() {
    if (!cancelled || !slots) return LastError(L"Cannot allocate HTTP synchronization handles");
    session = WinHttpOpen(L"webmount/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session) return LastError(L"Cannot initialize WinHTTP");
    if (!WinHttpSetTimeouts(session, 4000, 4000, 10000, 10000)) return LastError(L"Cannot configure HTTP timeouts");
    return {};
}
void HttpClient::Cancel() { SetEvent(cancelled.value); }
Result HttpClient::Request(const std::wstring& method, const std::wstring& url, const std::wstring& headers, size_t limit, bool headersOnly, const std::wstring& root, HttpReply& reply) {
    SetItem(url, L"", headers.find(L"Range:") == std::wstring::npos ? ItemState::Fetching : ItemState::Reading, method);
    HANDLE waits[]{cancelled.value, slots.value};
    DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 15000);
    if (wait != WAIT_OBJECT_0 + 1) return Fail(wait == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT, L"HTTP request queue cancelled or timed out");
    struct Slot { HANDLE value; ~Slot() { ReleaseSemaphore(value, 1, nullptr); } } slot{slots.value};
    auto current = url;
    ULONGLONG deadline = GetTickCount64() + (root.empty() ? 15000 : 30000);
    for (unsigned redirect = 0; redirect <= 5; ++redirect) {
        reply = {};
        std::wstring location;
        auto result = Exchange(method, current, headers, limit, headersOnly, reply, location, deadline);
        if (!result) {
            if (result.code != ERROR_OPERATION_ABORTED) SetItem(url, L"", ItemState::Error, result.message);
            return result;
        }
        switch (reply.status) {
        case 301: case 302: case 303: case 307: case 308: {
            if (location.empty()) return Fail(ERROR_BAD_NET_RESP, L"HTTP redirect has no Location header");
            std::wstring next;
            result = CombineUrl(current, location, next);
            if (!result) return result;
            UrlParts before, after;
            SplitUrl(current, before);
            SplitUrl(next, after);
            if (before.secure && !after.secure) return Fail(ERROR_ACCESS_DENIED, L"HTTPS downgrade redirect refused");
            if (!root.empty() && !WithinRoot(root, next)) return Fail(ERROR_ACCESS_DENIED, L"Redirect leaves the mounted directory tree");
            SetItem(url, L"", ItemState::Fetching, L"redirect: " + next);
            current = std::move(next);
            continue;
        }
        default: return {};
        }
    }
    return Fail(ERROR_TOO_MANY_CMDS, L"Too many HTTP redirects");
}
Result HttpClient::Exchange(const std::wstring& method, const std::wstring& url, const std::wstring& headers, size_t limit, bool headersOnly, HttpReply& reply, std::wstring& location, ULONGLONG deadline) {
    UrlParts parts;
    auto result = SplitUrl(url, parts);
    if (!result) return result;
    Internet connection(WinHttpConnect(session, parts.host.c_str(), parts.port, 0));
    if (!connection.value) return LastError(L"Cannot connect to HTTP host");
    Operation operation;
    if (!operation.event || !operation.closed) return LastError(L"Cannot allocate HTTP request events");
    HINTERNET request = WinHttpOpenRequest(connection.value, method.c_str(), parts.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) return LastError(L"Cannot create HTTP request");
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&operation);
    if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        auto error = LastError(L"Cannot initialize HTTP request context");
        WinHttpCloseHandle(request);
        return error;
    }
    if (WinHttpSetStatusCallback(request, Operation::Notify, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        auto error = LastError(L"Cannot initialize HTTP completion callback");
        WinHttpCloseHandle(request);
        return error;
    }
    operation.request = request;
    DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) || !WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth))) return LastError(L"Cannot configure HTTP request policy");
    operation.headers = L"Accept-Encoding: identity\r\n" + headers;
    operation.Reset();
    if (!WinHttpSendRequest(request, operation.headers.c_str(), static_cast<DWORD>(operation.headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, context)) return LastError(L"Cannot send HTTP request");
    result = operation.Wait(cancelled.value, deadline);
    if (!result) return result;
    operation.Reset();
    if (!WinHttpReceiveResponse(request, nullptr)) return LastError(L"Cannot receive HTTP headers");
    result = operation.Wait(cancelled.value, deadline);
    if (!result) return result;
    DWORD size = sizeof(reply.status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &reply.status, &size, WINHTTP_NO_HEADER_INDEX)) return LastError(L"Cannot read HTTP status");
    reply.url = url;
    reply.length = Header(request, WINHTTP_QUERY_CONTENT_LENGTH);
    reply.range = Header(request, WINHTTP_QUERY_CONTENT_RANGE);
    reply.modified = Header(request, WINHTTP_QUERY_LAST_MODIFIED);
    reply.etag = Header(request, WINHTTP_QUERY_ETAG);
    reply.type = Header(request, WINHTTP_QUERY_CONTENT_TYPE);
    reply.encoding = Header(request, WINHTTP_QUERY_CONTENT_ENCODING);
    location = Header(request, WINHTTP_QUERY_LOCATION);
    if (headersOnly || (reply.status != 200 && reply.status != 206)) return {};
    if (!reply.encoding.empty() && Lower(reply.encoding) != L"identity") return Fail(ERROR_NOT_SUPPORTED, L"Server ignored identity encoding; byte offsets would be unreliable");
    uint64_t length = 0;
    if (ParseNumber(reply.length, length) && length > limit) return Fail(ERROR_NOT_SUPPORTED, L"HTTP response exceeds the memory limit; server may not support byte ranges");
    auto& buffer = operation.buffer;
    for (;;) {
        operation.Reset();
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)) return LastError(L"Cannot read HTTP response body");
        result = operation.Wait(cancelled.value, deadline);
        if (!result) return result;
        if (!operation.received) break;
        if (operation.received > limit - reply.bytes.size()) return Fail(ERROR_NOT_SUPPORTED, L"HTTP response exceeded the memory limit");
        reply.bytes.insert(reply.bytes.end(), buffer.begin(), buffer.begin() + operation.received);
    }
    if (ParseNumber(reply.length, length) && length != reply.bytes.size()) return Fail(ERROR_BAD_NET_RESP, L"Truncated HTTP response");
    return {};
}
}
