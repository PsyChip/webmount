#pragma once
#include "app.h"
#include <winhttp.h>

namespace webmount {
struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};
Result SplitUrl(const std::wstring& url, UrlParts& parts);
Result NormalizeUrl(std::wstring input, std::wstring& output);
Result CombineUrl(const std::wstring& base, const std::wstring& link, std::wstring& output);
bool WithinRoot(const std::wstring& root, const std::wstring& url);
struct HttpReply {
    DWORD status = 0;
    std::wstring url;
    std::wstring length;
    std::wstring range;
    std::wstring modified;
    std::wstring etag;
    std::wstring type;
    std::wstring encoding;
    std::vector<unsigned char> bytes;
};
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    Result Initialize();
    Result Request(const std::wstring& method, const std::wstring& url, const std::wstring& headers, size_t limit, bool headersOnly, const std::wstring& root, HttpReply& reply);
    void Cancel();
private:
    Result Exchange(const std::wstring& method, const std::wstring& url, const std::wstring& headers, size_t limit, bool headersOnly, HttpReply& reply, std::wstring& location, ULONGLONG deadline);
    HINTERNET session = nullptr;
    Handle cancelled;
    Handle slots;
};
Result HttpStatus(DWORD status);
}
