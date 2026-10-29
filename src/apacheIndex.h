#pragma once
#include "httpClient.h"

namespace webmount {
struct IndexEntry {
    std::wstring name;
    std::wstring url;
    bool directory = false;
    uint64_t size = 0;
    uint64_t modified = 0;
};
bool AddIndexEntry(std::vector<IndexEntry>& entries, const std::wstring& base, std::wstring link, bool directory, uint64_t size, uint64_t modified = 0);
Result ParseIndex(const std::vector<unsigned char>& bytes, const std::wstring& url, const std::wstring& contentType, std::vector<IndexEntry>& entries);
}
