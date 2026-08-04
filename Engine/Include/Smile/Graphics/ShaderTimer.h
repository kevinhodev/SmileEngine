#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Instrumentacao de timer nos shaders de ray tracing (tecnica do artigo da NVIDIA
    // "Profiling DXR shaders with timer instrumentation"). O shader le o timer global da GPU
    // antes e depois do trecho caro e grava o delta num alvo; o visualizador de render targets
    // mostra em heatmap. E a resposta p/ "ONDE no quadro o raio custa caro" — o FGpuProfiler
    // responde "quanto o passe inteiro custa", que e outra pergunta.
    //
    // Depende da extensao HLSL da NVAPI (NvGetSpecial). Fora de GPU NVIDIA, ou sem o SDK no
    // build, IsAvailable() e false e as permutacoes instrumentadas nem sao criadas — o resto do
    // renderer nao muda em nada. Ver Shaders/Debug/ShaderTimer.hlsli p/ o lado do shader.
    //
    // Uma instancia por PASSE instrumentado (cada um tem seu dominio de dispatch: o ReSTIR GI e
    // full-res, o trace de reflexao e half-res).
    class FShaderTimer {
    public:
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        // Slot falso da extensao (u999, space0). Convencao do RTXDI, e PRECISA casar com o
        // NV_SHADER_EXTN_SLOT do ShaderTimer.hlsli.
        static constexpr u32 kExtnSlot  = 999;
        static constexpr u32 kExtnSpace = 0;

        // Liga a NVAPI e reserva o slot falso NO DEVICE. A partir daqui toda PSO criada nele
        // enxerga a extensao — fica ligado p/ o app inteiro em vez de um escopo por PSO porque
        // u999 nao colide com nada e o custo e zero p/ quem nao usa o slot. False = sem NVAPI
        // (outro fabricante, driver antigo, ou build sem o SDK).
        static bool InitializeApi(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        static void ShutdownApi();
        static bool IsAvailable();

        // Handle bindado no root param do slot falso. O driver troca os acessos a esse UAV pela
        // instrucao real, entao o recurso nunca e tocado de fato: ele existe so p/ a debug layer
        // nao acusar tabela de descritores nao setada no dispatch.
        static D3D12_GPU_DESCRIPTOR_HANDLE ExtnTable(const FTextureSRVHeap& SRVHeap);

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Release(FTextureSRVHeap& SRVHeap);

        bool IsReady() const { return Target != nullptr; }
        // Indice bindless do UAV — e isto que vai no CB do passe instrumentado.
        u32  UavSlot() const { return UAVSlot; }
        u32  SrvSlot() const { return SRVSlot; }

        // Zera o alvo e o deixa em UNORDERED_ACCESS. Obrigatorio todo frame: pixel que sai cedo
        // (ceu, rugosidade alta) nao escreve, e sem o clear ele congelaria a medida de um frame
        // antigo — o heatmap mentiria justamente onde nao houve trabalho.
        void Clear(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);
        // Deixa legivel pelo visualizador (que e um passe grafico).
        void ToRead(ID3D12GraphicsCommandList* CL);

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> Target;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        u32 UAVSlot = kInvalidSlot;
        u32 SRVSlot = kInvalidSlot;
    };
}
