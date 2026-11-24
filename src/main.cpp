// OptiCaptureHost
// OptiCaptureHost captures one window with Windows Graphics Capture and displays it in a
// borderless D3D12 overlay. Its FFX optical-flow and temporal scaffold is retained for follow-up work.

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dwmapi.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"
#include "blit_shader.h"
#include "fsr_flow_shader.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) DxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID id, void** object) = 0;
};

constexpr int kHotkeyQuit = 1;
constexpr int kHotkeyInteractive = 2;
constexpr int kHotkeyMenu = 3;

bool IsExtendedVk(int vk)
{
    switch (vk)
    {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
        return true;
    default:
        return false;
    }
}
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);

    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void Fail(const wchar_t* text)
{
    MessageBoxW(nullptr, text, L"OptiCaptureHost", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Keep foreground-window interception local to modules loaded in this host process. The target game is not patched.
using GetForegroundFn = HWND(WINAPI*)();
GetForegroundFn g_realGetForeground = nullptr;
HWND g_spoofTarget = nullptr;
HWND g_spoofOverlay = nullptr;

HWND WINAPI SpoofedGetForegroundWindow()
{
    const HWND real = g_realGetForeground ? g_realGetForeground() : nullptr;
    return (real && real == g_spoofTarget && g_spoofOverlay) ? g_spoofOverlay : real;
}

int PatchImportsOfModule(HMODULE mod)
{
    int patched = 0;
    __try
    {
        auto* base = reinterpret_cast<BYTE*>(mod);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return 0;

        auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; imp->Name; ++imp)
        {
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
            for (; thunk->u1.Function; ++thunk)
            {
                if (thunk->u1.Function != reinterpret_cast<ULONG_PTR>(g_realGetForeground))
                    continue;
                DWORD old = 0;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                {
                    thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&SpoofedGetForegroundWindow);
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                    ++patched;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return patched;
}

// Patches every module that is not part of Windows (the graphics driver's own folder counts as "not Windows"), once.
void PatchNewModules(std::vector<HMODULE>& seen)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;

    wchar_t win[MAX_PATH] = {};
    GetSystemWindowsDirectoryW(win, MAX_PATH);
    const std::wstring winDir = Lower(win);
    const HMODULE self = GetModuleHandleW(nullptr);

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            const HMODULE m = me.hModule;
            if (m == self || std::find(seen.begin(), seen.end(), m) != seen.end())
                continue;
            seen.push_back(m);

            const std::wstring path = Lower(me.szExePath);
            const bool windowsFile = path.rfind(winDir, 0) == 0 && path.find(L"\\driverstore\\") == std::wstring::npos;
            if (windowsFile)
                continue;

            const int n = PatchImportsOfModule(m);
            if (n > 0)
                Log("Focus spoof: %d import(s) patched in %s", n, Narrow(me.szModule).c_str());
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

std::wstring WindowTitle(HWND h)
{
    wchar_t buf[512] = {};
    GetWindowTextW(h, buf, 512);
    return buf;
}

bool GetTargetBounds(HWND target, RECT& r)
{
    if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        return true;
    return GetWindowRect(target, &r) != FALSE;
}

// ---------------------------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------------------------

struct Options
{
    std::wstring title;
    int delaySec = 5;
    bool clickThrough = true;
    int menuKey = VK_INSERT;
    HWND hwnd = nullptr;
    DWORD parent = 0;
};

bool ParseOptions(Options& o)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool ok = true;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--title") o.title = next();
        else if (arg == L"--delay") o.delaySec = std::clamp(_wtoi(next().c_str()), 0, 120);
        else if (arg == L"--no-clickthrough") o.clickThrough = false;
        else if (arg == L"--hwnd") o.hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(next().c_str(), nullptr, 10)));
        else if (arg == L"--parent") o.parent = static_cast<DWORD>(_wtoi64(next().c_str()));
        else if (arg == L"--menu-key") o.menuKey = std::clamp(static_cast<int>(wcstol(next().c_str(), nullptr, 0)), 1, 254);
        else ok = false;
    }
    LocalFree(argv);
    return ok;
}
// Window picking
// ---------------------------------------------------------------------------------------------------------------

struct FindCtx
{
    std::wstring needle;
    HWND found = nullptr;
};

BOOL CALLBACK FindProc(HWND h, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h) || IsIconic(h))
        return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId())
        return TRUE;

    if (Lower(WindowTitle(h)).find(ctx->needle) != std::wstring::npos)
    {
        ctx->found = h;
        return FALSE;
    }
    return TRUE;
}

HWND PickTarget(const Options& o)
{
    if (o.hwnd)
        return IsWindow(o.hwnd) ? o.hwnd : nullptr;

    if (!o.title.empty())
    {
        FindCtx ctx;
        ctx.needle = Lower(o.title);
        EnumWindows(FindProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    Log("Switch to the game window now: capturing the foreground window in %d s", o.delaySec);
    MessageBeep(MB_OK);
    Sleep(static_cast<DWORD>(o.delaySec) * 1000);

    HWND h = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId())
            return nullptr;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_CLOSE) // the launcher's Stop button: end the message loop instead of destroying the window under it
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

class Host
{
  public:
    int Run(const Options& opt, const oc::Settings& settings);

  private:
    void RegisterHotkeys();
    void UnregisterHotkeys();
    void PollSettings();
    bool LauncherInFront() const;
    bool InitGraphics();
    bool InitCapture();
    bool InitOverlayAndSwapChain();
    bool InitBlit();
    bool CreateFsrViews();
    bool InitFsrPipelines();
    bool InitFsrTemporal();
    void ReleaseFsrTemporal();
    bool RecordFsrTemporal();
    void RecordBlit(ID3D12Resource* bb, UINT idx, bool temporalOutput);
    void OverlayPlace(const RECT& game, int& x, int& y);
    bool CreateShared();
    bool PresentFrame(ID3D11Texture2D* src);
    bool ComputeCrop(UINT capW, UINT capH, int& cx, int& cy, UINT& w, UINT& h);
    bool ApplyGeometry(UINT capW, UINT capH);
    bool RefreshGeometry();
    void LogGeometry();
    void MoveOverlay();
    void EnforceOverlayStyle();
    void ToggleInteractive();
    bool ForceForeground(HWND h);
    void OpenMenu();
    void SendMenuKey(bool up);
    void WaitFenceValue(UINT64 v);
    void WaitGpu();
    void Shutdown();
    void PublishLive(std::chrono::steady_clock::time_point now);

    Options opt_;
    HWND target_ = nullptr;
    HWND overlay_ = nullptr;
    HINSTANCE inst_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    // Resolution choices resize the target window in the launcher; scaled choices use this neutral bilinear blit.
    bool scaleOn_ = false;
    bool blitReady_ = false;
    UINT outW_ = 0;
    UINT outH_ = 0;
    ComPtr<ID3D12RootSignature> blitRoot_;
    ComPtr<ID3D12PipelineState> blitPso_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_, rtvHeap_;
    // The FFX context consumes estimated vectors/depth and writes its temporal output for display.
    HMODULE ffxModule_ = nullptr;
    ffxFunctions ffxFn_{};
    ffxContext ffxCtx_ = nullptr;
    ComPtr<ID3D12RootSignature> fsrRoot_;
    ComPtr<ID3D12PipelineState> fsrBlockPso_, fsrPixelPso_;
    ComPtr<ID3D12Resource> fsrTex_[2], fsrBlk_, fsrMv_, fsrDepth_, fsrReact_, fsrOut_;
    bool fsrPipesOk_ = false;
    bool fsrReady_ = false;
    bool fsrHavePrev_ = false;
    bool ffxUnavailableLogged_ = false;
    int fsrCur_ = 1;
    std::chrono::steady_clock::time_point fsrLast_{};
    UINT srvInc_ = 0;
    UINT rtvInc_ = 0;
    bool interactive_ = false;
    int menuKeyStage_ = 0; // 0 idle, 1 waiting to press the menu key, 2 waiting to release it
    std::chrono::steady_clock::time_point menuKeyAt_{};
    // Some games minimise themselves the moment they lose the focus (Ctrl+Shift+F10 / F11 give it to the overlay). For a few
    // seconds after such a shortcut a minimised game is put back on screen without taking the focus away from the overlay.
    std::chrono::steady_clock::time_point keepGameUntil_{};
    std::chrono::steady_clock::time_point keepGameCheck_{};
    void KeepGameVisible(std::chrono::steady_clock::time_point now);
    UINT swapFlags_ = 0;
    RECT lastBounds_ = {};
    UINT capW_ = 0; // size of the captured frame (the whole window)
    UINT capH_ = 0;
    int cropX_ = 0; // the part of that frame that is the window's client area (what we show)
    int cropY_ = 0;
    volatile bool targetClosed_ = false;
    bool focusSpoof_ = false;
    std::vector<HMODULE> spoofSeen_;
    std::chrono::steady_clock::time_point spoofScanAt_{};

    oc::Settings st_;
    unsigned long long iniStamp_ = 0;
    unsigned long long iniPending_ = 0;
    HANDLE parentProc_ = nullptr;

    // D3D12 side
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> dev12_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocs_;
    std::vector<UINT64> allocFence_;
    double waitMs_ = 0.0;    // stats: time spent waiting for the allocator fence / inside Present, per 5 s
    double presentMs_ = 0.0;

    // Live status for the launcher (OptiCaptureHost.status, once a second).
    std::atomic<unsigned> arrivals_{ 0 }; // frames Windows Graphics Capture delivered
    std::chrono::steady_clock::time_point liveAt_{};
    unsigned liveArrivals_ = 0;
    UINT64 liveFrames_ = 0;
    double liveWait_ = 0.0;
    double livePresent_ = 0.0;

    // Frame time diagnostics, logged every 5 s: gaps between captured frames and between the host's presents, and the
    // longest time the housekeeping (focus spoof scan, window checks) kept the presenting thread busy.
    std::mutex arrMutex_;
    std::vector<double> arrGaps_;
    std::chrono::steady_clock::time_point arrLast_{};
    std::vector<double> presGaps_;
    std::vector<double> latMs_; // age of the captured frame (Windows composed it) when the host hands it to Present
    std::vector<double> srcGaps_; // gaps between the capture time stamps Windows put on the frames
    long long lastSrc100ns_ = 0;
    // GPU time of the host's own copies (measured with timestamp queries)
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource> tsRead_;
    UINT64 tsFreq_ = 0;
    double gpu12Ms_ = 0.0, gpu12Max_ = 0.0;
    int gpu12N_ = 0;
    struct Q11
    {
        ComPtr<ID3D11Query> dj, a, b;
        bool pending = false;
    };
    Q11 q11_[4];
    int q11Idx_ = 0;
    bool q11Failed_ = false;
    double gpu11Ms_ = 0.0, gpu11Max_ = 0.0;
    int gpu11N_ = 0;
    std::chrono::steady_clock::time_point presLast_{};
    double houseMaxMs_ = 0.0;
    double spoofMaxMs_ = 0.0;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> frameFence_;
    UINT64 frameFenceValue_ = 0;
    HANDLE fenceEvent_ = nullptr;

    // D3D11 side (WGC needs a D3D11 device) and the shared texture/fence between the two
    ComPtr<ID3D11Device> dev11_;
    ComPtr<ID3D11DeviceContext> ctx11_;
    ComPtr<ID3D11DeviceContext4> ctx11_4_;
    ComPtr<ID3D11Fence> fence11_;
    ComPtr<ID3D12Fence> fence12_;
    UINT64 sharedFenceValue_ = 0;
    ComPtr<ID3D11Texture2D> shared11_;
    ComPtr<ID3D12Resource> shared12_;

    // Capture
    HANDLE frameEvent_ = nullptr;
    wgd11::IDirect3DDevice winrtDevice_{ nullptr };
    wgc::GraphicsCaptureItem item_{ nullptr };
    wgc::Direct3D11CaptureFramePool pool_{ nullptr };
    wgc::GraphicsCaptureSession session_{ nullptr };
};

bool Host::InitGraphics()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr))
    {
        Log("CreateDXGIFactory2 failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter_));
    if (FAILED(hr))
    {
        Log("No adapter found: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    DXGI_ADAPTER_DESC1 ad{};
    adapter_->GetDesc1(&ad);
    Log("Adapter: %s", Narrow(ad.Description).c_str());

    hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12_));
    if (FAILED(hr))
    {
        Log("D3D12CreateDevice failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev12_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    if (FAILED(hr))
        return false;

    hr = dev12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence_));
    if (FAILED(hr))
        return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // D3D11 device on the same adapter, for Windows Graphics Capture.
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev11_, nullptr, &ctx11_);
    if (FAILED(hr))
    {
        Log("D3D11CreateDevice failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<ID3D11Device5> dev11_5;
    if (FAILED(dev11_.As(&dev11_5)) || FAILED(ctx11_.As(&ctx11_4_)))
    {
        Log("ID3D11Device5 / ID3D11DeviceContext4 not available (Windows 10 1703 or newer is required)");
        return false;
    }

    // One fence shared between D3D11 (signals after the copy) and D3D12 (waits before reading).
    hr = dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_));
    if (FAILED(hr))
    {
        Log("ID3D11Device5::CreateFence failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    HANDLE fh = nullptr;
    hr = fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh);
    if (FAILED(hr))
        return false;
    hr = dev12_->OpenSharedHandle(fh, IID_PPV_ARGS(&fence12_));
    CloseHandle(fh);
    if (FAILED(hr))
    {
        Log("OpenSharedHandle (fence) failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

bool Host::InitCapture()
{
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(dev11_.As(&dxgiDevice)))
        return false;

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    winrtDevice_ = inspectable.as<wgd11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(target_, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));

    auto size = item_.Size();
    capW_ = static_cast<UINT>(size.Width);
    capH_ = static_cast<UINT>(size.Height);
    Log("Capture item: %ux%u", capW_, capH_);
    if (capW_ == 0 || capH_ == 0)
        return false;

    {
        int cx = 0;
        int cy = 0;
        UINT cw = capW_;
        UINT ch = capH_;
        ComputeCrop(capW_, capH_, cx, cy, cw, ch);
        cropX_ = cx;
        cropY_ = cy;
        width_ = cw;
        height_ = ch;
    }
    LogGeometry();

    item_.Closed([this](auto&&, auto&&) { targetClosed_ = true; });

    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    pool_.FrameArrived([this](auto&&, auto&&) {
        const auto tn = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(arrMutex_);
            if (arrLast_.time_since_epoch().count() != 0 && arrGaps_.size() < 4096)
                arrGaps_.push_back(std::chrono::duration<double, std::milli>(tn - arrLast_).count());
            arrLast_ = tn;
        }
        arrivals_.fetch_add(1, std::memory_order_relaxed);
        SetEvent(frameEvent_);
    });

    session_ = pool_.CreateCaptureSession(item_);
    try
    {
        session_.IsCursorCaptureEnabled(false);
    }
    catch (...)
    {
        Log("IsCursorCaptureEnabled not available");
    }
    // Ask Windows Graphics Capture for each frame DWM composes.
    try
    {
        session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 10000 }); // 1 ms
        Log("Capture MinUpdateInterval set to 1 ms");
    }
    catch (...)
    {
        Log("MinUpdateInterval not available (older Windows): capture may be limited to 60 fps");
    }
    try
    {
        session_.IsBorderRequired(false);
    }
    catch (...)
    {
        Log("IsBorderRequired(false) not available (a yellow border may be drawn around the captured window)");
    }
    return true;
}

bool Host::InitOverlayAndSwapChain()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OptiCaptureHostOverlay";
    RegisterClassExW(&wc);

    RECT bounds{};
    if (!GetTargetBounds(target_, bounds))
        return false;
    lastBounds_ = bounds;
    scaleOn_ = st_.targetScale != 0;
    outW_ = width_;
    outH_ = height_;
    if (scaleOn_)
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST), &mi))
            return false;
        outW_ = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
        outH_ = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    blitReady_ = InitBlit();
    if (!blitReady_)
    {
        if (scaleOn_)
            return false;
        Log("Neutral blit is unavailable; native capture remains on the raw path");
    }
    if (scaleOn_)
        Log("Target resolution mode %d: neutral blit %ux%u -> %ux%u", st_.targetScale, width_, height_, outW_, outH_);
    else
    {
        Log("Native target resolution: %ux%u", width_, height_);
    }

    int x = 0, y = 0;
    OverlayPlace(bounds, x, y);
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    if (opt_.clickThrough)
        ex |= WS_EX_TRANSPARENT;
    interactive_ = !opt_.clickThrough;
    overlay_ = CreateWindowExW(ex, wc.lpszClassName, L"OptiCaptureHost", WS_POPUP, x, y, static_cast<int>(outW_),
                               static_cast<int>(outH_), nullptr, nullptr, inst_, nullptr);
    if (!overlay_)
    {
        Log("CreateWindowEx failed: %lu", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(overlay_, 0, 255, LWA_ALPHA);

    swapFlags_ = 0;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = outW_;
    sd.Height = outH_;
    sd.Format = kFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swapFlags_;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = E_FAIL;
    for (int attempt = 1; attempt <= 6; ++attempt)
    {
        sc1.Reset();
        hr = factory_->CreateSwapChainForHwnd(queue_.Get(), overlay_, &sd, nullptr, nullptr, &sc1);
        if (SUCCEEDED(hr))
            break;
        Log("CreateSwapChainForHwnd failed (try %d of 6): 0x%08X", attempt, static_cast<unsigned>(hr));
        Sleep(1000);
    }
    if (FAILED(hr))
    {
        Log("CreateSwapChainForHwnd failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    factory_->MakeWindowAssociation(overlay_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(sc1.As(&swap_)))
        return false;

    allocs_.resize(sd.BufferCount);
    allocFence_.assign(sd.BufferCount, 0);
    for (auto& allocator : allocs_)
        if (FAILED(dev12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
            return false;
    if (FAILED(dev12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs_[0].Get(), nullptr, IID_PPV_ARGS(&list_))))
        return false;
    list_->Close();

    tsHeap_.Reset();
    tsRead_.Reset();
    {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = sd.BufferCount * 2;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(sd.BufferCount) * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!(SUCCEEDED(dev12_->CreateQueryHeap(&qh, IID_PPV_ARGS(&tsHeap_))) &&
              SUCCEEDED(dev12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&tsRead_))) &&
              SUCCEEDED(queue_->GetTimestampFrequency(&tsFreq_)) && tsFreq_ != 0))
        {
            tsHeap_.Reset();
            tsRead_.Reset();
        }
    }
    ShowWindow(overlay_, SW_SHOWNOACTIVATE);
    return CreateShared();
}

bool Host::InitBlit()
{
    if (!srvHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeap{};
        srvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeap.NumDescriptors = 17;
        srvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev12_->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&srvHeap_))))
            return false;
        srvInc_ = dev12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &parameter;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr) || FAILED(dev12_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&blitRoot_))))
    {
        Log("Blit root signature failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    const char* source = kBlitHlsl;
    const SIZE_T sourceLength = sizeof(kBlitHlsl) - 1;
    ComPtr<ID3DBlob> vs, ps, compileError;
    hr = D3DCompile(source, sourceLength, "blit.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs,
                    &compileError);
    if (SUCCEEDED(hr))
        hr = D3DCompile(source, sourceLength, "blit.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps,
                        &compileError);
    if (FAILED(hr))
    {
        Log("Blit shader failed: 0x%08X %s", static_cast<unsigned>(hr),
            compileError ? static_cast<const char*>(compileError->GetBufferPointer()) : "");
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = blitRoot_.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kFormat;
    pd.SampleDesc.Count = 1;
    if (FAILED(dev12_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&blitPso_))))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 32;
    if (FAILED(dev12_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap_))))
        return false;
    rtvInc_ = dev12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}
bool Host::CreateShared()
{
    shared11_.Reset();
    shared12_.Reset();

    D3D11_TEXTURE2D_DESC d{};
    d.Width = width_;
    d.Height = height_;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = kFormat;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = dev11_->CreateTexture2D(&d, nullptr, &shared11_);
    if (FAILED(hr))
    {
        Log("CreateTexture2D (shared) failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IDXGIResource1> res;
    if (FAILED(shared11_.As(&res)))
        return false;

    HANDLE h = nullptr;
    hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (FAILED(hr))
    {
        Log("CreateSharedHandle failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = dev12_->OpenSharedHandle(h, IID_PPV_ARGS(&shared12_));
    CloseHandle(h);
    if (FAILED(hr))
    {
        Log("OpenSharedHandle (texture) failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    if (!fsrPipesOk_)
    {
        fsrPipesOk_ = InitFsrPipelines();
        if (!fsrPipesOk_)
            Log("FFX optical-flow shaders are unavailable; capture remains on the raw path");
    }
    if (srvHeap_ && !CreateFsrViews())
        return false;
    return true;
}

// The scaled display covers the target monitor; native mode stays over the target window.
void Host::OverlayPlace(const RECT& game, int& x, int& y)
{
    if (scaleOn_)
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST), &mi))
        {
            x = mi.rcMonitor.left;
            y = mi.rcMonitor.top;
            return;
        }
    }
    x = game.left + cropX_;
    y = game.top + cropY_;
}

// FFX flow: the two compute passes that measure motion (fsr_flow_shader.h).
bool Host::InitFsrPipelines()
{
    if (!srvHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 17;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev12_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvHeap_))))
            return false;
        srvInc_ = dev12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    fsrRoot_.Reset();
    fsrBlockPso_.Reset();
    fsrPixelPso_.Reset();
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1;
    rp[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = rp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev12_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&fsrRoot_))))
    {
        Log("FFX flow: root signature failed");
        return false;
    }
    auto make = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
        ComPtr<ID3DBlob> cs, e;
        std::string src(kFsrFlowHlsl);
        const HRESULT h = D3DCompile(src.c_str(), src.size(), "fsr_flow.hlsl", nullptr, nullptr, entry, "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e);
        if (FAILED(h))
        {
            Log("FFX flow: shader %s failed: 0x%08X %s", entry, static_cast<unsigned>(h),
                e ? static_cast<const char*>(e->GetBufferPointer()) : "");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = fsrRoot_.Get();
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        const HRESULT h2 = dev12_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(h2))
            Log("FFX flow: pipeline %s failed: 0x%08X", entry, static_cast<unsigned>(h2));
        return SUCCEEDED(h2);
    };
    if (!(make("CSBlock", fsrBlockPso_) && make("CSPixel", fsrPixelPso_)))
        return false;
    return true;
}

static void FfxMessage(uint32_t type, const wchar_t* message)
{
    if (!message)
        return;
    char text[400] = {};
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text, static_cast<int>(sizeof(text)) - 1, nullptr, nullptr);
    Log("FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

void Host::ReleaseFsrTemporal()
{
    fsrReady_ = false;
    fsrHavePrev_ = false;
    if (ffxCtx_ && ffxFn_.DestroyContext)
        ffxFn_.DestroyContext(&ffxCtx_, nullptr);
    ffxCtx_ = nullptr;
    fsrTex_[0].Reset();
    fsrTex_[1].Reset();
    fsrBlk_.Reset();
    fsrMv_.Reset();
    fsrDepth_.Reset();
    fsrReact_.Reset();
    fsrOut_.Reset();
}

// The captured frame's size is fixed per shared texture: everything that depends on it (frame copies, vectors, depth, output and AMD's
// context) is made again whenever the shared texture is.
bool Host::InitFsrTemporal()
{
    if (!fsrPipesOk_ || !srvHeap_)
        return false;
    WaitGpu();
    ReleaseFsrTemporal();

    if (!ffxModule_)
    {
        const std::wstring root = oc::ExeDir();
        const wchar_t* files[] = { L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll" };
        for (const wchar_t* file : files)
        {
            const std::wstring path = root + file;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module)
            {
                Log("FFX runtime %ls could not be loaded (error %lu)", path.c_str(), GetLastError());
                continue;
            }
            ffxFunctions functions{};
            ffxLoadFunctions(&functions, module);
            if (!functions.CreateContext || !functions.DestroyContext || !functions.Dispatch)
            {
                Log("FFX runtime %ls does not expose the required API", path.c_str());
                FreeLibrary(module);
                continue;
            }
            ffxModule_ = module;
            ffxFn_ = functions;
            Log("FFX runtime %ls", path.c_str());
            break;
        }
        if (!ffxModule_)
        {
            if (!ffxUnavailableLogged_)
            {
                Log("FFX runtime is unavailable; place a compatible amd_fidelityfx_dx12.dll next to the executables to exercise the temporal scaffold");
                ffxUnavailableLogged_ = true;
            }
            return false;
        }
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](ComPtr<ID3D12Resource>& r, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        return SUCCEEDED(dev12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)));
    };
    const UINT bw = (width_ + 3) / 4, bh = (height_ + 3) / 4;
    const D3D12_RESOURCE_STATES nps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!make(fsrTex_[0], width_, height_, kFormat, D3D12_RESOURCE_FLAG_NONE, nps) ||
        !make(fsrTex_[1], width_, height_, kFormat, D3D12_RESOURCE_FLAG_NONE, nps) ||
        !make(fsrBlk_, bw, bh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nps) ||
        !make(fsrMv_, width_, height_, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nps) ||
        !make(fsrDepth_, width_, height_, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nps) ||
        !make(fsrReact_, width_, height_, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nps) ||
        !make(fsrOut_, outW_, outH_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
    {
        Log("FFX flow: textures could not be made");
        ReleaseFsrTemporal();
        return false;
    }

    // FFX descriptor layout.
    auto cpu = [&](UINT n) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(srvInc_) * n;
        return h;
    };
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT n) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev12_->CreateShaderResourceView(r, &sv, cpu(n));
    };
    auto uav = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT n) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = fmt;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev12_->CreateUnorderedAccessView(r, nullptr, &uv, cpu(n));
    };
    srv(fsrOut_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 6);
    srv(fsrTex_[0].Get(), kFormat, 7);
    srv(fsrTex_[1].Get(), kFormat, 8);
    srv(fsrBlk_.Get(), DXGI_FORMAT_R16G16_FLOAT, 9);
    uav(fsrBlk_.Get(), DXGI_FORMAT_R16G16_FLOAT, 10);
    uav(fsrMv_.Get(), DXGI_FORMAT_R16G16_FLOAT, 11);
    uav(fsrDepth_.Get(), DXGI_FORMAT_R32_FLOAT, 12);
    uav(fsrReact_.Get(), DXGI_FORMAT_R8_UNORM, 13);
    srv(fsrTex_[1].Get(), kFormat, 14);
    srv(fsrTex_[0].Get(), kFormat, 15);
    srv(fsrBlk_.Get(), DXGI_FORMAT_R16G16_FLOAT, 16);

    // FFX temporal context: gamma-encoded colour, estimated motion vectors, synthetic depth, and no jitter
    ffxCreateBackendDX12Desc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = dev12_.Get();
    // FidelityFX API 4.1.1 runtimes use this version descriptor: type 0x0001000b.
    struct VersionDesc { ffxCreateContextDescHeader header; uint32_t version; } versionDesc{};
    versionDesc.header.type = 0x0001000bu;
    versionDesc.version = (4u << 22) | (1u << 12) | 1u;
    versionDesc.header.pNext = &backendDesc.header;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &versionDesc.header;
    desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = { width_, height_ };
    desc.maxUpscaleSize = { outW_, outH_ };
    desc.fpMessage = FfxMessage;
    const ffxReturnCode_t rc = ffxFn_.CreateContext(&ffxCtx_, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ffxCtx_)
    {
        ffxCtx_ = nullptr;
        Log("FFX flow: AMD's runtime could not make its upscaling context (code %u)", static_cast<unsigned>(rc));
        ReleaseFsrTemporal();
        return false;
    }
    if (ffxFn_.Query)
    {
        ffxQueryGetProviderVersion used{};
        used.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffxFn_.Query(&ffxCtx_, &used.header) == FFX_API_RETURN_OK && used.versionName)
            Log("FFX flow: the runtime runs %s", used.versionName);
    }
    fsrReady_ = true;
    fsrHavePrev_ = false;
    fsrCur_ = 1;
    fsrLast_ = std::chrono::steady_clock::now();
    Log("FFX flow: %ux%u -> %ux%u", width_, height_, outW_, outH_);
    return true;
}

// Per frame, copy the capture, estimate motion and depth, then dispatch the retained FFX temporal context.
bool Host::RecordFsrTemporal()
{
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list_->ResourceBarrier(1, &b);
    };
    const D3D12_RESOURCE_STATES nps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    fsrCur_ ^= 1;
    ID3D12Resource* curTex = fsrTex_[fsrCur_].Get();
    barrier(curTex, nps, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyResource(curTex, shared12_.Get());
    barrier(curTex, D3D12_RESOURCE_STATE_COPY_DEST, nps);

    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetComputeRootSignature(fsrRoot_.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE base = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    auto slot = [&](UINT n) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = base;
        h.ptr += static_cast<UINT64>(srvInc_) * n;
        return h;
    };
    const UINT bw = (width_ + 3) / 4, bh = (height_ + 3) / 4;
    const UINT consts[4] = { width_, height_, bw, bh };
    list_->SetComputeRoot32BitConstants(0, 4, consts, 0);
    list_->SetComputeRootDescriptorTable(1, slot(fsrCur_ == 0 ? 7 : 14)); // current frame, frame before, block vectors
    list_->SetComputeRootDescriptorTable(2, slot(10));

    barrier(fsrBlk_.Get(), nps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list_->SetPipelineState(fsrBlockPso_.Get());
    list_->Dispatch((bw + 7) / 8, (bh + 7) / 8, 1);
    barrier(fsrBlk_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nps);

    barrier(fsrMv_.Get(), nps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(fsrDepth_.Get(), nps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(fsrReact_.Get(), nps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list_->SetPipelineState(fsrPixelPso_.Get());
    list_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    barrier(fsrMv_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nps);
    barrier(fsrDepth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nps);
    barrier(fsrReact_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nps);

    const auto now = std::chrono::steady_clock::now();
    float frameMs = static_cast<float>(std::chrono::duration<double, std::milli>(now - fsrLast_).count());
    fsrLast_ = now;
    // Use the WGC source-frame interval to reduce CPU scheduling jitter in the temporal input timing.
    if (!srcGaps_.empty() && srcGaps_.back() >= 2.0 && srcGaps_.back() <= 100.0)
        frameMs = static_cast<float>(0.7 * srcGaps_.back() + 0.3 * frameMs);
    if (frameMs < 1.0f || frameMs > 250.0f)
        frameMs = 16.666f;

    barrier(fsrOut_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list_.Get();
    d.color = ffxApiGetResourceDX12(curTex, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(fsrDepth_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(fsrMv_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.reactive = ffxApiGetResourceDX12(fsrReact_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ); // where the motion cannot be trusted
    d.output = ffxApiGetResourceDX12(fsrOut_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = { 0.0f, 0.0f };
    d.motionVectorScale = { 1.0f, -1.0f }; // sub-pixel vectors with inverted Y for D3D12
    d.renderSize = { width_, height_ };
    d.upscaleSize = { outW_, outH_ };
    d.enableSharpening = false;
    d.sharpness = 0.0f;
    d.frameTimeDelta = frameMs;
    d.preExposure = 1.0f;
    d.reset = !fsrHavePrev_;
    d.cameraNear = 0.1f;
    d.cameraFar = 1000.0f;
    d.cameraFovAngleVertical = 1.04719755f; // 60 degrees in radians
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t rc = ffxFn_.Dispatch(&ffxCtx_, &d.header);
    if (rc != FFX_API_RETURN_OK)
    {
        Log("FFX flow: dispatch failed (code %u); falling back to the captured frame", static_cast<unsigned>(rc));
        barrier(fsrOut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        fsrReady_ = false;
        fsrHavePrev_ = false;
        return false;
    }
    barrier(fsrOut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fsrHavePrev_ = true;
    return true;
}

// The shared texture is recreated whenever the game's picture size changes: point SRV 0 at the new one.
bool Host::CreateFsrViews()
{
    if (!srvHeap_ || !shared12_)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = kFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    dev12_->CreateShaderResourceView(shared12_.Get(), &view, srvHeap_->GetCPUDescriptorHandleForHeapStart());
    if (fsrPipesOk_ && !InitFsrTemporal() && !ffxUnavailableLogged_)
    {
        Log("FFX temporal runtime is unavailable; displaying the captured frame directly");
        ffxUnavailableLogged_ = true;
    }
    return true;
}

void Host::RecordBlit(ID3D12Resource* backBuffer, UINT index, bool temporalOutput)
{
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list_->ResourceBarrier(1, &barrier);
    };

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * rtvInc_;
    dev12_->CreateRenderTargetView(backBuffer, nullptr, rtv);
    if (!temporalOutput)
        transition(shared12_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootSignature(blitRoot_.Get());
    list_->SetPipelineState(blitPso_.Get());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(outW_), static_cast<float>(outH_), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(outW_), static_cast<LONG>(outH_) };
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_GPU_DESCRIPTOR_HANDLE srv = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    if (temporalOutput)
        srv.ptr += static_cast<UINT64>(srvInc_) * 6;
    list_->SetGraphicsRootDescriptorTable(0, srv);
    list_->DrawInstanced(3, 1, 0, 0);

    if (!temporalOutput)
        transition(shared12_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
}

void Host::WaitFenceValue(UINT64 value)
{
    if (value == 0 || frameFence_->GetCompletedValue() >= value)
        return;
    frameFence_->SetEventOnCompletion(value, fenceEvent_);
    WaitForSingleObject(fenceEvent_, 2000);
}

void Host::WaitGpu()
{
    if (!queue_ || !frameFence_)
        return;
    ++frameFenceValue_;
    queue_->Signal(frameFence_.Get(), frameFenceValue_);
    WaitFenceValue(frameFenceValue_);
}

bool Host::PresentFrame(ID3D11Texture2D* src)
{
    // 1) D3D11: copy the captured frame into the shared texture, then signal the shared fence.
    D3D11_BOX box{ static_cast<UINT>(cropX_), static_cast<UINT>(cropY_), 0, static_cast<UINT>(cropX_) + width_,
                   static_cast<UINT>(cropY_) + height_, 1 };
    Q11* q = nullptr;
    if (!q11Failed_)
    {
        q = &q11_[q11Idx_];
        q11Idx_ = (q11Idx_ + 1) & 3;
        if (!q->dj)
        {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            bool ok = SUCCEEDED(dev11_->CreateQuery(&qd, &q->dj));
            qd.Query = D3D11_QUERY_TIMESTAMP;
            ok = ok && SUCCEEDED(dev11_->CreateQuery(&qd, &q->a)) && SUCCEEDED(dev11_->CreateQuery(&qd, &q->b));
            if (!ok)
            {
                q11Failed_ = true;
                q = nullptr;
            }
        }
        else if (q->pending)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
            UINT64 ta = 0, tb = 0;
            if (ctx11_->GetData(q->dj.Get(), &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx11_->GetData(q->a.Get(), &ta, sizeof(ta), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx11_->GetData(q->b.Get(), &tb, sizeof(tb), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
            {
                if (!dd.Disjoint && tb > ta && dd.Frequency)
                {
                    const double ms = 1000.0 * static_cast<double>(tb - ta) / static_cast<double>(dd.Frequency);
                    gpu11Ms_ += ms;
                    gpu11Max_ = (std::max)(gpu11Max_, ms);
                    ++gpu11N_;
                }
                q->pending = false;
            }
            else
            {
                q = nullptr; // not ready yet: skip measuring this frame
            }
        }
    }
    if (q)
    {
        ctx11_->Begin(q->dj.Get());
        ctx11_->End(q->a.Get());
    }
    ctx11_->CopySubresourceRegion(shared11_.Get(), 0, 0, 0, 0, src, 0, &box);
    if (q)
    {
        ctx11_->End(q->b.Get());
        ctx11_->End(q->dj.Get());
        q->pending = true;
    }
    ++sharedFenceValue_;
    ctx11_4_->Signal(fence11_.Get(), sharedFenceValue_);
    ctx11_->Flush();

    // 2) D3D12: wait for that copy on the GPU, then copy into the back buffer and present.
    queue_->Wait(fence12_.Get(), sharedFenceValue_);

    const UINT bbIdx = swap_->GetCurrentBackBufferIndex();
    const UINT idx = bbIdx;
    const auto tw0 = std::chrono::steady_clock::now();
    WaitFenceValue(allocFence_[idx]);
    {
        const double w = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw0).count();
        waitMs_ += w;
        liveWait_ += w;
    }

    ComPtr<ID3D12Resource> bb;
    if (FAILED(swap_->GetBuffer(bbIdx, IID_PPV_ARGS(&bb))))
        return false;

    if (tsRead_ && allocFence_[idx] != 0)
    {
        D3D12_RANGE rr{ static_cast<SIZE_T>(idx) * 16, static_cast<SIZE_T>(idx) * 16 + 16 };
        void* base = nullptr;
        if (SUCCEEDED(tsRead_->Map(0, &rr, &base)) && base)
        {
            const UINT64* t = reinterpret_cast<const UINT64*>(static_cast<const unsigned char*>(base) + static_cast<size_t>(idx) * 16);
            if (t[1] > t[0])
            {
                const double ms = 1000.0 * static_cast<double>(t[1] - t[0]) / static_cast<double>(tsFreq_);
                gpu12Ms_ += ms;
                gpu12Max_ = (std::max)(gpu12Max_, ms);
                ++gpu12N_;
            }
            D3D12_RANGE wr{ 0, 0 };
            tsRead_->Unmap(0, &wr);
        }
    }

    allocs_[idx]->Reset();
    list_->Reset(allocs_[idx].Get(), nullptr);
    if (tsHeap_)
        list_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2);

    const bool temporalOutput = blitReady_ && fsrReady_ && RecordFsrTemporal();
    if (scaleOn_ || temporalOutput)
    {
        RecordBlit(bb.Get(), idx, temporalOutput);
    }
    else
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list_->ResourceBarrier(1, &b);

        list_->CopyResource(bb.Get(), shared12_.Get());

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list_->ResourceBarrier(1, &b);
    }

    if (tsHeap_)
    {
        list_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2 + 1);
        list_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2, 2, tsRead_.Get(), static_cast<UINT64>(idx) * 16);
    }
    list_->Close();
    ID3D12CommandList* lists[] = { list_.Get() };
    queue_->ExecuteCommandLists(1, lists);
    bb.Reset();

    const auto tp0 = std::chrono::steady_clock::now();
    HRESULT hr = swap_->Present(0, 0);
    {
        const double p = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count();
        presentMs_ += p;
        livePresent_ += p;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        Log("Device lost on Present: 0x%08X (removed reason 0x%08X)", static_cast<unsigned>(hr),
            static_cast<unsigned>(dev12_->GetDeviceRemovedReason()));
        return false;
    }

    ++frameFenceValue_;
    queue_->Signal(frameFence_.Get(), frameFenceValue_);
    allocFence_[idx] = frameFenceValue_;
    return true;
}

// The captured frame includes non-client areas; the overlay displays the target's client area.
bool Host::ComputeCrop(UINT capW, UINT capH, int& cx, int& cy, UINT& w, UINT& h)
{
    cx = 0;
    cy = 0;
    w = capW;
    h = capH;

    RECT fr{};
    RECT cr{};
    POINT p{ 0, 0 };
    if (!GetTargetBounds(target_, fr) || !GetClientRect(target_, &cr) || !ClientToScreen(target_, &p))
        return false;

    int x = p.x - fr.left;
    int y = p.y - fr.top;
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;

    if (x < 0)
    {
        cw += x;
        x = 0;
    }
    if (y < 0)
    {
        ch += y;
        y = 0;
    }
    if (x + cw > static_cast<int>(capW))
        cw = static_cast<int>(capW) - x;
    if (y + ch > static_cast<int>(capH))
        ch = static_cast<int>(capH) - y;

    if (cw < 16 || ch < 16)
        return false;

    cx = x;
    cy = y;
    w = static_cast<UINT>(cw);
    h = static_cast<UINT>(ch);
    return true;
}

void Host::LogGeometry()
{
    RECT fr{};
    RECT wr{};
    RECT cr{};
    POINT p{ 0, 0 };
    GetTargetBounds(target_, fr);
    GetWindowRect(target_, &wr);
    GetClientRect(target_, &cr);
    ClientToScreen(target_, &p);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST), &mi);

    Log("Window rect (%ld,%ld)-(%ld,%ld), frame bounds (%ld,%ld)-(%ld,%ld), client %ldx%ld at (%ld,%ld)", wr.left, wr.top,
        wr.right, wr.bottom, fr.left, fr.top, fr.right, fr.bottom, cr.right - cr.left, cr.bottom - cr.top, p.x, p.y);
    Log("Monitor (%ld,%ld)-(%ld,%ld); showing %ux%u cropped at (%d,%d) from a %ux%u capture", mi.rcMonitor.left,
        mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, width_, height_, cropX_, cropY_, capW_, capH_);
}

// Called when the captured frame size or the client area changed.
bool Host::ApplyGeometry(UINT capW, UINT capH)
{
    int cx = 0;
    int cy = 0;
    UINT w = capW;
    UINT h = capH;
    ComputeCrop(capW, capH, cx, cy, w, h);

    const bool capChanged = capW != capW_ || capH != capH_;
    const bool outChanged = w != width_ || h != height_;
    Log("Geometry changed: capture %ux%u, showing %ux%u at crop (%d,%d)", capW, capH, w, h, cx, cy);

    WaitGpu();

    capW_ = capW;
    capH_ = capH;
    cropX_ = cx;
    cropY_ = cy;

    if (outChanged)
    {
        width_ = w;
        height_ = h;
        if (!scaleOn_)
        {
            outW_ = width_;
            outH_ = height_;
            HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, swapFlags_);
            if (FAILED(hr))
            {
                Log("ResizeBuffers failed: 0x%08X", static_cast<unsigned>(hr));
                return false;
            }
        }

        // In scaled modes the swap chain stays monitor sized; only the shared input texture changes.
        if (!CreateShared())
            return false;
    }

    if (capChanged)
    {
        pool_.Recreate(winrtDevice_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                       winrt::Windows::Graphics::SizeInt32{ static_cast<int32_t>(capW), static_cast<int32_t>(capH) });
    }

    lastBounds_ = {};
    MoveOverlay();
    return true;
}

// Cheap check, a few times a second: did the client area change inside the same captured frame?
bool Host::RefreshGeometry()
{
    int cx = 0;
    int cy = 0;
    UINT w = 0;
    UINT h = 0;
    if (!ComputeCrop(capW_, capH_, cx, cy, w, h))
        return true;

    if (w != width_ || h != height_)
        return ApplyGeometry(capW_, capH_);

    if (cx != cropX_ || cy != cropY_)
    {
        cropX_ = cx;
        cropY_ = cy;
        lastBounds_ = {};
    }
    return true;
}

void Host::MoveOverlay()
{
    RECT r{};
    if (!GetTargetBounds(target_, r))
        return;

    if (IsIconic(target_))
    {
        ShowWindow(overlay_, SW_HIDE);
        return;
    }
    if (!IsWindowVisible(overlay_))
        ShowWindow(overlay_, SW_SHOWNOACTIVATE);

    EnforceOverlayStyle();

    // OptiScaler can also move or resize the window it presents to (its "Force Borderless" option): put it back on
    // the game's picture.
    int ovX = 0, ovY = 0;
    OverlayPlace(r, ovX, ovY);
    RECT orc{};
    if (GetWindowRect(overlay_, &orc) &&
        (orc.left != ovX || orc.top != ovY || orc.right - orc.left != static_cast<LONG>(outW_) ||
         orc.bottom - orc.top != static_cast<LONG>(outH_)))
    {
        lastBounds_ = {};
    }

    if (r.left != lastBounds_.left || r.top != lastBounds_.top || r.right != lastBounds_.right || r.bottom != lastBounds_.bottom)
    {
        lastBounds_ = r;
        SetWindowPos(overlay_, HWND_TOPMOST, ovX, ovY, static_cast<int>(outW_), static_cast<int>(outH_), SWP_NOACTIVATE);
    }
    else
    {
        // Keep the overlay above a game that re-asserts its own topmost state (but not above the launcher window).
        // Only when some other window actually got above the overlay: re-ordering the topmost windows every 250 ms
        // while the game is running makes DWM redo its composition and costs the game frames.
        if (!LauncherInFront() && GetWindow(overlay_, GW_HWNDPREV) != nullptr)
            SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// OptiScaler may restyle the window it presents to, which is this overlay. Reapply the host's layered and input styles.
void Host::EnforceOverlayStyle()
{
    constexpr LONG_PTR mask = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_APPWINDOW;
    LONG_PTR want = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (!interactive_)
        want |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

    const LONG_PTR have = GetWindowLongPtrW(overlay_, GWL_EXSTYLE);
    if ((have & mask) == want)
        return;

    Log("Overlay style was changed from outside (0x%08lX), restoring it", static_cast<unsigned long>(have));
    SetWindowLongPtrW(overlay_, GWL_EXSTYLE, (have & ~mask) | want);
    SetLayeredWindowAttributes(overlay_, 0, 255, LWA_ALPHA);
    SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    lastBounds_ = {};
}

void Host::ToggleInteractive()
{
    keepGameUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    interactive_ = !interactive_;
    LONG_PTR ex = GetWindowLongPtrW(overlay_, GWL_EXSTYLE);
    if (interactive_)
        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    else
        ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(overlay_, GWL_EXSTYLE, ex);
    SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Interactive mode gives the overlay focus for menu input; click-through mode returns focus to the target window.
    if (interactive_)
    {
        const bool ok = ForceForeground(overlay_);
        Log("Click-through OFF: overlay takes focus (%s). Mouse and keyboard now go to the overlay / OptiScaler menu.",
            ok ? "ok" : "WINDOWS REFUSED, click once on the picture");
    }
    else
    {
        ForceForeground(target_);
        Log("Click-through ON: focus returned to the game.");
    }
}

void Host::KeepGameVisible(std::chrono::steady_clock::time_point now)
{
    if (now >= keepGameUntil_ || now - keepGameCheck_ < std::chrono::milliseconds(80))
        return;
    keepGameCheck_ = now;
    if (target_ && IsWindow(target_) && IsIconic(target_))
    {
        ShowWindow(target_, SW_SHOWNOACTIVATE); // back on screen, the focus stays where the shortcut put it
        Log("The game minimised itself when it lost the focus: put back on screen");
    }
}

// Windows only lets the foreground process hand the foreground to someone else. Attaching to the current foreground
// window's input thread (and a synthetic Alt tap) is the usual way around that for a window the user asked for.
bool Host::ForceForeground(HWND h)
{
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();

    bool attached = false;
    if (fgThread != 0 && fgThread != me)
        attached = AttachThreadInput(me, fgThread, TRUE) != FALSE;

    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);

    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);

    if (attached)
        AttachThreadInput(me, fgThread, FALSE);

    Sleep(60);
    const bool ok = GetForegroundWindow() == h;
    Log("Foreground request: %s (foreground is now \"%s\")", ok ? "ok" : "failed",
        Narrow(WindowTitle(GetForegroundWindow())).c_str());
    return ok;
}

void Host::SendMenuKey(bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(opt_.menuKey);
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(opt_.menuKey), MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedVk(opt_.menuKey) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
}

// Ctrl+Shift+F10: take focus for the overlay and press OptiScaler's menu key for you (a moment later, so that
// OptiScaler has seen the focus change first). Press it again to close the menu.
void Host::OpenMenu()
{
    keepGameUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    if (!interactive_)
        ToggleInteractive();
    else
        ForceForeground(overlay_);

    Log("Pressing the menu key (vk 0x%02X) for OptiScaler", opt_.menuKey);
    menuKeyStage_ = 1;
    menuKeyAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
}

// Once a second: write what the launcher shows in its live panel.
void Host::PublishLive(std::chrono::steady_clock::time_point now)
{
    const double dt = std::chrono::duration<double>(now - liveAt_).count();
    liveAt_ = now;
    if (dt <= 0.0)
        return;

    oc::LiveStats ls;
    ls.stamp = GetTickCount64();
    ls.pid = GetCurrentProcessId();
    const unsigned arr = arrivals_.load(std::memory_order_relaxed);
    ls.captureFps = static_cast<double>(arr - liveArrivals_) / dt;
    liveArrivals_ = arr;

    ls.presentFps = static_cast<double>(liveFrames_) / dt;
    ls.presentMs = liveFrames_ ? livePresent_ / static_cast<double>(liveFrames_) : 0.0;
    ls.waitMs = liveFrames_ ? liveWait_ / static_cast<double>(liveFrames_) : 0.0;
    liveFrames_ = 0;
    livePresent_ = 0.0;
    liveWait_ = 0.0;

    const HWND fg = GetForegroundWindow();
    ls.focus = fg == target_ ? 0 : (fg == overlay_ ? 1 : 2);
    oc::WriteLiveStats(ls);
}

void Host::Shutdown()
{
    DeleteFileW(oc::StatusPath().c_str());
    DeleteFileW((oc::StatusPath() + L".tmp").c_str());
    try
    {
        if (session_)
            session_.Close();
        if (pool_)
            pool_.Close();
    }
    catch (...)
    {
    }

    WaitGpu();
    ReleaseFsrTemporal();
    if (ffxModule_)
    {
        FreeLibrary(ffxModule_);
        ffxModule_ = nullptr;
    }

    UnregisterHotkeys();
    if (parentProc_)
        CloseHandle(parentProc_);
    swap_.Reset();
    g_spoofOverlay = nullptr;
    if (overlay_)
        DestroyWindow(overlay_);
    if (fenceEvent_)
        CloseHandle(fenceEvent_);
    if (frameEvent_)
        CloseHandle(frameEvent_);
}

bool Host::LauncherInFront() const
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    return wcscmp(cls, L"OptiCaptureLauncher") == 0;
}

// ---- Stop shortcut: a second, independent way to see it --------------------------------------------------------
// The stop shortcut is a RegisterHotKey message that only reaches the capture loop between two frames, and another program
// can hold the same key. A small thread therefore also watches the keys themselves; if the capture loop does not react in
// time (stuck inside a Present or a driver call) the process is ended, so STOP never leaves a half working capture behind.
std::atomic<bool> g_quitReq{ false };   // stop was asked for (by either way)
std::atomic<bool> g_loopDone{ false };  // the capture loop has seen it and left
std::atomic<UINT> g_stopMods{ 0 };
std::atomic<UINT> g_stopVk{ 0 };
std::atomic<bool> g_keyThreadOn{ false };

bool StopKeysDown()
{
    const UINT vk = g_stopVk.load();
    const UINT mods = g_stopMods.load();
    if (vk == 0)
        return false;
    auto down = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
    if (!down(static_cast<int>(vk)))
        return false;
    if ((mods & MOD_CONTROL) && !down(VK_CONTROL))
        return false;
    if ((mods & MOD_SHIFT) && !down(VK_SHIFT))
        return false;
    if ((mods & MOD_ALT) && !down(VK_MENU))
        return false;
    return true;
}

void StartStopKeyWatcher()
{
    if (g_keyThreadOn.exchange(true))
        return;
    std::thread([] {
        bool was = false;
        ULONGLONG askedAt = 0;
        for (;;)
        {
            Sleep(20);
            const bool down = StopKeysDown();
            if (down && !was && !g_quitReq.load())
            {
                g_quitReq = true;
                Log("Stop shortcut seen by the key watcher");
            }
            was = down;
            if (g_quitReq.load())
            {
                if (askedAt == 0)
                    askedAt = GetTickCount64();
                if (!g_loopDone.load() && GetTickCount64() - askedAt > 2000)
                {
                    Log("The capture loop did not react to the stop shortcut in 2 s: ending the process");
                    TerminateProcess(GetCurrentProcess(), 0);
                }
            }
        }
    }).detach();
}

void Host::RegisterHotkeys()
{
    UnregisterHotkeys();
    g_stopMods = st_.stop.mods;
    g_stopVk = st_.stop.vk;
    StartStopKeyWatcher();
    const struct
    {
        int id;
        oc::Hotkey key;
        const char* name;
    } keys[] = {
        { kHotkeyQuit, st_.stop, "stop" },
        { kHotkeyInteractive, st_.focus, "focus" }, // Ctrl+Shift+F11: game <-> overlay (not shown in the launcher)
        { kHotkeyMenu, st_.menu, "menu" },          // Ctrl+Shift+F10: press OptiScaler's menu key for it
    };

    for (const auto& k : keys)
    {
        if (!RegisterHotKey(nullptr, k.id, k.key.mods | MOD_NOREPEAT, k.key.vk))
            Log("Could not register the %s shortcut (mods %u, vk %u): already used by another program", k.name, k.key.mods, k.key.vk);
    }
}

void Host::UnregisterHotkeys()
{
    UnregisterHotKey(nullptr, kHotkeyQuit);
    UnregisterHotKey(nullptr, kHotkeyInteractive);
    UnregisterHotKey(nullptr, kHotkeyMenu);
}

// The launcher rewrites OptiCaptureHost.ini when the user changes something; pick that up while running. The file is
// written key by key, so wait until its time stamp has stayed the same for one check before reading it.
void Host::PollSettings()
{
    const unsigned long long stamp = oc::FileStamp(oc::SettingsPath());
    if (stamp == iniStamp_)
    {
        iniPending_ = 0;
        return;
    }
    if (stamp != iniPending_)
    {
        iniPending_ = stamp;
        return;
    }
    iniStamp_ = stamp;
    iniPending_ = 0;

    const oc::Settings old = st_;
    oc::LoadSettings(st_);
    if (st_.targetScale != old.targetScale)
        Log("Resolution selection changed; restart capture to apply");
    if (st_.stop != old.stop || st_.focus != old.focus || st_.menu != old.menu)
    {
        RegisterHotkeys();
        Log("Shortcuts updated");
    }
}

int Host::Run(const Options& opt, const oc::Settings& settings)
{
    opt_ = opt;
    st_ = settings;
    Log("Swap chain buffers: 3");

    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
            Log("Primary display: %lux%lu at %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    }
    iniStamp_ = oc::FileStamp(oc::SettingsPath());
    inst_ = GetModuleHandleW(nullptr);

    if (opt_.parent != 0)
        parentProc_ = OpenProcess(SYNCHRONIZE, FALSE, opt_.parent);

    if (!wgc::GraphicsCaptureSession::IsSupported())
    {
        Fail(L"Windows Graphics Capture is not supported on this system (Windows 10 1903 or newer is required).");
        return 1;
    }

    target_ = PickTarget(opt_);
    if (!target_)
    {
        Fail(L"No window to capture. Use --title \"part of the window title\", or switch to the game during the countdown.");
        return 1;
    }
    Log("Target window: \"%s\"", Narrow(WindowTitle(target_)).c_str());
    if (opt_.parent == 0)
        MessageBeep(MB_ICONASTERISK);

    if (!InitGraphics() || !InitCapture() || !InitOverlayAndSwapChain())
    {
        Fail(L"Initialisation failed. See OptiCaptureHost.log next to the exe.");
        Shutdown();
        return 1;
    }

    RegisterHotkeys();

    // Keep this host-local workaround for upscalers that require the capture overlay to appear foreground.
    focusSpoof_ = true;
    if (focusSpoof_)
    {
        g_realGetForeground = reinterpret_cast<GetForegroundFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow"));
        if (g_realGetForeground)
        {
            g_spoofTarget = target_;
            g_spoofOverlay = overlay_;
            PatchNewModules(spoofSeen_);
            spoofScanAt_ = std::chrono::steady_clock::now();
            Log("Focus spoof on");
        }
        else
        {
            focusSpoof_ = false;
        }
    }
    else
    {
        Log("Focus spoof off");
    }

    session_.StartCapture();
    liveAt_ = std::chrono::steady_clock::now();
    Log("Capture started. Host shortcuts control stop, overlay focus, and the OptiScaler menu.");

    auto lastHousekeeping = std::chrono::steady_clock::now();
    UINT64 frames = 0;
    int slowLogged = 0;
    auto statStart = lastHousekeeping;
    bool running = true;

    while (running)
    {
        MsgWaitForMultipleObjects(1, &frameEvent_, FALSE, 50, QS_ALLINPUT);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            else if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyQuit)
            {
                g_quitReq = true;
                running = false;
            }
            else if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyInteractive)
                ToggleInteractive();
            else if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyMenu)
                OpenMenu();
            else
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (g_quitReq.load())
            running = false;
        if (!running)
            break;
        if (targetClosed_ || !IsWindow(target_))
        {
            Log("Captured window closed");
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        KeepGameVisible(now);

        if (menuKeyStage_ != 0 && now >= menuKeyAt_)
        {
            if (menuKeyStage_ == 1)
            {
                SendMenuKey(false);
                menuKeyStage_ = 2;
                menuKeyAt_ = now + std::chrono::milliseconds(120);
            }
            else
            {
                SendMenuKey(true);
                menuKeyStage_ = 0;
            }
        }

        if (now - lastHousekeeping > std::chrono::milliseconds(250))
        {
            lastHousekeeping = now;
            const auto hk0 = std::chrono::steady_clock::now();
            if (focusSpoof_ && now - spoofScanAt_ > std::chrono::seconds(1))
            {
                spoofScanAt_ = now;
                PatchNewModules(spoofSeen_);
                spoofMaxMs_ = (std::max)(spoofMaxMs_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hk0).count());
            }
            if (parentProc_ && WaitForSingleObject(parentProc_, 0) == WAIT_OBJECT_0)
            {
                Log("Launcher closed");
                break;
            }
            PollSettings();
            if (!RefreshGeometry())
                break;
            MoveOverlay();
            houseMaxMs_ = (std::max)(houseMaxMs_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hk0).count());
        }
        if (now - liveAt_ >= std::chrono::seconds(1))
            PublishLive(now);
        if (now - statStart > std::chrono::seconds(5))
        {
            const HWND fg = GetForegroundWindow();
            Log("Presented %.1f fps (focus: %s)", static_cast<double>(frames) / std::chrono::duration<double>(now - statStart).count(),
                fg == target_ ? "game" : (fg == overlay_ ? "overlay" : "other"));
            if (frames > 0)
                Log("  per frame: fence wait %.2f ms, inside Present %.2f ms", waitMs_ / frames, presentMs_ / frames);
            waitMs_ = 0.0;
            presentMs_ = 0.0;

            // Frame times: averages hide stutter, so show the spread (99th percentile, longest gap, how many gaps were
            // more than 1.5 times the average).
            {
                std::vector<double> ag;
                {
                    std::lock_guard<std::mutex> lk(arrMutex_);
                    ag.swap(arrGaps_);
                }
                auto describe = [](std::vector<double>& v, double& avg, double& p99, double& mx, int& slow)
                {
                    avg = p99 = mx = 0.0;
                    slow = 0;
                    if (v.empty())
                        return false;
                    double sum = 0.0;
                    for (double x : v)
                        sum += x;
                    avg = sum / static_cast<double>(v.size());
                    for (double x : v)
                        if (x > avg * 1.5)
                            ++slow;
                    std::sort(v.begin(), v.end());
                    mx = v.back();
                    p99 = v[std::min<size_t>(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.99))];
                    return true;
                };
                double a, p, m;
                int s;
                if (describe(ag, a, p, m, s))
                    Log("  capture gaps: avg %.1f ms, 99%% %.1f ms, max %.1f ms, %d of %zu slow", a, p, m, s, ag.size());
                if (describe(presGaps_, a, p, m, s))
                    Log("  present gaps: avg %.1f ms, 99%% %.1f ms, max %.1f ms, %d of %zu slow", a, p, m, s, presGaps_.size());
                if (describe(latMs_, a, p, m, s))
                    Log("  frame age at Present (negative = frame is for a vblank still ahead): avg %.1f ms, 99%% %.1f ms, max %.1f ms",
                        a - 500.0, p - 500.0, m - 500.0);
                latMs_.clear();
                if (describe(srcGaps_, a, p, m, s))
                    Log("  frame time stamps from Windows: avg %.1f ms, 99%% %.1f ms, max %.1f ms, %d of %zu slow", a, p, m, s, srcGaps_.size());
                srcGaps_.clear();
                if (gpu11N_ > 0 || gpu12N_ > 0)
                    Log("  host GPU time per frame: D3D11 copy %.2f ms (max %.2f), D3D12 copy %.2f ms (max %.2f)",
                        gpu11N_ ? gpu11Ms_ / gpu11N_ : 0.0, gpu11Max_, gpu12N_ ? gpu12Ms_ / gpu12N_ : 0.0, gpu12Max_);
                gpu11Ms_ = gpu11Max_ = gpu12Ms_ = gpu12Max_ = 0.0;
                gpu11N_ = gpu12N_ = 0;
                Log("  housekeeping: longest %.2f ms (focus spoof scan %.2f ms)", houseMaxMs_, spoofMaxMs_);
                presGaps_.clear();
                houseMaxMs_ = 0.0;
                spoofMaxMs_ = 0.0;
            }

            // Which windows are above the overlay right now (anything topmost that covers it costs the overlay its fast path).
            {
                std::string above;
                int n = 0;
                for (HWND w = GetWindow(overlay_, GW_HWNDPREV); w && n < 4; w = GetWindow(w, GW_HWNDPREV))
                {
                    if (!IsWindowVisible(w))
                        continue;
                    above += " [" + Narrow(WindowTitle(w)) + "]";
                    ++n;
                }
                Log("  above the overlay:%s", above.empty() ? " nothing" : above.c_str());
            }
            frames = 0;
            statStart = now;
        }

        // Take the newest captured frame and drop older ones (lower latency than queueing them).
        wgc::Direct3D11CaptureFrame latest{ nullptr };
        for (;;)
        {
            auto f = pool_.TryGetNextFrame();
            if (!f)
                break;
            latest = f;
        }
        if (!latest)
            continue;
        const auto cs = latest.ContentSize();
        if (cs.Width <= 0 || cs.Height <= 0)
            continue;
        if (static_cast<UINT>(cs.Width) != capW_ || static_cast<UINT>(cs.Height) != capH_)
        {
            latest = nullptr;
            if (!ApplyGeometry(static_cast<UINT>(cs.Width), static_cast<UINT>(cs.Height)))
                break;
            continue;
        }

        ComPtr<ID3D11Texture2D> tex;
        {
            auto surface = latest.Surface();
            winrt::com_ptr<DxgiInterfaceAccess> access;
            winrt::check_hresult(winrt::get_unknown(surface)->QueryInterface(__uuidof(DxgiInterfaceAccess), access.put_void()));
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf())));
        }

        {
            LARGE_INTEGER qpc{}, freq{};
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&freq);
            const double nowMs = 1000.0 * static_cast<double>(qpc.QuadPart) / static_cast<double>(freq.QuadPart);
            const long long src = latest.SystemRelativeTime().count();
            const double age = nowMs - static_cast<double>(src) / 10000.0;
            if (latMs_.size() < 4096 && age > -500.0 && age < 2000.0)
                latMs_.push_back(age + 500.0); // shifted so negative values survive the statistics; shown shifted back
            if (lastSrc100ns_ != 0 && srcGaps_.size() < 4096)
                srcGaps_.push_back(static_cast<double>(src - lastSrc100ns_) / 10000.0);
            lastSrc100ns_ = src;
        }

        const auto t0 = std::chrono::steady_clock::now();
        if (presLast_.time_since_epoch().count() != 0 && presGaps_.size() < 4096)
            presGaps_.push_back(std::chrono::duration<double, std::milli>(t0 - presLast_).count());
        presLast_ = t0;
        if (!PresentFrame(tex.Get()))
            break;
        ++frames;
        ++liveFrames_;
        const double presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (presentMs > 250.0 && slowLogged < 20)
        {
            ++slowLogged;
            Log("Slow PresentFrame: %.0f ms", presentMs);
        }
    }

    g_loopDone = true;
    Log("Shutting down%s", g_quitReq.load() ? " (stop shortcut)" : "");
    // A watchdog prevents a stuck graphics-module detach from leaving the launcher waiting forever.
    std::thread([] {
        Sleep(2500);
        Log("Shutdown took too long: ending the process");
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
    Shutdown();
    Log("Shutdown done");
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The log goes next to the exe.
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"OptiCaptureHost.log";
    _wfopen_s(&g_log, p.c_str(), L"w");

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        oc::Settings settings;
        oc::LoadSettings(settings);

        Options opt;
        if (!ParseOptions(opt))
        {
            Fail(L"Options:\n  --title <text>\n  --delay <seconds> (default 5)\n  --no-clickthrough\n"
                 L"  --menu-key <virtual-key code, default Insert>\n  --hwnd <window handle>  (used by the launcher)\n"
                 L"  --parent <process id>  (used by the launcher)\n\nTip: use OptiCaptureLauncher.exe.");
            return 2;
        }

        int rc = 0;
        {
            Host host;
            rc = host.Run(opt, settings);
        }
        // Avoid DLL detach handlers in graphics modules loaded into the host process during shutdown.
        Log("Exit %d", rc);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
        return rc;
    }
    catch (const winrt::hresult_error& e)
    {
        Log("Exception: 0x%08X %s", static_cast<unsigned>(e.code().value), Narrow(std::wstring(e.message().c_str())).c_str());
        Fail(L"Unexpected error. See OptiCaptureHost.log next to the exe.");
        return 1;
    }
    catch (const std::exception& e)
    {
        Log("Exception: %s", e.what());
        Fail(L"Unexpected error. See OptiCaptureHost.log next to the exe.");
        return 1;
    }
}
