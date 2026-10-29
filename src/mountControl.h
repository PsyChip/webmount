#pragma once
#include "app.h"

namespace webmount {
Result UserSid(std::wstring& sid);
Result EjectMounts(wchar_t letter = 0, bool allUsers = false);
Result LaunchBackground(const std::wstring& url, wchar_t letter = 0, bool persist = false, bool restore = false);
void ReportReady(HANDLE pipe, DWORD code, wchar_t letter, const std::wstring& message = L"");
class MountControl {
public:
    MountControl();
    ~MountControl();
    Result Reserve(wchar_t letter);
    void Start();
    void Finish();
    HANDLE StopEvent() const { return stop.value; }
    void RequestStop() { SetEvent(stop.value); }
private:
    void Serve();
    Handle stop;
    Handle finished;
    Handle pipe;
    std::thread worker;
};
}
