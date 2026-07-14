#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {

    // Sun shafts volumétricos: raymarch meia-res da câmera até o depth amostrando o
    // CSM por passo (fase HG, densidade acoplada ao height fog) + ACUMULAÇÃO TEMPORAL
    // ping-pong (o IGN por frame só funciona integrado ao longo dos frames — sem isso
    // vira mancha, lição do A/B anterior). O fog apply consome o resultado via t2 com
    // upsample bilateral e desliga o DirectionalInscattering analítico.
    class FSunShafts {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        // Recria os RTs meia-res quando a resolução de render muda (história invalida junto).
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        // SunColorTimesIntensity = radiância real da key light (cor x intensidade);
        // CollapsedFogParams vem de FFogPass::CollapsedFogParams (mesmo meio do fog do
        // frame); PrevViewProj interno vem do ViewProjUnjittered do frame anterior.
        void UpdateVolumetric(u32 FrameSlot, const Vec3& DirToSun,
                              const Vec3& SunColorTimesIntensity,
                              const Vec4& CollapsedFogParams, f32 NoiseFrame,
                              const Mat44& InvViewProjFull, const Vec3& CameraWorldPos,
                              const Mat44& ViewProjUnjittered);

        // Raymarch + temporal nos RTs próprios. Depth em PIXEL_SHADER_RESOURCE e CSM
        // legível (EnsureReadable) antes. Muda viewport pra meia-res — o chamador
        // re-seta o viewport full depois.
        void RecordVolumetric(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                              u32 DepthSRVSlot, D3D12_GPU_VIRTUAL_ADDRESS CSMConstantsAddr,
                              u32 CSMShadowSRVSlot);

        // Slot que o fog consome: história acumulada (ou o raw, com temporal off).
        u32 VolumetricSRVSlot() const {
            return (VolTemporal && HistoryValid) ? HistSRVSlot[LastWritten] : VolSRVSlot;
        }

        // Chamar quando o efeito ficar inativo num frame (toggle/height fog off):
        // história e PrevVP ficam obsoletos, o próximo frame ativo recomeça do zero.
        void ResetHistory() { HistoryValid = false; HasPrevVP = false; }

        bool IsInitialized() const { return Initialized; }

        void SetVolIntensity(f32 V)  { VolIntensity = V; }
        f32  GetVolIntensity() const { return VolIntensity; }
        void SetVolPhaseG(f32 V)     { VolPhaseG = V; }
        f32  GetVolPhaseG() const    { return VolPhaseG; }
        void SetVolSteps(f32 V)      { VolSteps = V; }
        f32  GetVolSteps() const     { return VolSteps; }
        void SetVolMaxDist(f32 V)    { VolMaxDist = V; }
        f32  GetVolMaxDist() const   { return VolMaxDist; }
        void SetVolDust(f32 V)       { VolDust = V; }
        f32  GetVolDust() const      { return VolDust; }
        void SetVolTemporal(bool V) {
            if (V && !VolTemporal) HistoryValid = false; // religou: história é lixo
            VolTemporal = V;
        }
        bool GetVolTemporal() const { return VolTemporal; }

    private:
        struct alignas(256) VolConstants {
            Vec4  SunDirPhase;    // xyz = dir p/ a key light, w = g da fase HG
            Vec4  SunColorInt;    // rgb = cor x intensidade da luz, w = intensidade do efeito
            Vec4  FogDensityP;    // x = dens1 colapsada, y = falloff1, z = dens2, w = falloff2
            Vec4  MarchParams;    // x = passos, y = dist max, z = frame IGN, w = poeira
            Vec4  ScreenParams;   // dims do RT meia-res
            Mat44 InvViewProj;
            Vec4  CameraWorldPos;
        };

        struct alignas(256) TemporalConstants {
            Vec4  ScreenParams;   // dims do RT meia-res
            Vec4  TemporalParams; // x = peso do frame atual, y = história válida
            Mat44 InvViewProj;    // atual (full)
            Mat44 PrevViewProj;   // anterior SEM jitter
            Vec4  CameraWorldPos;
        };

        void BuildVolRootSignature(ID3D12Device* Device);
        void BuildTemporalRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);
        void CreateRTs(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        void TransitionTo(ID3D12GraphicsCommandList* CommandList, ID3D12Resource* Resource,
                          D3D12_RESOURCE_STATES& Tracked, D3D12_RESOURCE_STATES After);

        static constexpr u32 kInvalidSlot   = 0xFFFFFFFFu;
        static constexpr f32 kTemporalBlend = 0.1f; // ~10 frames acumulados (UE froxel: 0.9 de história)

        // raymarch: root sig própria (CSM em b3/t11/s2, registers do CSMCommon)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> VolRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VolPSO;
        // temporal: b0 + raw t0 + história t1
        Microsoft::WRL::ComPtr<ID3D12RootSignature> TemporalRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> TemporalPSO;

        // RTV heap: 0 = VolRT, 1..2 = HistoryRT ping-pong
        Microsoft::WRL::ComPtr<ID3D12Resource> VolRT;
        Microsoft::WRL::ComPtr<ID3D12Resource> HistoryRT[2];
        FDescriptorHeap RTVHeap;
        u32 VolSRVSlot     = kInvalidSlot;
        u32 HistSRVSlot[2] = { kInvalidSlot, kInvalidSlot };
        D3D12_RESOURCE_STATES VolState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_STATES HistState[2] = { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        u32 RTWidth = 0, RTHeight = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> VolCB;
        Microsoft::WRL::ComPtr<ID3D12Resource> TemporalCB;
        u8* VolMappedBase      = nullptr;
        u8* TemporalMappedBase = nullptr;
        u32 FrameSlot          = 0;

        // ping-pong da história + bookkeeping da reprojeção
        u32   LastWritten  = 0;
        u32   NextHistory  = 0;
        bool  HistoryValid = false;
        bool  HasPrevVP    = false;
        Mat44 StoredVP     = Mat44::Identity(); // ViewProjUnjittered do frame anterior

        f32  VolIntensity = 1.0f;
        f32  VolPhaseG    = 0.7f;
        f32  VolSteps     = 32.0f;
        f32  VolMaxDist   = 128.0f; // feixe é efeito de perto; 400m diluía os passos
        f32  VolDust      = 8.0f;   // multiplicador da densidade efetiva do passe (satura, não estoura)
        bool VolTemporal  = true;

        D3D12_GPU_VIRTUAL_ADDRESS VolCBAddr() const {
            return VolCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(VolConstants);
        }
        D3D12_GPU_VIRTUAL_ADDRESS TemporalCBAddr() const {
            return TemporalCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(TemporalConstants);
        }

        bool Initialized = false;
    };
}
