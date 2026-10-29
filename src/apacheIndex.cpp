#include "apacheIndex.h"
#include "structuredIndex.h"
#include <charconv>
#include <cmath>
#include <set>

namespace webmount {
namespace {
struct Tag {
    std::string name;
    std::map<std::string, std::string> attributes;
    bool closing = false;
    size_t begin = 0;
    size_t end = 0;
};
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
bool NextTag(const std::string& html, size_t& position, Tag& tag) {
    while (position < html.size()) {
        size_t start = html.find('<', position);
        if (start == std::string::npos) return false;
        if (html.compare(start, 4, "<!--") == 0) {
            size_t end = html.find("-->", start + 4);
            if (end == std::string::npos) return false;
            position = end + 3;
            continue;
        }
        size_t cursor = start + 1;
        tag = {};
        tag.begin = start;
        if (cursor < html.size() && html[cursor] == '/') { tag.closing = true; ++cursor; }
        size_t name = cursor;
        while (cursor < html.size() && (std::isalnum(static_cast<unsigned char>(html[cursor])) || html[cursor] == '-')) ++cursor;
        if (name == cursor) { position = cursor + 1; continue; }
        tag.name = Lower(html.substr(name, cursor - name));
        while (cursor < html.size()) {
            while (cursor < html.size() && (Space(html[cursor]) || html[cursor] == '/')) ++cursor;
            if (cursor == html.size()) return false;
            if (html[cursor] == '>') { tag.end = cursor + 1; position = tag.end; return true; }
            size_t attr = cursor;
            while (cursor < html.size() && !Space(html[cursor]) && html[cursor] != '=' && html[cursor] != '>' && html[cursor] != '/') ++cursor;
            if (cursor == attr) { ++cursor; continue; }
            auto key = Lower(html.substr(attr, cursor - attr));
            while (cursor < html.size() && Space(html[cursor])) ++cursor;
            std::string value;
            if (cursor < html.size() && html[cursor] == '=') {
                ++cursor;
                while (cursor < html.size() && Space(html[cursor])) ++cursor;
                if (cursor == html.size()) return false;
                char quote = html[cursor];
                if (quote == '\'' || quote == '"') {
                    size_t valueStart = ++cursor;
                    while (cursor < html.size() && html[cursor] != quote) ++cursor;
                    if (cursor == html.size()) return false;
                    value = html.substr(valueStart, cursor - valueStart);
                    ++cursor;
                } else {
                    size_t valueStart = cursor;
                    while (cursor < html.size() && !Space(html[cursor]) && html[cursor] != '>') ++cursor;
                    value = html.substr(valueStart, cursor - valueStart);
                }
            }
            if (tag.attributes.size() < 64) tag.attributes.emplace(std::move(key), std::move(value));
        }
        return false;
    }
    return false;
}
std::string Attribute(const Tag& tag, const std::string& name) {
    auto found = tag.attributes.find(name);
    return found == tag.attributes.end() ? std::string{} : found->second;
}
void AppendUtf8(std::string& output, uint32_t value) {
    if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return;
    if (value < 0x80) output += static_cast<char>(value);
    else if (value < 0x800) { output += static_cast<char>(0xc0 | (value >> 6)); output += static_cast<char>(0x80 | (value & 63)); }
    else if (value < 0x10000) { output += static_cast<char>(0xe0 | (value >> 12)); output += static_cast<char>(0x80 | ((value >> 6) & 63)); output += static_cast<char>(0x80 | (value & 63)); }
    else { output += static_cast<char>(0xf0 | (value >> 18)); output += static_cast<char>(0x80 | ((value >> 12) & 63)); output += static_cast<char>(0x80 | ((value >> 6) & 63)); output += static_cast<char>(0x80 | (value & 63)); }
}
std::string Entities(std::string_view text) {
    std::string result;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '&') { result += text[index]; continue; }
        size_t end = text.find(';', index + 1);
        if (end == std::string_view::npos || end - index > 16) { result += '&'; continue; }
        auto entity = text.substr(index + 1, end - index - 1);
        uint32_t value = 0;
        if (entity == "amp") value = '&';
        else if (entity == "lt") value = '<';
        else if (entity == "gt") value = '>';
        else if (entity == "quot") value = '"';
        else if (entity == "apos" || entity == "#39") value = '\'';
        else if (entity == "nbsp") value = ' ';
        else if (entity.starts_with('#')) {
            entity.remove_prefix(1);
            int base = 10;
            if (entity.starts_with('x') || entity.starts_with('X')) { entity.remove_prefix(1); base = 16; }
            auto parsed = std::from_chars(entity.data(), entity.data() + entity.size(), value, base);
            if (parsed.ec != std::errc{} || parsed.ptr != entity.data() + entity.size()) value = 0;
        }
        if (!value) { result.append(text.substr(index, end - index + 1)); index = end; continue; }
        AppendUtf8(result, value);
        index = end;
    }
    return result;
}
std::string Trim(std::string value) {
    size_t first = value.find_first_not_of(" \t\r\n\f");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n\f") - first + 1);
}
std::string Text(std::string_view html) {
    std::string text;
    bool tag = false;
    char quote = 0;
    for (char ch : html) {
        if (!tag) {
            if (ch == '<') { tag = true; text += ' '; }
            else text += ch;
        } else if (quote) {
            if (ch == quote) quote = 0;
        } else if (ch == '\'' || ch == '"') quote = ch;
        else if (ch == '>') tag = false;
    }
    return Trim(Entities(text));
}
bool FindEnd(const std::string& html, const std::string& lower, const std::string& name, size_t position, size_t& end, size_t& after) {
    unsigned depth = 1;
    Tag tag;
    while (NextTag(html, position, tag)) {
        if (!tag.closing && (tag.name == "script" || tag.name == "style") && tag.name != name) {
            auto close = lower.find("</" + tag.name, position);
            if (close == std::string::npos) return false;
            position = close;
            continue;
        }
        if (tag.name != name) continue;
        if (!tag.closing) { if (++depth > 32) return false; }
        else if (!--depth) { end = tag.begin; after = tag.end; return true; }
    }
    return false;
}
int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::wstring DecodePath(const std::wstring& path) {
    auto bytes = Utf8(path);
    std::string output;
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] != '%') { output += bytes[index]; continue; }
        if (index + 2 >= bytes.size()) return {};
        int high = Hex(bytes[index + 1]), low = Hex(bytes[index + 2]);
        if (high < 0 || low < 0) return {};
        output += static_cast<char>((high << 4) | low);
        index += 2;
    }
    return Wide(output);
}
std::wstring Suffix(std::wstring_view text) {
    wchar_t value[24]{};
    swprintf_s(value, L"~%016llx", static_cast<unsigned long long>(HashName(text)));
    return value;
}
std::wstring WindowsName(const std::wstring& input) {
    auto stem = Lower(input.substr(0, input.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    bool reserved = stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" || stem == L"conin$" || stem == L"conout$";
    if (stem.size() == 4 && (stem.starts_with(L"com") || stem.starts_with(L"lpt"))) {
        wchar_t last = stem.back();
        reserved = (last >= L'1' && last <= L'9') || last == L'\u00b9' || last == L'\u00b2' || last == L'\u00b3';
    }
    std::wstring result;
    for (size_t index = 0; index < input.size(); ++index) {
        wchar_t value = input[index];
        bool escape = value < 32 || std::wstring_view(L"<>:\"/\\|?*~").find(value) != std::wstring_view::npos || ((value == L'.' || value == L' ') && index + 1 == input.size()) || (reserved && !index);
        if (escape) {
            wchar_t encoded[8]{};
            swprintf_s(encoded, L"~%04x", static_cast<unsigned>(value));
            result += encoded;
        } else result += value;
    }
    if (result.size() > 220) {
        result.resize(200);
        if (result.back() >= 0xd800 && result.back() <= 0xdbff) result.pop_back();
        result += Suffix(input);
    }
    return result;
}
bool SortQuery(const std::wstring& query) {
    if (query == L"NA" || query == L"ND" || query == L"SA" || query == L"SD" || query == L"MA" || query == L"MD") return true;
    if (query.empty()) return false;
    size_t start = 0;
    while (start < query.size()) {
        auto end = query.find_first_of(L"&;", start);
        if (end == std::wstring::npos) end = query.size();
        auto item = query.substr(start, end - start);
        bool valid = item == L"C=N" || item == L"C=M" || item == L"C=S" || item == L"C=D" || item == L"O=A" || item == L"O=D";
        if (!valid) return false;
        start = end + 1;
    }
    return true;
}
bool SizeValue(std::string text, uint64_t& size) {
    text = Trim(text);
    text.erase(std::remove(text.begin(), text.end(), ','), text.end());
    if (text.empty()) return false;
    double number = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || !std::isfinite(number) || number < 0) return false;
    auto suffix = Lower(Trim(text.substr(static_cast<size_t>(parsed.ptr - text.data()))));
    if (suffix != "" && suffix != "b") {
        auto power = std::string_view("kmgtpe").find(suffix.front());
        if (power == std::string_view::npos || (suffix.size() > 1 && suffix.substr(1) != "b" && suffix.substr(1) != "ib")) return false;
        for (size_t index = 0; index <= power; ++index) number *= 1024.;
    }
    if (number >= 9.0e18) return false;
    size = static_cast<uint64_t>(number);
    return true;
}
std::vector<std::string> Words(const std::string& text) {
    std::vector<std::string> result;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && Space(text[position])) ++position;
        size_t start = position;
        while (position < text.size() && !Space(text[position])) ++position;
        if (position > start) result.push_back(text.substr(start, position - start));
    }
    return result;
}
struct Anchor {
    std::wstring link;
    size_t begin = 0;
    size_t end = 0;
};
std::vector<Anchor> Anchors(const std::string& html) {
    auto lower = Lower(html);
    size_t position = 0;
    Tag tag;
    std::vector<Anchor> result;
    while (NextTag(html, position, tag)) {
        if (!tag.closing && (tag.name == "script" || tag.name == "style")) {
            size_t end = 0, after = 0;
            if (!FindEnd(html, lower, tag.name, position, end, after)) break;
            position = after;
            continue;
        }
        if (tag.closing || tag.name != "a") continue;
        size_t end = 0, after = 0;
        if (!FindEnd(html, lower, "a", position, end, after)) continue;
        auto link = Wide(Entities(Attribute(tag, "href")));
        if (!link.empty() && !Text(std::string_view(html).substr(position, end - position)).empty()) result.push_back({std::move(link), tag.begin, after});
        position = after;
    }
    return result;
}
enum class Column { Unknown, Name, Size, Modified, Kind };
Column HeaderColumn(const std::string& text) {
    auto value = Lower(Trim(text));
    if (value == "name" || value.starts_with("name ") || value.starts_with("name&") || value.starts_with("file name") || value == "filename") return Column::Name;
    if (value.starts_with("size") || value.starts_with("file size")) return Column::Size;
    if (value.starts_with("last modified") || value.starts_with("date") || value == "modified") return Column::Modified;
    if (value == "type") return Column::Kind;
    return Column::Unknown;
}
Column ClassColumn(const Tag& tag) {
    for (const auto& value : Words(Lower(Attribute(tag, "class")))) {
        if (value == "n" || value == "link" || value == "filename") return Column::Name;
        if (value == "s" || value == "size") return Column::Size;
        if (value == "m" || value == "date" || value == "modified") return Column::Modified;
        if (value == "t" || value == "type") return Column::Kind;
    }
    return Column::Unknown;
}
struct Cell {
    Tag tag;
    std::string body;
    std::string text;
    unsigned span = 1;
};
std::vector<Cell> Cells(const std::string& row) {
    auto lower = Lower(row);
    size_t position = 0;
    Tag tag;
    std::vector<Cell> result;
    while (NextTag(row, position, tag)) {
        if (tag.closing || (tag.name != "td" && tag.name != "th")) continue;
        size_t end = 0, after = 0;
        if (!FindEnd(row, lower, tag.name, position, end, after)) break;
        Cell cell;
        cell.tag = tag;
        cell.body = row.substr(position, end - position);
        cell.text = Text(cell.body);
        uint64_t span = 0;
        if (ParseNumber(Wide(Attribute(tag, "colspan")), span) && span && span <= 16) cell.span = static_cast<unsigned>(span);
        result.push_back(std::move(cell));
        position = after;
        if (result.size() > 64) break;
    }
    return result;
}
int TableEntries(const std::string& html, const Tag& table, const std::wstring& url, bool heading, std::vector<IndexEntry>& entries) {
    auto lower = Lower(html);
    auto id = Lower(Attribute(table, "id"));
    bool known = id == "list" || id == "table-content" || Lower(Attribute(table, "summary")) == "directory listing";
    bool lite = id == "table-content";
    bool nameHeader = false, detailHeader = false;
    std::vector<Column> columns;
    size_t position = 0;
    Tag tag;
    while (NextTag(html, position, tag)) {
        if (tag.closing || tag.name != "tr") continue;
        size_t end = 0, after = 0;
        if (!FindEnd(html, lower, "tr", position, end, after)) break;
        auto cells = Cells(html.substr(position, end - position));
        position = after;
        bool header = std::any_of(cells.begin(), cells.end(), [](const Cell& cell) { return cell.tag.name == "th"; });
        if (header) {
            std::vector<Column> next;
            for (const auto& cell : cells) {
                auto column = HeaderColumn(cell.text);
                nameHeader |= column == Column::Name;
                detailHeader |= column == Column::Size || column == Column::Modified;
                for (unsigned index = 0; index < cell.span; ++index) next.push_back(column);
            }
            if (std::any_of(next.begin(), next.end(), [](Column value) { return value != Column::Unknown; })) columns = std::move(next);
            continue;
        }
        bool directory = Lower(Attribute(tag, "class")) == "d";
        std::wstring link, fallback, modified;
        std::string dateNumber;
        uint64_t size = 0;
        size_t columnIndex = 0;
        for (const auto& cell : cells) {
            auto column = ClassColumn(cell.tag);
            if (column == Column::Unknown && columnIndex < columns.size()) column = columns[columnIndex];
            columnIndex += cell.span;
            auto links = Anchors(cell.body);
            if (!links.empty()) {
                if (fallback.empty()) fallback = links.front().link;
                if (column == Column::Name) link = links.front().link;
            }
            switch (column) {
            case Column::Size: {
                auto precise = Attribute(cell.tag, "data-value");
                if (precise.empty()) precise = Attribute(cell.tag, "data-sort");
                if (precise.empty() || !SizeValue(precise, size)) SizeValue(cell.text, size);
                break;
            }
            case Column::Modified: modified = Wide(cell.text); dateNumber = Attribute(cell.tag, "data-sort"); break;
            case Column::Kind: directory |= Lower(cell.text) == "directory" || Lower(cell.text) == "<dir>"; break;
            default: break;
            }
        }
        if (link.empty()) link = std::move(fallback);
        if (link.empty()) continue;
        directory |= link.substr(0, link.find(L'?')).ends_with(L'/');
        uint64_t time = ParseIndexTime(modified);
        uint64_t seconds = 0;
        if (!time && lite && !directory && ParseNumber(Wide(dateNumber), seconds) && seconds < 253402300800ull) time = (seconds + 11644473600ull) * 10000000ull;
        AddIndexEntry(entries, url, std::move(link), directory, size, time);
        if (entries.size() > 100000) break;
    }
    if (known && nameHeader && detailHeader) return 100;
    if (heading && nameHeader && detailHeader) return 80;
    return heading ? 40 : 0;
}
void PlainEntries(const std::string& html, const std::wstring& url, bool iis, std::vector<IndexEntry>& entries) {
    auto lower = Lower(html);
    for (const auto& anchor : Anchors(html)) {
        size_t start = 0;
        auto newline = html.rfind('\n', anchor.begin);
        if (newline != std::string::npos) start = newline + 1;
        auto br = lower.rfind("<br", anchor.begin);
        if (br != std::string::npos) {
            auto close = html.find('>', br);
            if (close < anchor.begin) start = std::max(start, close + 1);
        }
        auto end = html.find('\n', anchor.end);
        if (end == std::string::npos) end = html.size();
        br = lower.find("<br", anchor.end);
        if (br != std::string::npos) end = std::min(end, br);
        auto prefix = Text(std::string_view(html).substr(start, anchor.begin - start));
        auto suffix = Text(std::string_view(html).substr(anchor.end, end - anchor.end));
        bool directory = iis && Lower(prefix).find("<dir>") != std::string::npos;
        uint64_t size = 0;
        auto words = Words(iis ? prefix : suffix);
        if (iis) {
            bool sized = words.size() == 1 || prefix.find(':') != std::string::npos || (!words.empty() && words.front().find_first_of("/-") != std::string::npos);
            if (sized && !words.empty() && !directory) SizeValue(words.back(), size);
        } else {
            for (size_t index = 0; index < words.size(); ++index) {
                if (words[index].find(':') != std::string::npos && index + 1 < words.size()) { SizeValue(words[index + 1], size); break; }
            }
        }
        AddIndexEntry(entries, url, anchor.link, directory, size);
        if (entries.size() > 100000) break;
    }
}
Result ParseHtml(const std::vector<unsigned char>& bytes, const std::wstring& url, std::vector<IndexEntry>& entries) {
    std::string html(bytes.begin(), bytes.end());
    auto lower = Lower(html);
    UrlParts parts;
    auto result = SplitUrl(url, parts);
    if (!result) return result;
    auto expected = DecodePath(parts.path);
    while (expected.size() > 1 && expected.back() == L'/') expected.pop_back();
    bool heading = false, iis = false;
    size_t position = 0;
    Tag tag;
    while (NextTag(html, position, tag)) {
        if (tag.closing) continue;
        if (tag.name == "script" || tag.name == "style") {
            size_t end = 0, after = 0;
            if (!FindEnd(html, lower, tag.name, position, end, after)) break;
            position = after;
            continue;
        }
        if (tag.name != "title" && tag.name != "h1" && tag.name != "h2") continue;
        size_t end = 0, after = 0;
        if (!FindEnd(html, lower, tag.name, position, end, after)) continue;
        auto text = Text(std::string_view(html).substr(position, end - position));
        heading |= Lower(text).starts_with("index of ");
        auto wide = Wide(text);
        auto split = wide.find(L" - /");
        if (split != std::wstring::npos && split) {
            auto path = wide.substr(split + 3);
            while (path.size() > 1 && path.back() == L'/') path.pop_back();
            iis |= path == expected;
        }
        position = after;
    }
    int best = 0;
    position = 0;
    while (NextTag(html, position, tag)) {
        if (tag.closing) continue;
        size_t end = 0, after = 0;
        if (tag.name == "script" || tag.name == "style") {
            if (!FindEnd(html, lower, tag.name, position, end, after)) break;
            position = after;
            continue;
        }
        if (tag.name != "table" && tag.name != "pre" && tag.name != "ul") continue;
        if (!FindEnd(html, lower, tag.name, position, end, after)) continue;
        std::vector<IndexEntry> candidate;
        auto body = html.substr(position, end - position);
        int score = 0;
        if (tag.name == "table") score = TableEntries(body, tag, url, heading, candidate);
        else if (heading || (iis && tag.name == "pre")) {
            PlainEntries(body, url, iis && !heading, candidate);
            score = iis && !heading ? 90 : 40;
        }
        if (score > best) { entries = std::move(candidate); best = score; }
        position = after;
    }
    return best ? Result{} : Fail(ERROR_BAD_FORMAT, L"Page is not a supported directory listing");
}
}
bool AddIndexEntry(std::vector<IndexEntry>& entries, const std::wstring& base, std::wstring link, bool directory, uint64_t size, uint64_t modified) {
    if (entries.size() >= 100001 || link.empty() || link.find(L'#') != std::wstring::npos) return false;
    auto query = link.find(L'?');
    if (query != std::wstring::npos) {
        if (!SortQuery(link.substr(query + 1))) return false;
        link.resize(query);
        if (link.empty()) return false;
    }
    UrlParts parent, child;
    std::wstring target;
    if (!SplitUrl(base, parent) || !CombineUrl(base, link, target) || !WithinRoot(base, target) || !SplitUrl(target, child)) return false;
    auto relative = child.path.substr(parent.path.size());
    bool slash = relative.ends_with(L'/');
    if (slash) relative.pop_back();
    if (relative.empty() || relative.find(L'/') != std::wstring::npos) return false;
    auto name = DecodePath(relative);
    if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"/\\") != std::wstring::npos || name.find(L'\0') != std::wstring::npos) return false;
    directory |= slash;
    if (directory && !slash) target += L'/';
    entries.push_back({WindowsName(name), std::move(target), directory, directory ? 0 : size, modified});
    return true;
}
Result ParseIndex(const std::vector<unsigned char>& bytes, const std::wstring& url, const std::wstring& contentType, std::vector<IndexEntry>& entries) {
    entries.clear();
    if (bytes.empty() || bytes.size() > 16 * 1024 * 1024) return Fail(ERROR_BAD_FORMAT, L"Directory response is empty or too large");
    UrlParts parent;
    auto result = SplitUrl(url, parent);
    if (!result) return result;
    if (!parent.path.ends_with(L'/') || parent.path.find(L'?') != std::wstring::npos) return Fail(ERROR_BAD_FORMAT, L"Index URL did not resolve to a directory");
    size_t start = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    while (start < bytes.size() && Space(static_cast<char>(bytes[start]))) ++start;
    auto mime = Lower(contentType.substr(0, contentType.find(L';')));
    std::string_view body(reinterpret_cast<const char*>(bytes.data()) + start, bytes.size() - start);
    if (body.starts_with('[') || mime == L"application/json") result = ParseJsonIndex(bytes, url, entries);
    else if (body.starts_with("<?xml") || body.starts_with("<list") || mime == L"text/xml" || mime == L"application/xml") result = ParseXmlIndex(bytes, url, entries);
    else result = ParseHtml(bytes, url, entries);
    if (!result) { entries.clear(); return result; }
    if (entries.size() > 100000) { entries.clear(); return Fail(ERROR_NOT_ENOUGH_MEMORY, L"Directory exceeds the 100000-entry limit"); }
    std::set<std::wstring> seen;
    std::erase_if(entries, [&](const IndexEntry& entry) { return !seen.insert(entry.url).second; });
    std::sort(entries.begin(), entries.end(), [](const IndexEntry& left, const IndexEntry& right) { int order = NameCompare(left.name, right.name); return order ? order < 0 : left.url < right.url; });
    for (size_t first = 0; first < entries.size();) {
        size_t end = first + 1;
        while (end < entries.size() && !NameCompare(entries[first].name, entries[end].name)) ++end;
        if (end - first > 1) for (size_t index = first; index < end; ++index) entries[index].name += Suffix(entries[index].url);
        first = end;
    }
    std::sort(entries.begin(), entries.end(), [](const IndexEntry& left, const IndexEntry& right) { return NameCompare(left.name, right.name) < 0; });
    return {};
}
}
