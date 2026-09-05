// Ciclo de vida dos descritores do heap de SRV (Fase 3 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir: ate a Fase 3, soltar um range de descritor era `Free` —
// devolucao imediata ao free list. Enquanto o unico caminho que soltava descritor era troca de
// cena, com as duas filas drenadas, isso passava. A partir do momento em que o editor ROTACIONA a
// tabela de texturas de um material com a GPU em voo, um range devolvido cedo demais e reescrito
// pelo proximo Allocate enquanto uma command list ja submetida ainda o le. O sintoma nao e
// "textura errada por um frame": e comportamento indefinido.
//
// A propriedade sob teste e uma so, em varias formas: um range aposentado NAO volta ao free list
// enquanto as DUAS fences nao passarem, e volta assim que passarem.
//
// Precisa de device porque o FTextureSRVHeap cria dois heaps de descritor de verdade no
// Initialize; a aritmetica de free list e CPU pura, mas exercita-la contra um heap falso testaria
// outra coisa. WARP basta — nada aqui depende de GPU real.

#include <iostream>
#include <string_view>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/UploadQueue.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Texture.h"

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

    // O range aposentado fica FORA do free list ate as duas fences passarem, e volta depois.
    void TestarAposentadoriaPorFence(ID3D12Device* _Device) {
        std::cout << "[descritores] aposentadoria por fence\n";
        FTextureSRVHeap Heap;
        Heap.Initialize(_Device);

        const u32 First = Heap.Allocate(8);
        CheckEq(First, 0u, "primeiro range comeca no slot 0");
        CheckEq(Heap.UsedSlots(), 8u, "oito slots em uso");
        CheckEq(Heap.RetiredSlots(), 0u, "nada aposentado ainda");

        Heap.FreeDeferred(First, 8, FGpuFenceSet{ 10, 5 });
        CheckEq(Heap.UsedSlots(), 0u, "o aposentado sai de UsedSlots");
        CheckEq(Heap.RetiredSlots(), 8u, "e entra em RetiredSlots");

        // A proxima alocacao NAO pode cair sobre o range em espera.
        const u32 Second = Heap.Allocate(8);
        Check(Second != First, "range aposentado nao e reusado antes da fence");
        CheckEq(Second, 8u, "a alocacao seguinte pula o range em espera");

        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 9, 4 }), 0u, "fences atrasadas nao coletam");
        // So a direta chegou: a compute ainda pode estar lendo.
        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 10, 4 }), 0u,
                "so a direta ter passado nao basta");
        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 9, 5 }), 0u,
                "so a compute ter passado nao basta");
        CheckEq(Heap.RetiredSlots(), 8u, "o range continua preso");

        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 10, 5 }), 1u, "as duas fences liberam o range");
        CheckEq(Heap.RetiredSlots(), 0u, "nada mais aposentado");
        CheckEq(Heap.UsedSlots(), 8u, "o coletado nao volta a contar como usado");

        CheckEq(Heap.Allocate(8), First, "o range coletado volta ao free list");
    }

    // Varios aposentados com fences diferentes: cada um sai quando o SEU alvo e coberto. O do
    // MEIO da lista sair junto e o caso que um swap-and-pop mal feito erraria.
    void TestarColetaParcial(ID3D12Device* _Device) {
        std::cout << "[descritores] coleta parcial\n";
        FTextureSRVHeap Heap;
        Heap.Initialize(_Device);

        const u32 A = Heap.Allocate(4);
        const u32 B = Heap.Allocate(4);
        const u32 C = Heap.Allocate(4);
        Heap.FreeDeferred(A, 4, FGpuFenceSet{ 100, 0 });
        Heap.FreeDeferred(B, 4, FGpuFenceSet{ 200, 0 });
        Heap.FreeDeferred(C, 4, FGpuFenceSet{ 150, 0 });
        CheckEq(Heap.RetiredSlots(), 12u, "tres ranges aposentados");

        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 150, 0 }), 2u, "coleta os dois cobertos");
        CheckEq(Heap.RetiredSlots(), 4u, "sobra so o que ainda espera");

        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 200, 0 }), 1u, "o ultimo sai quando coberto");
        CheckEq(Heap.RetiredSlots(), 0u, "fila vazia");
    }

    // Coalescencia: dois ranges vizinhos coletados viram um range unico. Sem ela, uma alocacao
    // maior que cada metade falharia com o heap na pratica vazio.
    void TestarCoalescencia(ID3D12Device* _Device) {
        std::cout << "[descritores] coalescencia dos coletados\n";
        FTextureSRVHeap Heap;
        Heap.Initialize(_Device);

        const u32 A = Heap.Allocate(4); // 0..3
        const u32 B = Heap.Allocate(4); // 4..7
        Heap.FreeDeferred(A, 4, FGpuFenceSet{ 1, 1 });
        Heap.FreeDeferred(B, 4, FGpuFenceSet{ 1, 1 });
        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 1, 1 }), 2u, "os dois vizinhos sao coletados");
        CheckEq(Heap.Allocate(8), A, "os dois viram um range de oito");
    }

    // Heap CHEIO: a republicacao RECUSA, e o material fica exatamente como estava.
    //
    // Este e o caso que o P1 da revisao apontou. O caminho de edicao chega aqui com o ponteiro
    // e a flag do material JA trocados pelo chamador; se a alocacao lancasse, a excecao subiria
    // por um callback do Qt e o material CPU ficaria descrevendo uma textura que a tabela GPU e
    // o CBV nao conhecem — sem erro visivel, ate o proximo load.
    void TestarRecusaComHeapCheio(ID3D12Device* _Device) {
        std::cout << "[descritores] republicacao recusada com o heap cheio\n";
        FTextureSRVHeap Heap;
        Heap.Initialize(_Device);

        FMaterial Material;
        Material.Finalize(_Device, Heap);
        const u32 Base = Material.AlbedoDescriptorIndex();

        // Enche o que sobrou, EXATAMENTE: nao ha mais range de oito nenhum.
        const u32 Sobra = FTextureSRVHeap::kCapacity - Heap.UsedSlots();
        Heap.Allocate(Sobra);
        CheckEq(Heap.UsedSlots(), FTextureSRVHeap::kCapacity, "o heap ficou cheio");
        const u32 FalhasAntes = Heap.AllocationFailures();

        u32 Retired = 0xDEADBEEFu;
        Check(!Material.RepublishTextureTable(_Device, Heap, Retired),
              "republicacao recusa em vez de lancar");
        CheckEq(Retired, FMaterial::kInvalidDescriptorTable,
                "nada foi aposentado na recusa");
        CheckEq(Material.AlbedoDescriptorIndex(), Base,
                "a base publicada continua a mesma");
        CheckEq(Heap.UsedSlots(), FTextureSRVHeap::kCapacity,
                "a recusa nao consumiu slot nenhum");
        CheckEq(Heap.RetiredSlots(), 0u, "nem aposentou nada");
        CheckEq(Heap.AllocationFailures(), FalhasAntes + 1u,
                "a recusa e contada em AllocationFailures");

        // E a tabela do material continua utilizavel: republicar de novo, com espaco de volta,
        // parte da MESMA base.
        Heap.Free(Base + kMaterialTextureSlots, kMaterialTextureSlots);
        u32 Segunda = 0xDEADBEEFu;
        Check(Material.RepublishTextureTable(_Device, Heap, Segunda),
              "com espaco de volta a republicacao aceita");
        CheckEq(Segunda, Base, "e aposenta a base que estava publicada antes da recusa");
    }

    // A troca interativa precisa criar a PROPRIA textura antes de republicar a tabela do
    // material. Heap cheio deve ser decidido antes de abrir a fila COPY: testar apenas a
    // republicacao com uma textura ja pronta deixaria o caminho real ainda capaz de lancar por
    // dentro do callback Qt e abandonar uma command list parcialmente gravada.
    void TestarImportacaoRecusadaAntesDoUpload(ID3D12Device* _Device) {
        std::cout << "[descritores] importacao recusada antes do upload\n";
        FTextureSRVHeap Heap;
        Heap.Initialize(_Device);
        Heap.Allocate(FTextureSRVHeap::kCapacity);

        FUploadQueue Upload;
        Upload.Initialize(_Device);

        FTextureCPUData Data;
        Data.Width  = 1;
        Data.Height = 1;
        Data.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        FMipData Mip;
        Mip.Width  = 1;
        Mip.Height = 1;
        Mip.Pixels = { 255, 0, 255, 255 };
        Data.Mips.push_back(std::move(Mip));

        const u32 FalhasAntes = Heap.AllocationFailures();
        FTexture Texture = FTexture::TryCreateFromCPU(_Device, Upload, Heap, Data);
        Check(!Texture.IsValid(), "heap cheio recusa a textura sem lancar");
        CheckEq(Heap.UsedSlots(), FTextureSRVHeap::kCapacity,
                "a recusa nao consome nem devolve descritor alheio");
        CheckEq(Heap.AllocationFailures(), FalhasAntes + 1u,
                "a recusa da textura fica observavel");
        CheckEq(Upload.RingChunkCount(), 0u,
                "nenhum staging nasceu: a fila nao foi aberta antes da recusa");

        Upload.Shutdown();
    }

    // O caminho real: o editor troca uma textura, o material publica outro range e devolve o
    // antigo. A base MUDA — e por isso a linha canonica do material fica suja — e a antiga so
    // volta a ser alocavel depois da coleta.
    void TestarRepublicacaoDoMaterial(ID3D12Device* _Device) {
        std::cout << "[descritores] republicacao da tabela do material\n";
        FTextureSRVHeap Heap;
        Heap.Initialize(_Device);

        // Material sem textura nenhuma: exercita o caminho de descritor NULO nas oito posicoes,
        // que e o que a rotacao passou a exigir. Sem ele, um range recem-alocado ficaria com
        // slots nao inicializados — e o preview de material copia os oito sem consultar flag.
        FMaterial Material;
        Material.Finalize(_Device, Heap);
        const u32 Base = Material.AlbedoDescriptorIndex();
        Check(Base != FMaterial::kInvalidDescriptorTable, "material finalizado tem tabela");
        CheckEq(Heap.UsedSlots(), kMaterialTextureSlots, "a tabela ocupa oito slots");

        u32 Old = 0xDEADBEEFu;
        Check(Material.RepublishTextureTable(_Device, Heap, Old), "a republicacao aceita");
        CheckEq(Old, Base, "a republicacao devolve a base antiga");
        Check(Material.AlbedoDescriptorIndex() != Old, "e publica uma base diferente");
        CheckEq(Heap.UsedSlots(), 2u * kMaterialTextureSlots,
                "as duas tabelas ocupam ate o aposentamento");

        Heap.FreeDeferred(Old, kMaterialTextureSlots, FGpuFenceSet{ 7, 3 });
        CheckEq(Heap.UsedSlots(), kMaterialTextureSlots, "so a tabela nova continua usada");
        CheckEq(Heap.RetiredSlots(), kMaterialTextureSlots, "a antiga espera as fences");

        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 6, 3 }), 0u, "fence da direta atrasada");
        CheckEq(Heap.CollectRetired(FGpuFenceSet{ 7, 3 }), 1u, "coletada quando as duas passam");
        CheckEq(Heap.Allocate(kMaterialTextureSlots), Old, "a base antiga volta a ser alocavel");

        // Material nao finalizado nao tem o que republicar, e recusar e melhor que publicar uma
        // tabela solta que ninguem vai liberar.
        FMaterial Fresh;
        u32 FreshRetired = 0xDEADBEEFu;
        Check(!Fresh.RepublishTextureTable(_Device, Heap, FreshRetired),
              "material sem tabela recusa a republicacao");
        CheckEq(FreshRetired, FMaterial::kInvalidDescriptorTable,
                "e nao devolve base nenhuma para aposentar");
    }
}

int main() {
    Microsoft::WRL::ComPtr<ID3D12Device> Device = CreateWarpDevice();
    if (!Device) {
        std::cerr << "FALHA: WARP indisponivel (o teste exige um ID3D12Device)\n";
        return 1;
    }

    TestarAposentadoriaPorFence(Device.Get());
    TestarColetaParcial(Device.Get());
    TestarCoalescencia(Device.Get());
    TestarRepublicacaoDoMaterial(Device.Get());
    TestarRecusaComHeapCheio(Device.Get());
    TestarImportacaoRecusadaAntesDoUpload(Device.Get());

    if (Failures != 0) {
        std::cerr << "\n" << Failures << " verificacao(oes) falharam\n";
        return 1;
    }
    std::cout << "\nTodas as verificacoes de ciclo de vida de descritor passaram\n";
    return 0;
}
