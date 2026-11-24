#include "optical_flow_engine.h"

#include <d3dcompiler.h>

#include <string>

#include "fsr_flow_shader.h"

namespace oc
{

namespace
{
constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList->ResourceBarrier(1, &barrier);
}
}

bool OpticalFlowEngine::Initialize(ID3D12Device* device, UINT width, UINT height, LogFn log)
{
    Release();
    if (!device || width == 0 || height == 0)
        return false;

    device_ = device;
    width_ = width;
    height_ = height;
    log_ = log;
    descriptorIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!CreatePipelines() || !CreateResources())
    {
        if (log_)
            log_("Optical flow resources could not be initialized; using raw capture");
        Release();
        return false;
    }
    ready_ = true;
    return true;
}

bool OpticalFlowEngine::CreatePipelines()
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 3;
    rootDesc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        if (log_)
            log_("Optical flow root signature serialization failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    hr = device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (FAILED(hr))
    {
        if (log_)
            log_("Optical flow root signature creation failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    auto createPipeline = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline) {
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> compileError;
        const std::string source(kFsrFlowHlsl);
        hr = D3DCompile(source.c_str(), source.size(), "fsr_flow.hlsl", nullptr, nullptr, entry, "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &compileError);
        if (FAILED(hr))
        {
            if (log_)
                log_("Optical flow shader %s failed: 0x%08X %s", entry, static_cast<unsigned>(hr),
                     compileError ? static_cast<const char*>(compileError->GetBufferPointer()) : "");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_.Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        hr = device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr) && log_)
            log_("Optical flow pipeline %s failed: 0x%08X", entry, static_cast<unsigned>(hr));
        return SUCCEEDED(hr);
    };

    return createPipeline("CSBlock", blockPso_) && createPipeline("CSPixel", pixelPso_);
}

bool OpticalFlowEngine::CreateResources()
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto createTexture = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& resource, UINT width, UINT height, DXGI_FORMAT format,
                             D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        return SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kReadState, nullptr,
                                                          IID_PPV_ARGS(&resource)));
    };

    const UINT blockWidth = (width_ + 3) / 4;
    const UINT blockHeight = (height_ + 3) / 4;
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!createTexture(frames_[0], width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE) ||
        !createTexture(frames_[1], width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE) ||
        !createTexture(blocks_, blockWidth, blockHeight, DXGI_FORMAT_R16G16_FLOAT, uav) ||
        !createTexture(motionVectors_, width_, height_, DXGI_FORMAT_R16G16_FLOAT, uav) ||
        !createTexture(depth_, width_, height_, DXGI_FORMAT_R32_FLOAT, uav) ||
        !createTexture(reactive_, width_, height_, DXGI_FORMAT_R8_UNORM, uav))
    {
        if (log_)
            log_("Optical flow textures could not be created");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 10;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors_))))
        return false;

    auto cpu = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptors_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(descriptorIncrement_) * index;
        return handle;
    };
    auto createSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(resource, &desc, cpu(index));
    };
    auto createUav = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT index) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(resource, nullptr, &desc, cpu(index));
    };

    createSrv(frames_[0].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    createSrv(frames_[1].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 1);
    createSrv(blocks_.Get(), DXGI_FORMAT_R16G16_FLOAT, 2);
    createSrv(frames_[1].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 3);
    createSrv(frames_[0].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 4);
    createSrv(blocks_.Get(), DXGI_FORMAT_R16G16_FLOAT, 5);
    createUav(blocks_.Get(), DXGI_FORMAT_R16G16_FLOAT, 6);
    createUav(motionVectors_.Get(), DXGI_FORMAT_R16G16_FLOAT, 7);
    createUav(depth_.Get(), DXGI_FORMAT_R32_FLOAT, 8);
    createUav(reactive_.Get(), DXGI_FORMAT_R8_UNORM, 9);
    return true;
}

bool OpticalFlowEngine::Record(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, FfxInputResources& output)
{
    if (!ready_ || !commandList || !source)
        return false;

    currentFrame_ ^= 1;
    ID3D12Resource* current = frames_[currentFrame_].Get();
    ID3D12Resource* previous = frames_[currentFrame_ ^ 1].Get();
    Transition(commandList, current, kReadState, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(current, source);
    Transition(commandList, current, D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
    if (!historyValid_)
    {
        Transition(commandList, previous, kReadState, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyResource(previous, source);
        Transition(commandList, previous, D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
    }

    ID3D12DescriptorHeap* heaps[] = { descriptors_.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(root_.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE base = descriptors_->GetGPUDescriptorHandleForHeapStart();
    auto gpu = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = base;
        handle.ptr += static_cast<UINT64>(descriptorIncrement_) * index;
        return handle;
    };

    const UINT blockWidth = (width_ + 3) / 4;
    const UINT blockHeight = (height_ + 3) / 4;
    const UINT constants[4] = { width_, height_, blockWidth, blockHeight };
    commandList->SetComputeRoot32BitConstants(0, 4, constants, 0);
    commandList->SetComputeRootDescriptorTable(1, gpu(currentFrame_ == 0 ? 0 : 3));
    commandList->SetComputeRootDescriptorTable(2, gpu(6));

    Transition(commandList, blocks_.Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(blockPso_.Get());
    commandList->Dispatch((blockWidth + 7) / 8, (blockHeight + 7) / 8, 1);
    Transition(commandList, blocks_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);

    Transition(commandList, motionVectors_.Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, depth_.Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, reactive_.Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(pixelPso_.Get());
    commandList->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    Transition(commandList, motionVectors_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
    Transition(commandList, depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
    Transition(commandList, reactive_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);

    output.color = current;
    output.depth = depth_.Get();
    output.motionVectors = motionVectors_.Get();
    output.reactive = reactive_.Get();
    output.reset = !historyValid_;
    historyValid_ = true;
    return true;
}

void OpticalFlowEngine::Release()
{
    ready_ = false;
    historyValid_ = false;
    currentFrame_ = 1;
    descriptors_.Reset();
    root_.Reset();
    blockPso_.Reset();
    pixelPso_.Reset();
    frames_[0].Reset();
    frames_[1].Reset();
    blocks_.Reset();
    motionVectors_.Reset();
    depth_.Reset();
    reactive_.Reset();
    device_.Reset();
    width_ = height_ = descriptorIncrement_ = 0;
}

} // namespace oc
