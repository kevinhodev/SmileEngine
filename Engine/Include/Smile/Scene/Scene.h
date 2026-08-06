#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Scene/Light.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    class FUploadQueue;

    // Capacidade a alocar para uma cena de N renderaveis.
    //
    // Cinco estruturas sao dimensionadas pelo tamanho da cena: o ObjectCB (`MaxObjects`), a
    // tabela de bounds do HiZ, a TLAS + uploads de instancia, o buffer de transforms do
    // TemporalMotion e o snapshot InstanceGeo do DDGI. Dimensionar pelo N EXATO fazia o
    // primeiro objeto criado no editor estourar todas de uma vez e cair no re-setup completo
    // (rebuild de todos os BLAS + DDGI + MeshLights) a cada Ctrl+D.
    //
    // A folga tem de ser a MESMA nas cinco: se uma ficar mais apertada, ela vira o gargalo e a
    // folga das outras nao serve para nada — o `NeedsResize` do Renderer dispara pela primeira
    // que estourar. Por isso a conta mora aqui, e nao replicada em cada SetupForScene.
    //
    // Custo: os slots sao pequenos (64 B por instance desc da TLAS, 80 B por InstanceGeo,
    // 128 B x frames por transform) — a folga de uma cena de 1591 objetos sai na casa das
    // centenas de KB, contra ~200 MB de pool de BLAS que o re-setup reconstroi.
    constexpr u32 SceneCapacityFor(u32 RenderableCount) {
        const u32 Slack = RenderableCount / 16u;
        return RenderableCount + (Slack > 64u ? Slack : 64u);
    }

    // Que TIPO de objeto um Id designa.
    //
    // O Id e unico em TODA a cena — mesh e luz saem do mesmo contador — mas as listas continuam
    // separadas e densas, que e o que o renderer percorre por frame. Este enum e a ponte entre as
    // duas coisas: o Id identifica, o Kind diz em qual lista procurar. Antes havia dois contadores
    // independentes, entao o numero 5 existia nas duas listas e um Id sozinho nao identificava
    // nada — quem quisesse guardar "este objeto" precisava carregar um par (tipo, indice) e todo
    // consumidor precisava saber tratar os dois casos.
    enum class ESceneObject : u8 { None = 0, Renderable, Light };

    // Onde uma identidade mora AGORA. O Index e cache: vale ate a lista mudar, e depois disso so
    // o Id continua valendo (e por isso que ele existe).
    struct FSceneObjectRef {
        u64          Id    = 0;
        ESceneObject Kind  = ESceneObject::None;
        u32          Index = 0;

        bool IsValid()      const { return Id != 0 && Kind != ESceneObject::None; }
        bool IsRenderable() const { return Kind == ESceneObject::Renderable; }
        bool IsLight()      const { return Kind == ESceneObject::Light; }
    };

    struct FTransform {
        Vec3 Position      = { 0.0f, 0.0f, 0.0f };
        Vec3 RotationEuler = { 0.0f, 0.0f, 0.0f }; 
        Vec3 Scale         = { 1.0f, 1.0f, 1.0f };

        Mat44 Matrix() const;
    };

    struct FRenderable {
        // Identidade ESTAVEL do renderavel (0 = ainda nao atribuida). Mesmo motivo do FLight::Id:
        // o INDICE na lista nao serve como identidade, porque remover o objeto 0 desloca todo
        // mundo. A diferenca e que aqui o indice tambem e ENDERECO DE GPU em quatro sistemas —
        // InstanceID da TLAS, slot do buffer de transforms do TemporalMotion, entrada do readback
        // do HiZ e o ID que o passe de picking grava. Por isso o indice continua sendo o que o
        // renderer usa por frame (denso, sem indirecao) e o Id so aparece onde algo precisa
        // sobreviver a uma mudanca da lista: selecao, undo futuro, serializacao futura.
        u64         Id = 0;

        std::string Name;
        FTransform  Transform;
        FGpuMesh*   Mesh     = nullptr;
        FMaterial*  Material = nullptr;
        bool        Visible  = true;

        // De qual renderavel do .sscene este objeto veio (-1 = nenhum, ex.: proxy do terreno).
        // Uma COPIA herda o indice da fonte: e o que diz qual geometria recriar ao carregar.
        // Nao confundir com Id — aquele e identidade de sessao, esta e a origem no ASSET, e e o
        // unico jeito de a persistencia (.smap) falar de um objeto entre execucoes. O indice na
        // lista viva nao serve: apagar um objeto desloca todos os seguintes.
        i32         CookedIndex = -1;
        // Criado no EDITOR (duplicata), nao carregado do asset. Separa "o objeto 42 do cozido,
        // movido" de "uma copia do objeto 42", que salvam de formas diferentes.
        bool        Spawned = false;
        // So existe pro ray tracing (TLAS + tabelas de hit shading): fica FORA do raster,
        // prepass, CSM e picking. Uso: proxy do terreno (F3) — o terreno real rasteriza
        // pelo FTerrain, o proxy da o chao pro DDGI/ReSTIR/reflexoes.
        bool        RaytracingOnly = false;

        // Caixa em espaco do OBJETO, como o cozido v7 a grava. E a fonte: nao muda quando o
        // objeto se move.
        Vec3        LocalAABBMin = { -1e9f, -1e9f, -1e9f };
        Vec3        LocalAABBMax = {  1e9f,  1e9f,  1e9f };

        // Caixa de MUNDO, derivada das duas de cima pelo Transform. E o que frustum culling,
        // HiZ, sombras locais e chuva leem — por isso vive como cache aqui em vez de ser
        // recalculada por frame para milhares de objetos.
        //
        // ⚠️ Quem muta o Transform TEM de chamar RefreshWorldBounds(). Antes da v7 dava para
        // remendar (o gizmo so translada, e translacao desloca a caixa exatamente), mas com
        // rotacao ou escala o remendo silenciosamente descreve outro volume.
        Vec3        AABBMin  = { -1e9f, -1e9f, -1e9f };
        Vec3        AABBMax  = {  1e9f,  1e9f,  1e9f };

        void RefreshWorldBounds();
    };

    class FScene {
    public:
        FGpuMesh* AddMesh(ID3D12Device* Device, const FMesh& Mesh);

        std::vector<FGpuMesh*> AddMeshesBatch(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                              const std::vector<FMesh>& Meshes);

        // Entra na lista com um Id novo (o Id que vier no argumento e ignorado, como no AddLight).
        FRenderable& AddRenderable(const FRenderable& Renderable);

        // Mutacao da ESTRUTURA da lista. Diferente de mexer num CAMPO de um renderavel que ja
        // existe: o indice de todos os que vem depois anda, e o indice e endereco de GPU (ver
        // FRenderable::Id). Estes metodos deixam a FScene consistente e o RENDERER desatualizado —
        // o caminho suportado com a cena carregada e Renderer::RemoveRenderable/DuplicateRenderable,
        // que fecham o ciclo do lado da GPU. A ordem dos sobreviventes e preservada.
        bool         RemoveRenderable(u64 Id);
        FRenderable* DuplicateRenderable(u64 Id);

        FRenderable*       FindRenderable(u64 Id);
        const FRenderable* FindRenderable(u64 Id) const;
        int                IndexOfRenderable(u64 Id) const; // -1 = nao existe
        u64                IdAt(u32 Index) const;           // 0 = fora da lista

        // ATENCAO: a referencia e mutavel para o renderer editar CAMPOS (transform, Visible) sem
        // indirecao. push_back/erase por fora daqui deixa o mapa de Id podre — use os metodos
        // acima. O que ainda faz push_back direto e a lista de LUZES, que tem indice proprio.
        std::vector<FRenderable>&       Renderables()       { return RenderableList; }
        const std::vector<FRenderable>& Renderables() const { return RenderableList; }

        FLight& AddLight(const FLight& Light);

        std::vector<FLight>&       Lights()       { return LightList; }
        const std::vector<FLight>& Lights() const { return LightList; }

        // Proxima identidade livre, COMUM a meshes e luzes. Nunca devolve 0 (0 = "sem
        // identidade"). O editor faz push_back direto na lista de luzes, entao quem consome
        // (renderer) atribui pra quem chegou com Id 0 — nao da pra centralizar tudo no AddLight.
        u64 AllocObjectId() { return ++NextObjectId_; }

        // Resolve uma identidade sem que o chamador precise saber de que tipo ela e. Devolve um
        // ref invalido se o Id nao existe (inclusive para Id 0).
        FSceneObjectRef FindObject(u64 Id) const;

        void Clear();

        // Versao dos transforms dos renderables — quem muta transform (gizmo do editor)
        // bumpa; o Renderer compara por frame p/ reconstruir SO a TLAS (BLAS intactos).
        u64  TransformsVersion() const { return TransformsVersion_; }
        void BumpTransformsVersion()   { ++TransformsVersion_; }

        // Versao da ESTRUTURA — muda quando a lista ganha ou perde um renderavel. Separada da
        // de transforms de proposito: aquela pede rebuild da TLAS (barato, por frame), esta pede
        // que TODO cache indexado por indice seja remapeado ou descartado. Um objeto que anda
        // bumpa so a primeira; um objeto que nasce ou morre bumpa as duas.
        u64  StructureVersion() const { return StructureVersion_; }

    private:
        void RebuildRenderableIndex();

        std::vector<std::unique_ptr<FGpuMesh>> MeshLibrary;
        std::vector<FRenderable>               RenderableList;
        std::vector<FLight>                    LightList;
        // Id -> indice em RenderableList. Reconstruido inteiro na remocao (a lista e densa e
        // ordenada, entao remover desloca a cauda); no add so cresce. Custo O(n) so em acao de
        // editor, nunca por frame — nada no caminho de frame consulta por Id.
        std::unordered_map<u64, u32>           RenderableIndexById_;
        u64                                    TransformsVersion_ = 0;
        u64                                    StructureVersion_  = 0;
        // UM contador para os dois tipos. Ver ESceneObject.
        u64                                    NextObjectId_      = 0;
    };
}
