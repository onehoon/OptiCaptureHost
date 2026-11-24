#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

namespace oc
{

using LogFn = void (*)(const char*, ...);

struct FfxInputResources
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* reactive = nullptr;
    bool reset = false;
};

class FfxInputBridge
{
  public:
    bool Initialize(ID3D12Device* device, UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight,
                    const char* adapterName, LogFn log, ffxApiMessage messageCallback);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, const FfxInputResources& inputs, long long sourceTimestamp100ns);
    void ResetHistory();
    void ReleaseContext();
    void Shutdown();

    bool Ready() const { return ready_; }
    ID3D12Resource* Output() const { return output_.Get(); }

  private:
    bool LoadRuntime();
    bool CreateOutput();
    bool HasRequiredFunctions(HMODULE module, ffxFunctions& functions) const;
    bool LoadSplitPair(const std::wstring& loaderPath, const std::wstring& upscalerPath, bool loaderBorrowed,
                       HMODULE loader, bool upscalerBorrowed, HMODULE upscaler);
    bool LoadMonolithic(const std::wstring& path, bool borrowed, HMODULE module);
    void LogModule(const wchar_t* role, HMODULE module, const std::wstring& path);
    void ResetTiming();

    LogFn log_ = nullptr;
    ffxApiMessage messageCallback_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> output_;
    UINT renderWidth_ = 0;
    UINT renderHeight_ = 0;
    UINT outputWidth_ = 0;
    UINT outputHeight_ = 0;
    bool ready_ = false;
    bool firstDispatchLogged_ = false;
    bool runtimeUnavailableLogged_ = false;

    HMODULE entryModule_ = nullptr;
    HMODULE upscalerModule_ = nullptr;
    bool ownsEntryModule_ = false;
    bool ownsUpscalerModule_ = false;
    std::wstring entryPath_;
    std::wstring upscalerPath_;
    ffxFunctions functions_{};
    ffxContext context_ = nullptr;

    // The FFX API contract keeps this descriptor chain alive until the context is destroyed.
    ffxCreateBackendDX12Desc backendDesc_{};
    ffxCreateContextDescUpscaleVersion versionDesc_{};
    ffxCreateContextDescUpscale contextDesc_{};

    long long previousSourceTimestamp100ns_ = 0;
    std::chrono::steady_clock::time_point previousDispatchTime_{};
};

} // namespace oc
