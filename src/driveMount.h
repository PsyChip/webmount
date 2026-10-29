#pragma once
#include "mountCache.h"
#include "mountControl.h"
#include "runtimeSetup.h"

namespace webmount {
Result CheckLetter(wchar_t letter);
class DriveMount {
public:
    DriveMount(MountCache& cache, MountControl& control);
    ~DriveMount();
    Result Start(wchar_t requested = 0);
    void Stop();
    wchar_t Letter() const { return letter; }
private:
    struct OpenFile {
        std::shared_ptr<FileNode> node;
        FileMeta metadata;
        std::mutex mutex;
        std::shared_ptr<DirView> view;
    };
    static DriveMount& Self(FSP_FILE_SYSTEM* fileSystem);
    static NTSTATUS VolumeInfo(FSP_FILE_SYSTEM* fileSystem, FSP_FSCTL_VOLUME_INFO* info);
    static NTSTATUS SecurityName(FSP_FILE_SYSTEM* fileSystem, PWSTR name, PUINT32 attributes, PSECURITY_DESCRIPTOR security, SIZE_T* size);
    static NTSTATUS Create(FSP_FILE_SYSTEM* fileSystem, PWSTR name, UINT32 options, UINT32 access, UINT32 attributes, PSECURITY_DESCRIPTOR security, UINT64 allocation, PVOID* context, FSP_FSCTL_FILE_INFO* info);
    static NTSTATUS Open(FSP_FILE_SYSTEM* fileSystem, PWSTR name, UINT32 options, UINT32 access, PVOID* context, FSP_FSCTL_FILE_INFO* info);
    static NTSTATUS Overwrite(FSP_FILE_SYSTEM* fileSystem, PVOID context, UINT32 attributes, BOOLEAN replace, UINT64 allocation, FSP_FSCTL_FILE_INFO* info);
    static VOID Close(FSP_FILE_SYSTEM* fileSystem, PVOID context);
    static NTSTATUS Read(FSP_FILE_SYSTEM* fileSystem, PVOID context, PVOID buffer, UINT64 offset, ULONG length, PULONG transferred);
    static NTSTATUS FileInfo(FSP_FILE_SYSTEM* fileSystem, PVOID context, FSP_FSCTL_FILE_INFO* info);
    static NTSTATUS Security(FSP_FILE_SYSTEM* fileSystem, PVOID context, PSECURITY_DESCRIPTOR security, SIZE_T* size);
    static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* fileSystem, PVOID context, PWSTR pattern, PWSTR marker, PVOID buffer, ULONG length, PULONG transferred);
    static NTSTATUS DirectoryEntry(FSP_FILE_SYSTEM* fileSystem, PVOID context, PWSTR name, FSP_FSCTL_DIR_INFO* info);
    static VOID DispatcherStopped(FSP_FILE_SYSTEM* fileSystem, BOOLEAN normally);
    NTSTATUS CopySecurity(PSECURITY_DESCRIPTOR target, SIZE_T* size);
    MountCache& cache;
    MountControl& control;
    FSP_FILE_SYSTEM* fileSystem = nullptr;
    FSP_FILE_SYSTEM_INTERFACE interface{};
    PSECURITY_DESCRIPTOR security = nullptr;
    wchar_t letter = 0;
};
}
