#include "mountCache.h"

namespace webmount {
namespace {
constexpr size_t cacheLimit = 64ull * 1024 * 1024;
constexpr uint64_t blockSize = 1024 * 1024;
bool ParseRange(const std::wstring& text, uint64_t& first, uint64_t& last, uint64_t& total) {
    if (!text.starts_with(L"bytes ")) return false;
    auto dash = text.find(L'-', 6), slash = text.find(L'/', 6);
    if (dash == std::wstring::npos || slash == std::wstring::npos || dash > slash) return false;
    return ParseNumber(std::wstring_view(text).substr(6, dash - 6), first) && ParseNumber(std::wstring_view(text).substr(dash + 1, slash - dash - 1), last) && ParseNumber(std::wstring_view(text).substr(slash + 1), total) && first <= last && last < total;
}
uint64_t HeaderTime(const std::wstring& text) {
    SYSTEMTIME system{};
    FILETIME file{};
    if (!text.empty() && WinHttpTimeToSystemTime(text.c_str(), &system) && SystemTimeToFileTime(&system, &file)) return (uint64_t(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    return 0;
}
}
FSP_FSCTL_FILE_INFO NodeInfo(const FileNode& node, const FileMeta* meta) {
    FSP_FSCTL_FILE_INFO info{};
    info.FileAttributes = node.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    info.FileSize = node.directory ? 0 : meta ? meta->size : node.listedSize;
    info.AllocationSize = (info.FileSize + 4095) & ~uint64_t(4095);
    info.CreationTime = node.created;
    info.LastWriteTime = meta && meta->modified ? meta->modified : node.listedModified ? node.listedModified : node.created;
    info.LastAccessTime = info.ChangeTime = info.LastWriteTime;
    info.IndexNumber = HashName(node.url);
    return info;
}
MountCache::~MountCache() {
    Cancel();
    if (prefetch.joinable()) prefetch.join();
}
void MountCache::Cancel() {
    {
        std::lock_guard lock(queueMutex);
        stopping = true;
        pending.clear();
        scheduled.clear();
    }
    http.Cancel();
    queueReady.notify_all();
}
void MountCache::StartPrefetch() {
    {
        std::lock_guard lock(queueMutex);
        if (stopping || prefetch.joinable()) return;
        prefetch = std::thread(&MountCache::PrefetchLoop, this);
    }
    std::shared_ptr<DirSlot> slot;
    {
        std::lock_guard lock(dirMutex);
        auto found = directories.find(root->url);
        if (found != directories.end()) slot = found->second;
    }
    if (!slot) return;
    std::shared_ptr<DirView> view;
    {
        std::lock_guard lock(slot->mutex);
        view = slot->view;
    }
    if (view) QueueChildren(root, view);
}
Result MountCache::Browse(const std::shared_ptr<FileNode>& node, std::shared_ptr<DirView>& view) {
    auto result = Directory(node, view);
    if (result) QueueChildren(node, view);
    return result;
}
void MountCache::QueueChildren(const std::shared_ptr<FileNode>& node, const std::shared_ptr<DirView>& view) {
    std::lock_guard lock(queueMutex);
    if (stopping) return;
    auto previous = scheduled.find(node->url);
    if (previous != scheduled.end() && previous->second.lock() == view) return;
    if (!std::any_of(view->entries.begin(), view->entries.end(), [](const auto& entry) { return entry->directory; })) return;
    if (pending.size() >= 16) {
        scheduled.erase(pending.back().key);
        pending.pop_back();
    }
    if (scheduled.size() >= 256) scheduled.erase(scheduled.begin());
    scheduled[node->url] = view;
    pending.push_front(PrefetchBatch{node->url, view, 0});
    for (const auto& entry : view->entries) if (entry->directory) SetItem(entry->url, entry->display, ItemState::Queued, L"directory listing");
    queueReady.notify_one();
}
void MountCache::PrefetchLoop() {
    for (;;) {
        std::shared_ptr<FileNode> node;
        {
            std::unique_lock lock(queueMutex);
            queueReady.wait(lock, [&] { return stopping || !pending.empty(); });
            if (stopping) return;
            while (!pending.empty() && !node) {
                auto& batch = pending.front();
                while (batch.next < batch.view->entries.size()) {
                    auto entry = batch.view->entries[batch.next++];
                    if (entry->directory) { node = std::move(entry); break; }
                }
                if (batch.next == batch.view->entries.size()) pending.pop_front();
            }
        }
        if (!node) continue;
        std::shared_ptr<DirView> view;
        auto result = Directory(node, view);
        if (!result && result.code != ERROR_OPERATION_ABORTED) SetItem(node->url, node->display, ItemState::Error, result.message);
    }
}
Result MountCache::Initialize(const std::wstring& url) {
    auto result = http.Initialize();
    if (!result) return result;
    root = std::make_shared<FileNode>();
    root->name = L"\\";
    root->display = L"/";
    root->url = url;
    root->directory = true;
    std::shared_ptr<DirView> view;
    result = FetchDirectory(root, view, true);
    if (!result) return result;
    auto slot = std::make_shared<DirSlot>();
    slot->view = std::move(view);
    slot->fetched = slot->used = GetTickCount64();
    directories[root->url] = std::move(slot);
    return {};
}
Result MountCache::FetchDirectory(const std::shared_ptr<FileNode>& node, std::shared_ptr<DirView>& view, bool initial) {
    SetItem(node->url, node->display, ItemState::Fetching, L"directory listing");
    HttpReply reply;
    auto result = http.Request(L"GET", node->url, L"Accept: application/json, text/html;q=0.9, application/xml;q=0.8, text/xml;q=0.8\r\n", 16 * 1024 * 1024, false, initial ? L"" : root->url, reply);
    if (!result) return result;
    if (reply.status == 406) {
        result = http.Request(L"GET", node->url, L"Accept: text/html, */*;q=0.8\r\n", 16 * 1024 * 1024, false, initial ? L"" : root->url, reply);
        if (!result) return result;
    }
    result = HttpStatus(reply.status);
    if (!result) return result;
    if (reply.status != 200) return Fail(ERROR_BAD_NET_RESP, L"Directory request returned a partial response");
    std::vector<IndexEntry> entries;
    result = ParseIndex(reply.bytes, reply.url, reply.type, entries);
    if (!result) return result;
    if (initial) node->url = reply.url;
    auto next = std::make_shared<DirView>();
    next->entries.reserve(entries.size());
    for (auto& entry : entries) {
        auto child = std::make_shared<FileNode>();
        child->name = std::move(entry.name);
        child->display = node->display + child->name + (entry.directory ? L"/" : L"");
        child->url = std::move(entry.url);
        child->directory = entry.directory;
        child->listedSize = entry.size;
        child->listedModified = entry.modified;
        child->parent = node;
        SetItem(child->url, child->display, ItemState::Listed, entry.directory ? L"directory" : L"file");
        next->entries.push_back(std::move(child));
    }
    SetItem(node->url, node->display, ItemState::Cached, std::to_wstring(next->entries.size()) + L" entries");
    view = std::move(next);
    return {};
}
Result MountCache::Directory(const std::shared_ptr<FileNode>& node, std::shared_ptr<DirView>& view) {
    if (!node->directory) return Fail(ERROR_DIRECTORY, L"Entry is not a directory");
    std::shared_ptr<DirSlot> slot;
    {
        std::lock_guard lock(dirMutex);
        auto found = directories.find(node->url);
        if (found != directories.end()) slot = found->second;
        else {
            if (directories.size() >= 256) {
                auto oldest = directories.end();
                for (auto entry = directories.begin(); entry != directories.end(); ++entry) {
                    if (entry->second.use_count() != 1 || entry->first == root->url) continue;
                    if (oldest == directories.end() || entry->second->used < oldest->second->used) oldest = entry;
                }
                if (oldest != directories.end()) directories.erase(oldest);
            }
            slot = std::make_shared<DirSlot>();
            directories[node->url] = slot;
        }
        slot->used = GetTickCount64();
    }
    std::lock_guard lock(slot->mutex);
    if (slot->view && GetTickCount64() - slot->fetched < 60000) {
        view = slot->view;
        SetItem(node->url, node->display, ItemState::Cached, std::to_wstring(view->entries.size()) + L" entries");
        return {};
    }
    auto result = FetchDirectory(node, view, false);
    if (!result) {
        if (result.code != ERROR_OPERATION_ABORTED) SetItem(node->url, node->display, ItemState::Error, result.message);
        return result;
    }
    slot->view = view;
    slot->fetched = GetTickCount64();
    return {};
}
Result MountCache::Resolve(const std::wstring& path, std::shared_ptr<FileNode>& node) {
    node = root;
    size_t position = 0;
    while (position < path.size()) {
        while (position < path.size() && path[position] == L'\\') ++position;
        if (position == path.size()) return {};
        auto end = path.find(L'\\', position);
        if (end == std::wstring::npos) end = path.size();
        auto name = path.substr(position, end - position);
        if (name == L"." || name == L".." || name.find(L':') != std::wstring::npos) return Fail(ERROR_INVALID_NAME, L"Invalid filesystem path");
        std::shared_ptr<DirView> view;
        auto result = Directory(node, view);
        if (!result) return result;
        auto found = std::lower_bound(view->entries.begin(), view->entries.end(), name, [](const auto& entry, const auto& key) { return NameCompare(entry->name, key) < 0; });
        if (found == view->entries.end() || NameCompare((*found)->name, name)) return Fail(end == path.size() ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND, L"Entry does not exist");
        node = *found;
        position = end;
    }
    return {};
}
Result MountCache::Metadata(const std::shared_ptr<FileNode>& node, FileMeta& meta) {
    if (node->directory) { meta = {}; return {}; }
    std::lock_guard lock(node->mutex);
    if (node->metadata && GetTickCount64() - node->metadataTime < 60000) { meta = *node->metadata; return {}; }
    HttpReply reply;
    auto result = http.Request(L"HEAD", node->url, L"", 0, true, root->url, reply);
    if (!result) return result;
    uint64_t size = 0;
    bool sized = reply.status == 200 && ParseNumber(reply.length, size);
    if (!sized) {
        if (reply.status != 200 && reply.status != 405 && reply.status != 501) return HttpStatus(reply.status);
        result = http.Request(L"GET", node->url, L"Range: bytes=0-0\r\n", 0, true, root->url, reply);
        if (!result) return result;
        switch (reply.status) {
        case 206: {
            uint64_t first = 0, last = 0;
            sized = ParseRange(reply.range, first, last, size) && first == 0 && last == 0;
            break;
        }
        case 200: sized = ParseNumber(reply.length, size); break;
        case 416: sized = reply.range == L"bytes */0"; size = 0; break;
        default: return HttpStatus(reply.status);
        }
    }
    if (!reply.encoding.empty() && Lower(reply.encoding) != L"identity") return Fail(ERROR_NOT_SUPPORTED, L"Server does not expose identity-encoded file metadata");
    if (!sized) return Fail(ERROR_NOT_SUPPORTED, L"Server does not provide an exact file size");
    if (size > INT64_MAX - 4095) return Fail(ERROR_FILE_TOO_LARGE, L"File exceeds supported Windows file size");
    meta.size = size;
    meta.modified = HeaderTime(reply.modified);
    meta.etag = reply.etag.starts_with(L"W/") ? L"" : reply.etag;
    meta.modifiedHeader = reply.modified;
    meta.cacheKey = node->url + L"\n" + meta.etag + L"\n" + meta.modifiedHeader + L"\n" + std::to_wstring(size);
    if (meta.etag.empty() && meta.modifiedHeader.empty()) meta.cacheKey += L"\n" + std::to_wstring(GetTickCount64());
    node->metadata = meta;
    node->metadataTime = GetTickCount64();
    SetItem(node->url, node->display, ItemState::Cached, L"metadata: " + std::to_wstring(size) + L" bytes");
    return {};
}
std::shared_ptr<std::vector<unsigned char>> MountCache::FindBlock(const std::wstring& key) {
    std::lock_guard lock(dataMutex);
    auto found = blocks.find(key);
    if (found == blocks.end()) return {};
    found->second.used = GetTickCount64();
    return found->second.bytes;
}
void MountCache::StoreBlock(const std::wstring& key, std::shared_ptr<std::vector<unsigned char>> bytes) {
    if (bytes->size() > cacheLimit) return;
    std::lock_guard lock(dataMutex);
    auto found = blocks.find(key);
    if (found != blocks.end()) { cachedBytes -= found->second.bytes->size(); blocks.erase(found); }
    while (!blocks.empty() && cachedBytes + bytes->size() > cacheLimit) {
        auto oldest = std::min_element(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) { return left.second.used < right.second.used; });
        cachedBytes -= oldest->second.bytes->size();
        blocks.erase(oldest);
    }
    cachedBytes += bytes->size();
    blocks.emplace(key, DataSlot{std::move(bytes), GetTickCount64()});
}
Result MountCache::Read(const std::shared_ptr<FileNode>& node, const FileMeta& meta, uint64_t offset, ULONG length, void* buffer, ULONG& transferred) {
    transferred = 0;
    if (offset >= meta.size || !length) return {};
    uint64_t remaining = std::min<uint64_t>(length, meta.size - offset);
    while (remaining) {
        uint64_t start = offset - offset % blockSize;
        uint64_t end = std::min<uint64_t>(meta.size - 1, start + blockSize - 1);
        auto fullKey = meta.cacheKey + L"\nfull";
        auto blockKey = meta.cacheKey + L"\n" + std::to_wstring(start);
        auto bytes = FindBlock(fullKey);
        bool full = static_cast<bool>(bytes);
        if (!bytes) bytes = FindBlock(blockKey);
        if (!bytes) {
            std::wstring headers = L"Range: bytes=" + std::to_wstring(start) + L"-" + std::to_wstring(end) + L"\r\n";
            if (!meta.etag.empty()) headers += L"If-Match: " + meta.etag + L"\r\n";
            else if (!meta.modifiedHeader.empty()) headers += L"If-Unmodified-Since: " + meta.modifiedHeader + L"\r\n";
            HttpReply reply;
            auto result = http.Request(L"GET", node->url, headers, cacheLimit, false, root->url, reply);
            if (!result) return result;
            result = HttpStatus(reply.status);
            if (!result) return result;
            if ((!meta.etag.empty() && !reply.etag.empty() && meta.etag != reply.etag) || (!meta.modifiedHeader.empty() && !reply.modified.empty() && meta.modifiedHeader != reply.modified)) return Fail(ERROR_FILE_INVALID, L"Remote file changed while reading");
            switch (reply.status) {
            case 206: {
                uint64_t actualStart = 0, actualEnd = 0, total = 0;
                if (!ParseRange(reply.range, actualStart, actualEnd, total) || actualStart != start || actualEnd != end || total != meta.size || reply.bytes.size() != end - start + 1) return Fail(ERROR_BAD_NET_RESP, L"Server returned an incorrect byte range");
                break;
            }
            case 200:
                if (reply.bytes.size() != meta.size) return Fail(ERROR_FILE_INVALID, L"Full response does not match the opened file size");
                full = true;
                SetItem(node->url, node->display, ItemState::Cached, L"whole file: " + std::to_wstring(meta.size) + L" bytes");
                break;
            default: return Fail(ERROR_BAD_NET_RESP, L"Unsupported read response");
            }
            bytes = std::make_shared<std::vector<unsigned char>>(std::move(reply.bytes));
            StoreBlock(full ? fullKey : blockKey, bytes);
        }
        uint64_t local = full ? offset : offset - start;
        if (local >= bytes->size()) return Fail(ERROR_BAD_NET_RESP, L"Cached file data is incomplete");
        size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, bytes->size() - local));
        memcpy(static_cast<unsigned char*>(buffer) + transferred, bytes->data() + local, count);
        transferred += static_cast<ULONG>(count);
        remaining -= count;
        offset += count;
    }
    SetItem(node->url, node->display, ItemState::Cached, std::to_wstring(meta.size) + L" bytes; last read " + std::to_wstring(transferred));
    return {};
}
}
