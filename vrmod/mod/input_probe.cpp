// hd2vr_inputprobe — which input API does the game read the mouse through?
// Hooks GetRawInputData/GetRawInputBuffer, IDirectInputDevice8 GetDeviceState/GetDeviceData and
// IGameInput GetCurrentReading/GetNextReading (shared vtables, found by creating our own objects),
// then logs call counts and mouse deltas seen every 2 s to vrmod\input.log for 30 s.
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "MinHook.h"

typedef unsigned __int64 QWORD;   // used by NEXTRAWINPUTBLOCK

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

static void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    HANDLE f = CreateFileW(L"C:\\HD2VR\\vrmod\\input.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, line, (DWORD)strlen(line), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

static volatile LONG nRawData, nRawBuffer, nRawMouse, nRawMouseInjected, nDiState, nDiData, nGiCurrent, nGiNext, nGiMouse;
static volatile LONG sumRawDx, sumRawInjDx;

using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using DiStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using DiDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using GiReadingFn = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT kind, void* device, void** reading);
using GiNextFn = HRESULT(STDMETHODCALLTYPE*)(void* self, void* ref, UINT kind, void* device, void** reading);
static GetRawInputDataFn oRawData;
static GetRawInputBufferFn oRawBuffer;
static DiStateFn oDiState;
static DiDataFn oDiData;
static GiReadingFn oGiCurrent;
static GiNextFn oGiNext;

static void NoteRaw(const RAWINPUT* r) {
    if (r->header.dwType != RIM_TYPEMOUSE) return;
    InterlockedIncrement(&nRawMouse);
    if (!r->header.hDevice) { InterlockedIncrement(&nRawMouseInjected); InterlockedAdd(&sumRawInjDx, r->data.mouse.lLastX); }
    else InterlockedAdd(&sumRawDx, r->data.mouse.lLastX);
}

static UINT WINAPI hkRawData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    InterlockedIncrement(&nRawData);
    UINT r = oRawData(h, cmd, data, size, hdr);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER)) NoteRaw((const RAWINPUT*)data);
    return r;
}
static UINT WINAPI hkRawBuffer(PRAWINPUT data, PUINT size, UINT hdr) {
    InterlockedIncrement(&nRawBuffer);
    UINT n = oRawBuffer(data, size, hdr);
    if (data && n != (UINT)-1) {
        PRAWINPUT p = data;
        for (UINT i = 0; i < n; ++i) { NoteRaw(p); p = NEXTRAWINPUTBLOCK(p); }
    }
    return n;
}
static HRESULT STDMETHODCALLTYPE hkDiState(IDirectInputDevice8W* d, DWORD n, LPVOID p) { InterlockedIncrement(&nDiState); return oDiState(d, n, p); }
static HRESULT STDMETHODCALLTYPE hkDiData(IDirectInputDevice8W* d, DWORD s, LPDIDEVICEOBJECTDATA o, LPDWORD n, DWORD f) { InterlockedIncrement(&nDiData); return oDiData(d, s, o, n, f); }
static const UINT kGiMouse = 0x00040000;
static HRESULT STDMETHODCALLTYPE hkGiCurrent(void* s, UINT kind, void* dev, void** r) {
    InterlockedIncrement(&nGiCurrent);
    if (kind & kGiMouse) InterlockedIncrement(&nGiMouse);
    return oGiCurrent(s, kind, dev, r);
}
static HRESULT STDMETHODCALLTYPE hkGiNext(void* s, void* ref, UINT kind, void* dev, void** r) {
    InterlockedIncrement(&nGiNext);
    if (kind & kGiMouse) InterlockedIncrement(&nGiMouse);
    return oGiNext(s, ref, kind, dev, r);
}

static void Hook(void* target, void* detour, void** orig, const char* name) {
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %s at %p: %d", name, target, s);
}

static DWORD WINAPI Worker(LPVOID) {
    MH_Initialize();
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    Hook((void*)GetProcAddress(u32, "GetRawInputData"), (void*)&hkRawData, (void**)&oRawData, "GetRawInputData");
    Hook((void*)GetProcAddress(u32, "GetRawInputBuffer"), (void*)&hkRawBuffer, (void**)&oRawBuffer, "GetRawInputBuffer");

    IDirectInput8W* di = nullptr;
    if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr))) {
        IDirectInputDevice8W* mouse = nullptr;
        if (SUCCEEDED(di->CreateDevice(GUID_SysMouse, &mouse, nullptr))) {
            void** vt = *(void***)mouse;
            Hook(vt[9], (void*)&hkDiState, (void**)&oDiState, "IDirectInputDevice8::GetDeviceState");
            Hook(vt[10], (void*)&hkDiData, (void**)&oDiData, "IDirectInputDevice8::GetDeviceData");
            mouse->Release();
        }
        di->Release();
    }

    if (HMODULE gi = GetModuleHandleW(L"GameInput.dll")) {
        using CreateFn = HRESULT(WINAPI*)(void**);
        auto create = (CreateFn)GetProcAddress(gi, "GameInputCreate");
        void* obj = nullptr;
        if (create && SUCCEEDED(create(&obj)) && obj) {
            void** vt = *(void***)obj;
            Hook(vt[4], (void*)&hkGiCurrent, (void**)&oGiCurrent, "IGameInput::GetCurrentReading");
            Hook(vt[5], (void*)&hkGiNext, (void**)&oGiNext, "IGameInput::GetNextReading");
            ((IUnknown*)obj)->Release();
        } else Log("GameInputCreate failed");
    }

    for (int t = 0; t < 15; ++t) {
        Sleep(2000);
        Log("2s: rawData %ld rawBuffer %ld rawMouse %ld (injected %ld) dx real %ld injected %ld | dinput state %ld data %ld | gameinput current %ld next %ld mouse-kind %ld",
            nRawData, nRawBuffer, nRawMouse, nRawMouseInjected, sumRawDx, sumRawInjDx, nDiState, nDiData, nGiCurrent, nGiNext, nGiMouse);
        nRawData = nRawBuffer = nRawMouse = nRawMouseInjected = sumRawDx = sumRawInjDx = nDiState = nDiData = nGiCurrent = nGiNext = nGiMouse = 0;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    Log("probe done, hooks disabled");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    }
    return TRUE;
}
