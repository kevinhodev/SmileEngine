#include "Smile/Graphics/CloudNoise.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"

namespace Smile {
    void FCloudNoise::Initialize(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                 FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return;

        BaseNoise.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                         kBaseRes, kBaseRes, kBaseRes, 1, true);
        DetailNoise.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           kDetailRes, kDetailRes, kDetailRes, 1, true);

        {
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = kWeatherRes;
            Desc.Height           = kWeatherRes;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            WeatherState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, WeatherState, nullptr,
                IID_PPV_ARGS(&WeatherTex)));

            WeatherSRVSlot = _SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.Texture2D.MipLevels     = 1;
            _SRVHeap.CreateSRV(_Device, WeatherTex.Get(), SRVDesc, WeatherSRVSlot);

            WeatherUAVSlot = _SRVHeap.Allocate(1);
            D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
            UAVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
            UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            _SRVHeap.CreateUAV(_Device, WeatherTex.Get(), UAVDesc, WeatherUAVSlot);
        }

        BaseNoisePSO.Initialize(_Device, "BakeBaseNoise.cs_6_0.cso", false);
        DetailNoisePSO.Initialize(_Device, "BakeDetailNoise.cs_6_0.cso", false);
        WeatherPSO.Initialize(_Device, "BakeWeather.cs_6_0.cso", false);

        Initialized = true;
        Bake(_CmdQueue, _SRVHeap);
        LogInfo("Ruido de nuvens bakado: base 128^3 + detalhe 32^3 + weather 512^2");
    }

    void FCloudNoise::Bake(FCommandQueue& _CmdQueue, FTextureSRVHeap& _SRVHeap) {
        _CmdQueue.ResetForRecording();
        auto* CL = _CmdQueue.List();

        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        BaseNoise.Transition(CL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        BaseNoisePSO.Bind(CL);
        u32 BaseRes = kBaseRes;
        CL->SetComputeRoot32BitConstants(0, 1, &BaseRes, 0);
        CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(BaseNoise.SRVSlot()));   
        CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(BaseNoise.UAVSlot(0)));
        CL->Dispatch(kBaseRes / 8, kBaseRes / 8, kBaseRes / 8);
        BaseNoise.Transition(CL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        DetailNoise.Transition(CL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DetailNoisePSO.Bind(CL);
        u32 DetailRes = kDetailRes;
        CL->SetComputeRoot32BitConstants(0, 1, &DetailRes, 0);
        CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(DetailNoise.SRVSlot()));
        CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DetailNoise.UAVSlot(0)));
        CL->Dispatch(kDetailRes / 8, kDetailRes / 8, kDetailRes / 8);
        DetailNoise.Transition(CL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        WeatherPSO.Bind(CL);
        u32 WeatherParams[2] = { kWeatherRes, 1337u };
        CL->SetComputeRoot32BitConstants(0, 2, WeatherParams, 0);
        CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(WeatherSRVSlot)); 
        CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(WeatherUAVSlot));
        CL->Dispatch(kWeatherRes / 8, kWeatherRes / 8, 1);
        {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = WeatherTex.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = WeatherState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            CL->ResourceBarrier(1, &B);
            WeatherState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        _CmdQueue.ExecuteAndSync(Lists, 1);
    }
}
