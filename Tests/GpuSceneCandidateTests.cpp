// Lista de candidatos e ranges (Fase 4 do GPU-DRIVEN-PLAN, decisao D5).
//
// Duas propriedades sob teste, e as duas tem modo de falha silencioso.
//
// 1. `RaytracingOnly` fica FORA. O proxy do terreno existe so para a TLAS e as tabelas de hit; o
//    terreno real e rasterizado pelo FTerrain. Sem herdar esse filtro, a expansao de ranges
//    desenha o chao DUAS vezes — e duas copias coincidentes do mesmo chao nao aparecem numa
//    comparacao visual estatica, so no custo e em z-fighting eventual.
//
// 2. Um range unico e N ranges sinteticos expandem para a MESMA lista. E o que autoriza world
//    cells a trocarem o produtor sem tocar no consumidor. Se as duas listas divergirem, a
//    diferenca so aparece quando alguem escrever o produtor por celula — fases adiante.
//
// A logica sob teste e CPU pura, mas o aparelho precisa de um device: o filtro de geometria
// pergunta `FGpuMesh::IsValid()`, e a unica maneira de um FGpuMesh ficar valido e apontar para uma
// fatia de um pool D3D12 de verdade. WARP basta — nada aqui e submetido.

#include <iostream>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Scene/GpuSceneCandidates.h"
#include "Smile/Scene/Scene.h"

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

    // Malha VALIDA o suficiente para o filtro de geometria. Um FGpuMesh nasce invalido, e todo
    // candidato cairia no balde "sem geometria" antes de chegar ao que o teste quer provar.
    Microsoft::WRL::ComPtr<ID3D12Device> CreateWarpDevice() {
        Microsoft::WRL::ComPtr<IDXGIFactory4> Factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)))) return nullptr;
        Microsoft::WRL::ComPtr<IDXGIAdapter> Warp;
        if (FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)))) return nullptr;
        Microsoft::WRL::ComPtr<ID3D12Device> Device;
        if (FAILED(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device))))
            return nullptr;
        return Device;
    }
}

int main() {
    std::cout << "Smile.GpuSceneCandidates\n";

    Microsoft::WRL::ComPtr<ID3D12Device> Device = CreateWarpDevice();
    if (!Device) {
        std::cerr << "  FAIL: device WARP nao pode ser criado\n";
        return 1;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> Pool =
        FGpuMesh::CreatePoolBuffer(Device.Get(), 64 * 1024);
    FGpuMesh Mesh;
    if (Pool) Mesh.InitFromPool(Pool, 0, 32 * 1024, 48 * 1024, 3, 3, 1);
    if (!Mesh.IsValid()) {
        std::cerr << "  FAIL: a malha do aparelho precisa ser valida\n";
        return 1;
    }

    FScene Scene;
    // Oito renderaveis, um de cada categoria que o produtor distingue, mais quatro normais.
    // Guardo os indices para as verificacoes nao dependerem da ordem de insercao.
    u32 IdxProxy = 0, IdxHidden = 0, IdxNoMesh = 0, IdxNoSlot = 0;
    std::vector<u32> ExpectedSlots;
    {
        auto Add = [&](const char* Name) -> FRenderable& {
            FRenderable R;
            R.Name = Name;
            R.Mesh = &Mesh;
            FRenderable& Added = Scene.AddRenderable(R);
            Added.GpuHandle = Scene.GpuSlots().Allocate();
            return Added;
        };

        for (u32 I = 0; I < 2; ++I) ExpectedSlots.push_back(Add("normal").GpuHandle.Slot);

        IdxProxy = static_cast<u32>(Scene.Renderables().size());
        Add("proxy do terreno").RaytracingOnly = true;

        ExpectedSlots.push_back(Add("normal").GpuHandle.Slot);

        IdxHidden = static_cast<u32>(Scene.Renderables().size());
        Add("oculto").Visible = false;

        IdxNoMesh = static_cast<u32>(Scene.Renderables().size());
        Add("sem malha").Mesh = nullptr;

        IdxNoSlot = static_cast<u32>(Scene.Renderables().size());
        {
            FRenderable& R = Add("sem residencia");
            Scene.GpuSlots().Retire(R.GpuHandle, FGpuFenceSet{});
        }

        ExpectedSlots.push_back(Add("normal").GpuHandle.Slot);
    }

    FGpuSceneCandidates Candidates;
    Check(Candidates.RefreshIfStale(Scene), "a primeira consulta constroi a lista");

    // --- quem entrou, e por que os outros nao ---------------------------------------------------
    CheckEq(Candidates.Stats().Candidates, 4u, "so os quatro renderaveis normais sao candidatos");
    CheckEq(Candidates.Stats().RaytracingOnly, 1u, "o proxy do terreno e contado como EXCLUIDO");
    CheckEq(Candidates.Stats().Hidden, 1u, "o oculto e contado a parte");
    CheckEq(Candidates.Stats().NoGeometry, 1u, "o sem malha e contado a parte");
    CheckEq(Candidates.Stats().InvalidSlot, 1u, "o sem residencia e contado a parte");
    CheckEq(Candidates.Stats().Overflow, 0u, "sem teto nao ha overflow");
    CheckEq(Candidates.HighWaterMark(), 4u, "o high-water acompanha o maior conjunto ja visto");

    // O slot do proxy NAO pode estar na lista. A contagem acima ja seria satisfeita por um filtro
    // que descartasse o renderavel errado, entao o teste confere a identidade.
    {
        const u32 ProxySlot = Scene.Renderables()[IdxProxy].GpuHandle.Slot;
        bool Found = false;
        for (u32 Slot : Candidates.InstanceIds()) Found = Found || Slot == ProxySlot;
        Check(!Found, "o slot do proxy RaytracingOnly nao aparece entre os candidatos");
    }

    // --- cena plana: um range so ----------------------------------------------------------------
    CheckEq(static_cast<u32>(Candidates.Ranges().size()), 1u, "cena plana produz um range unico");
    CheckEq(Candidates.Ranges()[0].FirstCandidate, 0u, "o range unico comeca no inicio da lista");
    CheckEq(Candidates.Ranges()[0].CandidateCount, 4u, "o range unico cobre a lista inteira");

    // --- range unico == N ranges sinteticos -----------------------------------------------------
    //
    // O GATE. Ranges de tamanhos diferentes, um deles vazio, cobrindo a mesma lista na mesma
    // ordem. Se o range voltar a ser interpretado como intervalo de SLOTS, esta comparacao quebra
    // — os slots vem de free list e nao sao contiguos.
    {
        std::vector<u32> DeUmRange, DeVarios;
        CheckEq(ExpandCandidateRanges(Candidates.Ranges(), Candidates.InstanceIds(), DeUmRange),
                0u, "o range unico nao e ignorado");

        const FInstanceRange Sinteticos[] = { { 0u, 1u }, { 1u, 0u }, { 1u, 2u }, { 3u, 1u } };
        CheckEq(ExpandCandidateRanges(Sinteticos, Candidates.InstanceIds(), DeVarios), 0u,
                "nenhum dos ranges sinteticos e ignorado");

        Check(DeUmRange == DeVarios,
              "range unico e ranges sinteticos expandem para a mesma lista");
        Check(DeUmRange == ExpectedSlots,
              "a lista expandida traz os slots dos candidatos, na ordem da cena");

        // A comparacao acima so tem valor se os slots NAO coincidirem com as posicoes na lista:
        // com uma lista identidade, um consumidor que interpretasse o range como intervalo de
        // SLOTS produziria a mesma saida e passaria. Os buracos vem dos renderaveis filtrados,
        // e esta guarda impede que uma remodelagem do aparelho os elimine sem querer.
        bool Identidade = true;
        for (u32 I = 0; I < DeUmRange.size(); ++I) Identidade = Identidade && DeUmRange[I] == I;
        Check(!Identidade, "os slots dos candidatos nao sao a propria posicao na lista");
    }

    // --- range fora dos limites e IGNORADO, nao truncado -----------------------------------------
    //
    // Truncar produziria uma lista mais curta e plausivel — geometria sumindo em silencio, que a
    // regra 6 do plano proibe. Ignorar deixa o range aparecer inteiro ou nao aparecer, e o retorno
    // denuncia.
    {
        std::vector<u32> Saida;
        const FInstanceRange Estourado[] = { { 0u, 2u }, { 3u, 99u } };
        CheckEq(ExpandCandidateRanges(Estourado, Candidates.InstanceIds(), Saida), 1u,
                "o range fora dos limites e contado como ignorado");
        CheckEq(static_cast<u32>(Saida.size()), 2u, "so o range valido contribui");

        // Soma que da a volta em u32: FirstCandidate + CandidateCount caberia se a conta fosse
        // feita em 32 bits, e o range absurdo passaria.
        std::vector<u32> Volta;
        const FInstanceRange Wrap[] = { { 0xFFFFFFF0u, 0x20u } };
        CheckEq(ExpandCandidateRanges(Wrap, Candidates.InstanceIds(), Volta), 1u,
                "range que da a volta em u32 e recusado");
        CheckEq(static_cast<u32>(Volta.size()), 0u, "nada e expandido a partir dele");
    }

    // --- a lista e PERSISTENTE: transform nao a reconstroi ---------------------------------------
    //
    // Se a versao de transforms entrasse no criterio, um arraste de gizmo reconstruiria a lista
    // por frame — o custo que ela existe para nao ter.
    Scene.BumpTransformsVersion();
    Check(!Candidates.RefreshIfStale(Scene), "mover objetos nao reconstroi a lista de candidatos");

    // Visibilidade, sim: ela decide membresia.
    Scene.Renderables()[IdxHidden].Visible = true;
    Scene.BumpStaticCastersVersion();
    Check(Candidates.RefreshIfStale(Scene), "mostrar um objeto reconstroi a lista");
    CheckEq(Candidates.Stats().Candidates, 5u, "o objeto revelado entra na lista");
    CheckEq(Candidates.Stats().Hidden, 0u, "e some do balde de ocultos");

    // --- teto e overflow -------------------------------------------------------------------------
    //
    // Overflow tem de ser CONTADO. Um teto que descartasse em silencio faria geometria sumir sem
    // deixar rastro, que e o modo de falha que a regra 6 do plano proibe.
    {
        FGpuSceneCandidates Apertado;
        Apertado.SetCapacity(2u);
        Apertado.Rebuild(Scene);
        CheckEq(Apertado.Stats().Candidates, 2u, "o teto limita a lista");
        CheckEq(Apertado.Stats().Overflow, 3u, "os que nao couberam sao contados");
        CheckEq(Apertado.Ranges()[0].CandidateCount, 2u, "o range acompanha o que coube");
    }

    (void)IdxNoMesh;
    (void)IdxNoSlot;

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
}
