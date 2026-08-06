#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace nrd { struct Instance; }

namespace Smile {
    // Driver D3D12-direto do NRD (RELAX_DIFFUSE_SPECULAR), SEM NRI. CreateInstance -> GetInstanceDesc
    // (pipelines DXIL + pools + 2 samplers, TODOS dimensionados pelo desc em runtime) -> por frame
    // SetCommonSettings + GetComputeDispatches (lista de DispatchDesc que ESTA classe executa).
    //
    // BLINDAGEM (o driver manual foi o suspeito da corrupcao anterior): heaps/rings PROPRIOS,
    // dimensionados pelos maximos do InstanceDesc com asserts; bring-up ISOLADO (RunSelfTest sobre IO
    // zerado, com GPU-Based Validation) ANTES de entrar no frame real; RE-BIND do SRVHeap da engine
    // pelo caller apos Denoise/RunSelfTest (o NRD liga heap proprio).
    class FNrdDenoiser {
    public:
        enum class ESignalProfile : u8 { Indirect = 0, Direct = 1 };

        void Initialize(ID3D12Device* Device,
                        ESignalProfile Profile = ESignalProfile::Indirect);
        void Shutdown();
        bool IsAvailable() const { return Available; }

        // (Re)cria pool textures (perm+trans) + IO textures (GI-res) + descritores de staging.
        void SetupForResize(ID3D12Device* Device, u32 Width, u32 Height);
        // Devolve pools + IO (o grosso da VRAM) sem derrubar pipelines, root signature nem os
        // descriptor heaps — reallocar depois e so um SetupForResize. Existe para o Renderer
        // poder manter a instancia viva mas SEM memoria enquanto o denoiser nao e o NRD ou o
        // consumidor (ReSTIR GI/DI) esta desligado. Chamar so com a GPU ociosa.
        void ReleaseResources() { ReleaseResize(); }
        bool IsReady() const { return Ready; }

        // Per frame: matrizes NAO jitteradas (column-major p/ o NRD = memcpy direto da row-major da
        // engine, lida transposta) + jitter + frameIndex -> nrd::SetCommonSettings.
        void SetFrame(const Mat44& ViewToClip, const Mat44& ViewToClipPrev,
                      const Mat44& WorldToView, const Mat44& WorldToViewPrev,
                      const Vec2& Jitter, const Vec2& JitterPrev, u32 FrameIndex);

        // Bring-up isolado: zera as IO e roda os dispatches (valida driver sem dado real). Loga 1x.
        void RunSelfTest(ID3D12GraphicsCommandList* CL);

        // Executa os dispatches do NRD lendo as IN (ja preenchidas pelo pack) -> OUT. Liga o heap
        // PROPRIO; o caller deve RE-LIGAR o SRVHeap da engine depois.
        void Denoise(ID3D12GraphicsCommandList* CL);

        // Invalida o historico de acumulacao: o proximo SetFrame usa CLEAR_AND_RESTART.
        // Chamar quando o sinal de entrada muda de natureza (toggle ReSTIR/NRD, corte de camera).
        void InvalidateHistory() { NeedsClear = true; }

        // Sincroniza o tracking interno com escritas externas (pack): IN -> UAV antes do pack,
        // OUT -> leitura combinada apos o Denoise (deferred em pixel ou composite em compute).
        void TransitionInputsToWrite(ID3D12GraphicsCommandList* CL);
        void TransitionOutputToRead(ID3D12GraphicsCommandList* CL);

        // IO textures (a engine cria SRV/UAV proprios nelas: UAV p/ o pack escrever, SRV do OUT p/ o
        // deferred/composite ler). Indices casam com EIo abaixo. RELAX_DIFFUSE_SPECULAR: 2 sinais
        // (difuso = ReSTIR GI; especular = reflexao) compartilham MV/NormalRough/ViewZ.
        // OUT do RELAX: .rgb = radiancia LINEAR (sem YCoCg, ao contrario do REBLUR), .w = history
        // length em frames.
        enum EIo {
            IO_MV = 0, IO_NORMAL_ROUGHNESS, IO_VIEWZ,
            IO_DIFF_RADIANCE_HITDIST, IO_SPEC_RADIANCE_HITDIST,
            IO_OUT_DIFF, IO_OUT_SPEC, IO_COUNT
        };
        ID3D12Resource* IoResource(EIo Which) const;

        u32 Width() const  { return RtWidth; }
        u32 Height() const { return RtHeight; }

    private:
        struct FNrdTexture {
            Microsoft::WRL::ComPtr<ID3D12Resource> Res;
            D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
            u32 SrvStaging = 0xFFFFFFFFu; // indice na StagingHeap (SRV)
            u32 UavStaging = 0xFFFFFFFFu; // indice na StagingHeap (UAV)
        };

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPipelines(ID3D12Device* Device);
        void CreateHeapsAndCB(ID3D12Device* Device);
        void ReleaseResize();
        FNrdTexture* MapResource(u32 nrdResourceType, u32 indexInPool);
        D3D12_CPU_DESCRIPTOR_HANDLE StagingCpu(u32 Index) const;
        void RecordDispatches(ID3D12GraphicsCommandList* CL); // loop comum (self-test e Denoise)

        nrd::Instance* Instance = nullptr;
        ESignalProfile SignalProfile = ESignalProfile::Indirect;
        // Rotulo do breakdown de VRAM. Separa as DUAS instancias (direta e indireta), que sao
        // alocadas em paralelo e na resolucao cheia — juntas elas dominam a categoria GI.
        const char* PoolLabel() const {
            return SignalProfile == ESignalProfile::Direct ? "NRD · pools (direta)"
                                                           : "NRD · pools (indireta)";
        }
        bool Available = false;
        bool Ready     = false;
        bool SelfTestLogged = false;

        Microsoft::WRL::ComPtr<ID3D12Device>        Dev;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> Pipelines; // 1:1 com InstanceDesc.pipelines

        std::vector<FNrdTexture> PermPool;   // permanentPool
        std::vector<FNrdTexture> TransPool;  // transientPool
        FNrdTexture Io[IO_COUNT];            // IN_MV, IN_NORMAL_ROUGHNESS, IN_VIEWZ, IN_DIFF_RAD_HITDIST, OUT

        // Heaps proprios (isolamento). Staging = NON-shader-visible (fonte do CopyDescriptors).
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> StagingHeap;
        u32 StagingCount = 0;
        u32 HandleSize   = 0;
        // TableHeap = shader-visible, ring de (perSetTex+perSetUav) por dispatch.
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TableHeap;
        u32 TableRingCapacity = 0; // em descritores
        u32 TableRingOffset   = 0;
        // CB ring (upload), uma entrada por dispatch.
        Microsoft::WRL::ComPtr<ID3D12Resource> CbRing;
        u8* CbRingMapped = nullptr;
        u32 CbStride     = 0;      // align(cbMax, 256)
        u32 CbRingCount  = 0;      // nº de entradas
        u32 CbRingOffset = 0;

        // Limites do InstanceDesc (blindagem: asserts contra estes).
        u32 PerSetTex = 0;
        u32 PerSetUav = 0;
        u32 SetsMax   = 0;
        u32 TableStride = 0; // PerSetTex + PerSetUav
        u32 ResourcesSpace = 0;
        u32 CbSpace = 1, CbReg = 0, SamplerBaseReg = 0;

        u32 RtWidth = 0, RtHeight = 0;
        bool NeedsClear = false; // limpa IO/pools no 1o record pos-setup
    };
}
