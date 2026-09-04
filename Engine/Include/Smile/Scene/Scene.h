#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Scene/GpuSceneHandle.h"
#include "Smile/Scene/Light.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    class FUploadQueue;
    struct FOpacityMicromapData;

    // Capacidade a alocar para uma cena de N renderaveis.
    //
    // Cinco estruturas sao dimensionadas pelo tamanho da cena: o ObjectCB (`MaxObjects`), a
    // tabela de bounds do HiZ, a TLAS + uploads de instancia, o buffer de transforms do
    // TemporalMotion e o snapshot InstanceGeo da cena de RT. Dimensionar pelo N EXATO fazia o
    // primeiro objeto criado no editor estourar todas de uma vez e cair no re-setup completo
    // (rebuild de todos os BLAS + DDGI + MeshLights) a cada Ctrl+D.
    //
    // A folga tem de ser a MESMA nas cinco: se uma ficar mais apertada, ela vira o gargalo e a
    // folga das outras nao serve para nada — o `NeedsResize` do Renderer dispara pela primeira
    // que estourar. Por isso a conta mora aqui, e nao replicada em cada SetupForScene.
    //
    // Custo por entrada de capacidade: 512 B de ObjectCB (256 x 2 frames), 256 B de transform do
    // TemporalMotion (128 x 2), 128 B de instance desc da TLAS (64 x 2 slots), 100 B de InstanceGeo
    // (ver o static_assert do FRTInstanceGeo) e 76 B do HiZ = 1072 B. A folga de uma cena de 1591
    // objetos sai na casa das centenas de KB, contra ~200 MB de pool de BLAS que o re-setup
    // reconstroi. Tabela por escala e por HEAP em Docs/GPU-DRIVEN-PLAN.md, Fase 0.
    constexpr u32 SceneCapacityFor(u32 RenderableCount) {
        const u32 Slack = RenderableCount / 16u;
        return RenderableCount + (Slack > 64u ? Slack : 64u);
    }

    // Capacidade do remap slot -> indice do motion temporal.
    //
    // Cobre o POOL, e nao so a cena: os slots incluem os APOSENTADOS, entao aposentar e
    // realocar mantendo a mesma contagem viva faz o pool crescer sozinho. Medido: 8 vivos em
    // churn levam o pool a 88 com o remap parado em 72, e como o Collect devolve slots mas nao
    // renumera os sobreviventes, os 8 vivos ficam SEM mapeamento e assim continuam.
    //
    // Com a folga por cima, e nao colado no pool: colado, o proximo slot estoura de novo, e
    // como a reacao a isso e re-setup de cena inteira, cada edicao reconstruiria TLAS e GI.
    constexpr u32 SlotRemapCapacityFor(u32 SceneCapacity, u32 PoolCapacity) {
        return SceneCapacityFor(SceneCapacity > PoolCapacity ? SceneCapacity : PoolCapacity);
    }

    // A condicao de re-setup que a contagem de renderables NAO expressa.
    constexpr bool SlotRemapNeedsGrowth(u32 PoolCapacity, u32 RemapCapacity) {
        return RemapCapacity > 0 && PoolCapacity > RemapCapacity;
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

    // Se o renderer pode assumir que este objeto estara no MESMO lugar no proximo frame.
    //
    // Nao e "ja foi movido alguma vez": um objeto arrastado no editor e depois solto volta a
    // ser estatico, e o que muda e que o cache de sombra dele precisa ser refeito uma vez. E
    // uma PROMESSA sobre o futuro, e e o que permite reaproveitar o shadow map entre frames em
    // vez de re-rasterizar a cena inteira — hoje 25% do frame na Bistro, com a camera parada.
    //
    // Estatico e o default porque cena importada e integralmente estatica; marcar o que se
    // move e a excecao. Mesma escolha da UE (Mobility, com Static default no editor) e da Flax
    // (StaticFlags, que vem todas ligadas). A Cry inverte — la a flag e
    // ERF_DYNAMIC_DISTANCESHADOWS, que EXCLUI o objeto do cache — mas o efeito e o mesmo.
    enum class EMobility : u8 { Static = 0, Dynamic };

    struct FRenderable {
        // Identidade ESTAVEL do renderavel (0 = ainda nao atribuida). Mesmo motivo do FLight::Id:
        // o INDICE na lista nao serve como identidade, porque remover o objeto 0 desloca todo
        // mundo. A diferenca e que aqui o indice tambem e ENDERECO DE GPU em quatro sistemas —
        // InstanceID da TLAS, slot do buffer de transforms do TemporalMotion, entrada do readback
        // do HiZ e o ID que o passe de picking grava. Por isso o indice continua sendo o que o
        // renderer usa por frame (denso, sem indirecao) e o Id so aparece onde algo precisa
        // sobreviver a uma mudanca da lista: selecao, undo futuro, serializacao futura.
        u64         Id = 0;

        // Residencia GPU. Separada do Id (identidade de EDITOR) e da posicao na lista (ordem
        // AUTORAL): e a identidade que os quatro consumidores de GPU vao passar a usar no lugar
        // do indice. Invalido significa "sem residencia" — a FScene ainda tem o objeto, mas
        // nada de GPU pode enderecar por ele.
        FGpuSceneHandle GpuHandle;

        std::string Name;
        FTransform  Transform;
        FGpuMesh*   Mesh     = nullptr;
        FMaterial*  Material = nullptr;
        // OMM depende do par geometria/material (UV + alpha + cutoff), nao apenas da malha.
        // shared_ptr faz instancias e duplicatas reaproveitarem o mesmo bake imutavel.
        std::shared_ptr<FOpacityMicromapData> OpacityMicromap;
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

        // Ver EMobility. Consumido pelo CSM; persistido no .smap quando difere do default.
        EMobility   Mobility = EMobility::Static;

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
        //
        // As fences sao EXIGIDAS, sem default. Sem elas a sincronia handle/renderavel ficaria
        // por convencao; com elas, a FScene nao consegue largar um renderavel sem aposentar o
        // handle dele. E um default {0,0} seria pior que nenhum: significaria "reusavel ja", em
        // silencio, que e exatamente o reuso cedo demais que o ticket existe para impedir.
        //
        // O valor tem de vir de uma FRONTEIRA DE SUBMISSAO (ver Renderer::CurrentFenceSet).
        bool         RemoveRenderable(u64 Id, const FGpuFenceSet& Required);
        FRenderable* DuplicateRenderable(u64 Id);

        FRenderable*       FindRenderable(u64 Id);
        const FRenderable* FindRenderable(u64 Id) const;
        int                IndexOfRenderable(u64 Id) const; // -1 = nao existe

        // Resolve uma identidade CAPTURADA NO PASSADO para a posicao de agora. Devolve -1 quando o
        // objeto nao existe mais, ou quando a residencia GPU dele ja foi para outro.
        //
        // Mora aqui, e nao no renderer, por dois motivos: e CPU pura (logo testavel sem device) e
        // e a mesma pergunta que todo consumidor com readback atrasado faz — o pick e o primeiro,
        // o HiZ vem depois.
        //
        // O ID e quem carrega o peso aqui: ele diz ONDE o objeto esta agora, e como Id nunca e
        // reciclado dentro da sessao (ver Clear), um objeto removido simplesmente nao resolve. So
        // com isso o pick ja para de trocar de objeto quando a lista anda.
        //
        // O HANDLE e guarda para o que o Id nao cobre, e vale dizer que HOJE ele e redundante NESTE
        // caminho -- removê-lo nao quebra nenhum teste de picking, porque o Id sozinho ja recusa o
        // removido. Ele existe por dois motivos concretos: consumidores SEM Id (o readback do HiZ
        // endereca por slot e nao tem identidade de editor) e um futuro caminho que aposente
        // residencia sem remover o renderavel, em que o Id resolveria para um objeto cuja
        // residencia ja e de outro.
        int ResolveCapturedIdentity(FGpuSceneHandle Handle, u64 Id) const;
        u64                IdAt(u32 Index) const;           // 0 = fora da lista

        // ATENCAO: a referencia e mutavel para o renderer editar CAMPOS (transform, Visible) sem
        // indirecao. push_back/erase por fora daqui deixa o mapa de Id podre — use os metodos
        // acima. O que ainda faz push_back direto e a lista de LUZES, que tem indice proprio.
        std::vector<FRenderable>&       Renderables()       { return RenderableList; }
        const std::vector<FRenderable>& Renderables() const { return RenderableList; }

        FLight& AddLight(const FLight& Light);

        // Malhas GPU vivas na cena. Contagem apenas: a biblioteca e privada de proposito (o dono
        // e o FScene), e a telemetria precisa do denominador, nao dos ponteiros.
        u32 MeshCount() const { return static_cast<u32>(MeshLibrary.size()); }

        std::vector<FLight>&       Lights()       { return LightList; }
        const std::vector<FLight>& Lights() const { return LightList; }

        // Proxima identidade livre, COMUM a meshes e luzes. Nunca devolve 0 (0 = "sem
        // identidade"). O editor faz push_back direto na lista de luzes, entao quem consome
        // (renderer) atribui pra quem chegou com Id 0 — nao da pra centralizar tudo no AddLight.
        u64 AllocObjectId() { return ++NextObjectId_; }

        // Resolve uma identidade sem que o chamador precise saber de que tipo ela e. Devolve um
        // ref invalido se o Id nao existe (inclusive para Id 0).
        FSceneObjectRef FindObject(u64 Id) const;

        void Clear(const FGpuFenceSet& Required);

        // Versao dos transforms dos renderables — quem muta transform (gizmo do editor)
        // bumpa; o Renderer compara por frame p/ reconstruir SO a TLAS (BLAS intactos).
        u64  TransformsVersion() const { return TransformsVersion_; }
        void BumpTransformsVersion()   { ++TransformsVersion_; }

        // Versao da ESTRUTURA — muda quando a lista ganha ou perde um renderavel. Separada da
        // de transforms de proposito: aquela pede rebuild da TLAS (barato, por frame), esta pede
        // que TODO cache indexado por indice seja remapeado ou descartado. Um objeto que anda
        // bumpa so a primeira; um objeto que nasce ou morre bumpa as duas.
        u64  StructureVersion() const { return StructureVersion_; }

        // Versao do conjunto ESTATICO de casters. Terceira versao porque as tres respondem a
        // perguntas diferentes e tem custos diferentes: transform pede rebuild da TLAS (barato,
        // por frame), estrutura pede remapear caches indexados por indice, e esta pede
        // RE-RASTERIZAR o shadow map cacheado — a mais cara das tres, e por isso a que menos
        // pode ser bumpada por engano.
        //
        // Bumpa quando algo que o mapa estatico contem muda: um estatico se move, aparece,
        // some, e ocultado, ou troca de mobilidade. NAO bumpa quando um DINAMICO se move, que
        // e o caso comum e justamente o que o cache tolera sem refazer nada.
        u64  StaticCastersVersion() const { return StaticCastersVersion_; }
        void BumpStaticCastersVersion()   { ++StaticCastersVersion_; }

        // Residencia GPU dos renderaveis. Mora aqui, ao lado do RenderableIndexById_ e do
        // NextObjectId_, porque e a mesma familia de coisa: identidade que precisa sobreviver a
        // mudancas da lista. E persiste pelo mesmo argumento que ja esta escrito no Clear() para o
        // NextObjectId_ — reusar identidade dentro da sessao faz uma referencia velha apontar em
        // silencio para outro objeto.
        FGpuSceneSlots&       GpuSlots()       { return GpuSlots_; }
        const FGpuSceneSlots& GpuSlots() const { return GpuSlots_; }

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
        u64                                    StaticCastersVersion_ = 0;
        // UM contador para os dois tipos. Ver ESceneObject.
        u64                                    NextObjectId_      = 0;
        // NAO e resetado no Clear, pela mesma razao do NextObjectId_. Ver o comentario la.
        FGpuSceneSlots                         GpuSlots_;
    };
}
