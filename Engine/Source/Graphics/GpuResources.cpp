#include "Smile/Graphics/GpuResources.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile::GpuResources {
    namespace {
        D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE Type) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = Type;
            // Os demais campos (CPUPageProperty, MemoryPoolPreference, node masks) ficam em
            // UNKNOWN/0, que o D3D12 resolve a partir do Type. So adapter multi-node precisaria
            // mexer neles, e a engine e single-GPU.
            return Heap;
        }

        ComPtr<ID3D12Resource> CreateCommitted(ID3D12Device* Device, D3D12_HEAP_TYPE HeapType,
                                               const D3D12_RESOURCE_DESC& Desc,
                                               D3D12_RESOURCE_STATES InitialState,
                                               const D3D12_CLEAR_VALUE* Clear) {
            const D3D12_HEAP_PROPERTIES Heap = HeapProps(HeapType);
            ComPtr<ID3D12Resource> Resource;
            SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                     InitialState, Clear,
                                                     IID_PPV_ARGS(&Resource)));
            return Resource;
        }
    }

    D3D12_RESOURCE_DESC Tex2DDesc(u32 _Width, u32 _Height, DXGI_FORMAT _Format,
                                  D3D12_RESOURCE_FLAGS _Flags, u32 _MipLevels, u32 _ArraySize) {
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = static_cast<UINT16>(_ArraySize);
        Desc.MipLevels        = static_cast<UINT16>(_MipLevels);
        Desc.Format           = _Format;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = _Flags;
        return Desc;
    }

    D3D12_RESOURCE_DESC Tex3DDesc(u32 _Width, u32 _Height, u32 _Depth, DXGI_FORMAT _Format,
                                  D3D12_RESOURCE_FLAGS _Flags, u32 _MipLevels) {
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = static_cast<UINT16>(_Depth);
        Desc.MipLevels        = static_cast<UINT16>(_MipLevels);
        Desc.Format           = _Format;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = _Flags;
        return Desc;
    }

    D3D12_RESOURCE_DESC BufferDesc(u64 _Bytes, D3D12_RESOURCE_FLAGS _Flags) {
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = _Bytes;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        // ROW_MAJOR e obrigatorio em buffer (o D3D12 rejeita UNKNOWN aqui).
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags            = _Flags;
        return Desc;
    }

    ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* _Device, u32 _Width, u32 _Height,
                                       DXGI_FORMAT _Format, D3D12_RESOURCE_FLAGS _Flags,
                                       D3D12_RESOURCE_STATES _InitialState,
                                       EVramCategory _Category,
                                       const D3D12_CLEAR_VALUE* _Clear,
                                       u32 _MipLevels, u32 _ArraySize) {
        const D3D12_RESOURCE_DESC Desc =
            Tex2DDesc(_Width, _Height, _Format, _Flags, _MipLevels, _ArraySize);
        ComPtr<ID3D12Resource> Texture =
            CreateCommitted(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc, _InitialState, _Clear);
        VramTracker::Register(Texture.Get(), _Category);
        return Texture;
    }

    ComPtr<ID3D12Resource> CreateTex3D(ID3D12Device* _Device, u32 _Width, u32 _Height, u32 _Depth,
                                       DXGI_FORMAT _Format, D3D12_RESOURCE_FLAGS _Flags,
                                       D3D12_RESOURCE_STATES _InitialState,
                                       EVramCategory _Category, u32 _MipLevels) {
        const D3D12_RESOURCE_DESC Desc =
            Tex3DDesc(_Width, _Height, _Depth, _Format, _Flags, _MipLevels);
        ComPtr<ID3D12Resource> Texture =
            CreateCommitted(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc, _InitialState, nullptr);
        VramTracker::Register(Texture.Get(), _Category);
        return Texture;
    }

    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _Device, u64 _Bytes,
                                        D3D12_RESOURCE_FLAGS _Flags,
                                        D3D12_RESOURCE_STATES _InitialState,
                                        EVramCategory _Category) {
        const D3D12_RESOURCE_DESC Desc = BufferDesc(_Bytes, _Flags);
        ComPtr<ID3D12Resource> Buffer =
            CreateCommitted(_Device, D3D12_HEAP_TYPE_DEFAULT, Desc, _InitialState, nullptr);
        VramTracker::Register(Buffer.Get(), _Category);
        return Buffer;
    }

    FUploadBuffer CreateUploadBuffer(ID3D12Device* _Device, u64 _SliceBytes, u32 _SliceCount,
                                     bool _ForConstantBuffer) {
        FUploadBuffer Out{};
        if (_SliceBytes == 0 || _SliceCount == 0) return Out;

        Out.SliceBytes = _ForConstantBuffer
            ? (_SliceBytes + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
              ~static_cast<u64>(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)
            : _SliceBytes;
        Out.SliceCount = _SliceCount;

        const D3D12_RESOURCE_DESC Desc = BufferDesc(Out.SliceBytes * _SliceCount);
        Out.Resource = CreateCommitted(_Device, D3D12_HEAP_TYPE_UPLOAD, Desc,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);

        // Range de leitura vazio: a CPU so escreve aqui. Sem Unmap — o mapeamento vive o
        // tempo todo do recurso, que e o ponto do upload heap persistente.
        const D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(Out.Resource->Map(0, &NoRead, reinterpret_cast<void**>(&Out.Mapped)));
        return Out;
    }

    ComPtr<ID3D12Resource> CreateReadbackBuffer(ID3D12Device* _Device, u64 _Bytes) {
        const D3D12_RESOURCE_DESC Desc = BufferDesc(_Bytes);
        return CreateCommitted(_Device, D3D12_HEAP_TYPE_READBACK, Desc,
                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvTex2D(DXGI_FORMAT _Format, u32 _MipLevels,
                                             u32 _MostDetailedMip) {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                    = _Format;
        Srv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels       = _MipLevels;
        Srv.Texture2D.MostDetailedMip = _MostDetailedMip;
        return Srv;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC UavTex2D(DXGI_FORMAT _Format, u32 _MipSlice) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format             = _Format;
        Uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Texture2D.MipSlice = _MipSlice;
        return Uav;
    }

    D3D12_RENDER_TARGET_VIEW_DESC RtvTex2D(DXGI_FORMAT _Format, u32 _MipSlice) {
        D3D12_RENDER_TARGET_VIEW_DESC Rtv{};
        Rtv.Format             = _Format;
        Rtv.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
        Rtv.Texture2D.MipSlice = _MipSlice;
        return Rtv;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvStructuredBuffer(u32 _NumElements, u32 _StrideBytes,
                                                        u64 _FirstElement) {
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                     = DXGI_FORMAT_UNKNOWN;
        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Buffer.FirstElement        = _FirstElement;
        Srv.Buffer.NumElements         = _NumElements;
        Srv.Buffer.StructureByteStride = _StrideBytes;
        Srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        return Srv;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC UavStructuredBuffer(u32 _NumElements, u32 _StrideBytes,
                                                         u64 _FirstElement) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                     = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.FirstElement        = _FirstElement;
        Uav.Buffer.NumElements         = _NumElements;
        Uav.Buffer.StructureByteStride = _StrideBytes;
        Uav.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;
        return Uav;
    }
}
