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
bool IsControllerDiagnosticsEnabled();
bool IsXInputEnabled();
bool IsXInputNativeActionsEnabled();
// Binding indices: A, B, X, Y, Start, Back, LB, RB, LT, RT, L3, R3,
// Up, Down, Right, Left. Mouse button values are 0=none, 1=left, 2=right.
unsigned GetXInputBindingKey(unsigned index);
int GetXInputBindingMouseButton(unsigned index);
void HandleControllerOverlayBinding(unsigned index, unsigned dik, bool pressed);
void HandleControllerOverlayNavigation(int horizontal, int vertical);
bool IsSteamActionBridgeActive();
void InstallSteamInputBridge(void* interfacePointer, const char* version);
void PollSelectorControllerPrompt(HWND window);
void ReportControllerDiagnostic(const char* format, ...);

} // namespace mmmfix
