#pragma once
#include "apacheIndex.h"
#include <condition_variable>
#include <deque>

namespace webmount {
struct FileMeta {
    uint64_t size = 0;
    uint64_t modified = 0;
    std::wstring etag;
    std::wstring modifiedHeader;
    std::wstring cacheKey;
};
struct FileNode {
    std::wstring name;
    std::wstring display;
    std::wstring url;
    bool directory = false;
    uint64_t listedSize = 0;
    uint64_t listedModified = 0;
    uint64_t created = NowFileTime();
    std::weak_ptr<FileNode> parent;
    std::mutex mutex;
    std::optional<FileMeta> metadata;
    ULONGLONG metadataTime = 0;
};
struct DirView {
    std::vector<std::shared_ptr<FileNode>> entries;
};
class MountCache {
public:
    ~MountCache();
    Result Initialize(const std::wstring& url);
    void StartPrefetch();
    Result Browse(const std::shared_ptr<FileNode>& node, std::shared_ptr<DirView>& view);
    Result Resolve(const std::wstring& path, std::shared_ptr<FileNode>& node);
    Result Directory(const std::shared_ptr<FileNode>& node, std::shared_ptr<DirView>& view);
    Result Metadata(const std::shared_ptr<FileNode>& node, FileMeta& meta);
    Result Read(const std::shared_ptr<FileNode>& node, const FileMeta& meta, uint64_t offset, ULONG length, void* buffer, ULONG& transferred);
    void Cancel();
    const std::wstring& RootUrl() const { return root->url; }
    const std::shared_ptr<FileNode>& Root() const { return root; }
private:
    struct DirSlot {
        std::mutex mutex;
        std::shared_ptr<DirView> view;
        ULONGLONG fetched = 0;
        ULONGLONG used = 0;
    };
    struct DataSlot {
        std::shared_ptr<std::vector<unsigned char>> bytes;
        ULONGLONG used = 0;
    };
    struct PrefetchBatch {
        std::wstring key;
        std::shared_ptr<DirView> view;
        size_t next = 0;
    };
    void QueueChildren(const std::shared_ptr<FileNode>& node, const std::shared_ptr<DirView>& view);
    void PrefetchLoop();
    Result FetchDirectory(const std::shared_ptr<FileNode>& node, std::shared_ptr<DirView>& view, bool initial);
    std::shared_ptr<std::vector<unsigned char>> FindBlock(const std::wstring& key);
    void StoreBlock(const std::wstring& key, std::shared_ptr<std::vector<unsigned char>> bytes);
    HttpClient http;
    std::shared_ptr<FileNode> root;
    std::mutex dirMutex;
    std::map<std::wstring, std::shared_ptr<DirSlot>> directories;
    std::mutex dataMutex;
    std::map<std::wstring, DataSlot> blocks;
    size_t cachedBytes = 0;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<PrefetchBatch> pending;
    std::map<std::wstring, std::weak_ptr<DirView>> scheduled;
    std::thread prefetch;
    bool stopping = false;
};
FSP_FSCTL_FILE_INFO NodeInfo(const FileNode& node, const FileMeta* meta = nullptr);
}
