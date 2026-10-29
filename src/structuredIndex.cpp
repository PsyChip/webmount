#include "structuredIndex.h"
#include <roapi.h>
#include <windows.data.json.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <cmath>

namespace webmount {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace Json = ABI::Windows::Data::Json;
struct RuntimeScope {
    HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
    ~RuntimeScope() { if (SUCCEEDED(status)) RoUninitialize(); }
};
bool JsonText(Json::IJsonObject* object, const wchar_t* key, std::wstring& text) {
    HString value;
    if (FAILED(object->GetNamedString(HStringReference(key).Get(), value.GetAddressOf()))) return false;
    UINT32 length = 0;
    const wchar_t* data = value.GetRawBuffer(&length);
    text.assign(data, length);
    return true;
}
bool JsonSize(Json::IJsonObject* object, uint64_t& size) {
    double number = 0;
    if (FAILED(object->GetNamedNumber(HStringReference(L"size").Get(), &number)) || !std::isfinite(number) || number < 0 || std::floor(number) != number || number > 9007199254740991.) return false;
    size = static_cast<uint64_t>(number);
    return true;
}
bool SafeName(std::wstring& name, bool directory) {
    if (directory && name.ends_with(L'/')) name.pop_back();
    if (name.empty() || name == L"." || name == L".." || name.size() > 16384 || name.find_first_of(L"/\\") != std::wstring::npos || name.find(L'\0') != std::wstring::npos) return false;
    return !Utf8(name).empty();
}
std::wstring NameLink(const std::wstring& name, bool directory) {
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result = L"./";
    for (unsigned char value : Utf8(name)) {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' || value == '~') result += value;
        else { result += L'%'; result += hex[value >> 4]; result += hex[value & 15]; }
    }
    if (directory) result += L'/';
    return result;
}
bool JsonDepth(const std::vector<unsigned char>& bytes) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (unsigned char ch : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        switch (ch) {
        case '"': quoted = true; break;
        case '[': case '{': if (++depth > 16) return false; break;
        case ']': case '}': if (!depth) return false; --depth; break;
        }
    }
    return !depth && !quoted;
}
std::wstring XmlAttribute(IXmlReader* reader, const wchar_t* name) {
    if (reader->MoveToAttributeByName(name, nullptr) != S_OK) return {};
    const wchar_t* value = nullptr;
    UINT length = 0;
    std::wstring result;
    if (SUCCEEDED(reader->GetValue(&value, &length))) result.assign(value, length);
    reader->MoveToElement();
    return result;
}
Result BadListing(const wchar_t* format) { return Fail(ERROR_BAD_FORMAT, std::wstring(L"Invalid ") + format + L" directory listing"); }
}
uint64_t ParseIndexTime(const std::wstring& text) {
    SYSTEMTIME system{};
    FILETIME file{};
    if (text.ends_with(L" GMT")) {
        if (!WinHttpTimeToSystemTime(text.c_str(), &system) || !SystemTimeToFileTime(&system, &file)) return 0;
        return (uint64_t(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    }
    if (text.size() < 20 || text[4] != L'-' || text[7] != L'-' || text[10] != L'T' || text[13] != L':' || text[16] != L':') return 0;
    auto number = [&](size_t position, size_t length, WORD& target) {
        uint64_t parsed = 0;
        if (!ParseNumber(std::wstring_view(text).substr(position, length), parsed) || parsed > 65535) return false;
        target = static_cast<WORD>(parsed);
        return true;
    };
    if (!number(0, 4, system.wYear) || !number(5, 2, system.wMonth) || !number(8, 2, system.wDay) || !number(11, 2, system.wHour) || !number(14, 2, system.wMinute) || !number(17, 2, system.wSecond)) return 0;
    size_t position = 19;
    uint64_t fraction = 0;
    if (position < text.size() && text[position] == L'.') {
        size_t digits = 0;
        ++position;
        while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
            if (digits < 7) fraction = fraction * 10 + text[position] - L'0';
            ++digits;
            ++position;
        }
        if (!digits) return 0;
        while (digits++ < 7) fraction *= 10;
    }
    int offset = 0;
    if (position + 1 == text.size() && text[position] == L'Z') {}
    else if (position + 6 == text.size() && (text[position] == L'+' || text[position] == L'-') && text[position + 3] == L':') {
        WORD hours = 0, minutes = 0;
        if (!number(position + 1, 2, hours) || !number(position + 4, 2, minutes) || hours > 23 || minutes > 59) return 0;
        offset = (hours * 60 + minutes) * (text[position] == L'+' ? 1 : -1);
    } else return 0;
    if (!SystemTimeToFileTime(&system, &file)) return 0;
    int64_t ticks = static_cast<int64_t>((uint64_t(file.dwHighDateTime) << 32) | file.dwLowDateTime);
    int64_t delta = int64_t(offset) * 60 * 10000000;
    if (ticks < delta || (delta < 0 && ticks > INT64_MAX + delta - static_cast<int64_t>(fraction))) return 0;
    return static_cast<uint64_t>(ticks - delta) + fraction;
}
Result ParseJsonIndex(const std::vector<unsigned char>& bytes, const std::wstring& url, std::vector<IndexEntry>& entries) {
    if (!JsonDepth(bytes)) return BadListing(L"JSON");
    auto text = Wide(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (text.empty()) return BadListing(L"JSON");
    if (text.front() == 0xfeff) text.erase(0, 1);
    RuntimeScope runtime;
    if (FAILED(runtime.status) && runtime.status != RPC_E_CHANGED_MODE) return Fail(static_cast<DWORD>(runtime.status), L"Cannot initialize Windows JSON parser");
    ComPtr<Json::IJsonArrayStatics> factory;
    HRESULT status = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonArray).Get(), IID_PPV_ARGS(&factory));
    if (FAILED(status)) return Fail(static_cast<DWORD>(status), L"Windows JSON parser is unavailable");
    HString input;
    if (FAILED(input.Set(text.c_str(), static_cast<UINT32>(text.size())))) return Fail(ERROR_NOT_ENOUGH_MEMORY, L"Cannot allocate JSON input");
    ComPtr<Json::IJsonArray> array;
    boolean parsed = false;
    if (FAILED(factory->TryParse(input.Get(), &array, &parsed)) || !parsed) return BadListing(L"JSON");
    ComPtr<ABI::Windows::Foundation::Collections::IVector<Json::IJsonValue*>> values;
    UINT32 count = 0;
    if (FAILED(array.As(&values)) || FAILED(values->get_Size(&count)) || count > 100000) return BadListing(L"JSON");
    enum class Schema { Unknown, Caddy, Nginx };
    Schema schema = Schema::Unknown;
    for (UINT32 index = 0; index < count; ++index) {
        ComPtr<Json::IJsonObject> item;
        if (FAILED(array->GetObjectAt(index, &item))) return BadListing(L"JSON");
        std::wstring name, type, link, modified;
        if (!JsonText(item.Get(), L"name", name)) return BadListing(L"JSON");
        boolean isDir = false;
        bool caddy = SUCCEEDED(item->GetNamedBoolean(HStringReference(L"is_dir").Get(), &isDir));
        Schema current = caddy ? Schema::Caddy : Schema::Nginx;
        if (schema != Schema::Unknown && schema != current) return BadListing(L"mixed-schema JSON");
        schema = current;
        bool directory = false, skip = false;
        uint64_t size = 0;
        switch (schema) {
        case Schema::Caddy:
            directory = isDir;
            if (!JsonText(item.Get(), L"url", link) || !JsonText(item.Get(), L"mod_time", modified) || (!directory && !JsonSize(item.Get(), size))) return BadListing(L"Caddy JSON");
            break;
        case Schema::Nginx:
            if (!JsonText(item.Get(), L"type", type) || !JsonText(item.Get(), L"mtime", modified)) return BadListing(L"nginx JSON");
            if (type == L"directory") directory = true;
            else if (type == L"file") { if (!JsonSize(item.Get(), size)) return BadListing(L"nginx JSON"); }
            else if (type == L"other") skip = true;
            else return BadListing(L"nginx JSON");
            break;
        default: return BadListing(L"JSON");
        }
        if (!SafeName(name, directory) || skip) continue;
        if (schema == Schema::Caddy) {
            std::vector<IndexEntry> expected, supplied;
            if (!AddIndexEntry(expected, url, NameLink(name, directory), directory, 0) || !AddIndexEntry(supplied, url, link, directory, 0)) continue;
            if (expected.front().name != supplied.front().name || expected.front().directory != supplied.front().directory) continue;
        } else link = NameLink(name, directory);
        AddIndexEntry(entries, url, std::move(link), directory, size, ParseIndexTime(modified));
    }
    return {};
}
Result ParseXmlIndex(const std::vector<unsigned char>& bytes, const std::wstring& url, std::vector<IndexEntry>& entries) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!stream) return Fail(ERROR_NOT_ENOUGH_MEMORY, L"Cannot allocate XML input");
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(IID_PPV_ARGS(&reader), nullptr))) return Fail(ERROR_NOT_SUPPORTED, L"Windows XML parser is unavailable");
    if (FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) || FAILED(reader->SetProperty(XmlReaderProperty_XmlResolver, 0)) || FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 3)) || FAILED(reader->SetInput(stream.Get()))) return BadListing(L"XML");
    enum class State { Start, Listing, Entry, Done };
    State state = State::Start;
    std::wstring name, element, modified;
    bool directory = false, skip = false;
    uint64_t size = 0;
    HRESULT status;
    XmlNodeType node;
    size_t count = 0;
    while ((status = reader->Read(&node)) == S_OK) {
        const wchar_t* raw = nullptr;
        UINT length = 0, depth = 0;
        if (FAILED(reader->GetDepth(&depth))) return BadListing(L"XML");
        switch (node) {
        case XmlNodeType_Element: {
            if (FAILED(reader->GetLocalName(&raw, &length))) return BadListing(L"XML");
            std::wstring tag(raw, length);
            if (FAILED(reader->GetNamespaceUri(&raw, &length)) || length) return BadListing(L"XML");
            switch (state) {
            case State::Start:
                if (depth || tag != L"list") return BadListing(L"nginx XML");
                state = reader->IsEmptyElement() ? State::Done : State::Listing;
                break;
            case State::Listing:
                if (depth != 1 || (tag != L"file" && tag != L"directory" && tag != L"other") || reader->IsEmptyElement()) return BadListing(L"nginx XML");
                element = tag;
                directory = tag == L"directory";
                skip = tag == L"other";
                modified = XmlAttribute(reader.Get(), L"mtime");
                if (modified.empty()) return BadListing(L"nginx XML");
                size = 0;
                if (!directory && !skip && (!ParseNumber(XmlAttribute(reader.Get(), L"size"), size) || size > INT64_MAX - 4095)) return BadListing(L"nginx XML");
                name.clear();
                state = State::Entry;
                break;
            default: return BadListing(L"XML");
            }
            break;
        }
        case XmlNodeType_Text: case XmlNodeType_CDATA: case XmlNodeType_Whitespace:
            if (FAILED(reader->GetValue(&raw, &length))) return BadListing(L"XML");
            if (state == State::Entry) {
                if (name.size() + length > 16384) return BadListing(L"XML");
                name.append(raw, length);
            } else if (node == XmlNodeType_Text || node == XmlNodeType_CDATA) return BadListing(L"XML");
            break;
        case XmlNodeType_EndElement:
            if (FAILED(reader->GetLocalName(&raw, &length))) return BadListing(L"XML");
            switch (state) {
            case State::Entry:
                if (depth != 2 || std::wstring_view(raw, length) != element || ++count > 100000) return BadListing(L"XML");
                if (!skip && SafeName(name, directory)) AddIndexEntry(entries, url, NameLink(name, directory), directory, size, ParseIndexTime(modified));
                state = State::Listing;
                break;
            case State::Listing:
                if (depth != 1 || std::wstring_view(raw, length) != L"list") return BadListing(L"XML");
                state = State::Done;
                break;
            default: return BadListing(L"XML");
            }
            break;
        case XmlNodeType_XmlDeclaration: case XmlNodeType_Comment: break;
        default: return BadListing(L"XML");
        }
    }
    return status == S_FALSE && state == State::Done ? Result{} : BadListing(L"XML");
}
}
