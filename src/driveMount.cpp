#include "driveMount.h"
#include <sddl.h>
#include <new>

namespace webmount {
Result CheckLetter(wchar_t letter) {
    if (!letter) return {};
    if (letter < L'A' || letter > L'Z') return Fail(ERROR_INVALID_PARAMETER, L"Invalid drive letter");
    DWORD occupied = GetLogicalDrives();
    if (!occupied) return LastError(L"Cannot inspect drive letters");
    wchar_t name[]{letter, L':', L'\0'};
    wchar_t target[1024]{};
    if ((occupied & (1u << (letter - L'A'))) || QueryDosDeviceW(name, target, static_cast<DWORD>(std::size(target)))) return Fail(ERROR_ALREADY_ASSIGNED, std::wstring(L"Drive ") + name + L" is already in use");
    DWORD error = GetLastError();
    if (error == ERROR_INSUFFICIENT_BUFFER) return Fail(ERROR_ALREADY_ASSIGNED, std::wstring(L"Drive ") + name + L" is already in use");
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return Fail(error, L"Cannot inspect the requested drive letter");
    return {};
}
DriveMount::DriveMount(MountCache& source, MountControl& owner) : cache(source), control(owner) {}
DriveMount::~DriveMount() { Stop(); if (security) LocalFree(security); }
DriveMount& DriveMount::Self(FSP_FILE_SYSTEM* value) { return *static_cast<DriveMount*>(value->UserContext); }
NTSTATUS DriveMount::CopySecurity(PSECURITY_DESCRIPTOR target, SIZE_T* size) {
    if (!size) return STATUS_SUCCESS;
    SIZE_T needed = GetSecurityDescriptorLength(security);
    SIZE_T capacity = *size;
    *size = needed;
    if (!target) return STATUS_SUCCESS;
    if (capacity < needed) return STATUS_BUFFER_OVERFLOW;
    memcpy(target, security, needed);
    return STATUS_SUCCESS;
}
NTSTATUS DriveMount::VolumeInfo(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO* info) {
    *info = {};
    constexpr wchar_t label[] = L"WebMount";
    info->VolumeLabelLength = sizeof(label) - sizeof(wchar_t);
    memcpy(info->VolumeLabel, label, sizeof(label));
    return STATUS_SUCCESS;
}
NTSTATUS DriveMount::SecurityName(FSP_FILE_SYSTEM* value, PWSTR name, PUINT32 attributes, PSECURITY_DESCRIPTOR target, SIZE_T* size) {
    auto& self = Self(value);
    std::shared_ptr<FileNode> node;
    auto result = self.cache.Resolve(name, node);
    if (!result) return fsp.status(result.code);
    if (attributes) *attributes = node->directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    return self.CopySecurity(target, size);
}
NTSTATUS DriveMount::Create(FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, UINT32, PSECURITY_DESCRIPTOR, UINT64, PVOID* context, FSP_FSCTL_FILE_INFO*) {
    *context = nullptr;
    return STATUS_MEDIA_WRITE_PROTECTED;
}
NTSTATUS DriveMount::Overwrite(FSP_FILE_SYSTEM*, PVOID, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO*) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}
NTSTATUS DriveMount::Open(FSP_FILE_SYSTEM* value, PWSTR name, UINT32 options, UINT32 access, PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    *context = nullptr;
    if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER)) return STATUS_MEDIA_WRITE_PROTECTED;
    if (options & FILE_DELETE_ON_CLOSE) return STATUS_MEDIA_WRITE_PROTECTED;
    auto& self = Self(value);
    auto file = std::unique_ptr<OpenFile>(new(std::nothrow) OpenFile);
    if (!file) return STATUS_INSUFFICIENT_RESOURCES;
    auto result = self.cache.Resolve(name, file->node);
    if (!result) return fsp.status(result.code);
    if ((options & FILE_DIRECTORY_FILE) && !file->node->directory) return STATUS_NOT_A_DIRECTORY;
    if ((options & FILE_NON_DIRECTORY_FILE) && file->node->directory) return STATUS_FILE_IS_A_DIRECTORY;
    result = self.cache.Metadata(file->node, file->metadata);
    if (!result) { LogError(result); return fsp.status(result.code); }
    *info = NodeInfo(*file->node, &file->metadata);
    *context = file.release();
    return STATUS_SUCCESS;
}
VOID DriveMount::Close(FSP_FILE_SYSTEM*, PVOID context) { delete static_cast<OpenFile*>(context); }
NTSTATUS DriveMount::Read(FSP_FILE_SYSTEM* value, PVOID context, PVOID buffer, UINT64 offset, ULONG length, PULONG transferred) {
    auto file = static_cast<OpenFile*>(context);
    *transferred = 0;
    if (file->node->directory) return STATUS_FILE_IS_A_DIRECTORY;
    if (offset >= file->metadata.size) return STATUS_END_OF_FILE;
    auto result = Self(value).cache.Read(file->node, file->metadata, offset, length, buffer, *transferred);
    if (!result) { LogError(result); return fsp.status(result.code); }
    return STATUS_SUCCESS;
}
NTSTATUS DriveMount::FileInfo(FSP_FILE_SYSTEM*, PVOID context, FSP_FSCTL_FILE_INFO* info) {
    auto file = static_cast<OpenFile*>(context);
    *info = NodeInfo(*file->node, &file->metadata);
    return STATUS_SUCCESS;
}
NTSTATUS DriveMount::Security(FSP_FILE_SYSTEM* value, PVOID, PSECURITY_DESCRIPTOR target, SIZE_T* size) { return Self(value).CopySecurity(target, size); }
NTSTATUS DriveMount::ReadDirectory(FSP_FILE_SYSTEM* value, PVOID context, PWSTR, PWSTR marker, PVOID buffer, ULONG length, PULONG transferred) {
    auto& self = Self(value);
    auto file = static_cast<OpenFile*>(context);
    *transferred = 0;
    if (!file->node->directory) return STATUS_NOT_A_DIRECTORY;
    std::lock_guard lock(file->mutex);
    if (!file->view || !marker) {
        auto result = self.cache.Browse(file->node, file->view);
        if (!result) { LogError(result); return fsp.status(result.code); }
    }
    std::vector<std::pair<std::wstring, std::shared_ptr<FileNode>>> entries;
    if (file->node != self.cache.Root()) {
        entries.emplace_back(L".", file->node);
        auto parent = file->node->parent.lock();
        entries.emplace_back(L"..", parent ? parent : self.cache.Root());
    }
    for (const auto& child : file->view->entries) entries.emplace_back(child->name, child);
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) { return NameCompare(left.first, right.first) < 0; });
    for (const auto& [name, node] : entries) {
        if (marker && NameCompare(name, marker) <= 0) continue;
        alignas(FSP_FSCTL_DIR_INFO) unsigned char storage[sizeof(FSP_FSCTL_DIR_INFO) + 512]{};
        auto info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage);
        info->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + name.size() * sizeof(wchar_t));
        {
            std::lock_guard nodeLock(node->mutex);
            info->FileInfo = NodeInfo(*node, node->metadata ? &*node->metadata : nullptr);
        }
        memcpy(info->FileNameBuf, name.data(), name.size() * sizeof(wchar_t));
        if (!fsp.addDir(info, buffer, length, transferred)) return STATUS_SUCCESS;
    }
    fsp.addDir(nullptr, buffer, length, transferred);
    return STATUS_SUCCESS;
}
NTSTATUS DriveMount::DirectoryEntry(FSP_FILE_SYSTEM* value, PVOID context, PWSTR name, FSP_FSCTL_DIR_INFO* info) {
    auto& self = Self(value);
    auto file = static_cast<OpenFile*>(context);
    std::shared_ptr<FileNode> node;
    switch (std::wstring_view(name).size()) {
    case 1: if (name[0] == L'.') node = file->node; break;
    case 2: if (name[0] == L'.' && name[1] == L'.') node = file->node->parent.lock(); break;
    }
    if (!node) {
        std::shared_ptr<DirView> view;
        auto result = self.cache.Directory(file->node, view);
        if (!result) return fsp.status(result.code);
        auto found = std::lower_bound(view->entries.begin(), view->entries.end(), std::wstring_view(name), [](const auto& entry, auto key) { return NameCompare(entry->name, key) < 0; });
        if (found == view->entries.end() || NameCompare((*found)->name, name)) return STATUS_OBJECT_NAME_NOT_FOUND;
        node = *found;
    }
    FileMeta meta;
    auto result = self.cache.Metadata(node, meta);
    if (!result) return fsp.status(result.code);
    auto returnedName = std::wstring_view(name);
    if (returnedName != L"." && returnedName != L"..") returnedName = node->name;
    info->Size = static_cast<UINT16>(sizeof(*info) + returnedName.size() * sizeof(wchar_t));
    info->FileInfo = NodeInfo(*node, &meta);
    memset(info->Padding, 0, sizeof(info->Padding));
    memcpy(info->FileNameBuf, returnedName.data(), returnedName.size() * sizeof(wchar_t));
    return STATUS_SUCCESS;
}
VOID DriveMount::DispatcherStopped(FSP_FILE_SYSTEM* value, BOOLEAN normally) {
    if (!normally) {
        auto& self = Self(value);
        Log(L"Filesystem dispatcher stopped by Windows");
        self.cache.Cancel();
        self.control.RequestStop();
    }
}
Result DriveMount::Start(wchar_t requested) {
    auto result = CheckLetter(requested);
    if (!result) return result;
    result = LoadRuntime();
    if (!result) return result;
    std::wstring sid;
    result = UserSid(sid);
    if (!result) return result;
    auto acl = L"O:" + sid + L"G:" + sid + L"D:P(A;;FRFX;;;" + sid + L")(A;;FRFX;;;SY)(A;;FRFX;;;BA)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &security, nullptr)) return LastError(L"Cannot create read-only volume permissions");
    interface.GetVolumeInfo = VolumeInfo;
    interface.GetSecurityByName = SecurityName;
    interface.Create = Create;
    interface.Open = Open;
    interface.Overwrite = Overwrite;
    interface.Close = Close;
    interface.Read = Read;
    interface.GetFileInfo = FileInfo;
    interface.GetSecurity = Security;
    interface.ReadDirectory = ReadDirectory;
    interface.GetDirInfoByName = DirectoryEntry;
    interface.DispatcherStopped = DispatcherStopped;
    FSP_FSCTL_VOLUME_PARAMS parameters{};
    parameters.Version = sizeof(parameters);
    parameters.SectorSize = 512;
    parameters.SectorsPerAllocationUnit = 8;
    parameters.MaxComponentLength = 255;
    parameters.VolumeCreationTime = NowFileTime();
    parameters.VolumeSerialNumber = static_cast<UINT32>(HashName(cache.RootUrl()));
    parameters.FileInfoTimeout = 1000;
    parameters.IrpTimeout = 120000;
    parameters.CasePreservedNames = 1;
    parameters.UnicodeOnDisk = 1;
    parameters.PersistentAcls = 1;
    parameters.ReadOnlyVolume = 1;
    parameters.UmFileContextIsUserContext2 = 1;
    parameters.PostCleanupWhenModifiedOnly = 1;
    parameters.FlushAndPurgeOnCleanup = 1;
    wcscpy_s(parameters.FileSystemName, L"webmount");
    NTSTATUS status = fsp.create(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &parameters, &interface, &fileSystem);
    if (!NT_SUCCESS(status)) return Fail(static_cast<DWORD>(status), L"Cannot create WinFsp volume");
    fileSystem->UserContext = this;
    DWORD occupied = GetLogicalDrives();
    if (!occupied) return LastError(L"Cannot enumerate existing drive letters");
    Result last = requested ? Fail(ERROR_ALREADY_ASSIGNED, L"The requested drive letter became unavailable") : Fail(ERROR_NO_MORE_ITEMS, L"No free drive letter from D: through Z:");
    wchar_t first = requested ? requested : L'D';
    wchar_t final = requested ? requested : L'Z';
    for (wchar_t drive = first; drive <= final; ++drive) {
        if (occupied & (1u << (drive - L'A'))) continue;
        wchar_t mount[]{drive, L':', L'\0'};
        wchar_t target[1024]{};
        if (QueryDosDeviceW(mount, target, static_cast<DWORD>(std::size(target)))) continue;
        result = control.Reserve(drive);
        if (!result) { last = result; continue; }
        status = fsp.mount(fileSystem, mount);
        if (!NT_SUCCESS(status)) { last = Fail(static_cast<DWORD>(status), L"Cannot assign drive letter"); continue; }
        letter = drive;
        break;
    }
    if (!letter) return last;
    status = fsp.start(fileSystem, 8);
    if (!NT_SUCCESS(status)) return Fail(static_cast<DWORD>(status), L"Cannot start filesystem dispatcher");
    control.Start();
    Log(std::wstring(L"Mounted ") + cache.RootUrl() + L" at " + letter + L": [read-only]");
    return {};
}
void DriveMount::Stop() {
    if (!fileSystem) return;
    cache.Cancel();
    fsp.unmount(fileSystem);
    fsp.stop(fileSystem);
    fsp.remove(fileSystem);
    fileSystem = nullptr;
    if (letter) Log(std::wstring(L"Unmounted ") + letter + L":");
    control.Finish();
}
}
