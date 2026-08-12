#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FGpuProfiler;
    class FTextureSRVHeap;

    // b0 compartilhado pelos passes de Lighting/ReSTIRDI*.cs.hlsl.
    struct alignas(256) ReSTIRDIConstants {
        Mat44 InvViewProj;
        Vec4  CameraPos;
        Vec4  ScreenParams; // W, H, 1/W, 1/H
        Vec4  Params;       // lightCount, frameIndex, shadowMask, rayEndMargin
        Vec4  Sampling;     // initialCandidates, teto ABSOLUTO de M, spatialCount, spatialRadius
        Vec4  Reuse;        // temporal, posRejectScale, normalReject, maxAge
        Vec4  TemporalPolicy; // x = permutation temporal (0/1); yzw reservados
        Vec4  RayEpsA;
        Vec4  RayEpsB;
        Mat44 View;          // world -> view; pack do NRD calcula viewZ linear
        Mat44 PrevViewProj;  // shadow motion vector: mundo anterior -> UV anterior
    };
    static_assert(sizeof(ReSTIRDIConstants) == 512,
                  "ReSTIRDIConstants deve ocupar fatias CBV alinhadas a 256 bytes");

    // ReSTIR DI de superficie primaria, em medida de ANGULO SOLIDO, sobre um pool combinado de
    // luzes analiticas locais E triangulos emissivos (FMeshLights). Pass A gera candidatas com
    // orcamento separado por pool — uniforme nas analiticas, por potencia via alias table nos
    // triangulos — e combina o historico; Pass B combina vizinhos, corrige o vies e traca no
    // maximo um shadow ray. O sol fica no caminho dedicado do deferred; desligado, o fallback e
    // o loop raster.
    class FReSTIRDI : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "ReSTIR DI"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device);

        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                            u32 Width, u32 Height,
                            u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                            u32 DepthSlot, u32 VelocitySlot, u32 TlasSlot, u32 InstanceSlot,
                            u32 MeshLightSlot, u32 MeshAliasSlot,
                            const u32 LightSlots[FCommandQueue::kFramesInFlight],
                            const u32 TransformSlots[FCommandQueue::kFramesInFlight],
                            const u32 SurfaceSlots[FCommandQueue::kFramesInFlight]);
        // Reescreve SO os descritores de mesh light das tabelas de trace, sem realocar alvo nem
        // reservoir. Chamar sempre que o FMeshLights se reconstruir fora do SetupForResize — hoje,
        // o caminho barato do Renderer::OnSceneStructureChanged. As tabelas guardam uma COPIA do
        // descritor, entao sem isto o DI le o pool/alias ja liberados e a tela estoura em branco.
        // Motivo completo no .cpp. O CHAMADOR garante as filas drenadas.
        void RefreshMeshLightDescriptors(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                         u32 MeshLightSlot, u32 MeshAliasSlot);
        void SetupNrdPack(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                          u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                          u32 DepthSlot, u32 VelocitySlot,
                          ID3D12Resource* NrdInViewZ, ID3D12Resource* NrdInNormalRough,
                          ID3D12Resource* NrdInMv, ID3D12Resource* NrdInDiffRadHit,
                          ID3D12Resource* NrdInSpecRadHit,
                          ID3D12Resource* NrdOutDiff, ID3D12Resource* NrdOutSpec);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Mat44& View,
                            const Mat44& PrevViewProj,
                            const Vec3& CameraPos,
                            u32 Width, u32 Height, u32 FrameIndex, u32 LightCount,
                            u32 ShadowRayMask, bool EnableTemporalPermutation,
                            u32 TemporalInstanceCount, bool MotionHistoryValid,
                            u32 MeshLightCount);
        void Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                    FGpuProfiler* Profiler = nullptr);
        void RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);
        void RecordNrdComposite(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        void InvalidateHistory() { NeedsClear = true; }
        void SetLightSetSignature(u64 Signature) {
            if (Signature != LightSetSignature) NeedsClear = true;
            LightSetSignature = Signature;
        }

        bool IsReady() const       { return Ready; }
        bool IsNrdReady() const    { return NrdReady; }
        u32  OutputSRVSlot() const { return OutSRV; }

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FComputePipeline InitialTemporalPSO; // 10 SRV, 2 UAV, bindless alpha-test
        FComputePipeline SpatialPSO;         // 12 SRV, 4 UAV, bindless alpha-test
        FComputePipeline NrdPackPSO;          // 8 SRV, 5 UAV
        FComputePipeline NrdCompositePSO;     // 6 SRV, 1 UAV

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        Microsoft::WRL::ComPtr<ID3D12Resource> RawDiffuse;  // rgb modulado, a = distancia da luz
        Microsoft::WRL::ComPtr<ID3D12Resource> RawSpecular; // rgb modulado, a = distancia da luz
        Microsoft::WRL::ComPtr<ID3D12Resource> ShadowMotion;// xy=MV, z=confianca, w=valido
        Microsoft::WRL::ComPtr<ID3D12Resource> ResW[2]; // R32F: W (o x1 saiu — reconstruido)
        Microsoft::WRL::ComPtr<ID3D12Resource> ResB[2]; // RGBA32_UINT: luz, uv, M+idade, n1 oct
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RawDiffuseState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RawSpecularState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ShadowMotionState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ResWState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES ResBState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kParityCount = 2;
        u32 OutSRV = kInvalidSlot;
        u32 OutUAV = kInvalidSlot;
        u32 RawDiffuseSRV = kInvalidSlot;
        u32 RawDiffuseUAV = kInvalidSlot;
        u32 RawSpecularSRV = kInvalidSlot;
        u32 RawSpecularUAV = kInvalidSlot;
        u32 ShadowMotionSRV = kInvalidSlot;
        u32 ShadowMotionUAV = kInvalidSlot;
        u32 ResWSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResBSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResWUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResBUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 InitialTable[kParityCount][FCommandQueue::kFramesInFlight] = {
            { kInvalidSlot, kInvalidSlot }, { kInvalidSlot, kInvalidSlot } };
        u32 InitialUAVTable[kParityCount] = { kInvalidSlot, kInvalidSlot };
        u32 SpatialTable[kParityCount][FCommandQueue::kFramesInFlight] = {
            { kInvalidSlot, kInvalidSlot }, { kInvalidSlot, kInvalidSlot } };
        u32 SpatialUAVTable = kInvalidSlot;
        u32 NrdPackSrvTable = kInvalidSlot;
        u32 NrdPackUavTable = kInvalidSlot;
        u32 NrdCompositeSrvTable = kInvalidSlot;
        u32 NrdOutDiffuseSRV = kInvalidSlot;
        u32 NrdOutSpecularSRV = kInvalidSlot;
        static_assert(FCommandQueue::kFramesInFlight == 2,
                      "ajustar inicializadores das tabelas do ReSTIR DI");

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;
        u32 FrameSlot = 0;
        ReSTIRDIConstants CPU{};

        u32 Width = 0, Height = 0;
        u32 FrameParity = 0;
        u64 LightSetSignature = 0;
        bool NeedsClear = true;
        bool Initialized = false;
        bool Ready = false;
        bool NrdReady = false;

        u32 InitialCandidates = 8;
        // RELATIVO ao M do frame atual, como o paper: o teto efetivo e MCapRatio * candidatas.
        // Era 20 ABSOLUTO, o que dava ~8x menos historico do que o paper pretende (20*8 = 160).
        // O ResB em UINT foi o que destravou isso — no formato antigo M saturava em 63.
        f32 MCapRatio = 20.0f;
        // De volta a 4 agora que a balance heuristic entrou no Pass B. O motivo de ter baixado
        // para 2 era o resolve normalizar por 1/M, cujo vies de escurecimento cresce com o numero
        // de vizinhos; com a correcao no lugar essa razao caiu. Cada vizinho a mais custa uma
        // avaliacao de DI_Evaluate e um reload de G-buffer, sem raio. O paper usa 5 no enviesado,
        // e o teto do array de dominios do shader e 8 vizinhos + o proprio pixel.
        u32 SpatialCount = 4;
        f32 SpatialRadius = 16.0f;
        f32 PosRejectScale = 0.01f;
        f32 NormalReject = 0.9f;
        // Subiu de 8 para 12 quando a visibilidade inicial (Alg. 5 passo 2) entrou: o MaxAge era
        // curativo para a falta dela, e com a amostra ocluida sendo descartada o historico pode
        // viver mais. O teto de 12 vinha do formato antigo do ResB; com a idade agora em 8 bits ela
        // vai ate 255, entao isto virou knob de tuning PURO, sem restricao de formato. Subir mais
        // deve ser medido, nao presumido, e por isso fica em 12 ate haver A/B.
        f32 MaxAge = 12.0f;
        bool Temporal = true;
        // Alg. 5 passo 2. Custa 1 raio/pixel a mais (2 no total com o resolve do Pass B); a chave
        // existe para dar A/B contra o custo.
        bool InitialVisibility = true;

        // Tira os triangulos emissivos do POOL de propostas (o buffer continua sendo extraido).
        // ON agora que existe orcamento por pool e alias table por potencia — sem os dois, o
        // recurso deixava a engine PIOR: proposta uniforme sobre 26.500 dava ~0,06% de chance de
        // uma candidata cair numa analitica, e o poste da rua sumia.
        //
        // Continua servindo de A/B: com off o pool volta a ser so analitico e o brilho tem de
        // bater com o caminho anterior, o que foi como a conversao de energia da migracao para
        // angulo solido foi validada.
        bool MeshLightsInPool = true;
        FRayEpsilonProfile RayEps;
    };
}
