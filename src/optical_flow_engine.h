#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "ffx_input_bridge.h"

namespace oc
{

class OpticalFlowEngine
{
  public:
    bool Initialize(ID3D12Device* device, UINT width, UINT height, LogFn log);
    bool Record(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, FfxInputResources& output);
    void ResetHistory() { historyValid_ = false; }
    void Release();

    bool Ready() const { return ready_; }

  private:
    bool CreatePipelines();
    bool CreateResources();

    LogFn log_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blockPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pixelPso_;
    Microsoft::WRL::ComPtr<ID3D12Resource> frames_[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> blocks_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motionVectors_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> reactive_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT descriptorIncrement_ = 0;
    UINT currentFrame_ = 1;
    bool historyValid_ = false;
    bool ready_ = false;
};

} // namespace oc
