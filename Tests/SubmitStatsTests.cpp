// Contadores de submissão do FDrawSubmitCache (Fase 3 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir: o número que julga o A/B do material bindless era
// `Draws - BindlessDraws`. Essa conta assume DOIS estados por draw, e existem TRÊS — o terceiro é
// "não consome material", e ele é maioria em passes inteiros. O z-prepass opaco desenha a cena
// sólida sem tocar em material nenhum e nunca pode incrementar `BindlessDraws`; pela conta antiga
// todo opaco aparecia como fallback e contaminava exatamente a métrica usada para decidir se o
// caminho novo vale a pena.
//
// E não é uma propriedade por PASSE: nas sombras e na chuva, só os casters alpha-tested amarram
// material, no MESMO laço dos que não amarram. Por isso o contador tem de ser por ITEM, no ponto
// da submissão.
//
// Precisa de device e de command list porque `DrawMesh` grava de verdade (IASetVertexBuffers +
// DrawIndexedInstanced) e `BindMaterial` escreve dois parâmetros de root. A list é gravada e
// NUNCA executada: o que está sob teste é a contagem, não o desenho. WARP basta.

#include <iostream>
#include <string_view>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Renderer/CpuFrameStats.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"

// `ComPtr` sem qualificar seria ambiguo: o namespace Smile ja tem o proprio alias.
using namespace Smile;

namespace {
    int Failures = 0;

    void Check(bool _Condition, std::string_view _Message) {
        if (!_Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << "\n";
        }
    }

    void CheckEq(u32 _Got, u32 _Want, std::string_view _Message) {
        if (_Got != _Want) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << " (obtido " << _Got << ", esperado " << _Want
                      << ")\n";
        }
    }

    // Aparelho minimo: device WARP, uma command list aberta, um heap de descritores e a root
    // signature que o `FMaterial::Bind` espera encontrar (CBV em b1, tabela t0..t7) mais as
    // constantes de root do caminho bindless.
    struct FRig {
        Microsoft::WRL::ComPtr<ID3D12Device>              Device;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    Allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> List;
        Microsoft::WRL::ComPtr<ID3D12RootSignature>       Root;
        Microsoft::WRL::ComPtr<ID3D12Resource>            Pool;
        FTextureSRVHeap                                   Heap;
        FGpuMesh                                          Mesh;
        FMaterial                                         Material;

        // Indice do parametro de constantes de root, o mesmo papel do
        // FPipelineState::kMaterialRootIndex na root signature de producao.
        static constexpr u32 kMaterialRootIndex = 3;

        bool Build() {
            Microsoft::WRL::ComPtr<IDXGIFactory4> Factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)))) return false;
            Microsoft::WRL::ComPtr<IDXGIAdapter> Warp;
            if (FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)))) return false;
            if (FAILED(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&Device))))
                return false;

            if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&Allocator))))
                return false;
            if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 Allocator.Get(), nullptr, IID_PPV_ARGS(&List))))
                return false;

            D3D12_DESCRIPTOR_RANGE MaterialRange{};
            MaterialRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            MaterialRange.NumDescriptors     = kMaterialTextureSlots;
            MaterialRange.BaseShaderRegister = 0;

            D3D12_ROOT_PARAMETER Params[4]{};
            Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            Params[0].Descriptor.ShaderRegister = 0;
            Params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            Params[1].Descriptor.ShaderRegister = 1;
            Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            Params[2].DescriptorTable.NumDescriptorRanges = 1;
            Params[2].DescriptorTable.pDescriptorRanges   = &MaterialRange;
            Params[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            Params[3].Constants.ShaderRegister = 4;
            Params[3].Constants.Num32BitValues = 2;

            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters = _countof(Params);
            Desc.pParameters   = Params;
            Microsoft::WRL::ComPtr<ID3DBlob> Blob, Error;
            if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob,
                                                   &Error)))
                return false;
            if (FAILED(Device->CreateRootSignature(0, Blob->GetBufferPointer(),
                                                   Blob->GetBufferSize(), IID_PPV_ARGS(&Root))))
                return false;

            Heap.Initialize(Device.Get());
            Material.Finalize(Device.Get(), Heap);

            // Malha valida de verdade: `DrawMesh` sai cedo (e conta SkippedInvalid) sem ela, e o
            // caminho que interessa nunca seria alcancado.
            Pool = FGpuMesh::CreatePoolBuffer(Device.Get(), 64 * 1024);
            if (!Pool) return false;
            Mesh.InitFromPool(Pool, 0, 32 * 1024, 48 * 1024, 3, 3, 1);
            if (!Mesh.IsValid()) return false;

            ID3D12DescriptorHeap* Heaps[] = { Heap.Native() };
            List->SetDescriptorHeaps(_countof(Heaps), Heaps);
            List->SetGraphicsRootSignature(Root.Get());
            return true;
        }
    };

    // O caso do P2 da revisao: um passe que desenha SEM pedir material nao produz fallback de
    // material. E o z-prepass opaco inteiro.
    void TestarDrawSemMaterial(FRig& _Rig) {
        std::cout << "[submissao] draw sem material nao e fallback\n";
        FDrawSubmitStats Stats;
        FDrawSubmitCache Submit;
        Submit.Stats = &Stats;

        for (int i = 0; i < 5; ++i) Submit.DrawMesh(_Rig.List.Get(), &_Rig.Mesh);

        CheckEq(Stats.Draws, 5u, "os cinco draws foram contados");
        CheckEq(Stats.MaterialFallbackDraws, 0u,
                "nenhum deles conta como fallback de material");
        CheckEq(Stats.BindlessDraws, 0u, "nem como bindless");
        CheckEq(Stats.MaterialBinds, 0u, "e nenhum bind foi emitido");
    }

    // Caminho classico: quem PEDE material e nao esta no bindless e fallback, mesmo quando o
    // cache pula a amarracao por repetir o material.
    void TestarFallbackClassico(FRig& _Rig) {
        std::cout << "[submissao] draw com material no caminho classico\n";
        FDrawSubmitStats Stats;
        FDrawSubmitCache Submit;
        Submit.Stats = &Stats;

        for (int i = 0; i < 5; ++i) {
            Submit.BindMaterial(_Rig.List.Get(), _Rig.Heap, &_Rig.Material);
            Submit.DrawMesh(_Rig.List.Get(), &_Rig.Mesh);
        }

        CheckEq(Stats.Draws, 5u, "os cinco draws foram contados");
        CheckEq(Stats.MaterialFallbackDraws, 5u, "os cinco consomem material");
        CheckEq(Stats.BindlessDraws, 0u, "nenhum e bindless");
        // O filtro do cache e sobre a AMARRACAO, nao sobre o consumo: um item que reusa o
        // material do anterior consome material do mesmo jeito. Confundir os dois faria o
        // fallback parecer 1 onde ele e 5.
        CheckEq(Stats.MaterialBinds, 1u, "mas so a primeira amarracao foi emitida");
    }

    // Caminho bindless: conta do outro lado, e nao no fallback.
    void TestarBindless(FRig& _Rig) {
        std::cout << "[submissao] draw com material no caminho bindless\n";
        FDrawSubmitStats Stats;
        FDrawSubmitCache Submit;
        Submit.Stats             = &Stats;
        Submit.Bindless          = true;
        Submit.MaterialRootIndex = FRig::kMaterialRootIndex;

        for (int i = 0; i < 5; ++i) {
            Submit.BindMaterialRow(_Rig.List.Get(), &_Rig.Material, 7u);
            Submit.DrawMesh(_Rig.List.Get(), &_Rig.Mesh);
        }

        CheckEq(Stats.Draws, 5u, "os cinco draws foram contados");
        CheckEq(Stats.BindlessDraws, 5u, "os cinco leram a tabela canonica");
        CheckEq(Stats.MaterialFallbackDraws, 0u, "e nenhum conta como fallback");
        CheckEq(Stats.MaterialRowSets, 1u, "so a primeira escrita da linha foi emitida");
        CheckEq(Stats.MaterialRowMisses, 0u, "a linha 7 nao e a linha default");
    }

    // `Bindless` descreve o modo do PASSE, nao afirma que cada item le material. Quando sombras
    // opacas migrarem para a root bindless, os casters opacos continuarao sem BindMaterialRow.
    // Contar todo draw do passe inflaria o A/B exatamente como a formula antiga inflava o
    // fallback, apenas do outro lado.
    void TestarBindlessSemMaterial(FRig& _Rig) {
        std::cout << "[submissao] passe bindless, draw sem material\n";
        FDrawSubmitStats Stats;
        FDrawSubmitCache Submit;
        Submit.Stats             = &Stats;
        Submit.Bindless          = true;
        Submit.MaterialRootIndex = FRig::kMaterialRootIndex;

        for (int i = 0; i < 5; ++i) Submit.DrawMesh(_Rig.List.Get(), &_Rig.Mesh);

        CheckEq(Stats.Draws, 5u, "os cinco draws foram contados");
        CheckEq(Stats.BindlessDraws, 0u,
                "modo bindless sem pedido de material nao vira consumo bindless");
        CheckEq(Stats.MaterialFallbackDraws, 0u,
                "e tambem nao vira fallback de material");
    }

    // O laco MISTO, que e a forma real das sombras e da chuva: no mesmo passe, casters
    // alpha-tested pedem material e os opacos nao. O pedido nao pode vazar de um item para o
    // seguinte.
    void TestarLacoMisto(FRig& _Rig) {
        std::cout << "[submissao] laco misto (sombras/chuva)\n";
        FDrawSubmitStats Stats;
        FDrawSubmitCache Submit;
        Submit.Stats = &Stats;

        for (int i = 0; i < 6; ++i) {
            const bool AlphaTested = (i % 2) == 1;
            if (AlphaTested) Submit.BindMaterial(_Rig.List.Get(), _Rig.Heap, &_Rig.Material);
            Submit.DrawMesh(_Rig.List.Get(), &_Rig.Mesh);
        }

        CheckEq(Stats.Draws, 6u, "os seis draws foram contados");
        CheckEq(Stats.MaterialFallbackDraws, 3u,
                "so os tres que pediram material contam como fallback");
    }

    // Malha invalida DEPOIS de um pedido de material: o pedido morre ali e nao e herdado pelo
    // item seguinte, que nao pediu nada.
    void TestarPedidoNaoVaza(FRig& _Rig) {
        std::cout << "[submissao] pedido nao vaza por malha invalida\n";
        FDrawSubmitStats Stats;
        FDrawSubmitCache Submit;
        Submit.Stats = &Stats;

        FGpuMesh Invalida; // sem InitFromPool: IsValid() falso
        Submit.BindMaterial(_Rig.List.Get(), _Rig.Heap, &_Rig.Material);
        Submit.DrawMesh(_Rig.List.Get(), &Invalida);
        Submit.DrawMesh(_Rig.List.Get(), &_Rig.Mesh);

        CheckEq(Stats.SkippedInvalid, 1u, "a malha invalida foi pulada");
        CheckEq(Stats.Draws, 1u, "so o segundo item virou draw");
        CheckEq(Stats.MaterialFallbackDraws, 0u,
                "e ele nao herdou o pedido de material do item pulado");
    }
}

int main() {
    FRig Rig;
    if (!Rig.Build()) {
        std::cerr << "FALHA: nao consegui montar o aparelho em WARP\n";
        return 1;
    }

    TestarDrawSemMaterial(Rig);
    TestarFallbackClassico(Rig);
    TestarBindless(Rig);
    TestarBindlessSemMaterial(Rig);
    TestarLacoMisto(Rig);
    TestarPedidoNaoVaza(Rig);

    Rig.List->Close(); // gravada e nunca executada: o que esta sob teste e a contagem

    if (Failures != 0) {
        std::cerr << "\n" << Failures << " verificacao(oes) falharam\n";
        return 1;
    }
    std::cout << "\nTodas as verificacoes de contagem de submissao passaram\n";
    return 0;
}
