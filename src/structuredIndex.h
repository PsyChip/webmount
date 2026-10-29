#pragma once
#include "apacheIndex.h"

namespace webmount {
uint64_t ParseIndexTime(const std::wstring& text);
Result ParseJsonIndex(const std::vector<unsigned char>& bytes, const std::wstring& url, std::vector<IndexEntry>& entries);
Result ParseXmlIndex(const std::vector<unsigned char>& bytes, const std::wstring& url, std::vector<IndexEntry>& entries);
}
