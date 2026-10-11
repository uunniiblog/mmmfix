#pragma once

#include <windows.h>

namespace mmmfix {

void Initialize(HMODULE selfModule);
void Shutdown();
bool IsOverlayVisible();
bool IsCursorRestoreEnabled();
bool IsGameWindowActive();
void ReacquireMouseDevices();
void RequestMouseWake();
void ReportMouseReacquire(LONG lostResult, LONG acquireResult, LONG retryResult);
void ReportMouseWake(const char* api, const char* edge);

} // namespace mmmfix
