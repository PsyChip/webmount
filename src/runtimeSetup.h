#pragma once
#include "app.h"

namespace webmount {
struct FspApi {
    decltype(&FspFileSystemCreate) create = nullptr;
    decltype(&FspFileSystemDelete) remove = nullptr;
    decltype(&FspFileSystemSetMountPoint) mount = nullptr;
    decltype(&FspFileSystemRemoveMountPoint) unmount = nullptr;
    decltype(&FspFileSystemStartDispatcher) start = nullptr;
    decltype(&FspFileSystemStopDispatcher) stop = nullptr;
    decltype(&FspFileSystemAddDirInfo) addDir = nullptr;
    decltype(&FspNtStatusFromWin32) status = nullptr;
    decltype(&FspFsctlStopService) stopDriver = nullptr;
};
inline FspApi fsp;
Result EnsureRuntime();
Result InstallRuntime();
Result UninstallRuntime();
Result LoadRuntime();
Result ElevateSetup(bool uninstall);
bool IsElevated();
std::wstring RuntimePath();
Result LockRuntime(Handle& lock);
void UnlockRuntime(Handle& lock);
}
