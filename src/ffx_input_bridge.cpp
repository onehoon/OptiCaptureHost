#include "ffx_input_bridge.h"

#include <windows.h>
#include <winver.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

namespace oc
{

namespace
{
static_assert(FFX_UPSCALER_VERSION_MAJOR == 4 && FFX_UPSCALER_VERSION_MINOR == 1 && FFX_UPSCALER_VERSION_PATCH == 1);
static_assert(FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 == 0x2u);
static_assert(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE == 0x00010000u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_UPSCALE == 0x00010001u);
static_assert(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION == 0x0001000bu);

std::wstring ModulePath(HMODULE module)
{
    if (!module)
        return {};
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    return std::wstring(buffer.data(), length);
}

std::wstring ExecutableDirectory()
{
    std::wstring path = ModulePath(GetModuleHandleW(nullptr));
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    path.resize(slash + 1);
    return path;
}

std::wstring DirectoryOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
}

bool SameDirectory(const std::wstring& left, const std::wstring& right)
{
    std::wstring leftDirectory = DirectoryOf(left);
    std::wstring rightDirectory = DirectoryOf(right);
    auto lower = [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); };
    std::transform(leftDirectory.begin(), leftDirectory.end(), leftDirectory.begin(), lower);
    std::transform(rightDirectory.begin(), rightDirectory.end(), rightDirectory.begin(), lower);
    return !leftDirectory.empty() && leftDirectory == rightDirectory;
}

std::string ToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string FileVersion(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return "unknown";

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || !info ||
        infoSize < sizeof(VS_FIXEDFILEINFO))
        return "unknown";

    char version[64]{};
    sprintf_s(version, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
              HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return version;
}

HMODULE LoadModuleAtPath(const std::wstring& path)
{
    if (path.empty() || !FileExists(path))
        return nullptr;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
}

bool FfxInputBridge::HasRequiredFunctions(HMODULE module, ffxFunctions& functions) const
{
    functions = {};
    if (!module)
        return false;
    ffxLoadFunctions(&functions, module);
    return functions.CreateContext && functions.DestroyContext && functions.Dispatch;
}

void FfxInputBridge::LogModule(const wchar_t* role, HMODULE module, const std::wstring& path)
{
    if (!log_)
        return;
    const std::wstring actualPath = path.empty() ? ModulePath(module) : path;
    const std::string pathUtf8 = ToUtf8(actualPath);
    const std::string roleUtf8 = ToUtf8(role ? role : L"FFX module");
    const std::string version = FileVersion(actualPath);
    log_("FFX %s: %s (file version %s)", roleUtf8.c_str(), pathUtf8.empty() ? "unknown path" : pathUtf8.c_str(),
         version.c_str());
}

bool FfxInputBridge::LoadSplitPair(const std::wstring& loaderPath, const std::wstring& upscalerPath, bool loaderBorrowed,
                                   HMODULE loader, bool upscalerBorrowed, HMODULE upscaler)
{
    bool ownsLoader = false;
    bool ownsUpscaler = false;
    if (!loader)
    {
        loader = LoadModuleAtPath(loaderPath);
        ownsLoader = loader != nullptr;
    }
    if (!upscaler)
    {
        upscaler = LoadModuleAtPath(upscalerPath);
        ownsUpscaler = upscaler != nullptr;
    }

    const std::wstring actualLoaderPath = ModulePath(loader);
    const std::wstring actualUpscalerPath = ModulePath(upscaler);
    if (!loader || !upscaler || !SameDirectory(actualLoaderPath, actualUpscalerPath))
    {
        if (log_)
        {
            const std::string loaderUtf8 = ToUtf8(actualLoaderPath);
            const std::string upscalerUtf8 = ToUtf8(actualUpscalerPath);
            log_("FFX split runtime rejected: loader '%s', upscaler '%s' must be a matching pair in one directory",
                 loaderUtf8.c_str(), upscalerUtf8.c_str());
        }
        if (ownsLoader)
            FreeLibrary(loader);
        if (ownsUpscaler)
            FreeLibrary(upscaler);
        return false;
    }

    ffxFunctions functions{};
    if (!HasRequiredFunctions(loader, functions))
    {
        if (log_)
            log_("FFX split loader does not expose the required public API");
        if (ownsLoader)
            FreeLibrary(loader);
        if (ownsUpscaler)
            FreeLibrary(upscaler);
        return false;
    }

    entryModule_ = loader;
    upscalerModule_ = upscaler;
    ownsEntryModule_ = ownsLoader && !loaderBorrowed;
    ownsUpscalerModule_ = ownsUpscaler && !upscalerBorrowed;
    entryPath_ = actualLoaderPath;
    upscalerPath_ = actualUpscalerPath;
    functions_ = functions;
    LogModule(L"loader", entryModule_, entryPath_);
    LogModule(L"upscaler provider", upscalerModule_, upscalerPath_);
    return true;
}

bool FfxInputBridge::LoadMonolithic(const std::wstring& path, bool borrowed, HMODULE module)
{
    bool owns = false;
    if (!module)
    {
        module = LoadModuleAtPath(path);
        owns = module != nullptr;
    }
    ffxFunctions functions{};
    if (!module || !HasRequiredFunctions(module, functions))
    {
        if (module && owns && !borrowed)
            FreeLibrary(module);
        if (log_)
            log_("FFX monolithic runtime is unavailable or does not expose the required public API");
        return false;
    }

    entryModule_ = module;
    upscalerModule_ = nullptr;
    ownsEntryModule_ = owns && !borrowed;
    ownsUpscalerModule_ = false;
    entryPath_ = ModulePath(module);
    upscalerPath_.clear();
    functions_ = functions;
    LogModule(L"monolithic runtime", entryModule_, entryPath_);
    return true;
}

bool FfxInputBridge::LoadRuntime()
{
    if (entryModule_ && functions_.CreateContext && functions_.DestroyContext && functions_.Dispatch)
        return true;

    HMODULE loadedLoader = GetModuleHandleW(L"amd_fidelityfx_loader_dx12.dll");
    HMODULE loadedUpscaler = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll");
    HMODULE loadedMonolithic = GetModuleHandleW(L"amd_fidelityfx_dx12.dll");
    const std::wstring directory = ExecutableDirectory();
    const std::wstring loaderPath = directory + L"amd_fidelityfx_loader_dx12.dll";
    const std::wstring upscalerPath = directory + L"amd_fidelityfx_upscaler_dx12.dll";
    const std::wstring monolithicPath = directory + L"amd_fidelityfx_dx12.dll";

    if (loadedLoader || loadedUpscaler)
    {
        const std::wstring canonicalPath = ModulePath(loadedLoader ? loadedLoader : loadedUpscaler);
        const std::wstring canonicalDirectory = DirectoryOf(canonicalPath);
        const std::wstring pairedLoaderPath = canonicalDirectory + L"amd_fidelityfx_loader_dx12.dll";
        const std::wstring pairedUpscalerPath = canonicalDirectory + L"amd_fidelityfx_upscaler_dx12.dll";
        if (loadedLoader && loadedUpscaler && !SameDirectory(ModulePath(loadedLoader), ModulePath(loadedUpscaler)))
        {
            if (log_)
                log_("FFX split modules are already loaded from different directories; refusing to mix providers");
            return false;
        }
        return LoadSplitPair(pairedLoaderPath, pairedUpscalerPath, loadedLoader != nullptr, loadedLoader,
                             loadedUpscaler != nullptr, loadedUpscaler);
    }

    // Reuse a runtime already loaded by OptiScaler so its public API hooks also observe host-created contexts.
    if (loadedMonolithic)
        return LoadMonolithic(ModulePath(loadedMonolithic), true, loadedMonolithic);

    const bool hasLoader = FileExists(loaderPath);
    const bool hasUpscaler = FileExists(upscalerPath);
    if (hasLoader && hasUpscaler && LoadSplitPair(loaderPath, upscalerPath, false, nullptr, false, nullptr))
        return true;
    if ((hasLoader || hasUpscaler) && log_)
        log_("FFX split runtime files are incomplete or invalid; checking the monolithic fallback");

    if (FileExists(monolithicPath) && LoadMonolithic(monolithicPath, false, nullptr))
        return true;

    if (!runtimeUnavailableLogged_ && log_)
    {
        log_("FFX runtime unavailable: expected a matching loader/upscaler pair or amd_fidelityfx_dx12.dll beside the host");
        runtimeUnavailableLogged_ = true;
    }
    return false;
}

bool FfxInputBridge::CreateOutput()
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = outputWidth_;
    desc.Height = outputHeight_;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&output_)));
}

bool FfxInputBridge::Initialize(ID3D12Device* device, UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight,
                                const char* adapterName, LogFn log, ffxApiMessage messageCallback)
{
    ReleaseContext();
    log_ = log;
    messageCallback_ = messageCallback;
    device_ = device;
    renderWidth_ = renderWidth;
    renderHeight_ = renderHeight;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    ready_ = false;
    firstDispatchLogged_ = false;
    ResetTiming();

    if (log_)
        log_("FFX input bridge: adapter %s, render %ux%u, output %ux%u", adapterName ? adapterName : "unknown",
             renderWidth_, renderHeight_, outputWidth_, outputHeight_);
    if (!device_ || renderWidth_ == 0 || renderHeight_ == 0 || outputWidth_ == 0 || outputHeight_ == 0 || !LoadRuntime())
        return false;

    if (!CreateOutput())
    {
        if (log_)
            log_("FFX output texture creation failed; using raw capture");
        return false;
    }

    backendDesc_ = {};
    backendDesc_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc_.device = device_.Get();
    versionDesc_ = {};
    versionDesc_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc_.header.pNext = &backendDesc_.header;
    versionDesc_.version = FFX_UPSCALER_VERSION;
    contextDesc_ = {};
    contextDesc_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    contextDesc_.header.pNext = &versionDesc_.header;
    contextDesc_.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    contextDesc_.maxRenderSize = { renderWidth_, renderHeight_ };
    contextDesc_.maxUpscaleSize = { outputWidth_, outputHeight_ };
    contextDesc_.fpMessage = messageCallback_;

    const ffxReturnCode_t result = functions_.CreateContext(&context_, &contextDesc_.header, nullptr);
    if (result != FFX_API_RETURN_OK || !context_)
    {
        ReleaseContext();
        if (log_)
            log_("FFX context creation failed (code %u); using raw capture", static_cast<unsigned>(result));
        return false;
    }

    if (functions_.Query)
    {
        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (functions_.Query(&context_, &provider.header) == FFX_API_RETURN_OK && provider.versionName && log_)
            log_("FFX provider version: %s", provider.versionName);
    }

    ready_ = true;
    if (log_)
        log_("FFX context created for %ux%u -> %ux%u", renderWidth_, renderHeight_, outputWidth_, outputHeight_);
    return true;
}

void FfxInputBridge::ResetTiming()
{
    previousSourceTimestamp100ns_ = 0;
    previousDispatchTime_ = {};
}

void FfxInputBridge::ResetHistory()
{
    ResetTiming();
}

bool FfxInputBridge::Dispatch(ID3D12GraphicsCommandList* commandList, const FfxInputResources& inputs,
                              long long sourceTimestamp100ns)
{
    if (!ready_ || !commandList || !inputs.color || !inputs.depth || !inputs.motionVectors || !inputs.reactive || !output_)
        return false;

    const auto now = std::chrono::steady_clock::now();
    bool discontinuity = inputs.reset;
    float frameTimeMs = 16.666f;
    if (!discontinuity && sourceTimestamp100ns > 0 && previousSourceTimestamp100ns_ > 0)
    {
        const long long delta100ns = sourceTimestamp100ns - previousSourceTimestamp100ns_;
        const double sourceDeltaMs = static_cast<double>(delta100ns) / 10000.0;
        if (sourceDeltaMs >= 1.0 && sourceDeltaMs <= 250.0)
            frameTimeMs = static_cast<float>(sourceDeltaMs);
        else
            discontinuity = true;
    }
    else
    {
        discontinuity = true;
    }

    if (discontinuity)
    {
        if (previousDispatchTime_.time_since_epoch().count() != 0)
        {
            const double fallbackMs = std::chrono::duration<double, std::milli>(now - previousDispatchTime_).count();
            if (fallbackMs >= 1.0 && fallbackMs <= 250.0)
                frameTimeMs = static_cast<float>(fallbackMs);
        }
    }
    previousDispatchTime_ = now;
    previousSourceTimestamp100ns_ = sourceTimestamp100ns > 0 ? sourceTimestamp100ns : 0;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &barrier);

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = commandList;
    dispatch.color = ffxApiGetResourceDX12(inputs.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(inputs.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(inputs.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.reactive = ffxApiGetResourceDX12(inputs.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = ffxApiGetResourceDX12(output_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = { 0.0f, 0.0f };
    dispatch.motionVectorScale = { 1.0f, -1.0f };
    dispatch.renderSize = { renderWidth_, renderHeight_ };
    dispatch.upscaleSize = { outputWidth_, outputHeight_ };
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = frameTimeMs;
    dispatch.preExposure = 1.0f;
    dispatch.reset = discontinuity;
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.04719755f;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    const ffxReturnCode_t result = functions_.Dispatch(&context_, &dispatch.header);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);
    if (result != FFX_API_RETURN_OK)
    {
        ready_ = false;
        ResetTiming();
        if (log_)
            log_("FFX dispatch failed (code %u); using raw capture", static_cast<unsigned>(result));
        return false;
    }

    if (!firstDispatchLogged_)
    {
        firstDispatchLogged_ = true;
        if (log_)
            log_("FFX first dispatch succeeded");
    }
    return true;
}

void FfxInputBridge::ReleaseContext()
{
    ready_ = false;
    if (context_ && functions_.DestroyContext)
    {
        const ffxReturnCode_t result = functions_.DestroyContext(&context_, nullptr);
        if (result != FFX_API_RETURN_OK && log_)
            log_("FFX context destruction returned code %u", static_cast<unsigned>(result));
    }
    context_ = nullptr;
    output_.Reset();
    backendDesc_ = {};
    versionDesc_ = {};
    contextDesc_ = {};
    ResetTiming();
}

void FfxInputBridge::Shutdown()
{
    ReleaseContext();
    if (ownsEntryModule_ && entryModule_)
        FreeLibrary(entryModule_);
    if (ownsUpscalerModule_ && upscalerModule_)
        FreeLibrary(upscalerModule_);
    entryModule_ = nullptr;
    upscalerModule_ = nullptr;
    ownsEntryModule_ = false;
    ownsUpscalerModule_ = false;
    entryPath_.clear();
    upscalerPath_.clear();
    functions_ = {};
    device_.Reset();
}

} // namespace oc
