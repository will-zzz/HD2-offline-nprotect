// The hot-reloadable VR module. See vr_session.h.
//
// Two ways to show the game:
//  - stereo (real VR, needs the render-camera hook hd2vr_cam*.dll): alternate-eye rendering. Each
//    game frame renders one eye: before the frame we hand the hook that eye's pose (relative to a
//    recentered reference, so the game camera is the player's "body") and a projection matching
//    the eye's field of view. When the frame is presented we copy it into that eye's swapchain and
//    submit both eyes as a projection layer, each with the pose it was rendered with, so the
//    runtime's reprojection covers the eye that is a frame old. Aim stays on the mouse.
//  - screen: a quad layer (a big screen in front of the player, fixed in the room or locked to
//    the head), with optional head-to-mouse.
// The copy from the back buffer is a fullscreen-triangle draw, not CopyResource, so it works
// whatever format the game and the runtime use. The game's back buffer holds sRGB-encoded values;
// the shader decodes them to linear, because OpenXR runtimes treat colour as linear (an sRGB
// swapchain re-encodes on write). Log: vrmod\vr.log.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include "vr_session.h"
#include "cam_hook.h"
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <psapi.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hd2vr::vr {

static const ULONGLONG kRetryMs = 5000;
static const UINT kFrames = 3;               // command allocators in flight
static const float kPi = 3.14159265f;
static const wchar_t kConfigPath[] = L"C:\\HD2VR\\vrmod\\vr_config.txt";

// Settings from vr_config.txt (key=value lines). Defaults match the dashboard's.
struct Config {
    bool stereo = true;          // real VR through the camera hook (falls back to the screen without it)
    float worldScale = 1.0f;     // game metres per real metre of head movement
    bool fovOverride = true;     // render with the headset's field of view instead of the game's
    int eyeLag = 0;              // presented frame = the pose sent this many frames before the latest used
    bool bodyFollow = true;      // head yaw turns the Helldiver (the game camera) with in-process mouse input
    float bodyGain = 0.25f;      // fraction of the remaining turn sent per frame
    float bodyDeadzone = 1.0f;   // degrees of head/body difference left alone
    float mouseCountsPerDeg = 24.3f;  // the game's mouse sensitivity (measured: 400 counts = 16.5 deg)
    int eyeTest = 0;             // diagnostic: 1 tilts the right eye 15 degrees; 4 mono (no eye separation); 6 direction grid; 10 synthetic wall at 2 m; 2 turns it round and
                                 // logs each presented frame's brightness with its eye tag (probe)
    bool headMouse = false;      // head yaw/pitch -> mouse movement (screen mode)
    float sensX = 12.0f;         // mouse counts per degree of head yaw
    float sensY = 12.0f;         // mouse counts per degree of head pitch
    bool invertY = false;
    bool headLocked = false;     // screen follows the head (use it with head-mouse)
    float distance = 2.5f;       // metres
    float width = 3.2f;          // metres
};
static Config gCfg;
static ULONGLONG gCfgStamp = 0;
static ULONGLONG gNextCfgCheck = 0;
static bool gHeadMouseToggled = false;   // Home key flips head-mouse on top of the config

static XrInstance gInstance = XR_NULL_HANDLE;
static XrSystemId gSystem = XR_NULL_SYSTEM_ID;
static XrSession gSession = XR_NULL_HANDLE;
static XrSpace gSpace = XR_NULL_HANDLE;      // LOCAL: fixed in the room
static XrSpace gViewSpace = XR_NULL_HANDLE;  // VIEW: follows the head
static ID3D12CommandQueue* gQueue = nullptr; // for the GPU wait on shutdown

// head-mouse state
static bool gHaveLastPose = false;
static float gLastYaw = 0, gLastPitch = 0;
static float gAccX = 0, gAccY = 0;
static XrSessionState gState = XR_SESSION_STATE_UNKNOWN;
static bool gRunning = false;
static ULONGLONG gNextTry = 0;
static char gStatus[192] = "VR: starting";

// An OpenXR swapchain the size of the game's back buffer, with an RTV per image.
struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images;
    ID3D12DescriptorHeap* rtv = nullptr;
    UINT w = 0, h = 0;
};
static Swap gScreen;          // screen mode
static Swap gEye[2];          // stereo mode, left and right
static DXGI_FORMAT gSwapFormat = DXGI_FORMAT_UNKNOWN;

// stereo state
static Hd2vrCamSetPoseFn gCamSetPose = nullptr;
static Hd2vrCamUsedFn gCamUsed = nullptr;
static Hd2vrCamActiveFn gCamActive = nullptr;
static int gCamFeatures = 0;
static ULONGLONG gNextCamLookup = 0;
static char gCamDll[32] = "";
struct Sent { unsigned seq; int eye; XrPosef pose; XrFovf fov; XrQuaternionf relRot; XrVector3f relPos; XrFovf render; };
static Sent gSent[16] = {};            // poses handed to the hook, by seq
static Sent gCur[2] = {};              // both eyes' latest poses (diagnostics)
static XrFovf gCurNativeFov[2] = {};
static unsigned gSeq = 0;
static int gNextEye = 0;
struct EyeImage { bool valid; XrPosef pose; XrFovf fov; };
static EyeImage gEyeImage[2] = {};
static bool gHaveRef = false;          // recenter reference: yaw + position of the head
static float gBodyRef = 0;             // body follow: game yaw the reference direction maps to
static bool gHaveBodyRef = false;
static XrQuaternionf gRefRot{ 0, 0, 0, 1 };
static XrVector3f gRefPos{ 0, 0, 0 };
static bool gLoggedThreads = false;
static LONG gStereoFrames = 0, gStereoMatched = 0;

// blit resources
static ID3D12RootSignature* gRootSig = nullptr;
static ID3D12PipelineState* gPso = nullptr;
static DXGI_FORMAT gPsoFormat = DXGI_FORMAT_UNKNOWN;
static ID3D12DescriptorHeap* gSrvHeap = nullptr;
static UINT gRtvInc = 0;
static ID3D12CommandAllocator* gAlloc[kFrames] = {};
static ID3D12GraphicsCommandList* gList = nullptr;
static ID3D12Fence* gFence = nullptr;
static HANDLE gFenceEvent = nullptr;
static UINT64 gFenceVal[kFrames] = {};
static UINT64 gFenceNext = 0;
static UINT gFrameSlot = 0;

static void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    HANDLE f = CreateFileW(L"C:\\HD2VR\\vrmod\\vr.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, line, (DWORD)strlen(line), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

static void SetStatus(const char* fmt, ...) {
    char s[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    if (strcmp(s, gStatus) != 0) {
        strcpy_s(gStatus, s);
        Log("%s", s);
    }
}

const char* Status() { return gStatus; }

static const char* XrStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (gInstance && XR_SUCCEEDED(xrResultToString(gInstance, r, buf))) return buf;
    sprintf_s(buf, "XrResult %d", (int)r);
    return buf;
}

// ---- teardown ---------------------------------------------------------------------------------
static void DestroySwap(Swap& s) {
    if (s.sc) xrDestroySwapchain(s.sc);
    if (s.rtv) s.rtv->Release();
    s = Swap{};
}

static void DestroySwapchains() {
    DestroySwap(gScreen);
    DestroySwap(gEye[0]);
    DestroySwap(gEye[1]);
    gEyeImage[0].valid = gEyeImage[1].valid = false;
}

static void DestroySession() {
    DestroySwapchains();
    if (gViewSpace) xrDestroySpace(gViewSpace);
    if (gSpace) xrDestroySpace(gSpace);
    if (gSession) xrDestroySession(gSession);
    gViewSpace = XR_NULL_HANDLE;
    gSpace = XR_NULL_HANDLE;
    gSession = XR_NULL_HANDLE;
    gRunning = false;
    gState = XR_SESSION_STATE_UNKNOWN;
}

static void DestroyInstance() {
    DestroySession();
    if (gInstance) xrDestroyInstance(gInstance);
    gInstance = XR_NULL_HANDLE;
    gSystem = XR_NULL_SYSTEM_ID;
}

// ---- setup ------------------------------------------------------------------------------------
static bool InitInstance() {
    const char* exts[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "hd2vr");
    strcpy_s(ci.applicationInfo.engineName, "hd2vr");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;   // widest runtime support
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &gInstance);
    if (XR_FAILED(r)) {
        gInstance = XR_NULL_HANDLE;
        SetStatus("VR: no OpenXR runtime (start Quest Link or SteamVR) [%d]", (int)r);
        return false;
    }
    XrInstanceProperties props{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(gInstance, &props);
    XrSystemGetInfo sg{ XR_TYPE_SYSTEM_GET_INFO };
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(gInstance, &sg, &gSystem);
    if (XR_FAILED(r)) {
        SetStatus("VR: runtime '%s' has no headset connected (%s)", props.runtimeName, XrStr(r));
        DestroyInstance();
        return false;
    }
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(gInstance, gSystem, &sp);
    Log("runtime '%s', headset '%s'", props.runtimeName, sp.systemName);
    return true;
}

static bool CreateSession(ID3D12Device* device, ID3D12CommandQueue* queue) {
    PFN_xrGetD3D12GraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(gInstance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsD3D12KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    if (!getReq || XR_FAILED(getReq(gInstance, gSystem, &req))) {
        SetStatus("VR: runtime refused D3D12 graphics requirements");
        return false;
    }
    const LUID luid = device->GetAdapterLuid();
    if (memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0)
        Log("warning: the game's GPU is not the headset's GPU");

    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = gSystem;
    XrResult r = xrCreateSession(gInstance, &sci, &gSession);
    if (XR_FAILED(r)) {
        gSession = XR_NULL_HANDLE;
        SetStatus("VR: xrCreateSession failed (%s)", XrStr(r));
        return false;
    }
    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(gSession, &rs, &gSpace);
    if (XR_SUCCEEDED(r)) {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        r = xrCreateReferenceSpace(gSession, &rs, &gViewSpace);
    }
    if (XR_FAILED(r)) {
        SetStatus("VR: xrCreateReferenceSpace failed (%s)", XrStr(r));
        DestroySession();
        return false;
    }
    gQueue = queue;
    SetStatus("VR: session created, waiting for the headset");
    return true;
}

static void PollEvents() {
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(gInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* s = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            gState = s->state;
            Log("session state -> %d", (int)gState);
            if (gState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(gSession, &bi);
                gRunning = XR_SUCCEEDED(r);
                if (gRunning) SetStatus("VR: running");
                else SetStatus("VR: xrBeginSession failed (%s)", XrStr(r));
            } else if (gState == XR_SESSION_STATE_STOPPING) {
                xrEndSession(gSession);
                gRunning = false;
                SetStatus("VR: session stopped");
            } else if (gState == XR_SESSION_STATE_EXITING || gState == XR_SESSION_STATE_LOSS_PENDING) {
                DestroySession();
                SetStatus("VR: session ended, will retry");
                gNextTry = GetTickCount64() + kRetryMs;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            DestroyInstance();
            SetStatus("VR: runtime went away, will retry");
            gNextTry = GetTickCount64() + kRetryMs;
            return;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Fullscreen triangle; decodes sRGB (unless the source is already linear float) and writes linear.
static const char kShader[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer Params : register(b0) { uint decodeSrgb; uint grid; float tanR; float tanU; float3 eyePos; float tanL; float3 axX; float tanD; float3 axY; float pad2; float3 axZ; float pad3; float4 srcTan; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 ToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 PSMain(VSOut i) : SV_Target {
    // Output pixel -> direction tangents in the declared (output) frustum.
    float2 t = float2(lerp(tanL, tanR, i.uv.x), lerp(tanU, tanD, i.uv.y));
    float2 suv = i.uv;
    // srcTan (L, R, U, D): the frustum the game rendered with, when it differs from the output's.
    // Link mis-places a projection layer whose fov isn't the eye's own, so we resample into it.
    if (srcTan.y > srcTan.x) suv = float2((t.x - srcTan.x) / (srcTan.y - srcTan.x), (srcTan.z - t.y) / (srcTan.z - srcTan.w));
    float3 c = src.Sample(smp, suv).rgb;
    if (any(suv < 0) || any(suv > 1)) c = 0;
    c = decodeSrgb ? ToLinear(saturate(c)) : c;
    if (grid == 2) {
        // diagnostic: a checkerboard wall 2 m ahead of the recentre point, ray-traced from this eye's
        // tagged pose (reference frame: x right, y up, -z forward). Lines every 25 cm.
        float3 d = lerp(tanL, tanR, i.uv.x) * axX + lerp(tanU, tanD, i.uv.y) * axY - axZ;
        c *= 0.25;
        if (d.z < 0) {
            float t = (-2 - eyePos.z) / d.z;
            float2 h = (eyePos + t * d).xy;
            float2 f = abs(frac(h / 0.25 + 0.5) - 0.5) * 0.25;
            if (min(f.x, f.y) < 0.006) c = float3(0.1, 1, 1);
            if (abs(h.x) < 0.012 || abs(h.y) < 0.012) c = float3(1, 0.1, 0.1);
        }
    } else if (grid) {
        // diagnostic: lines every 10 degrees of direction (from the frustum's tangents), red at 0
        float ax = degrees(atan(lerp(tanL, tanR, i.uv.x))), ay = degrees(atan(lerp(tanU, tanD, i.uv.y)));
        float dx = abs(ax - round(ax / 10) * 10), dy = abs(ay - round(ay / 10) * 10);
        c *= 0.35;
        if (dx < 0.25 || dy < 0.25) c = float3(0.1, 1, 0.1);
        if (abs(ax) < 0.4 || abs(ay) < 0.4) c = float3(1, 0.1, 0.1);
    }
    return float4(c, 1);
}
)";

static bool CreateBlit(ID3D12Device* device, DXGI_FORMAT rtvFormat) {
    if (gPso && gPsoFormat == rtvFormat) return true;
    if (gPso) { gPso->Release(); gPso = nullptr; }

    if (!gRootSig) {
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 1, &range };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 24 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp{};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{ 2, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gRootSig)))) {
            Log("root signature failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (blob) blob->Release();
            if (err) err->Release();
            return false;
        }
        blob->Release();
    }

    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "hd2vr_blit", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "hd2vr_blit", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
        Log("shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (vs) vs->Release();
        if (err) err->Release();
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = gRootSig;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gPso));
    vs->Release();
    ps->Release();
    if (FAILED(hr)) { Log("pipeline state failed: 0x%08lx", hr); return false; }
    gPsoFormat = rtvFormat;

    if (!gSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gSrvHeap)))) return false;
        for (UINT i = 0; i < kFrames; ++i)
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gAlloc[i])))) return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gAlloc[0], nullptr, IID_PPV_ARGS(&gList)))) return false;
        gList->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence)))) return false;
        gFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    return true;
}

static DXGI_FORMAT PickSwapchainFormat() {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(gSession, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(gSession, count, &count, formats.data());
    const DXGI_FORMAT preferred[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
    for (DXGI_FORMAT p : preferred)
        for (int64_t f : formats)
            if (f == p) return p;
    return count ? (DXGI_FORMAT)formats[0] : DXGI_FORMAT_UNKNOWN;
}

static bool EnsureSwap(Swap& s, const char* name, ID3D12Device* device, ID3D12Resource* backBuffer) {
    const D3D12_RESOURCE_DESC bd = backBuffer->GetDesc();
    if (s.sc && s.w == (UINT)bd.Width && s.h == bd.Height) return true;
    DestroySwap(s);

    gSwapFormat = PickSwapchainFormat();
    if (gSwapFormat == DXGI_FORMAT_UNKNOWN) { SetStatus("VR: runtime offers no swapchain formats"); return false; }
    XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = gSwapFormat;
    sc.sampleCount = 1;
    sc.width = (uint32_t)bd.Width;
    sc.height = bd.Height;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XrResult r = xrCreateSwapchain(gSession, &sc, &s.sc);
    if (XR_FAILED(r)) { s.sc = XR_NULL_HANDLE; SetStatus("VR: xrCreateSwapchain failed (%s)", XrStr(r)); return false; }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumerateSwapchainImages(s.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& i : imgs) s.images.push_back(i.texture);

    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtv)))) { DestroySwap(s); return false; }
    gRtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = gSwapFormat;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (uint32_t i = 0; i < n; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * gRtvInc;
        device->CreateRenderTargetView(s.images[i], &rv, h);
    }
    s.w = (UINT)bd.Width;
    s.h = bd.Height;
    Log("%s swapchain %ux%u, format %d (game back buffer format %d), %u images", name, s.w, s.h, (int)gSwapFormat, (int)bd.Format, n);
    return CreateBlit(device, gSwapFormat);
}

static bool IsFloatFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT;
}

// grid: diagnostic direction grid over the image, for a frustum with these half-angle tangents.
static XrVector3f QRot(XrQuaternionf q, XrVector3f v);

static void Blit(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, const Swap& s, uint32_t imageIndex,
                 UINT grid = 0, const XrFovf* fov = nullptr, const Sent* sent = nullptr, const XrFovf* srcFov = nullptr) {
    const UINT slot = gFrameSlot;
    gFrameSlot = (gFrameSlot + 1) % kFrames;
    if (gFence->GetCompletedValue() < gFenceVal[slot]) {
        gFence->SetEventOnCompletion(gFenceVal[slot], gFenceEvent);
        WaitForSingleObject(gFenceEvent, INFINITE);
    }
    gAlloc[slot]->Reset();
    gList->Reset(gAlloc[slot], gPso);

    const D3D12_RESOURCE_DESC bd = backBuffer->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = bd.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(backBuffer, &sv, gSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backBuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    gList->ResourceBarrier(1, &b);

    // OpenXR hands D3D12 colour swapchain images over in RENDER_TARGET state; leave them in it.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s.rtv->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)imageIndex * gRtvInc;
    gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    gList->SetGraphicsRootSignature(gRootSig);
    gList->SetDescriptorHeaps(1, &gSrvHeap);
    gList->SetGraphicsRootDescriptorTable(0, gSrvHeap->GetGPUDescriptorHandleForHeapStart());
    float consts[24] = {};
    const UINT flags[2] = { IsFloatFormat(bd.Format) ? 0u : 1u, grid };
    memcpy(consts, flags, sizeof(flags));
    if (fov) {
        consts[2] = tanf(fov->angleRight);
        consts[3] = tanf(fov->angleUp);
        consts[7] = tanf(fov->angleLeft);
        consts[11] = tanf(fov->angleDown);
    }
    if (sent) {
        const XrVector3f ax = QRot(sent->relRot, { 1, 0, 0 }), ay = QRot(sent->relRot, { 0, 1, 0 }), az = QRot(sent->relRot, { 0, 0, 1 });
        const float v[] = { sent->relPos.x, sent->relPos.y, sent->relPos.z, consts[7], ax.x, ax.y, ax.z, consts[11],
                            ay.x, ay.y, ay.z, 0, az.x, az.y, az.z, 0 };
        memcpy(consts + 4, v, sizeof(v));
    }
    if (srcFov) {
        consts[20] = tanf(srcFov->angleLeft);
        consts[21] = tanf(srcFov->angleRight);
        consts[22] = tanf(srcFov->angleUp);
        consts[23] = tanf(srcFov->angleDown);
    }
    gList->SetGraphicsRoot32BitConstants(1, 24, consts, 0);
    D3D12_VIEWPORT vp{ 0, 0, (float)s.w, (float)s.h, 0, 1 };
    D3D12_RECT sr{ 0, 0, (LONG)s.w, (LONG)s.h };
    gList->RSSetViewports(1, &vp);
    gList->RSSetScissorRects(1, &sr);
    gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gList->DrawInstanced(3, 1, 0, 0);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    gList->ResourceBarrier(1, &b);
    gList->Close();
    ID3D12CommandList* lists[] = { gList };
    queue->ExecuteCommandLists(1, lists);
    gFenceVal[slot] = ++gFenceNext;
    queue->Signal(gFence, gFenceVal[slot]);
}

// ---- config -----------------------------------------------------------------------------------
static void LoadConfig() {
    const ULONGLONG now = GetTickCount64();
    if (now < gNextCfgCheck) return;
    gNextCfgCheck = now + 500;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(kConfigPath, GetFileExInfoStandard, &a)) return;
    const ULONGLONG stamp = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    if (stamp == gCfgStamp) return;
    gCfgStamp = stamp;
    FILE* f = nullptr;
    if (_wfopen_s(&f, kConfigPath, L"r") || !f) return;
    Config c;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {}, val[128] = {};
        if (sscanf_s(line, " %63[^= ] = %127s", key, (unsigned)sizeof(key), val, (unsigned)sizeof(val)) != 2) continue;
        const float v = (float)atof(val);
        if (!strcmp(key, "stereo")) c.stereo = v != 0;
        else if (!strcmp(key, "world_scale") && v > 0.05f) c.worldScale = v;
        else if (!strcmp(key, "fov_override")) c.fovOverride = v != 0;
        else if (!strcmp(key, "eye_lag") && v >= 0 && v <= 3) c.eyeLag = (int)v;
        else if (!strcmp(key, "eye_test")) c.eyeTest = (int)v;
        else if (!strcmp(key, "body_follow")) c.bodyFollow = v != 0;
        else if (!strcmp(key, "body_gain") && v > 0 && v <= 1) c.bodyGain = v;
        else if (!strcmp(key, "body_deadzone") && v >= 0 && v < 45) c.bodyDeadzone = v;
        else if (!strcmp(key, "mouse_cpd") && v > 0.5f && v < 500) c.mouseCountsPerDeg = v;
        else if (!strcmp(key, "head_mouse")) c.headMouse = v != 0;
        else if (!strcmp(key, "sens_x")) c.sensX = v;
        else if (!strcmp(key, "sens_y")) c.sensY = v;
        else if (!strcmp(key, "invert_y")) c.invertY = v != 0;
        else if (!strcmp(key, "head_locked")) c.headLocked = v != 0;
        else if (!strcmp(key, "distance") && v > 0.3f) c.distance = v;
        else if (!strcmp(key, "width") && v > 0.3f) c.width = v;
    }
    fclose(f);
    gCfg = c;
    gHeadMouseToggled = false;
    gHaveLastPose = false;
    Log("config: body_follow=%d gain=%.2f deadzone=%.1f mouse_cpd=%.1f", c.bodyFollow, c.bodyGain, c.bodyDeadzone, c.mouseCountsPerDeg);
    Log("config: stereo=%d eye_lag=%d world_scale=%.2f fov_override=%d head_mouse=%d sens=%.1f/%.1f invert_y=%d head_locked=%d distance=%.2f width=%.2f",
        c.stereo, c.eyeLag, c.worldScale, c.fovOverride, c.headMouse, c.sensX, c.sensY, c.invertY, c.headLocked, c.distance, c.width);
}

static bool HeadMouseOn() { return gCfg.headMouse != gHeadMouseToggled; }

// ---- head tracking v1: head yaw/pitch changes -> relative mouse movement -------------------------
static void HeadMouse(XrTime time, HWND window) {
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(gViewSpace, gSpace, time, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        gHaveLastPose = false;
        return;
    }
    // Forward vector of the head (OpenXR looks down -Z, Y up).
    const XrQuaternionf q = loc.pose.orientation;
    const float fx = -2.0f * (q.w * q.y + q.x * q.z);
    const float fy = 2.0f * (q.w * q.x - q.y * q.z);
    const float fz = -1.0f + 2.0f * (q.x * q.x + q.y * q.y);
    const float yaw = atan2f(fx, -fz) * 180.0f / kPi;                       // + = looking right
    const float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fy))) * 180.0f / kPi; // + = looking up
    if (!gHaveLastPose) {
        gLastYaw = yaw;
        gLastPitch = pitch;
        gHaveLastPose = true;
        return;
    }
    float dyaw = yaw - gLastYaw;
    if (dyaw > 180.0f) dyaw -= 360.0f;
    if (dyaw < -180.0f) dyaw += 360.0f;
    const float dpitch = pitch - gLastPitch;
    gLastYaw = yaw;
    gLastPitch = pitch;
    if (fabsf(dyaw) > 30.0f || fabsf(dpitch) > 30.0f) return;   // tracking glitch or recenter
    if (!window || GetForegroundWindow() != window) { gAccX = gAccY = 0; return; }
    // Keep the sub-count remainder so slow head turns still add up.
    gAccX += dyaw * gCfg.sensX;
    gAccY += -dpitch * gCfg.sensY * (gCfg.invertY ? -1.0f : 1.0f);
    const LONG dx = (LONG)gAccX, dy = (LONG)gAccY;
    gAccX -= (float)dx;
    gAccY -= (float)dy;
    if (!dx && !dy) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// ---- stereo: head pose -> render-camera hook -------------------------------------------------------
static XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static XrQuaternionf QConj(XrQuaternionf q) { return { -q.x, -q.y, -q.z, q.w }; }
static XrVector3f QRot(XrQuaternionf q, XrVector3f v) {
    const XrQuaternionf p{ v.x, v.y, v.z, 0 };
    const XrQuaternionf r = QMul(QMul(q, p), QConj(q));
    return { r.x, r.y, r.z };
}
// OpenXR (x right, y up, z back) -> Stingray camera space (x right, y forward, z up)
static void ToStingray(XrVector3f o, float* out) { out[0] = o.x; out[1] = -o.z; out[2] = o.y; }

// The hook lives in the mod DLL (hd2vr_mod10+); a standalone test copy (hd2vr_cam<N>.dll) injected
// later chains over it and wins while it's active.
static int ModuleRank(const char* name) {
    int n = 0;
    if (_strnicmp(name, "hd2vr_cam", 9) == 0) return 1000 + atoi(name + 9);
    if (_strnicmp(name, "hd2vr_mod", 9) == 0) n = atoi(name + 9);
    return n;
}

static bool FindCamHook() {
    if (gCamSetPose && gCamActive && gCamActive()) return true;
    if (gCamSetPose) { Log("stereo: camera hook %s went inactive", gCamDll); gCamSetPose = nullptr; gCamUsed = nullptr; gCamActive = nullptr; }
    const ULONGLONG now = GetTickCount64();
    if (now < gNextCamLookup) return false;
    gNextCamLookup = now + 2000;
    HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return false;
    int bestRank = -1;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
        auto set = (Hd2vrCamSetPoseFn)GetProcAddress(mods[i], "hd2vr_cam_set_pose");
        auto used = (Hd2vrCamUsedFn)GetProcAddress(mods[i], "hd2vr_cam_used");
        auto active = (Hd2vrCamActiveFn)GetProcAddress(mods[i], "hd2vr_cam_active");
        if (!set || !used || !active || !active()) continue;
        char path[MAX_PATH] = "";
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        const int rank = ModuleRank(name);
        if (rank <= bestRank) continue;
        bestRank = rank;
        gCamSetPose = set;
        gCamUsed = used;
        gCamActive = active;
        auto features = (Hd2vrCamFeaturesFn)GetProcAddress(mods[i], "hd2vr_cam_features");
        gCamFeatures = features ? features() : 0;
        strncpy_s(gCamDll, name, _TRUNCATE);
    }
    if (gCamSetPose) Log("stereo: using camera hook in %s (features %d)", gCamDll, gCamFeatures);
    return gCamSetPose != nullptr;
}

static void ReleaseCamera() {
    if (gCamSetPose) gCamSetPose(nullptr);
    gEyeImage[0].valid = gEyeImage[1].valid = false;
}

// Yaw-only reference from the head pose, so "forward" in the room is the game camera's forward.
static void Recenter(const XrPosef& head) {
    const XrVector3f f = QRot(head.orientation, { 0, 0, -1 });
    const float yaw = atan2f(-f.x, -f.z);                 // rotation about +Y (up)
    gRefRot = { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
    gRefPos = head.position;
    gHaveRef = true;
    gHaveBodyRef = false;      // body follow re-anchors to the game camera
    Log("stereo: recentered (yaw %.1f deg)", yaw * 180.0f / kPi);
}

// ---- body follows head ----------------------------------------------------------------------------
// gBodyRef is the game yaw the room's reference direction maps to. The hook's yaw lock renders the view
// at gBodyRef + head yaw whatever the game camera's yaw is this frame, and we steer the game camera
// (and so the Helldiver) toward that with in-process mouse input (the game ignores input injected from
// other processes). The player's own mouse turns move gBodyRef instead, so the mouse still turns you:
// a low-level mouse hook tells real moves from ours (injected events are flagged).
static float gLastGameYaw = 0;
static bool gHaveLastGameYaw = false;
static float gInjectRemainder = 0;
static volatile ULONGLONG gUserMouseAt = 0;      // when the player last moved the real mouse
static HANDLE gMouseThread = nullptr;
static DWORD gMouseThreadId = 0;

static float WrapPi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

static LRESULT CALLBACK MouseLL(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && !(((const MSLLHOOKSTRUCT*)lp)->flags & LLMHF_INJECTED))
        gUserMouseAt = GetTickCount64();
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI MouseThread(LPVOID) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&MouseLL, &self);
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseLL, self, 0);
    Log("body follow: mouse watch %s", hook ? "on" : "FAILED");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

static void StartMouseWatch() {
    if (!gMouseThread) gMouseThread = CreateThread(nullptr, 0, MouseThread, nullptr, 0, &gMouseThreadId);
}

static void StopMouseWatch() {   // before the module unloads: the hook procedure lives here
    if (!gMouseThread) return;
    PostThreadMessageW(gMouseThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(gMouseThread, 2000);
    CloseHandle(gMouseThread);
    gMouseThread = nullptr;
}

// Head yaw relative to the recenter reference (radians, + = left, like the game's yaw).
static bool HeadYawRel(XrTime time, float* yaw) {
    XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(gViewSpace, gSpace, time, &head)) || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return false;
    const XrVector3f f = QRot(QMul(QConj(gRefRot), head.pose.orientation), { 0, 0, -1 });
    *yaw = atan2f(-f.x, -f.z);
    return true;
}

static void BodyFollow(XrTime time, bool foreground) {
    if (!gCfg.bodyFollow || !gHaveRef || !(gCamFeatures & 1)) { gHaveBodyRef = false; return; }
    Hd2vrCamUsed u{};
    gCamUsed(&u);
    if (!u.frames) return;
    const float g = u.gameYaw;
    float head = 0;
    if (!HeadYawRel(time, &head)) return;
    if (!gHaveBodyRef) {           // view starts where the game camera points
        gBodyRef = WrapPi(g - head);
        gHaveBodyRef = true;
        gHaveLastGameYaw = false;
        Log("body follow: on (game yaw %.1f, head %.1f deg)", g * 180 / kPi, head * 180 / kPi);
    }
    const bool userMoving = GetTickCount64() - gUserMouseAt < 150;
    static ULONGLONG nextLog = 0;
    if (GetTickCount64() >= nextLog) {
        nextLog = GetTickCount64() + 500;
        Log("body: game %.1f head %.1f ref %.1f target %.1f err %.1f deg, user %d, fg %d, frames %ld",
            g * 180 / kPi, head * 180 / kPi, gBodyRef * 180 / kPi, WrapPi(gBodyRef + head) * 180 / kPi,
            WrapPi(gBodyRef + head - g) * 180 / kPi, userMoving, foreground, u.frames);
    }
    if (userMoving && gHaveLastGameYaw)          // the player turns with the mouse: turn the view too
        gBodyRef = WrapPi(gBodyRef + WrapPi(g - gLastGameYaw));
    gLastGameYaw = g;
    gHaveLastGameYaw = true;
    if (userMoving || !foreground) { gInjectRemainder = 0; return; }

    const float err = WrapPi(gBodyRef + head - g) * 180.0f / kPi;     // degrees the body must turn left
    if (fabsf(err) < gCfg.bodyDeadzone) { gInjectRemainder = 0; return; }
    gInjectRemainder += -err * gCfg.mouseCountsPerDeg * gCfg.bodyGain;  // + mouse x turns right
    LONG dx = (LONG)gInjectRemainder;
    if (dx > 3000) dx = 3000;
    if (dx < -3000) dx = -3000;
    gInjectRemainder -= (float)dx;
    if (!dx) return;
    // Safety: if we keep steering but the reported game yaw doesn't move, the yaw source is wrong;
    // stop rather than spin the player.
    static LONG sentSinceCheck = 0;
    static float yawAtCheck = 0;
    if (!sentSinceCheck) yawAtCheck = g;
    sentSinceCheck += labs(dx);
    if (sentSinceCheck > (LONG)(gCfg.mouseCountsPerDeg * 20)) {          // ~20 degrees of steering
        const bool moved = fabsf(WrapPi(g - yawAtCheck)) * 180 / kPi > 3;
        sentSinceCheck = 0;
        if (!moved) {
            gCfg.bodyFollow = false;
            gHaveBodyRef = false;
            Log("body follow: game yaw didn't respond to steering; turned off (fix, then re-apply the config)");
            return;
        }
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// The eye's field of view as a symmetric frustum that covers it (the game's projection is symmetric).
static XrFovf CoverFov(const XrFovf& f, float* vfov, float* aspect) {
    const float h = fmaxf(tanf(-f.angleLeft), tanf(f.angleRight));
    const float v = fmaxf(tanf(f.angleUp), tanf(-f.angleDown));
    *vfov = 2.0f * atanf(v);
    *aspect = h / v;
    return { -atanf(h), atanf(h), atanf(v), -atanf(v) };
}

static XrFovf GameFov(float vfov, float aspect) {
    const float v = tanf(vfov * 0.5f), h = v * aspect;
    return { -atanf(h), atanf(h), atanf(v), -atanf(v) };
}

// Hands the hook the pose for the next game frame (one eye, alternating).
static void SendNextEye(XrTime time, bool recenterKey) {
    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = gSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(gSession, &li, &vs, 2, &n, views)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;

    if (!gHaveRef || recenterKey) {
        XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(gViewSpace, gSpace, time, &head)) &&
            (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            Recenter(head.pose);
        if (!gHaveRef) return;
    }

    // diagnostic (eye_test 11): both eyes' current poses, for drawing the synthetic wall every frame
    for (int e = 0; e < 2; ++e) {
        const XrQuaternionf ri = QConj(gRefRot);
        const XrVector3f de{ views[e].pose.position.x - gRefPos.x, views[e].pose.position.y - gRefPos.y,
                             views[e].pose.position.z - gRefPos.z };
        float vf, as;
        gCur[e] = { 0, e, views[e].pose, CoverFov(views[e].fov, &vf, &as), QMul(ri, views[e].pose.orientation), QRot(ri, de), {} };
        gCurNativeFov[e] = views[e].fov;
    }

    const int eye = gNextEye;
    gNextEye ^= 1;
    XrPosef p = views[eye].pose;
    if (gCfg.eyeTest == 4 || gCfg.eyeTest == 8 || gCfg.eyeTest == 9) {   // diagnostic mono: both eyes rendered (and submitted) from the head centre
        p.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
        p.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        p.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
    }
    const XrQuaternionf refInv = QConj(gRefRot);
    const XrQuaternionf rel = QMul(refInv, p.orientation);
    const XrVector3f d{ p.position.x - gRefPos.x, p.position.y - gRefPos.y, p.position.z - gRefPos.z };
    const XrVector3f relPos = QRot(refInv, d);

    Hd2vrCamPose cp = {};
    ToStingray(QRot(rel, { 1, 0, 0 }), &cp.head[0]);     // right
    ToStingray(QRot(rel, { 0, 0, -1 }), &cp.head[4]);    // forward
    ToStingray(QRot(rel, { 0, 1, 0 }), &cp.head[8]);     // up
    ToStingray(relPos, &cp.head[12]);
    for (int i = 12; i < 15; ++i) cp.head[i] *= gCfg.worldScale;
    cp.head[15] = 1.0f;
    if (gCfg.eyeTest == 2 && eye == 1) {
        for (int k = 0; k < 8; ++k) cp.head[k] = -cp.head[k];   // turn round: negate right and forward
    } else if (gCfg.eyeTest == 8 && eye == 1) {
        // same position, turned 10 degrees about the head's up axis: right/forward rows mix
        const float c = cosf(10.0f * kPi / 180.0f), s = sinf(10.0f * kPi / 180.0f);
        for (int k = 0; k < 3; ++k) {
            const float r = cp.head[0 + k], f = cp.head[4 + k];
            cp.head[0 + k] = c * r + s * f;
            cp.head[4 + k] = -s * r + c * f;
        }
    } else if (gCfg.eyeTest == 1 && eye == 1) {
        // roll the right eye 15 degrees about its own forward axis: right/up rows mix
        const float c = cosf(15.0f * kPi / 180.0f), s = sinf(15.0f * kPi / 180.0f);
        for (int k = 0; k < 3; ++k) {
            const float r = cp.head[0 + k], u = cp.head[8 + k];
            cp.head[0 + k] = c * r - s * u;
            cp.head[8 + k] = s * r + c * u;
        }
    }

    XrFovf fov;
    if (gCfg.fovOverride) {
        fov = CoverFov(views[eye].fov, &cp.vfov, &cp.aspect);
    } else {
        Hd2vrCamUsed u{};
        gCamUsed(&u);
        fov = GameFov(u.gameVfov > 0 ? u.gameVfov : 0.96f, u.gameAspect > 0 ? u.gameAspect : 16.0f / 9.0f);
    }
    cp.seq = ++gSeq;
    if (!cp.seq) cp.seq = ++gSeq;           // 0 means "untouched"
    cp.enabled = 1;
    cp.yawLock = gCfg.bodyFollow && gHaveBodyRef ? 1 : 0;
    cp.yawRef = gBodyRef;
    gCamSetPose(&cp);
    static int sDiag = 0;
    if (sDiag < 6) {   // which side of the head each eye's camera sits on (camera-local, metres)
        ++sDiag;
        const XrVector3f c{ (views[0].pose.position.x + views[1].pose.position.x) * 0.5f - gRefPos.x,
                            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f - gRefPos.y,
                            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f - gRefPos.z };
        float cs[3];
        ToStingray(QRot(refInv, c), cs);
        const float side = (cp.head[12] - cs[0]) * cp.head[0] + (cp.head[13] - cs[1]) * cp.head[1] + (cp.head[14] - cs[2]) * cp.head[2];
        Log("eye %d: offset from head centre along the eye's right = %+.4f m; fov L%.1f R%.1f U%.1f D%.1f deg; render vfov %.1f aspect %.3f",
            eye, side, views[eye].fov.angleLeft * 57.3f, views[eye].fov.angleRight * 57.3f, views[eye].fov.angleUp * 57.3f,
            views[eye].fov.angleDown * 57.3f, cp.vfov * 57.3f, cp.aspect);
    }
    gSent[cp.seq % 16] = { cp.seq, eye, p, views[eye].fov, rel, relPos, fov };
}

// ---- diagnostic: which eye is each presented frame really? ---------------------------------------
// With eye_test=2 the right eye looks backwards, so its frames have a different brightness. For
// 120 frames, copy the back buffer to the CPU (stalling the GPU) and log brightness next to the eye
// the tag says (for lag 0 and lag 1).
static ID3D12Resource* gProbeBuf = nullptr;
static int gProbeLeft = 120;

static void Probe(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer) {
    if (gProbeLeft <= 0 || !gFence || !gList) return;
    const D3D12_RESOURCE_DESC bd = backBuffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!gProbeBuf) {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gProbeBuf)))) { gProbeLeft = 0; return; }
        Log("probe: start (right eye turned round)");
    }
    Hd2vrCamUsed u{};
    gCamUsed(&u);
    const Sent& s0 = gSent[u.seq % 16];
    const Sent& s1 = gSent[(u.seq - 1) % 16];
    const int e0 = s0.seq == u.seq ? s0.eye : -1, e1 = s1.seq == u.seq - 1 ? s1.eye : -1;

    const UINT slot = gFrameSlot;
    gFrameSlot = (gFrameSlot + 1) % kFrames;
    if (gFence->GetCompletedValue() < gFenceVal[slot]) { gFence->SetEventOnCompletion(gFenceVal[slot], gFenceEvent); WaitForSingleObject(gFenceEvent, INFINITE); }
    gAlloc[slot]->Reset();
    gList->Reset(gAlloc[slot], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backBuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gList->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{ gProbeBuf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{ backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    gList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    gList->ResourceBarrier(1, &b);
    gList->Close();
    ID3D12CommandList* lists[] = { gList };
    queue->ExecuteCommandLists(1, lists);
    gFenceVal[slot] = ++gFenceNext;
    queue->Signal(gFence, gFenceVal[slot]);
    gFence->SetEventOnCompletion(gFenceVal[slot], gFenceEvent);
    WaitForSingleObject(gFenceEvent, 1000);

    const unsigned char* px = nullptr;
    D3D12_RANGE rr{ 0, (SIZE_T)total };
    if (FAILED(gProbeBuf->Map(0, &rr, (void**)&px)) || !px) return;
    double sum = 0;
    int n = 0;
    for (UINT y = bd.Height / 8; y < bd.Height * 7 / 8; y += 16)       // skip the HUD edges
        for (UINT x = 0; x < (UINT)bd.Width; x += 16) {
            const unsigned char* p = px + fp.Offset + (UINT64)y * fp.Footprint.RowPitch + x * 4;
            sum += p[0] + p[1] + p[2];
            ++n;
        }
    if (gCfg.eyeTest == 7 || gCfg.eyeTest == 8 || gCfg.eyeTest == 9) {   // dump the frame (BGRA/RGBA8 rows) for offline disparity measurement
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"C:\\HD2VR\\vrmod\\frame_seq%u_eye%d_%ux%u_pitch%u.raw", u.seq, e0, (UINT)bd.Width, bd.Height, fp.Footprint.RowPitch);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(f, px + fp.Offset, fp.Footprint.RowPitch * bd.Height, &w, nullptr);
            CloseHandle(f);
        }
        if (gProbeLeft > 4) gProbeLeft = 4;
    }
    D3D12_RANGE none{ 0, 0 };
    gProbeBuf->Unmap(0, &none);
    Log("probe: seq %u  tag(lag0)=%d tag(lag1)=%d  brightness %.1f", u.seq, e0, e1, n ? sum / n / 3 : 0.0);
    if (--gProbeLeft == 0) Log("probe: done");
}

// The frame being presented rendered with some seq we sent: copy it into that eye's swapchain.
static void CaptureEye(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer) {
    Hd2vrCamUsed u{};
    gCamUsed(&u);
    ++gStereoFrames;
    if (!u.seq) return;
    // The render thread may already have updated the cameras for the next frame before presenting
    // this one; eye_lag says how many poses back the presented image is.
    const unsigned want = u.seq - (unsigned)gCfg.eyeLag;
    const Sent& s = gSent[want % 16];
    if (!want || s.seq != want) return;
    ++gStereoMatched;
    Swap& sw = gEye[s.eye];
    if (!EnsureSwap(sw, s.eye ? "right eye" : "left eye", device, backBuffer)) return;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(sw.sc, &ai, &index))) return;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(sw.sc, &wi)))
        Blit(device, queue, backBuffer, sw, index, gCfg.eyeTest == 6 ? 1u : gCfg.eyeTest == 10 ? 2u : 0u, &s.fov, &s, &s.render);
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sw.sc, &ri);
    gEyeImage[s.eye] = { true, s.pose, s.fov };
}

// ---- shutdown (before the host unloads this module) ----------------------------------------------
static void WaitGpu() {
    if (!gFence || !gQueue) return;
    const UINT64 v = ++gFenceNext;
    gQueue->Signal(gFence, v);
    if (gFence->GetCompletedValue() < v) {
        gFence->SetEventOnCompletion(v, gFenceEvent);
        WaitForSingleObject(gFenceEvent, 2000);
    }
}

template <typename T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void Shutdown() {
    StopMouseWatch();
    ReleaseCamera();
    WaitGpu();
    DestroyInstance();
    SafeRelease(gPso);
    SafeRelease(gRootSig);
    SafeRelease(gSrvHeap);
    SafeRelease(gList);
    for (auto& a : gAlloc) SafeRelease(a);
    SafeRelease(gFence);
    SafeRelease(gProbeBuf);
    if (gFenceEvent) { CloseHandle(gFenceEvent); gFenceEvent = nullptr; }
    gPsoFormat = DXGI_FORMAT_UNKNOWN;
    gQueue = nullptr;
    Log("module shut down");
}

// ---- per frame --------------------------------------------------------------------------------
static void OnFrame(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, HWND window) {
    if (!device || !queue || !backBuffer) return;
    LoadConfig();
    const bool foreground = window && GetForegroundWindow() == window;
    if (foreground && (GetAsyncKeyState(VK_HOME) & 1)) {
        gHeadMouseToggled = !gHeadMouseToggled;
        gHaveLastPose = false;
        Log("Home: head tracking %s", HeadMouseOn() ? "on" : "off");
    }
    const bool recenterKey = foreground && (GetAsyncKeyState(VK_END) & 1);
    // Alternate-eye tagging assumes the game presents on the thread that updates the cameras.
    if (!gLoggedThreads && gCfg.stereo && FindCamHook()) {
        Hd2vrCamUsed u{};
        gCamUsed(&u);
        if (u.tid) {
            gLoggedThreads = true;
            Log("stereo: present thread %lu, render thread %lu%s", GetCurrentThreadId(), u.tid,
                GetCurrentThreadId() == u.tid ? " (same: eye tagging is exact)" : " (different: eye tagging may lag a frame)");
        }
    }
    if (!gInstance) {
        if (GetTickCount64() < gNextTry) return;
        if (!InitInstance()) { gNextTry = GetTickCount64() + kRetryMs; return; }
    }
    PollEvents();
    if (!gInstance) { ReleaseCamera(); return; }
    if (!gSession) {
        ReleaseCamera();
        if (GetTickCount64() < gNextTry) return;
        if (!CreateSession(device, queue)) { gNextTry = GetTickCount64() + kRetryMs; return; }
    }
    if (!gRunning) { ReleaseCamera(); return; }

    const bool stereo = gCfg.stereo && FindCamHook();
    if (!stereo) ReleaseCamera();

    // The frame now being presented was rendered with the pose sent one frame ago.
    if (stereo && gCfg.eyeTest >= 2) Probe(device, queue, backBuffer);   // 3: probe without turning
    if (stereo && gCfg.eyeTest != 11) CaptureEye(device, queue, backBuffer);

    XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(gSession, &wi, &fs))) return;
    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame(gSession, &bi))) return;

    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layerCount = 0;
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                               { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };

    if (stereo) {
        gHaveLastPose = false;
        if (gCfg.bodyFollow) StartMouseWatch();
        BodyFollow(fs.predictedDisplayTime, foreground);
        SendNextEye(fs.predictedDisplayTime, recenterKey);
        if (gCfg.eyeTest == 11 && fs.shouldRender) {
            // diagnostic: the synthetic wall drawn into BOTH eyes every frame from each eye's current
            // pose (no alternation). PageUp swaps the eye positions used for drawing.
            static bool swapPos = false, nativeFov = false;
            if (foreground && (GetAsyncKeyState(VK_PRIOR) & 1)) { swapPos = !swapPos; Log("wall test: eye positions %s", swapPos ? "SWAPPED" : "normal"); }
            if (foreground && (GetAsyncKeyState(VK_NEXT) & 1)) { nativeFov = !nativeFov; Log("wall test: %s fov", nativeFov ? "NATIVE asymmetric" : "symmetric cover"); }
            for (int e = 0; e < 2; ++e) {
                Sent c = gCur[e];
                if (swapPos) c.relPos = gCur[e ^ 1].relPos;
                if (nativeFov) c.fov = gCurNativeFov[e];
                Swap& sw = gEye[e];
                if (!EnsureSwap(sw, e ? "right eye" : "left eye", device, backBuffer)) continue;
                uint32_t index = 0;
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrSwapchainImageWaitInfo wi2{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi2.timeout = XR_INFINITE_DURATION;
                if (XR_FAILED(xrAcquireSwapchainImage(sw.sc, &ai, &index))) continue;
                if (XR_SUCCEEDED(xrWaitSwapchainImage(sw.sc, &wi2)))
                    Blit(device, queue, backBuffer, sw, index, 2u, &c.fov, &c);
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(sw.sc, &ri);
                gEyeImage[e] = { true, gCur[e].pose, c.fov };
            }
        }
        if (fs.shouldRender && gEyeImage[0].valid && gEyeImage[1].valid) {
            for (int e = 0; e < 2; ++e) {
                pv[e].pose = gEyeImage[e].pose;
                pv[e].fov = gEyeImage[e].fov;
                pv[e].subImage.swapchain = gEye[e].sc;
                pv[e].subImage.imageRect.extent = { (int32_t)gEye[e].w, (int32_t)gEye[e].h };
            }
            proj.space = gSpace;
            proj.viewCount = 2;
            proj.views = pv;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        }
    } else {
        if (HeadMouseOn()) HeadMouse(fs.predictedDisplayTime, window);
        else gHaveLastPose = false;
        if (fs.shouldRender && EnsureSwap(gScreen, "screen", device, backBuffer)) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            XrSwapchainImageWaitInfo wwi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wwi.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(gScreen.sc, &ai, &index)) &&
                XR_SUCCEEDED(xrWaitSwapchainImage(gScreen.sc, &wwi))) {
                Blit(device, queue, backBuffer, gScreen, index);
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(gScreen.sc, &ri);

                quad.space = gCfg.headLocked ? gViewSpace : gSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = gScreen.sc;
                quad.subImage.imageRect.extent = { (int32_t)gScreen.w, (int32_t)gScreen.h };
                quad.pose.orientation.w = 1.0f;
                quad.pose.position.z = -gCfg.distance;
                quad.size = { gCfg.width, gCfg.width * (float)gScreen.h / (float)gScreen.w };
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            }
        }
    }
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrResult r = xrEndFrame(gSession, &ei);
    if (XR_FAILED(r)) SetStatus("VR: xrEndFrame failed (%s)", XrStr(r));
    else if (stereo && layerCount) SetStatus("VR: stereo through %s (%ux%u per eye)", gCamDll, gEye[0].w, gEye[0].h);
    else if (stereo) SetStatus("VR: stereo, waiting for both eyes (%ld/%ld frames matched)", gStereoMatched, gStereoFrames);
    else if (layerCount) SetStatus("VR: showing the game (%ux%u), %s screen, head tracking %s", gScreen.w, gScreen.h,
                                   gCfg.headLocked ? "head-locked" : "room", HeadMouseOn() ? "ON" : "off");
}

} // namespace hd2vr::vr

// ---- exports for vr_host -----------------------------------------------------------------------
extern "C" __declspec(dllexport) void hd2vr_vr_frame(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                     ID3D12Resource* backBuffer, HWND window) {
    hd2vr::vr::OnFrame(device, queue, backBuffer, window);
}
extern "C" __declspec(dllexport) const char* hd2vr_vr_status() { return hd2vr::vr::Status(); }
extern "C" __declspec(dllexport) void hd2vr_vr_shutdown() { hd2vr::vr::Shutdown(); }
