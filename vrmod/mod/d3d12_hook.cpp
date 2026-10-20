// D3D12 / DXGI hook for the HD2 VR mod.
//
// Technique (standard for D3D12 overlays):
//  1. Spin up a THROWAWAY device + command queue + swapchain to read the vtable function
//     pointers for IDXGISwapChain::Present (vtable idx 8), IDXGISwapChain1::Present1 (idx 22)
//     and ID3D12CommandQueue::ExecuteCommandLists (idx 10).
//  2. MinHook those pointers (they're shared by the game's real objects).
//  3. hkExec captures the game's real command queue (ImGui_ImplDX12 submits on it).
//  4. hkPresent1 builds the ImGui overlay once per frame into our own command list, submits it
//     on the game's queue, then calls the original Present1.
//
// HD2 presents through Present1 (flip model); plain Present (slot 8) is hooked only for diagnostics.
#include "d3d12_hook.h"
#include "vr_host.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace hd2vr {

using PresentFn  = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecFn     = void    (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PresentFn           oPresent  = nullptr;
static Present1Fn          oPresent1 = nullptr;
static ExecFn              oExec     = nullptr;
static ID3D12CommandQueue* gQueue    = nullptr;   // captured game command queue

static constexpr UINT kMaxBuffers = 8;
static ID3D12Device*               gDevice        = nullptr;
static ID3D12DescriptorHeap*       gRtvHeap       = nullptr;
static ID3D12DescriptorHeap*       gSrvHeap       = nullptr;
static ID3D12GraphicsCommandList*  gCmdList       = nullptr;
static ID3D12CommandAllocator*     gAlloc[kMaxBuffers]    = {};
static ID3D12Resource*             gBack[kMaxBuffers]     = {};
static D3D12_CPU_DESCRIPTOR_HANDLE gRtv[kMaxBuffers]      = {};
static UINT64                      gFenceVal[kMaxBuffers] = {};
static ID3D12Fence*                gFence         = nullptr;
static HANDLE                      gFenceEvent    = nullptr;
static UINT64                      gFenceCounter  = 0;
static UINT                        gBufCount      = 0;
static HWND                        gHwnd          = nullptr;
static bool                        gInit          = false;
static bool                        gInitFailed    = false;
static UINT64                      gFrames        = 0;
static bool                        gOverlayVisible = false;   // hidden by default; Insert toggles
static bool                        gVrFailed      = false;

static void LogStep(const char* msg) {
    HANDLE lf = CreateFileW(L"C:\\HD2VR\\vrmod\\mod.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (lf != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(lf, msg, (DWORD)lstrlenA(msg), &w, nullptr); WriteFile(lf, "\r\n", 2, &w, nullptr); CloseHandle(lf); }
}

// --- hook: capture the real command queue ---
static void STDMETHODCALLTYPE hkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!gQueue && q) { gQueue = q; LogStep("exec: queue captured"); }        // first queue the game drives with = the one ImGui needs
    oExec(q, n, lists);
}

// Build the ImGui device objects against the game's swapchain. Runs once, on the first present
// after the game's queue has been captured.
static bool InitOverlay(IDXGISwapChain3* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    gHwnd = desc.OutputWindow;
    gBufCount = desc.BufferCount;
    if (gBufCount == 0 || gBufCount > kMaxBuffers) return false;

    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&gDevice)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, gBufCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC srvd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(gDevice->CreateDescriptorHeap(&rtvd, IID_PPV_ARGS(&gRtvHeap)))) return false;
    if (FAILED(gDevice->CreateDescriptorHeap(&srvd, IID_PPV_ARGS(&gSrvHeap)))) return false;

    // One RTV per swapchain back buffer; one command allocator per back buffer so a frame can
    // be recorded while the GPU still executes the previous overlay submission.
    UINT rtvInc = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < gBufCount; ++i) {
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&gBack[i])))) return false;
        gRtv[i].ptr = rtvStart.ptr + (SIZE_T)i * rtvInc;
        gDevice->CreateRenderTargetView(gBack[i], nullptr, gRtv[i]);
        if (FAILED(gDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gAlloc[i])))) return false;
    }
    if (FAILED(gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gAlloc[0], nullptr, IID_PPV_ARGS(&gCmdList)))) return false;
    gCmdList->Close();
    if (FAILED(gDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence)))) return false;
    gFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gFenceEvent) return false;

    ImGui::CreateContext();
    // No imgui.ini: ImGui would write it to the working directory, i.e. the Steam install.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(gHwnd);
    ImGui_ImplDX12_Init(gDevice, (int)gBufCount, desc.BufferDesc.Format, gSrvHeap,
                        gSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                        gSrvHeap->GetGPUDescriptorHandleForHeapStart());
    return true;
}

// Record the overlay for this frame's back buffer and submit it on the game's queue.
// Called from inside Present, so the back buffer is in the PRESENT state.
static void RenderOverlay(IDXGISwapChain3* sc, ID3D12CommandQueue* q) {
    UINT idx = sc->GetCurrentBackBufferIndex();
    if (idx >= gBufCount) return;

    // Wait until the GPU is done with the previous overlay submission that used this allocator.
    if (gFence->GetCompletedValue() < gFenceVal[idx]) {
        gFence->SetEventOnCompletion(gFenceVal[idx], gFenceEvent);
        WaitForSingleObject(gFenceEvent, INFINITE);
    }
    gAlloc[idx]->Reset();
    gCmdList->Reset(gAlloc[idx], nullptr);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::Begin("hd2vr");
    ImGui::Text("HD2 VR mod: injected, D3D12 overlay live");
    ImGui::Text("frame %llu  %.1f fps", (unsigned long long)gFrames, ImGui::GetIO().Framerate);
    ImGui::Text("%s", vrhost::Status());
    ImGui::End();
    ImGui::Render();

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = gBack[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gCmdList->ResourceBarrier(1, &b);
    gCmdList->OMSetRenderTargets(1, &gRtv[idx], FALSE, nullptr);
    gCmdList->SetDescriptorHeaps(1, &gSrvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gCmdList);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    gCmdList->ResourceBarrier(1, &b);
    gCmdList->Close();

    ID3D12CommandList* lists[] = { gCmdList };
    q->ExecuteCommandLists(1, lists);
    gFenceVal[idx] = ++gFenceCounter;
    q->Signal(gFence, gFenceVal[idx]);
}

// SEH guard: a fault inside the overlay must not take the game down with it.
static bool SafeInit(IDXGISwapChain3* sc) {
    __try { return InitOverlay(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeRender(IDXGISwapChain3* sc) {
    __try { RenderOverlay(sc, gQueue); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeVr(IDXGISwapChain3* sc) {
    __try {
        const UINT idx = sc->GetCurrentBackBufferIndex();
        if (idx < gBufCount) vrhost::Frame(gDevice, gQueue, gBack[idx], gHwnd);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void OnPresent(IDXGISwapChain3* sc, const char* who) {
    if (gInitFailed) return;
    if (!gInit) {
        if (!gQueue) return;   // wait until the game has submitted work, so we have its queue
        char buf[96];
        sprintf_s(buf, "%s: first frame, init", who); LogStep(buf);
        if (!SafeInit(sc)) { LogStep("present: overlay init FAILED"); gInitFailed = true; return; }
        SetWindowTextW(gHwnd, L"HELLDIVERS 2 [HD2VR LIVE]");
        HANDLE lf = CreateFileW(L"C:\\HD2VR\\vrmod\\hook_live.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (lf != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(lf, who, (DWORD)lstrlenA(who), &w, nullptr); CloseHandle(lf); }
        gInit = true;
        LogStep("present: overlay live");
    }
    ++gFrames;
    if (GetAsyncKeyState(VK_INSERT) & 1) gOverlayVisible = !gOverlayVisible;
    if (gOverlayVisible && !SafeRender(sc)) { LogStep("present: render FAULT, overlay disabled"); gOverlayVisible = false; }
    // After the overlay, so the overlay shows up in the headset too.
    if (!gVrFailed && !SafeVr(sc)) { LogStep("present: VR FAULT, VR disabled"); gVrFailed = true; }
}

// --- hook: plain Present (diagnostic; this game doesn't go through it) ---
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain3* sc, UINT sync, UINT flags) {
    OnPresent(sc, "present");
    return oPresent(sc, sync, flags);
}

// --- hook: Present1 (the path HD2 actually uses) ---
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain3* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    OnPresent(sc, "present1");
    return oPresent1(sc, sync, flags, params);
}

// Create a throwaway device+queue+swapchain to read the vtable slots we need to hook.
static bool ResolveVtableTargets(void** outPresent, void** outPresent1, void** outExec) {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) return false;
    auto pD3D12CreateDevice = (decltype(&D3D12CreateDevice))GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pD3D12CreateDevice) return false;

    ID3D12Device* dev = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd{};
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { dev->Release(); return false; }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { q->Release(); dev->Release(); return false; }

    // A hidden window is enough to create a swapchain for vtable discovery.
    WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"hd2vr_tmp";
    RegisterClassExW(&wc);
    HWND tmp = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = 2; scd.Width = 8; scd.Height = 8; scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    bool ok = SUCCEEDED(factory->CreateSwapChainForHwnd(q, tmp, &scd, nullptr, nullptr, &sc1));

    if (ok) {
        void** scVtbl = *reinterpret_cast<void***>(sc1);
        void** qVtbl  = *reinterpret_cast<void***>(q);
        *outPresent  = scVtbl[8];    // IDXGISwapChain::Present
        *outPresent1 = scVtbl[22];   // IDXGISwapChain1::Present1
        *outExec     = qVtbl[10];    // ID3D12CommandQueue::ExecuteCommandLists
    }

    if (sc1) sc1->Release();
    if (tmp) DestroyWindow(tmp);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    factory->Release(); q->Release(); dev->Release();
    return ok;
}

bool InstallD3D12Hook() {
    LogStep("install: start");
    void *pPresent = nullptr, *pPresent1 = nullptr, *pExec = nullptr;
    if (!ResolveVtableTargets(&pPresent, &pPresent1, &pExec)) { LogStep("install: resolve FAILED"); return false; }
    char buf[192];
    sprintf_s(buf, "install: present=%p present1=%p exec=%p", pPresent, pPresent1, pExec); LogStep(buf);
    MH_STATUS s1 = MH_Initialize();
    if (s1 != MH_OK && s1 != MH_ERROR_ALREADY_INITIALIZED) { LogStep("install: MH_Initialize FAILED"); return false; }
    MH_STATUS s2 = MH_CreateHook(pPresent, (void*)hkPresent, (void**)&oPresent);
    sprintf_s(buf, "install: create present -> %d", (int)s2); LogStep(buf);
    if (s2 != MH_OK && s2 != MH_ERROR_ALREADY_CREATED) { LogStep("install: create present FAILED"); return false; }
    MH_STATUS s5 = MH_CreateHook(pPresent1, (void*)hkPresent1, (void**)&oPresent1);
    sprintf_s(buf, "install: create present1 -> %d", (int)s5); LogStep(buf);
    if (s5 != MH_OK && s5 != MH_ERROR_ALREADY_CREATED) { LogStep("install: create present1 FAILED"); return false; }
    MH_STATUS s3 = MH_CreateHook(pExec, (void*)hkExec, (void**)&oExec);
    sprintf_s(buf, "install: create exec -> %d", (int)s3); LogStep(buf);
    if (s3 != MH_OK && s3 != MH_ERROR_ALREADY_CREATED) { LogStep("install: create exec FAILED"); return false; }
    MH_STATUS s4 = MH_EnableHook(MH_ALL_HOOKS);
    sprintf_s(buf, "install: enable -> %d", (int)s4); LogStep(buf);
    LogStep("install: done");
    return s4 == MH_OK;
}

void RemoveD3D12Hook() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    // TODO: ImGui_ImplDX12_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    // Skipped on purpose: DLL_PROCESS_DETACH runs under the loader lock, and the game is exiting anyway.
}

} // namespace hd2vr
