#pragma once

// Tabelas CANONICAS residentes na GPU (Fase 2 do GPU-DRIVEN-PLAN).
//
// Hoje ha uma: a de materiais. A de instancias e a de meshes vem nas proximas etapas da fase, e
// este header e a casa delas — por isso o nome no plural.
//
// O que a tabela resolve, em uma frase: editar um material passa a tocar UMA linha de 64 B em vez
// de reescrever o snapshot inteiro da cena (2.479 registros de 100 B na Emerald) com as duas
// filas drenadas.
//
// Tres decisoes que nao sao obvias:
//
//  1. DEFAULT heap, nao upload. O snapshot por instancia mora em upload heap — memoria do host,
//     lida por PCIe em placa discreta — e e por isso que reescreve-lo exigia drenar: escrever com
//     frames em voo corrompe o que eles leem. A tabela vive em VRAM e recebe as linhas sujas por
//     CopyBufferRegion na command list do frame, entao o versionamento e o da FILA e nao precisa
//     de Flush nenhum.
//
//  2. Uma linha e SUJA por CONTEUDO, nao por aviso. O editor sinaliza "algum material mudou" sem
//     dizer qual (MaterialsBridge chama MarkMaterialRTStateDirty sem argumento), e o sidecar
//     reaplica dezenas de materiais de uma vez. Recalcular todas as linhas e comparar custa
//     ~14 KB de memcmp nas cenas atuais e devolve exatamente o conjunto que mudou — que e o que o
//     gate da fase exige provar.
//
//  3. O descritor da tabela viaja DENTRO do snapshot por instancia (InstanceGeo::MaterialSrv), e
//     nao por um cbuffer. Assim os treze shaders que ja leem InstanceGeo nao precisam de root
//     signature nova nem de tabela de descritores nova: eles indexam o heap direto, como ja fazem
//     para VB/IB/payload de triangulo. Custa 4 B por instancia.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Scene/GpuMaterialRecord.h"

#include <d3d12.h>
#include <span>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Snapshot por INSTANCIA lido pelo InstanceID da TLAS. Espelhado em HLSL como `InstanceGeo`
    // (Shaders/Scene/GpuSceneTables.hlsli); a ABI e verificada em WARP.
    //
    // Depois da Fase 2 ele so carrega ENDERECO. Tinha 100 B e treze campos de material COPIADOS
    // por instancia — a duplicacao que obrigava toda edicao de material a reescrever a cena
    // inteira, num upload heap sem versao por frame, com as duas filas drenadas.
    //
    // Continua morando em upload heap e continua sendo montado pela FRaytracingScene: quem
    // preenche e o dono da correspondencia com a TLAS, e essa correspondencia (a ordem, com os
    // buracos preservados) e o contrato da Fase 1 que nao se mexe aqui.
    struct FGpuInstanceGeo {
        // VB/IB continuam aqui: o MeshLightExtract ainda percorre a malha por vertice, e o
        // raster/BLAS seguem consumindo os mesmos buffers. Quem saiu deles foi o caminho de HIT
        // (PT_LoadHitSurface, HitFaceNormal, HitIsBackface, HitGeomNormal, AlphaTestPass), que
        // desde a v8 do cozido le o TriangleSrv.
        u32 VertexSrv   = 0;
        u32 IndexSrv    = 0;
        // Payload pre-cozido por triangulo (FRTTriangle, 32 B), indexado por PrimitiveIndex.
        // Compartilhado por TODAS as instancias da mesma FGpuMesh — ele descreve geometria em
        // espaco local, entao duas instancias da mesma malha leem o mesmo buffer.
        u32 TriangleSrv = 0;
        // Descritor da tabela de materiais e a linha dentro dela. O descritor viaja aqui, e nao
        // num cbuffer, porque assim os treze shaders que ja leem este snapshot alcancam a tabela
        // sem root signature nova — eles ja indexam o heap direto para VB/IB/payload. Custa 4 B
        // por instancia e economiza 84.
        u32 MaterialSrv = 0xFFFFFFFFu;
        u32 MaterialId  = 0; // FGpuMaterialTable::kDefaultRow
    };
    static_assert(sizeof(FGpuInstanceGeo) == 20, "FGpuInstanceGeo deve casar com o HLSL (20 B)");
    static_assert(offsetof(FGpuInstanceGeo, VertexSrv)   ==  0);
    static_assert(offsetof(FGpuInstanceGeo, IndexSrv)    ==  4);
    static_assert(offsetof(FGpuInstanceGeo, TriangleSrv) ==  8);
    static_assert(offsetof(FGpuInstanceGeo, MaterialSrv) == 12);
    static_assert(offsetof(FGpuInstanceGeo, MaterialId)  == 16);

    // Copia as linhas novas sobre o shadow e devolve os indices das que MUDARAM.
    //
    // Livre, inline e CPU pura de proposito: e o coracao do gate "uma edicao toca uma linha", e um
    // teste dele nao pode depender de device, fila nem heap de descritores — nem de linkar o .cpp
    // desta tabela, que arrasta a fabrica de recursos inteira.
    //
    // Shadow menor que Fresh cresce; Shadow maior e truncado. Linha nova conta como mudada: a GPU
    // nunca a recebeu.
    inline u32 DiffMaterialRows(std::span<const FGpuMaterialRecord> Fresh,
                                std::vector<FGpuMaterialRecord>& Shadow,
                                std::vector<u32>& OutChanged) {
        OutChanged.clear();
        const u32 Count = static_cast<u32>(Fresh.size());
        const u32 Known = static_cast<u32>(Shadow.size() < Count ? Shadow.size() : Count);
        for (u32 I = 0; I < Known; ++I) {
            if (Shadow[I] != Fresh[I]) {
                Shadow[I] = Fresh[I];
                OutChanged.push_back(I);
            }
        }
        if (Shadow.size() != Count) Shadow.resize(Count);
        for (u32 I = Known; I < Count; ++I) {
            Shadow[I] = Fresh[I];
            OutChanged.push_back(I);
        }
        return static_cast<u32>(OutChanged.size());
    }

    class FGpuMaterialTable {
    public:
        // Linha 0 e o material DEFAULT (renderavel sem material). Reservada, e nao "o primeiro
        // material": um MaterialId que caia em 0 por engano tem de produzir o cinza 0.7 do
        // snapshot antigo, e nao a aparencia de um material real da cena.
        static constexpr u32 kDefaultRow = 0;

        // Slices de upload por frame em voo. Igual a FCommandQueue::kFramesInFlight (assert no
        // .cpp): a CPU escreve o slice deste frame enquanto a GPU ainda pode estar copiando o do
        // frame anterior.
        static constexpr u32 kFrameSlots = 2;

        static constexpr u32 kRowBytes = static_cast<u32>(sizeof(FGpuMaterialRecord));

        // (Re)cria buffer, descritor e linhas para o conjunto de materiais da cena.
        //
        // ⚠️ CONTRATO: substitui um recurso que as duas filas podem estar lendo. O chamador drena
        // antes — e ja drena hoje, porque isto so acontece em carga/troca de cena. NAO e o
        // caminho de edicao de material: aquele passa por MarkDirty + RecordUpload, sem dreno.
        //
        // O SLOT do descritor e preservado entre chamadas quando a capacidade nao muda, e mesmo
        // quando muda o SRV e reescrito no MESMO indice. Isso importa: o indice viaja dentro do
        // InstanceGeo e e lido via ResourceDescriptorHeap, entao trocar o recurso por baixo com o
        // indice estavel e o que faz os leitores acompanharem sem republicar tabela nenhuma.
        void SetupForMaterials(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               std::span<FMaterial* const> Materials);

        void Release(FTextureSRVHeap& SRVHeap);

        // "Algum material mudou." Nao diz qual, porque quem chama tambem nao sabe (ver a nota 2 do
        // topo). O conjunto real sai da comparacao feita no RecordUpload.
        void MarkDirty() { Dirty_ = true; }

        // Sobe as linhas que mudaram, na command list do FRAME. Sem dreno, sem Map do recurso
        // final, sem barreira global.
        //
        // Tem de ser gravado ANTES do primeiro leitor do frame — o que inclui a fila de COMPUTE,
        // porque o update do DDGI le a tabela pela mesma indirecao. Isso e satisfeito gravando no
        // topo da command list da direta: o trabalho async e submetido com SubmitAfter sobre um
        // segmento posterior desta mesma lista.
        void RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot);

        u32 Srv()      const { return SrvSlot_; }
        u32 RowCount() const { return static_cast<u32>(RowMaterials_.size()); }
        u32 Capacity() const { return Capacity_; }
        bool IsBuilt() const { return Buffer_ != nullptr; }

        // Linha de um material. Devolve kDefaultRow para nulo ou desconhecido — desconhecido
        // acontece de verdade: o proxy do terreno e materiais criados fora do import nao estao na
        // lista de materiais da cena.
        u32 RowOf(const FMaterial* Material) const;

        // === Telemetria (gate da fase: bytes persistentes por tabela e bytes sujos por frame) ===
        u64 PersistentBytes() const { return static_cast<u64>(Capacity_) * kRowBytes; }
        u64 StagingBytes()    const { return PersistentBytes() * kFrameSlots; }
        u32 LastDirtyRows()   const { return LastDirtyRows_; }
        u64 LastDirtyBytes()  const { return static_cast<u64>(LastDirtyRows_) * kRowBytes; }
        // Linhas subidas desde o ultimo Setup. Acumulado porque o instantaneo por frame e quase
        // sempre zero: o que interessa e se um caminho esta sujando a tabela por FRAME, e isso so
        // aparece no acumulado.
        u64 TotalRowsUploaded() const { return TotalRowsUploaded_; }
        u32 LastCopyRegions()   const { return LastCopyRegions_; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> Buffer_;
        GpuResources::FUploadBuffer            Staging_;
        u32                                    SrvSlot_ = 0xFFFFFFFFu;
        u32                                    Capacity_ = 0;

        // Ponteiro por linha. Guardado, e nao consultado no editor a cada upload, porque a ordem
        // das linhas e o contrato do MaterialId gravado no InstanceGeo.
        std::vector<FMaterial*>          RowMaterials_;
        // O inverso, para o RowOf nao ser linear: ele e chamado uma vez por RENDERAVEL.
        std::unordered_map<const FMaterial*, u32> RowByMaterial_;
        // O que a GPU tem AGORA. A comparacao contra isto e o que transforma "algo mudou" em
        // "estas linhas mudaram".
        std::vector<FGpuMaterialRecord>  Shadow_;
        std::vector<FGpuMaterialRecord>  Fresh_;   // buffer de trabalho, evita alocar por frame
        std::vector<u32>                 Changed_; // idem

        bool Dirty_ = false;
        u32  LastDirtyRows_ = 0;
        u32  LastCopyRegions_ = 0;
        u64  TotalRowsUploaded_ = 0;
    };
}
