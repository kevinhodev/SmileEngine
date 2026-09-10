# GPU-driven rendering em escala e caminho para geometria virtual

> **Estado em 2026-09-01 — PLANO, NÃO IMPLEMENTAÇÃO.** O raster principal continua CPU-driven,
> o HiZ devolve visibilidade por readback e o formato compilado aceito pelo checkout é v8. Um
> protótipo anterior de geometria virtual gerou assets v9 e um sidecar `.svgeo`, mas foi
> deliberadamente abandonado e seus arquivos auxiliares foram removidos. Este documento preserva
> somente as conclusões verificadas que ainda importam para a próxima implementação.
>
> **Atualização 2026-09-01, pós-recook:** os 11 pares foram recozidos com o cooker v8 deste
> checkout. Os 22 arquivos estão em v8, `SMeshEntry` voltou a 64 B e todas as contagens
> (renderables, meshes, materiais) reproduzem exatamente as do v9. O primeiro item da Fase 0
> está fechado; nenhum asset v9 permanece na árvore.
>
> **Responsabilidades acordadas:** Opus é o implementador e único escritor dos milestones; Codex é
> o revisor sênior read-only; o proprietário da engine decide divergências e promoção de fase. O
> revisor não corrige o diff que revisa.
>
> **Exceção autorizada pelo proprietário em 2026-09-03:** Codex assume a implementação
> restante da Fase 1 nesta continuação. Não iniciar a Fase 2 automaticamente. O estado e a
> evidência atuais estão em [GPU-DRIVEN-PHASE1-VALIDATION.md](GPU-DRIVEN-PHASE1-VALIDATION.md);
> as notas datadas abaixo preservam o raciocínio das etapas anteriores.

## 1. Resultado pretendido

A Smile deve deixar de pagar custo de CPU, bindings e submissão proporcional ao número total de
renderables. O renderer alvo mantém cena, materiais e geometria em tabelas persistentes na GPU,
recebe ranges compactos de instâncias, decide visibilidade na GPU e produz argumentos indiretos sem
readback no caminho de frame.

O núcleo precisa escalar antes de existir geometria virtual:

```text
CPU/editor                         GPU persistente
-----------                        ---------------
handles estáveis            ────>  instance/meta/transform tables
dirty pages                 ────>  mesh/material tables
spatial nodes aprovados     ────>  candidate instance ranges
                                          │
                                          ▼
                                expandir ranges + culling
                                          │
                      ┌───────────────────┼───────────────────┐
                      ▼                   ▼                   ▼
                    depth              GBuffer             shadows
                      │                   │                   │
                      └──── count → prefix → scatter → indirect args
                                          │
                                          ▼
                                   ExecuteIndirect
```

Essa fundação deve aceitar, sem ser reescrita, a evolução posterior:

```text
GPU-driven por instância
        ↓
clusters planos
        ↓
hierarquia de clusters + erro projetado
        ↓
geometry pages + streaming
        ↓
rasterização híbrida de micropolygon
        ↓
Smile Virtual Geometry
```

O objetivo não é reproduzir de imediato o sistema completo de *Assassin's Creed Mirage*, Nanite ou
Micropolygon. É escolher contratos que não bloqueiem essa evolução.

## 2. Envelope de escala

As cenas atuais são casos de correção e regressão; não são o teto do projeto. O desenho deve
suportar os seguintes regimes:

| Regime | Instâncias lógicas | Residentes | Candidatas | Uso |
|---|---:|---:|---:|---|
| pequeno | 10 mil | 10 mil | até 10 mil | desenvolvimento e testes rápidos |
| médio | 100 mil | 100 mil | 20–100 mil | stress de submissão e culling |
| grande | 1 milhão | até 1 milhão | 50–250 mil | gate estrutural do GPU Scene |
| mundo | dezenas de milhões | orçamento configurável | ranges de células visíveis | streaming e Moana-scale |

Os 28 milhões de instâncias lógicas da Moana não devem implicar 28 milhões de registros completos
residentes. Mesmo um registro de 64 B consumiria aproximadamente 1,67 GiB. Mundo lógico, conjunto
residente e conjunto candidato são domínios distintos.

Invariantes de escala:

- o custo CPU por frame cresce com páginas sujas, células candidatas e classes de pipeline, não com
  o número total de objetos;
- a remoção ou inclusão de um objeto não renumera a identidade persistente dos sobreviventes;
- o resultado do culling não volta à CPU para decidir o que desenhar;
- crescimento de capacidade, rebuild estrutural e drains de fila são eventos excepcionais, nunca o
  caminho estável de frame;
- a cena pode conter mais instâncias lógicas do que o raster, o TLAS ou a VRAM mantêm residentes;
- telemetria pode usar readback atrasado, mas não pode participar da decisão de renderização.

Metas de desempenho só serão fixadas depois da Fase 0. A meta arquitetural inicial é reduzir
radicalmente a submissão CPU sem piorar o critical path da GPU. Números do Mirage são referência,
não promessa para a Smile.

## 3. Evidência do estado atual

### 3.1 Raster e listas de draw

`Renderer::BuildDrawLists` percorre a lista inteira, escreve um `ObjectConstants` de 256 B por
renderable, resolve material, consulta o resultado antigo do HiZ, executa frustum culling na CPU e
ordena a lista visível. Ver
[`RendererPasses.cpp`](../Engine/Source/Graphics/Renderer/RendererPasses.cpp#L1891).

Z-prepass e GBuffer ainda iteram `VisibleScratch`, vinculam constantes por objeto/material e emitem
uma draw por item. O wrapper final chama `DrawIndexedInstanced(IndexCount, 1, ...)` em
[`GpuMesh.cpp`](../Engine/Source/Graphics/Resources/GpuMesh.cpp#L87).

Consequências:

- CPU e command list crescem com renderables visíveis;
- o mesmo conjunto é reprocessado em passes diferentes;
- a geometria deduplicada pelo cooker não vira instancing automaticamente;
- o sort reduz mudanças de estado, mas não remove draws.

### 3.2 HiZ com readback

`FHiZOcclusion` grava o teste na GPU, copia o resultado para um readback ring e a CPU consome o slot
depois de `kFramesInFlight`. O contrato está documentado em
[`HiZOcclusion.h`](../Engine/Include/Smile/Graphics/Scene/HiZOcclusion.h#L16), a cópia ocorre em
[`HiZOcclusion.cpp`](../Engine/Source/Graphics/Scene/HiZOcclusion.cpp#L269) e o mapeamento é resolvido
em [`HiZOcclusion.cpp`](../Engine/Source/Graphics/Scene/HiZOcclusion.cpp#L282).

Esse readback é incompatível com o objetivo: o buffer de visibilidade deve alimentar contagem,
compactação e indirect args na própria GPU.

### 3.3 Bindless parcial

O heap global possui 16.384 slots e free list em
[`TextureSRVHeap.h`](../Engine/Include/Smile/Graphics/Backend/D3D12/TextureSRVHeap.h#L13). O ray
tracing já publica índices de VB, IB, payload e texturas num registro de 100 B, `FRTInstanceGeo`, em
[`RaytracingScene.cpp`](../Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L60).

O raster ainda vincula uma tabela contígua de oito texturas por material em
[`Material.cpp`](../Engine/Source/Graphics/Resources/Material.cpp#L148). Sua root signature principal
permite input assembler, mas ainda não usa heap diretamente indexado; ver
[`PipelineState.cpp`](../Engine/Source/Graphics/Backend/D3D12/PipelineState.cpp#L221).

A engine exige Shader Model 6.9 e DXR 1.2 em
[`D3D12Device.cpp`](../Engine/Source/Graphics/Backend/D3D12/D3D12Device.cpp#L155), portanto o baseline
de hardware já é compatível com descriptor heap indexing. Mesh shader tier ainda precisa ser
consultado explicitamente antes da trilha de virtual geometry depender dele.

### 3.4 Geometria e indirect já existentes

As geometrias são colocadas em pools grandes na importação da cena, uma base adequada para offsets e
geometry pages. O formato v7 também deixou de bakear transform nos vértices e deduplica partes que
compartilham a mesma geometria.

O sistema de água prova o padrão necessário:

```text
count → prefix → scatter → build args → ExecuteIndirect
```

A command signature é criada em
[`Water.cpp`](../Engine/Source/Graphics/Water/Water.cpp#L599), as passadas são disparadas perto de
[`Water.cpp`](../Engine/Source/Graphics/Water/Water.cpp#L1011), o shader mantém counts/offsets/cursors
em [`WaterGenerateDraws.cs.hlsl`](../Shaders/Water/WaterGenerateDraws.cs.hlsl#L227) e o consumo ocorre
em [`Water.cpp`](../Engine/Source/Graphics/Water/Water.cpp#L1125).

O GPU-driven geral deve reutilizar os contratos provados ali, não copiar literalmente o subsistema
de água.

### 3.5 Identidade atual

`FRenderable::Id` já é uma identidade estável de sessão, mas quatro consumidores ainda usam o
índice denso da `RenderableList` como endereço GPU: TLAS `InstanceID`, transforms temporais, entrada
do HiZ e picking. O próprio contrato está registrado em
[`Scene.h`](../Engine/Include/Smile/Scene/Scene.h#L79).

Free list e generation counter só podem entrar junto da migração desses quatro consumidores. Fazer
um por vez produz IDs que parecem válidos, mas apontam para outro objeto após remoção/reuso.

### 3.6 Cenas atuais e instancing real

Os headers dos 11 pares `.sscene/.smesh` atuais indicam:

| Cena | Renderables | Meshes | Relação |
|---|---:|---:|---:|
| MEASURE_ONE | 7.293 | 7.293 | 1:1 |
| MEASURE_SEVEN | 10.740 | 10.740 | 1:1 |
| MEASURE_SEVEN_COLORED_LIGHTS | 10.740 | 10.740 | 1:1 |
| BistroExterior | 1.591 | 1.591 | 1:1 |
| BistroInterior | 2.062 | 2.062 | 1:1 |
| BistroInterior_Wine | 2.289 | 2.289 | 1:1 |
| EmeraldSquare_Day | 2.479 | 281 | 8,82 instâncias/mesh |
| EmeraldSquare_Dusk | 2.487 | 289 | 8,61 instâncias/mesh |
| NewSponza_Main_Yup | 405 | 405 | 1:1 |
| NewSponza_Main_Zup | 405 | 405 | 1:1 |
| SunTemple | 1.056 | 1.056 | 1:1 |

Instancing por mesh colapsa draws na Emerald, mas não é a solução geral. Bindless, classificação por
PSO, indirect e culling atacam o custo por draw em todas as famílias.

### 3.7 Arqueologia v8/v9

O checkout aceita `kCookedVersion = 8` e `SMeshEntry` de 64 B em
[`CookedFormat.h`](../Engine/Include/Smile/Scene/CookedFormat.h#L8). O loader rejeita versões
diferentes em [`SceneLoader.cpp`](../Engine/Source/Scene/SceneLoader.cpp#L77).

Os 11 assets estavam marcados como v9 até o recook de 2026-09-01 (ver a nota de estado no topo);
hoje estão em v8. O protótipo abandonado acrescentava
48 B ao `SMeshEntry`, produzindo um stride de 112 B e três regiões de meshlets:

```text
u64 ClusterOffset      + u32 ClusterCount
u32 ClusterVertexCount + u64 ClusterVertexOffset
u64 ClusterTriOffset   + u32 ClusterTriCount
u32 TriangleIndexStride (= 3)
```

Nos 11 `.smesh` foram verificados 37.151 entries e 341.909 clusters, com máximo de 256 vértices e
128 triângulos por cluster; `RTTriangleCount == IndexCount/3`, `ClusterTriCount == IndexCount` e os
bounds de arquivo passaram sem falhas. O `.smesh` v9 continha apenas clusters planos: não havia
hierarquia nem métrica de erro. O `.svgeo` experimental foi removido a pedido do proprietário e não
é fonte de verdade.

Decisões:

- não aumentar a constante para 9 apenas para aceitar os arquivos;
- não tentar ressuscitar o protótipo removido;
- recookar uma baseline coerente v8 na Fase 0 — **feito em 2026-09-01**, ver a nota de estado no topo;
- reservar um bump deliberado posterior para as tabelas canônicas;
- versionar virtual geometry num sidecar independente quando houver consumidor real.

## 4. Decisões de arquitetura

### D1 — Um escritor, um revisor

Opus implementa e corrige. Codex revisa o diff, executa diagnósticos read-only e produz findings. O
revisor não usa `apply_patch`, formatter ou fix automático no milestone revisado. A documentação
oficial do plano pode ser atualizada pelo implementador depois da aprovação.

### D2 — Três identidades, três funções

Não usar um único inteiro como identidade universal:

```text
ObjectId         u64 estável de editor/sessão
GpuHandle        {slot, generation} estável enquanto o recurso vive
VisibleIndex     u32 denso e transitório, válido somente no frame/pass
```

O TLAS recebe um remap do conjunto RT residente; `InstanceID` não é a identidade global da cena.

### D3 — Tabelas canônicas, snapshots especializados

`FRTInstanceGeo` não vira a tabela universal. O estado canônico será separado em instância, mesh e
material. Raster, RT, temporal, picking e culling consomem views/snapshots adequados ao próprio ABI.

Isso elimina a duplicação atual de campos de material por instância e permite atualizar uma linha de
material sem reconstruir todo `InstanceGeo`.

### D4 — Persistente paginado, transitório por frame

Tabelas persistentes usam páginas, free list, generation counter e dirty page/range upload. Buffers
de candidatos, visíveis, counts, offsets, cursors, `InstanceInfo` e indirect args pertencem ao frame
slot e podem crescer por high-water mark.

Não será criado um compilador genérico estilo SIG. Layouts serão escritos explicitamente e terão
`static_assert`, espelho HLSL e testes de ABI.

### D5 — Ranges desde o primeiro dispatch

A entrada do culling é sempre uma lista de `FInstanceRange`. Enquanto a cena for plana, ela produz
um único range. World cells mudam o produtor, não o consumidor.

**O range endereça a lista de candidatos, nunca um intervalo de slots.** Um `{FirstSlot, SlotCount}`
só descreve uma célula se a alocação de slots for particionada pela hierarquia — e isso conflita
diretamente com a free list global da Fase 1: assim que houver reuso de slot e objetos migrando
entre células, os membros de uma célula deixam de ser contíguos, e o produtor teria de emitir
milhares de ranges unitários ou varrer objetos de outras células. A indireção por
`CandidateInstanceIds` desacopla os dois: o range descreve um span da lista de índices, e a lista
carrega slots arbitrários.

Vale notar como a Anvil escapa disso: no artigo do Mirage os ranges são contíguos porque as
instâncias são alocadas POR `LeafNode`, com o limite de 16.384 justificado por ranges não poderem
atravessar páginas de alocação (§1.3.2). Ou seja, eles pagam contiguidade com um alocador
particionado pela hierarquia. Não dá para ter free list global **e** ranges contíguos em slot; este
plano escolhe a free list, então a contiguidade tem de viver na lista de candidatos.

`CandidateInstanceIds` é sempre materializado — **não existe modo "range identidade"**. Uma flag que
pulasse a lista faria `FirstCandidate` voltar a ser interpretado como slot, reintroduzindo o contrato
que este parágrafo acabou de rejeitar, e o consumidor só continuaria idêntico com um
`CandidateSourceMode` explícito nas dispatch constants mais testes para os dois modos. O custo que a
flag economizaria não existe: para a cena plana a lista é **persistente**, reconstruída apenas em
mudança estrutural, não por frame.

### D6 — Material bindless, estado na chave

Identidade e parâmetros de material não dividem batches. Estado que muda PSO permanece:

```text
BatchKey = pass
         + pipeline/permutation
         + geometry page/format
         + opacity class
         + two-sided class
         + deformation class
```

Albedo, metal/rough, emissivo, constantes e índices de texturas ficam no `FGpuMaterialRecord`.

`Blend` continua fora do caminho opaco GPU-driven inicial porque exige ordenação. `AlphaTest` entra
depois do opaco sólido e possui PSO próprio.

### D7 — Visibilidade fica na GPU

Frustum, HZB e classificação produzem máscaras/listas GPU. Readback é permitido para contadores de
debug atrasados, nunca como input de uma draw do frame.

### D8 — Direct queue primeiro

O primeiro caminho roda na direct queue para provar correção e custo sem introduzir ownership entre
queues. Async compute só entra com timestamps mostrando sobreposição útil e sem prejudicar DDGI,
ReSTIR ou outras cargas compute.

### D9 — CPU fallback explícito

Enquanto não forem promovidos, permanecem no caminho CPU:

- transparência ordenada;
- terrain e water;
- picking/selection até sua fase de migração;
- skinned/deformáveis;
- materiais/permutations não suportados;
- debug geometry e ferramentas do editor.

O fallback é instrumentado. Crescimento silencioso do fallback é regressão.

### D10 — Virtual geometry é sidecar

O formato-base descreve cena, materiais, instâncias e geometria convencional. Um `.svgeo` versionado
independentemente descreve clusters, hierarquia, erro e páginas. Alterar a hierarquia não deve
invalidar materiais, transforms ou payload de RT.

### D11 — Clusters não congelam antes do consumidor

O protótipo 256/128 prova que o bake é possível, não que esses são os parâmetros corretos. Tamanho,
encoding, normal cone, quantização e erro serão escolhidos na Fase 9 com rasterizador e métricas
reais.

### D12 — RT é consumidor, não dono

BLAS/TLAS não governam a identidade do raster. Virtual geometry deve declarar política RT própria:
LOD de BLAS, conjunto residente, proxies, cache, rebuild/refit e exclusões. Não se coloca todo o
mundo lógico no TLAS.

## 5. Contratos de dados alvo

Os nomes abaixo são de trabalho. A fase responsável pode refiná-los, mas não pode quebrar os
invariantes sem atualizar este documento.

### 5.1 Handle e slot

```cpp
struct FGpuSceneHandle {
    // Escopada na struct, e nao no namespace: `kInvalidSlot` ja existe duas vezes na engine
    // (FTextureSRVHeap, FBvhDebugView) para SLOT DE DESCRIPTOR HEAP, que e outro conceito. Uma
    // terceira em escopo de Smile leria como o mesmo nome significando duas coisas.
    static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

    // Com inicializador, e nao sem: sem ele, `FGpuSceneHandle{}` produz {0,0} — que identificaria
    // o primeiro ocupante do slot 0 — e `FGpuSceneHandle H;` deixa os dois indeterminados.
    u32 Slot       = kInvalidSlot;
    u32 Generation = 0;          // 0 e reservada: nunca alocado / invalido
};
```

**Ressalva sobre as "duas guardas":** elas nao sao independentes em todo caminho. `memset` e
`calloc` produzem `{0,0}`, contornando o inicializador de `Slot`, e nesse caso quem protege e
**apenas** a generation zero. E por isso que a generation 0 e reservada, e nao so uma convencao.

- slot reutilizado somente depois de a última GPU fence que o referencia completar;
- generation incrementa no reuso;
- `ObjectId` continua separado e não é enviado inteiro para todo shader;
- debug builds validam generation onde o custo permitir;
- picking devolve handle/remap, não índice da `RenderableList`.

### 5.2 Tabela de instâncias

Preferir colunas/páginas por padrão de acesso, não um struct gordo lido por todos os passes:

```text
InstanceMeta   : MeshId, MaterialId, CellId, Flags, Generation
InstanceBounds : center/radius ou AABB compacta
Transform      : matriz atual em espaço declarado
PrevTransform  : histórico somente onde necessário
```

Culling lê bounds/meta; raster lê transform/meta; RT publica seu snapshot. Separar colunas evita
carregar duas matrizes para uma thread que só testa uma esfera.

Espaços devem ser explícitos no nome/tipo. Mundo autoral e camera-relative não podem compartilhar um
campo ambíguo.

### 5.3 Tabela de meshes

```text
MeshId
LocalBounds
GeometryPageId
VertexOffset / VertexCount / VertexFormat
IndexOffset / IndexCount / IndexFormat
SubmeshRange
Pass/feature flags
VirtualGeometryId opcional
RT geometry/payload references
```

Offsets são relativos a páginas/recursos conhecidos, não endereços permanentes serializados.

### 5.4 Tabela de materiais

```text
MaterialId
TextureDescriptorBase ou índices explícitos
ConstantOffset
ShadingModel
OpacityClass
RasterFlags
RTFlags
PipelineClass
```

Uma mudança de textura/constante atualiza somente a linha/página do material. Mudança de
`PipelineClass` também invalida a classificação dos usuários, mas não renumera o material.

O heap atual de 16.384 descriptors deve ganhar telemetria de ocupação antes de ser considerado
suficiente. Moana/Ptex e conteúdo futuro exigirão outro plano de texturing; virtual texturing não faz
parte do núcleo GPU-driven, mas será necessário para o benchmark completo.

### 5.5 Entrada e saída de culling

```cpp
// Range é um span da LISTA DE CANDIDATOS, não um intervalo de slots.
struct FInstanceRange {
    u32 FirstCandidate; // índice em CandidateInstanceIds
    u32 CandidateCount;
};
// CandidateInstanceIds[FirstCandidate + i] -> FGpuSceneHandle/slot estável

struct FGpuBatchKey {
    u32 PipelineClass;
    u32 GeometryClass;
    u32 PassClass;
    u32 Flags;
};
```

Buffers transitórios mínimos:

- `CandidateInstanceRanges`;
- `CandidateInstanceIds`;
- `PassedInstanceIds`/`PassedSubmeshIds`;
- `BatchCounts`, `BatchOffsets`, `BatchCursors`;
- `InstanceInfos` compactado;
- `IndirectArgs` e `IndirectCount`;
- contadores de overflow e diagnóstico.

O MVP usa batch IDs conhecidos no load/cooker. Hash GPU ou radix sort só entram quando conteúdo
dinâmico demonstrar que IDs pré-calculados não bastam.

## 6. Pipeline alvo do frame

### 6.1 Atualização persistente

1. aplicar mutações estruturais que já passaram pela fence de segurança;
2. atualizar páginas sujas de instance/meta/transform/material;
3. publicar remaps para consumidores que usam frame slots;
4. selecionar cells/nodes no broad phase quando ele existir;
5. escrever `CandidateInstanceRanges`.

### 6.2 Main view

1. expandir ranges;
2. testar frustum e pass masks;
3. selecionar candidatos do depth prepass;
4. contar/prefixar/compactar por batch;
5. desenhar depth por `ExecuteIndirect`;
6. construir HZB do frame;
7. aplicar oclusão atual e conservadora aos candidatos do GBuffer;
8. reconstruir counts/offsets/args quando necessário;
9. desenhar GBuffer por `ExecuteIndirect`.

No primeiro MVP, depth e GBuffer podem compartilhar a lista frustum-visible e o HZB atual apenas
beneficia o GBuffer. Reprojeção do HZB anterior para reduzir o prepass é otimização posterior.

### 6.3 Shadows e passes adicionais

O culling por frame produz pass mask contra todos os frusta relevantes. Cada pass pode acrescentar
teste específico:

- cascata/spot frustum;
- reprojeção de receivers para shadow casters;
- HZB específico quando houver retorno medido;
- regras de alpha/two-sided/deformation.

Sun e local shadows não devem reconstruir a cena inteira na CPU. O caminho atual percorre `AllItems`
em [`RendererPasses.cpp`](../Engine/Source/Graphics/Renderer/RendererPasses.cpp#L1248); essa é uma
meta explícita da Fase 7.

### 6.4 Barriers e estados

Contrato mínimo por buffer:

```text
UAV clear/write
    → UAV barrier entre produtores dependentes
    → NON_PIXEL_SHADER_RESOURCE para consumo compute/VS
    → INDIRECT_ARGUMENT para args/count
```

- transições pertencem ao dono do subsistema;
- nenhum helper esconde queue wait global;
- frame slots evitam sobrescrever recursos ainda em voo;
- resize/capacity growth pode drenar fora do steady state e precisa de contador;
- direct/compute ownership deve ser documentado antes de async compute.

## 7. Regras para o implementador

1. Um milestone funcional por diff; não misturar formato, bindless, culling e virtual geometry.
2. Preservar o caminho CPU como A/B até o gate da fase responsável.
3. Não mudar default no mesmo commit que introduz a alternativa.
4. Toda estrutura compartilhada C++/HLSL recebe `static_assert` e teste de offsets/stride.
5. Todo buffer GPU recebe nome, capacidade, high-water mark e contador de overflow.
6. Overflow falha conservadoramente: desenhar demais é aceitável; desaparecer geometria não é.
7. Nenhuma readback nova decide visibilidade ou draw.
8. Não introduzir `WaitIdle`/`Flush` no frame estável para facilitar lifetime.
9. Formatar somente arquivos tocados e manter fontes explícitas no CMake.
10. Mudança arquitetural aprovada atualiza `ARCHITECTURE.md` no mesmo milestone.
11. Perf só é declarada com cena, câmera, resolução, preset, build e distribuição de amostras.
12. O pacote de revisão é produzido antes de pedir aprovação.

## 8. Fases de implementação

Há doze fases, numeradas de 0 a 11. Fases 0–8 entregam o renderer GPU-driven geral. Fases 9–11 são
a trilha de virtual geometry e não bloqueiam a promoção do núcleo.

## Fase 0 — Baseline coerente, formato e instrumentos

### Objetivo

Restaurar um checkout executável, congelar medidas e impedir que a próxima arquitetura seja avaliada
contra assets incompatíveis.

### Trabalho

- ~~recookar os 11 pares `.sscene/.smesh` com o cooker v8 atual~~ — **feito em 2026-09-01**;
- ~~**registrar a RECEITA do recook, não só o resultado**~~ — **feito**:
  [`Tools/Scenes.cook.json`](../Tools/Scenes.cook.json) (receita, incluindo quais cenas usam
  `--opaque-glass` e por quê) + [`Tools/CookScenes.ps1`](../Tools/CookScenes.ps1) (runner com
  `-Record`/`-Verify`) + `Tools/cook-baseline.json` (hashes). Antes disso a única fonte que dizia
  quem usa a flag era um comentário em [`main.cpp`](../Tools/Cooker/main.cpp#L296), e um recook sem
  a receita mudaria o vidro da Emerald sem nenhum diff de código;
- confirmar que loader, raster e RT abrem todas as famílias relevantes — **loader FEITO**, por
  [`Tests/CookedSceneLoadTests.cpp`](../Tests/CookedSceneLoadTests.cpp), registrado como 11 testes
  `Smile.CookedSceneLoad.*` (um por cena, para a falha apontar o arquivo). Valida cabeçalhos,
  coerência entre contadores e vetores, **todas** as referências de renderável
  (mesh/material/parent, transform finito) e **todos** os index buffers, mais o pareamento
  posicional do payload de RT e sanidade da AABB. Existe porque o harness de OMM não servia como
  prova de carga: ele só validava index buffer de malha *alpha-tested* e pulava silenciosamente
  renderável com índice inválido, então nas cinco cenas sem material masked (Sponza ×2, BEEPLE ×3)
  não validava índice nenhum — provava apenas que `LoadCookedSceneData` retornou sucesso.
  **Raster e RT continuam por verificar**: exigem o editor rodando, não há harness headless;
- ~~registrar hashes/build~~ — **feito** pelo `cook-baseline.json`, que fingerprinta as **entradas**
  e não só as saídas: SHA256 de cada FBX (também gitignored), da receita e do binário do cooker,
  mais commit, `treeDirty`, `config` e a versão esperada. Sem isso, outro FBX no mesmo caminho ou um
  cooker vindo de árvore suja seriam indistinguíveis. Três propriedades que o `-Verify` sustenta e
  que valem como contrato para quem for medir em cima disso:
  - **schema obrigatório.** Ausência de qualquer fingerprint é falha, não "passa por omissão".
    Guards do tipo *se presente* fazem um verificador aceitar baseline incompleta em silêncio;
  - **`provenanceMode`.** `cooked` = este script cozinhou as saídas com aquele binário naquela
    execução; `adopted` = as saídas já existiam e o hash é só do cooker que estava presente na
    hora, que pode não ser o produtor real. Sem o campo, qualquer ferramenta leria `cooker.sha256`
    como produtor comprovado nos dois casos.

    O fluxo que produz a baseline mais forte — `cooked` **com** `treeDirty: false` — tem três
    passos, nesta ordem: (1) commitar a ferramenta; (2) rodar o recook completo com a árvore
    limpa; (3) commitar só a baseline. O manifesto referencia legitimamente o commit **produtor**
    anterior; ele não precisa referenciar o commit que o contém, e essa defasagem de um commit não
    enfraquece nada. O que **não** funciona é regravar com `-Record` depois do commit: isso
    rebaixaria o modo para `adopted`. Uma baseline `cooked` + `treeDirty: true` é aceitável apenas
    como estado transitório, nunca como entrega — ela contradiz o próprio gate de baseline limpa;
  - **formato validado nas três bordas.** O bloco `expect` da receita é conferido em `-Record`, em
    `-Verify` e depois de cada cook, antes de virar hash — é o que impede a ferramenta de
    oficializar um formato que este checkout rejeita (ex.: os antigos v9).

  Recook confirmado **determinístico**: recozinhar uma cena reproduz o SHA256;
- não confundir os antigos arquivos v9 com formato suportado;
- adicionar timers CPU para `BuildDrawLists`, sort, uploads e emissão de passes;
- registrar draws potenciais/executadas, bindings, renderables, materiais, meshes e bytes de object
  constants;
- expor ocupação/high-water mark do SRV heap;
- ~~criar gerador ou modo de cena sintética com 10k, 100k e 1M instâncias, preservando geometria
  compartilhada e variação controlável de materiais~~ — **feito**: `SmileCooker --synthetic`
  ([`Tools/Cooker/Synthetic.cpp`](../Tools/Cooker/Synthetic.cpp)), com `--instances`, `--meshes`,
  `--materials`, `--masked-percent`, `--tess` e `--spacing`. Gera um cozido v8 em vez de povoar a
  cena em runtime, para a sintética entrar pelo **mesmo** caminho de load das reais — loader,
  testes, painel e `profile_snapshot` não sabem que ela é sintética. As três escalas carregam
  pelo harness real (`Smile.CookedSceneLoad.Synthetic_*`), com todas as referências e index
  buffers validados: 1M em **163 ms** de leitura de CPU.

  As cenas são geradas **no diretório de build**, por uma fixture do ctest
  (`Smile.SyntheticScene.Generate.*`), e não em `Assets/Scenes/`. Registrar por `if(EXISTS)`
  sobre um diretório ignorado fazia o gate depender de arquivos locais: num checkout limpo os
  três testes sumiam em silêncio e a suíte seguia verde com menos testes — um gate que
  desaparece sozinho não é gate. A receita (contagens e flags) fica versionada no
  `Tests/CMakeLists.txt`, e as mesmas contagens vão ao harness como `--expect-*`, para o teste
  provar a escala anunciada em vez de conferir o arquivo consigo mesmo.

  **Custo de GPU por instância** — o que o gate pede documentado. São **cinco** estruturas
  dimensionadas por `SceneCapacityFor(N)` (folga de N/16, mínimo 64), e o comentário no topo de
  [`Scene.h`](../Engine/Include/Smile/Scene/Scene.h#L18) as enumera. Custo por entrada de
  capacidade:

  ⚠️ **Separado por HEAP, e o heap importa mais que o total.** Quase tudo aqui é
  `D3D12_HEAP_TYPE_UPLOAD` (`GpuResources::CreateUploadBuffer`), que é memória visível pela CPU —
  **não** o orçamento de VRAM local. Somar tudo e concluir "cabe/não cabe na placa" é trocar um
  domínio pelo outro:

  | Estrutura | por entrada | heap |
  |---|---:|---|
  | `ObjectCB` (256 B × 2 frames) | 512 B | UPLOAD |
  | TemporalMotion (128 B × 2 frames) | 256 B | UPLOAD |
  | TLAS uploads (64 B × 2 slots) | 128 B | UPLOAD |
  | `InstanceGeo` (`FRTInstanceGeo`) | 100 B | UPLOAD |
  | HiZ bounds (32 B × 2 slots) | 64 B | UPLOAD |
  | HiZ readback (4 B × 2 slots) | 8 B | READBACK (memória de sistema) |
  | HiZ resultado | 4 B | **DEFAULT** |
  | | **1.072 B** | |

  | N | capacidade | UPLOAD | READBACK | DEFAULT |
  |---:|---:|---:|---:|---:|
  | 10k | 10.625 | 10,7 MiB | 0,08 MiB | 0,04 MiB |
  | 100k | 106.250 | 107,4 MiB | 0,81 MiB | 0,41 MiB |
  | 1M | 1.062.500 | **1.074,1 MiB** | 8,1 MiB | **4,1 MiB** |

  Ou seja, em 1M as estruturas por instância pedem ~1,05 GiB de heap **UPLOAD** e apenas
  **4,1 MiB de DEFAULT**.

  `ObjectCB` + TemporalMotion — os dois maiores, ambos matriz por instância por frame — são
  **71,6% do footprint total reservado** (768 de 1.072 B) e **72,5% dentro do heap UPLOAD**
  (768 de 1.060 B). São números de *footprint*, não de tráfego: quantos bytes são de fato
  **escritos por frame** é outra medida, depende de quais buffers o frame toca, e exigiria
  instrumentar as escritas — não está medido.

  ⚠️ **O teto de VRAM da 3060 Ti continua desconhecido** e esta tabela não responde a ele: a AS
  resultante da TLAS, o scratch, os BLAS e os alvos de render ficam todos de fora, e são eles que
  moram em DEFAULT. A resposta tem de sair do **delta de memória local/não-local medido em
  runtime** (`GetGpuMemoryInfo`), com o editor aberto na `Synthetic_1M`. Só o caminho de CPU está
  verificado;
- ~~fixar bookmarks/reprodução para as cenas de gate~~ — **feito**. O conjunto obrigatório é
  **Bistro + Emerald + as três sintéticas**; `MEASURE_SEVEN` saiu dos requisitos (ver abaixo).

  O achado que definiu o slice: os `.cameras.json` viviam sob `Assets/Scenes/`, que é gitignored,
  então **nenhum bookmark era versionado**. Um baseline cuja pose de câmera existe só na máquina
  de quem gerou não é reproduzível — "capturar do slot 0 na Bistro" produziria uma imagem
  diferente em cada checkout, e é o mesmo modo de falha que os assets sintéticos tinham. O
  `.gitignore` passou a excluir o conteúdo e reincluir só `*.cameras.json` (o git não reinclui
  arquivo cujo diretório pai está excluído, então o padrão precisa dos três passos).

  **Sintéticas: derivadas, não autoradas.** Elas são procedurais e moram no diretório de build,
  então não há onde versionar uma pose escolhida à mão — e derivar da grade é melhor de qualquer
  forma, porque a pose fica reproduzível por construção a partir da receita, que já é versionada.
  O `--synthetic` emite os 4 slots, escalando com a grade (10k → distância 99; 1M → 450). Cada
  slot exercita um **regime** diferente, que é o que a medida precisa separar:

  | slot | pose | o que mede |
  |---|---|---|
  | 0 | fora, de frente, todo o volume no quadro | teto de submissão |
  | 1 | no centro do volume | oclusão máxima — é o slot que o HiZ tem de justificar |
  | 2 | canto, olhando para fora | piso: custo de culling quase sem submissão |
  | 3 | oblíquo de um canto | regime intermediário, onde a fração visível responde à câmera |

  Quatro ângulos bonitos do mesmo regime não distinguiriam ganho de culling de ganho de submissão.

  **Cenas reais: 4 slots autorados em cada.** Bistro e Emerald foram fixadas no editor e estão
  versionadas, com as quatro poses distintas e validadas pelo parser do gate. Poses de cena real
  não são deriváveis — precisam ser escolhidas olhando a cena, porque uma câmera de baseline mal
  escolhida é pior que nenhuma: torna toda medição futura silenciosamente irrelevante.

  **`MEASURE_SEVEN` saiu do conjunto obrigatório.** Ela é a maior contagem do acervo (10.740),
  mas exercita iluminação e RT muito mais que submissão em escala — e o eixo de escala passou a
  ser coberto pelas sintéticas, que vão a 1M com geometria e materiais controláveis. Manter uma
  cena pesada como gate por um papel que outra cobre melhor só encarece a rodada. O sidecar dela
  continua no repositório e continua validado pelo gate (arquivo versionado tem de ser parseável);
  o que mudou é que ela não é mais **requisito** dos itens 8 e 9;
- capturar baseline de performance — **régua feita**, em
  `Tools/SmileMCP/scripts/gpu-driven-baseline.mjs` (`npm run benchmark:gpu-driven`).

  Percorre o corpus de gate inteiro, 5 cenas × 4 slots, e grava um relatório JSON em
  `build/bin/Release/Captures/Benchmarks/`. O que ela mede é **custo de submissão na CPU** —
  `BuildDrawLists`, culling/ordenação, listas de sombra e `Submit` — com as contagens que
  explicam esse custo ao lado (renderáveis, draw items, visíveis, ocluídos, e draws/binds por
  passe). O tempo de GPU entra como contexto e não como objeto: nenhuma fase promete baixá-lo
  diretamente, e tratá-lo como métrica principal faria a Fase 4 parecer fracasso por medir a
  coisa errada.

  Uma cena por lançamento do editor. Não existe comando de trocar cena no bridge, e o
  `smile_run_editor` recusa reaproveitar um editor aberto com outra `.sscene` — fechar entre
  cenas não é zelo, é o único caminho.

  **A asserção que dá sentido à rodada** é conferir a pose de volta com `smile_camera_get` depois
  do `bookmarkSlot`. O `profile_configure` chama `Bookmarks->Restore`, mas `Restore` devolve
  `true` mesmo quando o `SetCameraPose` recusa o teleporte (é o que o protocolo de captura faz
  durante uma sessão travada). Sem conferir, os quatro slots poderiam medir a **mesma** câmera e
  a baseline sairia como um número repetido quatro vezes, com aparência de quatro regimes. A
  comparação de yaw é em módulo 360, porque o sidecar guarda o ângulo acumulado — a Emerald tem
  slots em 538° e 452°, de quem girou mais de uma volta.

  A rodada também audita o log do PID do editor por erro de D3D12/DXGI: o profiler devolve
  números mesmo depois de a API ter reclamado, e uma rodada verde com `DEVICE_REMOVED` no log
  não é uma rodada verde.

  **Árvore limpa é requisito, não recomendação** — está no gate de saída desta fase. A régua lê o
  `buildCommit` do editor e recusa rodar quando ele termina em `-dirty` (o `StampVersion.cmake`
  sufixa assim quando `git status --porcelain` não está vazio, arquivo não rastreado incluído).
  Uma baseline tirada de árvore suja não serve como "antes": meses depois ninguém consegue dizer
  o que estava compilado. `--allow-dirty` existe para shakedown da própria régua e marca o
  relatório com `dirtyBuild: true`; relatório assim não vale como baseline.

  **BASELINE DE REGISTRO** — versionada em `Docs/Baselines/`, com SHA-256 no `MANIFEST.md` de
  lá. Os relatórios saem em `build/bin/Release/Captures/Benchmarks/`, que o git ignora, e são
  promovidos por `Tools/SmileMCP/scripts/promote-baseline.mjs`: uma baseline que não sobrevive a
  um clone não é baseline, é lembrança.

  O publicador tem teste próprio (`Tests/PromoteBaselineTests.mjs`, no `ctest` como
  `Smile.PromoteBaseline`), que roda num diretório temporário. Ele existe porque todos os defeitos
  do promotor foram achados por revisão humana lendo o código — escape de diretório pelo `--as`,
  sobrescrita silenciosa da baseline anterior, publicação não transacional, manifesto apontando
  para PNG inexistente —, e nenhum dos outros testes o executa. Publicação é transacional
  (staging + troca no fim) e o namespace é **imutável**: republicar por cima exige `--replace`.

  | arquivo | build | o que tem |
  |---|---|---|
  | `perf-f781288-4scenes.json` | `f781288` | perf das 4 primeiras cenas, 16 pontos com run completo |
  | `perf-capture-c452b1c-1M.json` | `c452b1c` | `synthetic_1M` completa: 4 pontos + GPU por passe + captura |
  | `capture-c452b1c-4scenes.json` | `c452b1c` | as 16 capturas visuais das 4 primeiras cenas |

  RTX 3060 Ti, viewport 1298×775 (saída = render), preset `gameplay_rr`, TOD 10h, 60 amostras de
  frames distintos por ponto. Pose de cada slot conferida contra o sidecar; auditoria de log limpa
  nas cinco cenas. **As duas builds comparam entre si**: `git diff f781288..c452b1c -- Engine/
  Editor/ Shaders/` é vazio — só mudaram script de benchmark e documentação.

  **Por que a 1M está num arquivo separado.** No relatório de `f781288` os quatro pontos dela
  aparecem no `summary` mas não nos `runs`: a publicação do detalhe acontecia no retorno da
  função, e uma asserção de **teardown** disparou antes dele — o editor da 1M não morreu em 180 s
  após `close_editor`. As medições estavam feitas; o que se perdeu foi o registro dos tempos de
  GPU por passe, que são gate desta fase. Corrigido em `c452b1c` (cada ponto é publicado assim
  que é medido, e o caminho verde confere `summary == runs`), e a 1M foi recapturada.

  Dois defeitos ficaram anotados à parte: a asserção de teardown mudou de lugar em `a9db333`
  (passou a ser cobrada antes do lançamento da cena seguinte, que é onde a contaminação
  aconteceria), e o `smile_close_editor` **afirma** que o editor parou baseado numa chamada de
  status com 500 ms de timeout — numa cena a ~3 FPS esse timeout estoura sem o editor ter ido a
  lugar nenhum.

  Custo de CPU por estágio, mediana em ms:

  | cena / slot | build | cull | batch | shadow | submit | **total** | visíveis / N |
  |---|--:|--:|--:|--:|--:|--:|---|
  | bistro s0 | 0,34 | 0,05 | 0,06 | 0,10 | 0,55 | **1,10** | 913 / 1.592 |
  | bistro s1 | 0,37 | 0,05 | 0,06 | 0,10 | 0,63 | **1,20** | 893 / 1.592 |
  | bistro s2 | 0,38 | 0,01 | 0,00 | 0,10 | 0,12 | **0,62** | 38 / 1.592 |
  | bistro s3 | 0,37 | 0,05 | 0,06 | 0,10 | 0,55 | **1,12** | 923 / 1.592 |
  | emerald s0 | 0,52 | 0,08 | 0,05 | 0,10 | 1,31 | **2,07** | 1.588 / 2.479 |
  | emerald s1 | 0,50 | 0,04 | 0,03 | 0,10 | 0,44 | **1,11** | 562 / 2.479 |
  | emerald s2 | 0,50 | 0,01 | 0,00 | 0,10 | 0,01 | **0,62** | 6 / 2.479 |
  | emerald s3 | 0,50 | 0,02 | 0,01 | 0,10 | 0,10 | **0,73** | 163 / 2.479 |
  | 10k s0 | 1,95 | 0,44 | 0,22 | 0,22 | 2,54 | **5,36** | 9.999 / 10.000 |
  | 10k s1 | 1,97 | 0,07 | 0,02 | 0,22 | 0,45 | **2,73** | 698 / 10.000 |
  | 10k s2 | 1,97 | 0,03 | 0,00 | 0,21 | 0,00 | **2,22** | 0 / 10.000 |
  | 10k s3 | 1,96 | 0,49 | 0,21 | 0,22 | 4,05 | **6,93** | 9.272 / 10.000 |
  | 100k s0 | 21,09 | 5,85 | 3,59 | 2,83 | 10,38 | **43,74** | 97.732 / 100.000 |
  | 100k s1 | 20,92 | 1,22 | 0,16 | 2,75 | 0,60 | **25,66** | 7.136 / 100.000 |
  | 100k s2 | 20,78 | 0,61 | 0,00 | 2,74 | 0,00 | **24,13** | 0 / 100.000 |
  | 100k s3 | 20,74 | 5,80 | 2,18 | 2,77 | 7,46 | **38,95** | 83.012 / 100.000 |
  | 1M s0 | 203,49 | 54,77 | 56,84 | 31,91 | 192,40 | **539,41** | 755.521 / 1.000.000 |
  | 1M s1 | 201,91 | 15,77 | 2,34 | 31,41 | 12,54 | **263,98** | 79.507 / 1.000.000 |
  | 1M s2 | 202,74 | 6,03 | 0,00 | 31,37 | 0,00 | **240,14** | 0 / 1.000.000 |
  | 1M s3 | 201,68 | 45,54 | 37,77 | 31,80 | 141,91 | **458,69** | 540.858 / 1.000.000 |

  **O que a medida diz, e é o argumento inteiro desta série de fases:** `BuildDrawLists` não
  responde à visibilidade. Custa 1,96 ms em 10k, ~20,9 ms em 100k e ~202 ms em 1M, e o valor é o
  **mesmo** no slot 0 (tudo visível) e no slot 2 (nada visível) — 203,49 contra 202,74 em 1M.
  `ShadowLists` se comporta igual (~31,4 ms nos dois). Somados, dão **234,11 ms** — o custo por
  objeto pago antes de qualquer decisão de culling, linear em N (10,9× de 10k para 100k; 9,8× de
  100k para 1M). O piso completo do slot 2 é 240,14 ms, porque ainda entram 6,03 ms de `CullSort`:
  ele escala com o que está visível, mas não chega a zero, já que percorrer para descartar também
  custa.

  `CullSort`, `BatchSort` e `Submit` são os únicos que escalam com o que está visível — a parte
  do trabalho que corresponde a alguma coisa. Em 1M/s0 eles somam 304 ms dos 539.

  Isso fixa a ordem de ataque: o alvo das Fases 1-2 não é acelerar o culling, é **deixar de
  percorrer a cena na CPU**. Zerar o `CullSort` inteiro mexeria em 6 dos 240 ms de piso; os outros
  234 são travessia que nenhuma otimização de culling alcança.

  Duas leituras laterais da mesma rodada:

  - **`ObjectCB` medido**: 407 KB na Bistro, 634 KB na Emerald e **256 MB em 1M** (256 B por
    objeto por frame). É o número concreto por trás da aposentadoria do `ObjectCB` na Fase 4.
  - **Heap de SRV na baseline da Fase 0**: o pico então medido era **7.238** descritores
    (Bistro); Emerald 3.913 e as sintéticas 1.126 (poucos materiais por construção). Com o
    `kCapacity` de 16.384 daquela rodada, a cena mais densa usava 44%. A Fase 2 mudou essa
    demanda estruturalmente: a tabela canônica reserva três descritores por mesh, e a
    MEASURE_SEVEN (10.740 meshes) passou a pedir 32.220 só para geometria, ou 33.810 com os
    1.590 já residentes. A Fase 3 elevou o teto para **65.536**; a baseline histórica continua
    correta, mas sua antiga folga não descreve mais o consumidor atual.

- ~~capturar baseline **visual** conforme `CAPTURE-PROTOCOL.md`~~ — **feita**, 20 capturas (as
  mesmas 5 cenas × 4 slots), preset científico, N=128, TOD 10h. A régua de performance ganhou
  `--capture` e `--capture-only`, para que a metade visual saia da **mesma passagem, pose e build**
  que a de performance. Duas rodadas separadas comparariam imagens de câmeras que só parecem
  iguais.

  Os **regimes são distintos e declarados**, e isso é de propósito: o profiling usa `gameplay_rr`,
  porque é o custo que interessa medir, e a captura usa o preset científico, que desliga o que
  poluiria comparação de imagem. Os dois constam do JSON.

  A captura roda **antes** do profiling no mesmo slot, e mexe no estado — reset determinístico,
  N frames de aquecimento e `ElapsedTime` fixo. Medido: a 1M com `--capture` deu 547,0 ms no slot 0
  contra 539,4 ms sem, ~1,4% acima. Por isso o modo faz parte da **chave da rodada**: comparar um
  número tirado com `--capture` contra outro sem é comparar dois regimes.

  **As referências estão versionadas em `Docs/Baselines/Reference/<baseline>/`** — com namespace
  pelo nome da baseline, porque `<cena>_slot<N>` sozinho não é identidade: quando a Fase 1 publicar
  o seu próprio `bistro_slot0.png`, ele sobrescreveria o da Fase 0 e o hash guardado aqui passaria
  a divergir do arquivo, apagando a referência do "antes" justamente na hora de comparar com ela.
  O manifesto do protocolo vai junto, com o campo `image` reescrito para o nome publicado (copiado
  cru, ele apontava para o nome longo de origem, que não existe no pacote) e o original preservado
  em `sourceImage`. PNG e manifesto são publicados como **par obrigatório** — antes a ausência do
  manifesto era só um aviso, e aviso não impede publicar imagem nova ao lado de manifesto velho.
  Os ~23 MiB custam espaço e ficam mesmo assim, pela mesma razão dos JSON: o que não sobrevive a um
  clone não é referência.

  **Achado que muda como comparar: a captura NÃO reproduz byte a byte.** Recapturando a Bistro do
  mesmo build, mesmo bookmark e mesmo N, os quatro slots deram hash diferente. A diferença é
  pequena — **11,5% dos subpixels, delta médio 1,08 e máximo 20** em 255 — ou seja ruído residual
  de RT/denoiser, não outra imagem. Mas isso derruba duas coisas:

  - guardar só o hash e regenerar a imagem quando precisar **não funciona** (foi a primeira ideia,
    e foi por isso que os PNGs acabaram versionados);
  - a comparação visual da Fase 1 tem de ser por **tolerância** — delta máximo e fração de pixels
    acima de um limiar —, nunca por igualdade. Um gate de igualdade exata reprovaria uma mudança
    que não mudou nada.

  Os limiares ainda não estão calibrados: 1,08 de média com máximo 20 é o ruído de **duas capturas
  da mesma build**, então é o piso do que qualquer limiar precisa tolerar. Calibrar em cima de mais
  de uma repetição por cena é trabalho da Fase 1, quando houver um "depois" para comparar.

### Arquivos prováveis

- `Tools/Cooker/main.cpp`
- `Engine/Include/Smile/Scene/CookedFormat.h`
- `Engine/Source/Scene/SceneLoader.cpp`
- `Engine/Source/Graphics/Renderer/RendererPasses.cpp`
- `Engine/Source/Graphics/Renderer/RendererCapture.cpp`
- `Engine/Include/Smile/Graphics/Backend/D3D12/TextureSRVHeap.h`
- `Tools/SmileMCP/scripts/gpu-driven-baseline.mjs`
- testes de cooker/cooked format e ferramentas de benchmark

### Gate de saída

- todas as cenas carregam com v8 e nenhum asset v9 é necessário;
- build Release e testes passam;
- baseline possui commit/hash limpo, câmera, resolução e N declarados;
- tempos CPU de preparação/submissão e tempos GPU por pass estão separados;
- 1M instâncias sintéticas podem ser carregadas ou a limitação exata de memória é documentada;
- nenhuma implementação GPU-driven entrou nesta fase.

## Fase 1 — Identidade estável e migração dos quatro consumidores

### Objetivo

Separar identidade, slot GPU e índice visível sem alterar a imagem nem o método de desenho.

### Trabalho

- **pré-requisito de API**: fazer a fila direta expor o último valor submetido — devolver a fence
  em toda submissão relevante, ou expor `LastSignaledValue()`. Hoje `EndFrame`, `Flush` e
  `ExecuteAndSync` devolvem `void`, e sem isso o ticket de aposentadoria não é implementável (ver
  "Quando o ticket captura as fences"). O `FAsyncComputeQueue` já devolve, no `SubmitAfter`;
- definir `FGpuSceneHandle {slot,generation}` e allocator com deferred reuse por fence;
- manter `FRenderable::Id` como identidade de editor;
- criar remaps explícitos para TLAS, temporal transforms, HiZ e picking;
- impedir que `RenderableList` seja endereço persistente implícito;
- tratar add/remove/duplicate, ocultação, reload e troca de cena;
- testar geração, stale handles, wrap policy, remoção do primeiro/meio/último e reuso após fence;
- manter o raster CPU-driven para isolar a migração.

### Decisão registrada: o `erase` estável fica

O `RenderableList` mantém **ordem autoral** e remoção por `erase` estável. Swap-and-pop quebraria
os ranges `[begin,end)` das pastas do Scene Outliner, a visibilidade persistida por offset e a
semântica dos assets — muita superfície, e nada disso ajuda o caminho por frame. Pensando em 1M, o
destino provável nem é um `vector` monolítico com swap-and-pop, e sim armazenamento paginado; então
migrar o Outliner para membership por `ObjectId` é trabalho próprio, deliberadamente **fora** desta
fase.

Os três papéis ficam separados assim:

| papel | quem |
|---|---|
| ordem autoral | posição em `RenderableList` |
| identidade de editor | `ObjectId` |
| identidade de residência GPU | `GpuHandle {slot, generation}` |

Slot aposentado até a fence; `generation` incrementa no reuso. Nenhum consumidor pode guardar
índice da lista entre mutações.

### Limitação medida: O(N) por edição de editor

O preço declarado do `erase` estável é `erase + RebuildRenderableIndex()` continuar O(N). Medido
(`Tests/SceneRemovalBenchmark.cpp`, `Smile.SceneRemovalBenchmark.Smoke` no ctest com N pequeno; a
rodada de 1M é manual):

| N | remoção no início | no meio | no fim |
|--:|--:|--:|--:|
| 20.000 | 1,22 ms | 1,28 ms | 1,07 ms |
| 1.000.000 | 204,10 ms | 203,98 ms | 191,19 ms |

**A posição quase não importa, e isso identifica a causa.** Remover o último elemento desloca
**zero** elementos, então os 191 ms desse caso são o `RebuildRenderableIndex()` isolado — não há
memmove nenhum ali. Dos 204 ms do pior caso, ~191 são o rebuild do mapa (O(N) incondicional) e
~13 ms são o memmove do `erase`, a única parte que a posição afeta.

Medido três vezes na 1M para confirmar que não é ruído: início 207–215 ms, meio 203–208 ms, fim
192–196 ms.

**O(N) por edição é ACEITO nesta fase, e não há conserto barato.** ~200 ms na 1M não é irrelevante —
é um engasgo visível —, mas nada no escopo da Fase 1 o remove, e é importante não prometer que
remove. Em particular, **manutenção incremental do mapa não resolve**: com `erase` estável, remover
no início ou no meio muda o índice de *todos* os elementos posteriores, então manter `Id → índice`
correto obriga a atualizar a cauda inteira — continua O(N). Ela só torna a remoção no **fim** O(1),
que já é o caso mais barato, e deixa o pior caso onde está.

Eliminar de verdade exigiria trocar o que sustenta a ordem: swap-and-pop, tombstones com slot-map,
ou armazenamento estável/paginado com outra indireção. Todos passam pelos ranges do Outliner e pela
visibilidade persistida por offset, e nenhum ajuda o caminho por frame. Fica **fora** da Fase 1, e
se churn estrutural frequente em runtime virar requisito, abre-se uma migração própria do Outliner
para membership por `ObjectId`.

O que a medição de fato decide para o desenho é mais modesto e mais útil: a posição na lista não é
o eixo de custo, então não vale complicar a Fase 1 para evitar deslocamento. O eixo é N.

### Estado de partida dos quatro consumidores

Levantado, e o HiZ confirmado por três pontos independentes — ele é **por objeto**, indexado pelo
índice denso da cena, e não por batch:

| consumidor | endereço hoje | evidência |
|---|---|---|
| TLAS | `Inst.InstanceID = ThisIdx` | `RaytracingScene.cpp:162` |
| TemporalMotion | `PreviousModels[i]` | `TemporalMotionVectors.cpp:190` |
| Picking | `ObjectId = índice + 1` | `Picking.h:30` |
| HiZ | `WriteBounds(FrameSlot, SceneIndex, …)` → shader `VisibleOut[i]` → CPU `OcclusionVis[A.SceneIndex]` | `HiZOcclusion.cpp:216`, `Shaders/HZB/HZBTest.cs.hlsl:37`, `RendererPasses.cpp:2014` |

Nenhum deles estava quebrado no ponto de partida, e vale registrar porque eu errei isso primeiro: os índices eram
frágeis mas **protegidos por descarte global**. Há duas rotas independentes — o
`SceneState->PreviousModels.clear()` explícito no `OnSceneStructureChanged`
(`RendererScene.cpp:178`, com comentário nomeando o risco) e o domínio `SceneStructure`, que inclui
`TemporalMotion | HiZOcclusion` (`HistoryDomain.h:248`). A Fase 1 **endurece** isso; não conserta
uma quebra em produção.

### Contrato do allocator: quatro estados, fixados desde o início

| estado | significado |
|---|---|
| `Free` | slot disponível para alocação |
| `Live` | slot ocupado por um renderável existente |
| `Retired` | o objeto morreu; indisponível **até as fences passarem** |
| `Exhausted` | generation esgotada; indisponível **para sempre** |

São **quatro**, e o quarto não é luxo. `Retired` foi definido como temporário — "até as fences
passarem" —, e aposentar no wrap carregava nele um segundo significado, "indisponível para sempre".
Com os dois no mesmo estado o `Collect` fica ambíguo e pode contar o mesmo slot esgotado
repetidamente, inflando o contador de overflow até ele deixar de significar coisa alguma.

A transição é `Retired -> Exhausted` **depois** das fences, quando `Generation == UINT32_MAX`, e o
contador de esgotados incrementa **exatamente uma vez**, nessa transição. Um slot `Exhausted` nunca
mais aparece em `Collect`.

`IsAlive(handle)` exige **estado `Live` E `generation` igual** — as duas coisas, não uma.
Comparar só a generation não identifica handle vivo: entre a remoção e o reuso o slot está
`Retired` e a generation **ainda não mudou**, então um resultado atrasado de um objeto já removido
passaria pela comparação. A generation só protege contra o ocupante *seguinte*; o estado é o que
protege contra a janela entre morte e reuso.

**O ticket de aposentadoria carrega uma fence por FILA, e não uma fence genérica.** Verificado no
código: `FCommandQueue` (direta) e `FAsyncComputeQueue` são classes separadas, cada uma com o seu
`FenceValue`, e não existe fence agregada que prove a conclusão das duas — é por isso que
`OnSceneStructureChanged` faz `DirectQueue.Flush()` **e** `ComputeQueue.WaitIdle()` em sequência
(`RendererScene.cpp:201-202`, e o mesmo par em 70-71 e 213-214).

```cpp
struct FGpuFenceSet { u64 Direct; u64 AsyncCompute; };

void Retire(FGpuSceneHandle Handle, FGpuFenceSet RequiredFences);
void Collect(FGpuFenceSet CompletedFences);
```

Uma fila ter passado e a outra não mantém o slot `Retired`: só volta a `Free` quando
`Completed.Direct >= Required.Direct` **e** `Completed.AsyncCompute >= Required.AsyncCompute`. O
allocator recebe **números** do renderer e não conhece D3D12 — fica CPU puro e testável, que é o
que permite cobrir geração, handle obsoleto, wrap e reuso sem device.

### Ciclo de vida do handle: inválido, generation inicial, wrap e reset

Sem estas quatro políticas escritas, "testar wrap policy, reload e troca de cena" não é testável —
não há contra o quê comparar.

**Representação inválida.** `kInvalidSlot = 0xFFFFFFFF`, e `FGpuSceneHandle{}` default-construído
tem `Slot = kInvalidSlot` e `Generation = 0`. Duas guardas independentes, e não uma: um struct
zerado é a entrada acidental mais comum — `memset`, `resize`, campo esquecido — e com `Slot`
defaultando a `0` ele identificaria o primeiro ocupante do slot 0. Qualquer uma das duas bastaria;
juntas custam nada e fecham o caso zerado por dois caminhos.

**Generation inicial.** `Generation = 0` é **reservada** para "nunca alocado / inválido". Um registro
de slot novo nasce em 0, e a primeira alocação leva a 1 — o primeiro ocupante de qualquer slot usa
`Generation = 1`. Assim `IsAlive({slot, 0})` é falso para todo slot, em qualquer estado, sem caso
especial.

**Overflow.** Generation é `u32` e cresce estritamente. Se ela desse a volta, um handle antigo o
bastante voltaria a casar com o ocupante atual — a troca silenciosa de identidade que o handle
existe para impedir, e a pior possível, porque `IsAlive` a aprovaria. Regra: o slot vai para **`Exhausted`** e nunca
mais volta a `Free`. A transição acontece **depois** das fences (`Retired -> Exhausted`), e não no
momento do incremento: o slot ainda precisa esperar a GPU soltar o ocupante anterior.

Vale o número, porque decide se isso é uma decisão dura ou um não-evento: são 2³² reusos **do mesmo
slot**. A 60 FPS, com aquele slot sendo reciclado a cada frame, dá ~2,3 anos de churn contínuo. O
custo prático é zero, então não há razão para inventar reciclagem de generation. O que **não** pode
é ser silencioso: o allocator conta os slots aposentados por overflow e expõe o contador, senão a
capacidade encolheria sem ninguém saber por quê.

**Reset e reload.** O allocator **persiste** através de reload e troca de cena; ele não é recriado.
Essa é a política inteira, e ela já entrega a propriedade desejada: como a generation de cada slot
cresce monotonicamente e nunca é reposta, um handle da cena anterior não pode ressuscitar — o slot
dele só volta a ser alocado com generation **maior**.

`Clear`/troca de cena **aposenta todo slot `Live` pelo caminho normal**, com ticket de fence; não há
atalho que devolva slots direto a `Free`, porque isso seria reuso sem incremento de generation.

**A sequência do reload é drenada, e a ordem importa mais do que parece:**

```
Clear/RetireAll(required)  ->  Collect(completed >= required)  ->  alocar a nova cena
```

Alocar a cena nova **antes** do `Collect` não é incorreto do ponto de vista de identidade — o handle
antigo continua inválido de qualquer jeito —, mas faz o pool **dobrar**: com 1M de slots ainda em
`Retired`, a nova cena não acha nada em `Free` e cria outro 1M. E é uma falha que o teste ingênuo
não pega, porque ele só olha o handle antigo. Por isso o teste de reload exige as três coisas
juntas: **o mesmo slot reutilizado**, com **generation maior**, e **capacidade inalterada**.

Duas coisas ficam explicitamente proibidas, e é o que o teste 6 protege:

- **recriar o allocator** numa troca de cena. Uma instância nova nasce com todas as generations em
  0, e aí um handle antigo volta a casar com o mesmo par `{slot, generation}`. Se algum dia for
  mesmo necessário destruir e recriar, todos os remaps têm de ser descartados antes — eles são os
  únicos que guardam handles;
- **incremento em massa** das generations como forma de invalidar. Só seria correto depois de drenar
  as filas e descartar todos os remaps, e nesse ponto não acrescenta nada sobre a regra monotônica
  por slot, que já é suficiente. Fica de fora por não pagar a complexidade.

### Testes indispensáveis do allocator

Todos CPU puros, sem device, no espírito do `SceneIdentityTests`:

1. `FGpuSceneHandle{}` nunca fica vivo — `IsAlive` falso em qualquer estado do pool;
2. o primeiro handle do slot 0 não coincide com o inválido;
3. reuso incrementa a generation;
4. o handle anterior continua inválido depois do reuso;
5. wrap não transforma um handle muito antigo em válido (o slot é aposentado, e o contador sobe);
6. `Clear`/troca de cena seguida de nova alocação não ressuscita handle da cena anterior — e, na
   sequência drenada, **reutiliza o mesmo slot com generation maior e capacidade inalterada**. Só
   "o handle antigo continua inválido" deixaria passar o pool dobrando;
7. uma fila ter passado e a outra não mantém o slot `Retired` — `Collect` parcial não libera;
8. wrap leva a `Exhausted` **depois** das fences, e o contador de esgotados sobe exatamente uma vez
   por slot, mesmo com `Collect` chamado várias vezes.

**Como testar o wrap sem 4 bilhões de ciclos:** não se exercita a política gastando as generations
de verdade — seriam 2³² alocações por slot, e o teste levaria horas para provar uma transição. A
transição é exposta por um caminho controlado de teste (helper puro que semeia a generation, ou
acesso restrito ao teste), e o que se verifica é o **comportamento na borda**: em `UINT32_MAX`, o
slot vai para `Exhausted` depois das fences e o contador sobe uma vez.

Mais um de **integração**, que os CPU-puros não alcançam: confirmar que o dono real do allocator
**não é recriado** na troca de cena, **e que a capacidade fica inalterada** depois dela. As duas
coisas, não só a primeira: um owner que persiste com o pool dobrado passaria na metade do teste e
falharia no que importa. A política de persistência mora fora do allocator, então um teste que só
exercita o allocator isolado nunca a vê quebrar.

### Quando o ticket captura as fences

Dois valores são necessários e **não suficientes**: o ticket só é seguro se for capturado depois de
**toda command list capaz de referenciar o slot já ter sido submetida**. Com uma lista ainda em
gravação, o último valor sinalizado não a cobre, e o slot voltaria a `Free` cedo — o objeto novo
tomaria o lugar enquanto a GPU ainda lê o velho.

**Onde a fronteira está hoje, e de onde vem a garantia.** Não vem do dreno, e é importante não
descrever errado: `Renderer::RemoveRenderable` e `DuplicateRenderable` alteram a `FScene`
**primeiro** e só depois chamam `OnSceneStructureChanged`, que é onde o `DirectQueue.Flush()` +
`ComputeQueue.WaitIdle()` acontecem (`RendererScene.cpp:139-148`). A segurança vem do
`RendererHandle`, que "serializa o acesso externo com `RenderFrame` e os jobs do renderer" por um
`recursive_mutex` (`Editor/Include/SmileEditor/Viewport/RenderThread.h:13`): mutação externa e
`RenderFrame` disputam o **mesmo lock**, então a edição acontece **entre frames**, nunca durante a
gravação. A fronteira, portanto, já é a que interessa — entre o `EndFrame` e o próximo
`BeginFrame` — e existe independentemente do dreno.

**A mutação não precisa esperar a GPU terminar.** Esse é o ponto do deferred reuse. Ao remover, o
handle vira `Retired` **imediatamente** e recebe os **últimos valores realmente submetidos** nas
duas filas. `Collect` consulta os valores concluídos e libera o slot só depois que ambos passaram.
O objeto some **imediatamente da cena CPU** e não entra no próximo frame submetido; frames já em
voo ainda podem mostrá-lo. O *slot* é que espera.

Escrever isso ao contrário — "o dreno acontece antes da mutação" — convidaria duas implementações
erradas: capturar do lado errado da fronteira, ou deixar uma janela em que o objeto morreu e o
handle continua `Live`.

**Falta uma peça na API das filas, e ela é pré-requisito.** O caso "sem trabalho novo nesta fila,
mas com trabalho anterior ainda em voo" exige o **último valor submetido**, e hoje a fila direta não
o fornece:

| membro | hoje | serve? |
|---|---|---|
| `EndFrame(...)` | `void` | não |
| `Flush()` | `void` | não |
| `ExecuteAndSync(...)` | `void` | não |
| `SubmitSegmentAndContinue()` | devolve `u64` | parcial — pode ser **anterior** ao signal do `EndFrame` |
| `NativeFence()->GetCompletedValue()` | concluído | não — é outra grandeza |

A mudança concreta, antes de conectar o allocator: **devolver a fence em toda submissão relevante,
ou expor `LastSignaledValue()`**, e o renderer manter o último valor de cada fila. Sem isso o ramo
descrito acima não é implementável com a API proposta — e o `FAsyncComputeQueue` já está do lado
certo, porque `SubmitAfter` devolve o valor sinalizado.

**"Próxima fence" sai do desenho.** Ela só faria sentido para trabalho realmente gravado e com
submissão garantida, e para as mutações externas de hoje esse caminho não é necessário: com a
fronteira entre frames, o último submetido já cobre tudo que pode referenciar o slot. Deixá-la no
contrato só abriria a armadilha da fila **ociosa**, em que `Completed` nunca alcança um `Required`
de "próxima" e o slot fica preso em `Retired` para sempre.

### Problema de origem: o id temporal vem da TLAS

**Estado atual:** resolvido pelos remaps de frame nos três leitores; o texto abaixo documenta
o defeito que motivou a migração, não uma pendência nova. Ver o relatório da continuação.

Os históricos de CPU (`PrevMVP` do raster e transforms do motion vector de RT) já são endereçados
por slot de residência. O que **não** ficou resolvido é a interpretação do id na GPU: o
`Shaders/Temporal/TemporalSurface.cs.hlsl` (linhas 52-53) grava `CommittedInstanceID() + 1` em
`CurrentSurface`, e o `DualMotion.cs.hlsl`
lê esse id da superfície **anterior** para indexar `InstanceTransforms` do frame **atual**.

Com `[A, B, C]`, remover A faz o id 1 gravado para B no frame passado passar a identificar C. O
histórico está certo e a leitura é que erra — e nenhum trabalho de CPU conserta isso, porque o id
gravado na superfície é o índice da TLAS.

Fica amarrado à **migração da TLAS** (quarto consumidor), e é pré-requisito para remover a
invalidação global: enquanto o descarte existir, ele protege; sem ele e sem a TLAS migrada, o
defeito passa a ser observável. Manter o raster CPU-driven não impede o remap necessário nos
shaders temporais — são coisas separadas.

### Contrato da tabela por frame (opção C), antes de implementar

A identidade temporal **não** cabe no `InstanceID`: ele tem 24 bits, e o canal float da superfície
dá os mesmos ~24 bits de inteiro exato. Com 1M de objetos gastando 20, sobrariam ~4 para a
generation — 15 reusos antes de aliasear, que é falha silenciosa, exatamente o que esta fase existe
para eliminar. Por isso a identidade vai para uma **tabela por frame**, e o id na superfície volta a
ser só um índice nela.

**1. Resolver a identidade antiga é metade do caminho.** O fluxo tem de ser inteiro:

```
id anterior → tabela ANTERIOR → {slot, generation} → localização atual válida → transform atual
```

Descobrir o slot e usá-lo direto em `InstanceTransforms` continuaria errado, porque aquele buffer é
indexado pela lista. Ou entra um remap para o índice atual, ou os transforms migram para
endereçamento por slot com validação de residência. E a tabela antiga identifica **quem existia**,
não prova que ele continua vivo: objeto removido, residência aposentada ou generation diferente
rejeitam a correspondência e caem no fallback.

**2. Dois slices UPLOAD não são automaticamente seguros.** O `BeginFrame` espera a fence do uso
anterior *daquele frame slot* (`CommandQueue.cpp:63`), mas a tabela de F também é lida por F+1: ao
começar F+2, esperar F não prova que F+1 terminou de lê-la. Sobrescrever o UPLOAD ali é corrida.
A escolha tem de ser explícita, entre:

**Decidido: DEFAULT em ping-pong, com staging UPLOAD protegido pelas fences.** O contrato:

- a CPU preenche o staging do frame **depois** da espera do `BeginFrame`;
- a cópia para DEFAULT roda na **fila direta**, antes dos consumidores, com as transições
  necessárias;
- a sobrescrita da tabela de F por F+2 fica ordenada **depois** das leituras de F+1 na GPU. A CPU
  não sobrescreve diretamente a memória que esses shaders consultam;
- se outra fila passar a consumir a tabela, entram **dependências explícitas** — a ordenação da
  direta não cobre outra fila automaticamente;
- o orçamento conta DEFAULT + staging + o remap atual, e não só as duas tabelas de handles.

Isso separa o lifetime do upload do lifetime do histórico, e evita acrescentar uma espera de CPU
pelo consumidor do frame seguinte.

A alternativa descartada era UPLOAD com mais slices. Com `kFramesInFlight = 2`, dois slices são
inseguros pelo motivo acima; dimensionar para o último consumidor amarraria o número de slices a um
atraso que hoje não está declarado em lugar nenhum.

Tabela, superfície, contagem e validade pertencem ao **mesmo frame** — inclusive depois de pausa do
passe, resize e reload.

**3. A migração alcança mais leitores que o `DualMotion`.** O `ReflectionMotion.hlsli:55` compara
índices de frames diferentes (`previousInstance != receiverInstance`); com a tabela, isso passa a
comparar as **identidades resolvidas**. O `ReSTIRDISpatial.cs.hlsl:130` também consome superfície e
transforms. E a superfície anterior é validada contra a **contagem anterior**, não a atual.

Não é preciso compactar a TLAS neste slice: dá para preservar o índice atual com buracos, desde que
TLAS, `InstanceGeo` e remap compartilhem a mesma convenção.

**4. A faixa de 24 bits — fechado.** O decoder fazia `(uint)(encoded + 0.5f)`, e essa soma não é
representável em float32 a partir de 2²³: o id 8.388.608 voltava como 8.388.609, o objeto vizinho,
sem erro nenhum. O `+0.5f` saiu — o valor vem de um `Load`, sem filtragem, de um canal que recebeu
inteiro exato, então truncar já é exato.

O teto ficou onde de fato está: o float32 representa inteiros exatamente até 2²⁴ **inclusive**,
então o maior ID é `0xFFFFFF` = 16.777.215 (cujo *encoded* é exatamente 2²⁴), que é também o teto
do `InstanceID` de 24 bits do DXR. A **contagem** endereçável é 16.777.216, um a mais, porque o
índice começa em zero — e por isso são duas constantes, com o guard comparando contagem com
contagem. Misturar as duas grandezas recusava duas cenas válidas.

O gate do decoder lê o **fonte do shader de produção**, e não só a aritmética: executar HLSL exigiria
GPU, e o critério que importa é que reintroduzir o `+0.5f` lá deixe o teste vermelho — verificado,
junto com a divergência de teto entre shader e CPU.

**Testes centrais do slice:** `[A,B,C] → [B,C]`, renderável pulado na TLAS, slot reciclado, objeto
removido sem reuso, e tabela anterior ainda em consumo.

A migração conjunta agora tem teste com renderer e GPU reais. O descarte global de identidade
saiu de `SceneStructure` (TemporalMotion/HiZ, modelos raster e picking pendente). Corte de câmera,
resize/reload e invalidações de radiância continuam válidos. Os drenos que protegem
`InstanceGeo` e `MeshLights` também permanecem: handles estáveis não versionam esses buffers.

### Desenho do remap do HiZ

Remap em **ring por frame**: `HiZTestIndex → GpuHandle {slot, generation}`. Ao consumir o readback
atrasado, valida-se com `IsAlive(handle)` — não só a generation, pela razão acima — e publica-se
visibilidade **por slot**.

A visibilidade publicada por slot tem de **carregar/estampar a generation**, ou ser **zerada na
alocação**: sem isso, um resultado que sobreviveu ao readback chegaria ao próximo ocupante do slot,
que é a mesma troca de identidade que o handle existe para impedir, só deslocada um nível.

Isso preserva o dispatch compacto — sem varrer os buracos da capacidade — e faz o resultado de um
objeto que morreu entre o teste e o readback ser descartado em vez de aplicado a quem tomou o lugar
dele.

### Arquivos prováveis

- `Engine/Include/Smile/Scene/Scene.h`
- `Engine/Source/Scene/Scene.cpp`
- `Engine/Source/Graphics/RayTracing/RaytracingScene.cpp`
- `Engine/Source/Graphics/PostProcess/TemporalMotionVectors.cpp`
- `Engine/Source/Graphics/Scene/HiZOcclusion.cpp`
- `Engine/Source/Graphics/Editor/Picking.cpp`
- `Engine/Source/Graphics/Renderer/RendererScene.cpp`

### Gate de saída

**Estado: fechado em 2026-09-04.** A implementação, as provas negativas, a execução em WARP/RTX,
a comparação das oito imagens e a correção do editor preso no frame zero estão registradas em
[GPU-DRIVEN-PHASE1-VALIDATION.md](GPU-DRIVEN-PHASE1-VALIDATION.md). A Fase 2 exige liberação
explícita do proprietário; este fechamento não a inicia automaticamente.

- nenhum consumidor usa posição na `RenderableList` como identidade durável;
- selecionar, duplicar, remover e mover não troca histórico entre objetos;
- TLAS hit, picking, motion vectors e HiZ apontam para o mesmo objeto depois de compactação/reuso;
- comparação visual CPU antes/depois não encontra regressão;
- testes cobrem handles obsoletos e reuse após fence.

## Fase 2 — Tabelas canônicas e formato-base deliberado

> **Estado em 2026-09-05 — FECHADA.** Materiais, meshes e instâncias estão integrados; comparação
> visual, validação interativa, ABI em WARP, Release/Debug 59/59 e a medição da instância canônica
> na sintética de 1M estão concluídos. Detalhe, evidência e as fronteiras que permanecem para as
> fases seguintes em
> [GPU-DRIVEN-PHASE2-PROGRESS.md](GPU-DRIVEN-PHASE2-PROGRESS.md).
>
> **Decisão do proprietário, 2026-09-04 — o formato-base NÃO é bumpado nesta fase.** A verificação
> que motivou a pergunta: a Fase 2 não precisa persistir byte nenhum. O `SMeshEntry` v8 já traz
> bounds locais, offsets e contagens de VB/IB e a região de `FRTTriangle`; formato de vértice e de
> índice são únicos no runtime, não há submeshes, geometry pages são Fase 4/8 e clusters são
> Fase 9+. Bumpar agora custaria recozinhar os 11 pares e regravar `Tools/cook-baseline.json` para
> acrescentar campos sem consumidor — o oposto de "sem serializar consumidor imaginário".
>
> O `VirtualGeometryId` fica declarado e RESERVADO no `FGpuMeshRecord` (runtime), com valor zero e
> sem contraparte no arquivo. O bump deliberado — número **10**, pulando o 9 queimado pelo
> protótipo descartado — acontece quando houver byte novo de verdade: geometry pages na Fase 8 ou
> o sidecar `.svgeo` na Fase 9.

### Objetivo

Introduzir `FGpuSceneInstance`, `FGpuMeshRecord` e `FGpuMaterialRecord` como fontes canônicas sem
alterar ainda a submissão raster.

### Trabalho

- escolher layouts paginados/SoA por consumidor e orçar bytes por instância residente;
- criar espelhos C++/HLSL com ABI testada;
- publicar `FGpuSceneInstanceTable` por slot estável em quatro colunas SoA (meta, bounds,
  transform atual e anterior), com dirty rows e telemetria;
- fazer o HiZ consumir os bounds canônicos e os históricos CPU de raster/temporal derivarem da
  mesma fonte; os buffers GPU especializados só são aposentados na Fase 4;
- separar material do snapshot por instância do RT;
- fazer `FRTInstanceGeo` consumir/derivar da tabela canônica, sem virar tabela universal;
- implementar dirty rows/pages para transform, material e estrutura;
- tomar uma decisão deliberada sobre o formato-base; nesta fase a decisão foi **não** persistir
  campos sem consumidor. Quando houver byte novo real, usar v10 documentada em vez de reaproveitar
  semanticamente a v9 do experimento descartado;
- não incluir hierarquia de clusters nessa versão;
- declarar `VirtualGeometryId` opcional/reservado sem serializar consumidor imaginário.

### Arquivos prováveis

- novos headers em `Engine/Include/Smile/Graphics/Scene/`
- novas implementações em `Engine/Source/Graphics/Scene/`
- `Engine/Include/Smile/Scene/CookedFormat.h`
- `Engine/Source/Scene/SceneLoader.cpp`
- `Engine/Source/Graphics/Renderer/RendererSceneImport.cpp`
- `Engine/Source/Graphics/RayTracing/RaytracingScene.cpp`
- `Shaders/GI/DDGICommon.hlsli` ou um include de scene tables dedicado

### Gate de saída

- atualização de uma propriedade de material toca uma linha/página de material, não todos os
  `FRTInstanceGeo` usuários;
- **o OMM é invalidado quando suas ENTRADAS mudam.** A chave do cache não referencia material:
  `BlasByMesh` é `map<const FGpuMesh*, map<const FOpacityMicromapData*, GPUVA>>` em
  [`RaytracingScene.h`](../Engine/Include/Smile/Graphics/RayTracing/RaytracingScene.h#L98), e a
  dedup do bake é o compartilhamento do `shared_ptr<FOpacityMicromapData>` em
  [`Scene.h`](../Engine/Include/Smile/Scene/Scene.h#L94) — normalizar material para `MaterialId`
  não quebra essa chave por si só, e trocá-la por `MaterialId` também não resolveria nada, porque o
  conteúdo do material pode mudar mantendo o mesmo id. O risco real é de VERSIONAMENTO: o OMM é
  derivado de malha + textura de alpha + fator de alpha + cutoff, e a Fase 2 torna exatamente esses
  campos alcançáveis por uma indireção que hoje não notifica ninguém. Uma edição de material que
  toca só a linha da tabela pode deixar o micro-map obsoleto em silêncio. O invariante a provar é:
  mudou qualquer entrada do bake, o `FOpacityMicromapData` é substituído/versionado e os BLAS
  afetados são reconstruídos. O sintoma de falha cai em folhagem alpha-tested, onde micro-map
  errado é quase invisível numa comparação estática;
- o caminho estável não chama `Flush`/`WaitIdle` por edição de material;
- layouts e versões possuem testes de ABI, offsets e bounds;
- uma cena grande reporta bytes persistentes por tabela e dirty bytes por frame;
- raster CPU e RT mantêm paridade visual.

## Fase 3 — Raster bindless sem indirect

> **Estado em 2026-09-05 — ESTRUTURALMENTE COMPLETA, NÃO VALIDADA EM GPU.** As slices estão na
> árvore, Release fecha 61/61, Release e Debug compilam e o cliente TypeScript compila; o default
> continua no caminho clássico (regra 3). A revisão do proprietário achou 1 P1 e 3 P2, todos
> corrigidos — entre eles
> um em que **o próprio contador usado para julgar o A/B mentia**, contando como fallback de
> material todo draw que nunca consumiu material. O que falta é o que exige placa: ligar o
> toggle na Bistro e na Emerald, provar paridade e medir. **A fase não fecha antes disso.** Detalhe,
> evidência, decisões reversíveis e a fila do que falta em
> [GPU-DRIVEN-PHASE3-PROGRESS.md](GPU-DRIVEN-PHASE3-PROGRESS.md).
>
> A primeira tarefa — confirmar capability e contrato antes de qualquer desenho — achou duas
> lacunas reais: `ResourceBindingTier` nunca era consultado (os passes de compute que já usam
> `ResourceDescriptorHeap` rodavam por sorte de hardware), e a ordem heap → root signature, que a
> spec de Dynamic Resources exige, era satisfeita por acidente. As duas estão fechadas. A versão
> da root signature confirmou-se não ser a questão, como o texto abaixo antecipava.
>
> A continuação migrou as root signatures próprias das sombras: caster opaco segue sem material
> (PS nulo), enquanto alpha-tested lê a linha canônica. No caminho apareceu e foi corrigido um
> P1 antigo: as tabelas estavam em estado `NON_PIXEL` apesar de já serem lidas pelo G-buffer PS.
> Translúcidos ficam no clássico por decisão D9.

### Objetivo

Remover bindings por material/draw mantendo as mesmas listas CPU e as mesmas draws. Essa separação
permite atribuir qualquer diferença visual exclusivamente ao novo acesso a dados.

### Trabalho

- **primeira tarefa, antes de qualquer desenho: confirmar capability e contrato de direct heap
  indexing** — `ResourceBindingTier`, ordem de binding e lifetime dos descriptors. A versão da root
  signature **não** é a questão (ver abaixo). A raster
  serializa hoje com `D3D_ROOT_SIGNATURE_VERSION_1` e a única flag é
  `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` ([`PipelineState.cpp`](../Engine/Source/Graphics/Backend/D3D12/PipelineState.cpp#L221));
  `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` aparece uma única vez na engine, no caminho de compute
  ([`ComputePipeline.cpp`](../Engine/Source/Graphics/Backend/D3D12/ComputePipeline.cpp#L162)), e
  também passa por `D3D12SerializeRootSignature` com `VERSION_1`
  ([`PipelineUtils.cpp`](../Engine/Source/Graphics/Backend/D3D12/PipelineUtils.cpp#L13)). Ou seja,
  **o precedente de que funciona já existe na árvore** — DDGI, ReSTIR e SHaRC usam
  `ResourceDescriptorHeap` em produção por esse caminho. **A flag não é recurso de root signature
  1.1**: ela pertence a `D3D12_ROOT_SIGNATURE_FLAGS`, enum compartilhado por `_DESC` (1.0) e
  `_DESC1` (1.1), e o requisito documentado é Shader Model 6.6 + Resource Binding Tier 3 — RS 1.1
  trata de volatilidade/staticness de descriptors, outro eixo. A serialização `VERSION_1` do
  compute confirma na prática. Logo **não** se espera `SerializeVersionedRootSignature` nem mudança
  no compute; o que precisa ser verificado é outra coisa:
  - **`ResourceBindingTier` nunca é consultado.** O device lê apenas `Options.ResourceHeapTier` de
    `D3D12_FEATURE_DATA_D3D12_OPTIONS` ([`D3D12Device.cpp`](../Engine/Source/Graphics/Backend/D3D12/D3D12Device.cpp#L188));
    `ResourceBindingTier` é campo *irmão* do mesmo struct e não é lido nem exposto. SM 6.6 já está
    garantido pelo hard requirement de SM 6.9, mas o Tier 3 precisa de query explícita;
  - ordem de `SetDescriptorHeaps` / `SetGraphicsRootSignature` no caminho raster;
  - lifetime e null descriptors sob heap indexado diretamente.
- migrar a root signature raster para heap diretamente indexado;
- fazer VS/PS receber `InstanceSlot`/`MaterialId` e ler tabelas globais;
- substituir CBV de objeto por transform/instance record quando possível;
- manter classes de PSO para opaque, masked, two-sided, blend e permutations;
- validar descriptor lifetime, null descriptors e heap capacity;
- adaptar z-prepass, GBuffer e shadows opacas por etapas;
- manter material preview/editor em caminho separado se necessário, sem duplicar o ABI canônico.

### Arquivos prováveis

- `Engine/Source/Graphics/Backend/D3D12/PipelineState.cpp`
- `Engine/Source/Graphics/Resources/Material.cpp`
- `Engine/Include/Smile/Graphics/Resources/Material.h`
- `Engine/Source/Graphics/Renderer/RendererPasses.cpp`
- shaders de depth/GBuffer/shadow e includes de material
- `Engine/Include/Smile/Graphics/Backend/D3D12/TextureSRVHeap.h`

### Gate de saída

- CPU draws continuam iguais, mas binds de material por item são eliminados no caminho migrado;
- opaque, masked, two-sided, emissivo, motion vectors e TAA/DLSS guides mantêm paridade;
- nenhum descriptor é reutilizado antes da fence;
- SRV heap possui ocupação/overflow observável;
- fallback não-bindless está explícito e contado.

## Fase 4 — GPU Scene persistente e ranges

> **Estado em 2026-09-10 — ABERTA, oito consumidores migrados.** Release fecha 66/66, Debug compila e
> o cliente TypeScript compila. Três dos seis itens do gate estão fechados; o de escala está provado
> em 4.096 objetos e **falta medir em 1M**. O `ObjectCB` **já pode deixar de ser alocado** — os sete
> consumidores leem o transform da tabela, a alocação é sob demanda e a troca de toggle é
> transacional nos dois sentidos —, e o `TransformBuffer` do TemporalMotion tem só a matemática
> pronta, com paridade provada em WARP. **Nenhum dos dois foi medido**, e é a medição que o gate
> exige. Detalhe, evidência, casos negativos e a fila do que falta em
> [GPU-DRIVEN-PHASE4-PROGRESS.md](GPU-DRIVEN-PHASE4-PROGRESS.md).
>
> A terceira revisão achou dois P1 no `ObjectCB` sob demanda — liberar o buffer não exigia que o eixo
> de material estivesse LIGADO (só que as PSOs existissem), e a falha de alocação lançava em vez de
> recusar — e **uma afirmação de estado errada minha**: eu disse que faltavam "1M + documentação"
> com o `TransformBuffer` intacto. Regra que sai daí: um item de gate não sai da lista porque o outro
> item do mesmo bullet andou.
>
> O achado que definiu a primeira slice: as linhas sujas já eram poucas — o gate da Fase 2 provava
> isso —, mas o **custo de descobri-las** era a tabela inteira, duas vezes (o produtor revarria os
> renderáveis a cada bump; o diff percorria a capacidade). Em 1M são ~191 MB de `memcmp` por frame
> para achar uma linha, e `rowsDirty` valia 1 nos dois casos. O contador que existia media o
> resultado, não o trabalho.
>
> A caixa de correio de sujos na `FScene` é **dica, não verdade**: bumpar sem índice invalida a
> caixa e joga o produtor na varredura completa. Não há como bumpar sem uma das duas coisas
> acontecer, então esquecer o índice custa trabalho a mais, nunca uma linha desatualizada.
>
> A expansão na GPU escreve por **posição**, e não por append atômico: nesta fase não há rejeição,
> então a saída é 1:1 e fica byte a byte comparável com a referência CPU — que é o gate. Compactar
> só ganha sentido na Fase 6, quando houver rejeição de verdade.
>
> **A revisão reprovou a primeira versão: seis achados, três bloqueadores, todos corrigidos.** Uma
> regressão de motion vector (índice repetido na caixa de correio colapsava `PrevTransform` em
> `Transform` no exato frame do movimento) e três checagens que eu havia documentado como decisão
> deliberada: a generation não era validada, o overflow produzia lista parcial utilizável, e a GPU
> lia fora do buffer onde a CPU recusava. O padrão vale para as fases seguintes — **quando o
> comentário explica por que NÃO verificar, o comentário é o defeito.**
>
> Três rodadas seguidas acharam defeito no **aparato de teste**, e não na implementação: dois slots
> por renderável, um A/B de picking que era falso-verde, e um conjunto de matrizes cuja rotação de
> eixo único escondia um sinal de cofator trocado. Irmã da regra acima — **código de teste merece a
> mesma desconfiança que código de produção**, e um `Require` que não fica vermelho em estado nenhum
> é indistinguível de nenhum `Require`.

### Objetivo

Publicar a cena na GPU por páginas sujas e estabelecer `FInstanceRange` como única entrada do futuro
culling.

### Trabalho

- evoluir a `FGpuSceneInstanceTable` criada na Fase 2 para páginas/ranges sujos, allocator e
  high-water mark; não criar uma segunda GPU scene paralela;
- upload inicial por páginas e updates incrementais por dirty ranges;
- produzir um range único para cena plana;
- **excluir a classe `RaytracingOnly` do conjunto candidato de raster.** `FRenderable::RaytracingOnly`
  ([`Scene.h`](../Engine/Include/Smile/Scene/Scene.h#L108)) marca renderáveis que existem só para
  TLAS e tabelas de hit — hoje o proxy do terreno — e que ficam fora de raster, prepass, CSM e
  picking. O laço CPU atual os filtra explicitamente em
  [`RendererPasses.cpp`](../Engine/Source/Graphics/Renderer/RendererPasses.cpp#L1922); a expansão
  de ranges precisa herdar esse filtro, ou o terreno passa a ser desenhado duas vezes — uma pelo
  `FTerrain` e outra pelo proxy. O D9 lista terrain/water como fallback, mas esta é uma classe
  distinta: não é fallback, é exclusão;
- expandir ranges na GPU e validar slots/generation em debug;
- publicar contadores de candidatos, inválidos e overflow;
- não executar culling ainda: todos os candidatos válidos passam;
- medir custo de upload em cena estática, transform storm e edição de material;
- **aposentar o `ObjectCB` e o `TransformBuffer` do TemporalMotion.** São os dois maiores
  consumidores por instância — 512 B e 256 B por entrada, 71,6% do footprint reservado por instância (ver a tabela da
  Fase 0). Sem esta tarefa explícita, a tabela paginada vira um **terceiro** buffer por instância
  em vez de substituir os que existem, e o custo sobe em vez de cair. O `ObjectCB` some quando o
  VS passa a ler a tabela — a Fase 3 troca o *binding*, é **aqui** que o buffer deixa de ser
  alocado; o TemporalMotion exige migrar o consumidor do histórico para a coluna `PrevTransform`.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/Scene/GpuScene.h`
- `Engine/Source/Graphics/Scene/GpuScene.cpp`
- `Engine/Source/Graphics/Renderer/RendererScene.cpp`
- novos shaders `Shaders/GpuScene/`
- manifestos CMake de engine e shaders

### Gate de saída

- cena estática envia zero dirty pages depois do assentamento;
- mover um objeto não faz upload proporcional ao total;
- remover/reusar slot respeita generation e fences;
- 1M instâncias não exigem reconstrução/upload integral por frame;
- **`ObjectCB` e o `TransformBuffer` do TemporalMotion deixaram de ser alocados**, verificado por
  medição de memória e não por leitura de código: o custo por instância tem de CAIR, não somar
  mais uma tabela;
- range único e múltiplos ranges sintéticos produzem a mesma lista expandida.

## Fase 5 — Indirect main view sem oclusão

### Objetivo

Trocar submissão por renderable por `ExecuteIndirect`, inicialmente desenhando o mesmo conjunto
frustum-visible ou até todos os candidatos para maximizar depuração.

### Trabalho

- definir command signature e layout de args/count;
- pré-calcular batch IDs por pass/PSO/geometry class;
- implementar count → prefix → scatter → args reutilizando o padrão da água;
- compactar `InstanceInfo`/visible slots;
- executar depth e GBuffer por grupos de geometry page/PSO;
- adicionar modo A/B CPU versus GPU-driven no mesmo build;
- incluir canário/guard regions e contadores de out-of-bounds;
- manter transparentes, terrain, water, editor helpers e unsupported no fallback.

### Arquivos prováveis

- novo `GpuDrivenRenderer`/`GpuDrawStream` em `Graphics/Renderer` ou `Graphics/Scene`
- `Engine/Source/Graphics/Renderer/RendererPasses.cpp`
- `Engine/Source/Graphics/Backend/D3D12/PipelineState.cpp`
- novos shaders de classify/count/prefix/scatter/args
- `Shaders/Water/WaterGenerateDraws.cs.hlsl` como referência, não dependência

### Gate de saída

- imagem e contagens coincidem com o caminho CPU para opaque suportado;
- argumentos, count buffer e instance ranges não ultrapassam capacidade;
- CPU submission cai de proporcional a objetos para proporcional a classes/pages;
- nenhuma readback decide a draw;
- alternar A/B no mesmo bookmark não exige reload da cena.

## Fase 6 — Culling GPU e HiZ residente

### Objetivo

Mover frustum e oclusão para a GPU, eliminar o readback do HiZ como decisão de frame e manter
comportamento conservador.

### Trabalho

- culling por frame contra frusta/pass masks;
- depth prepass baseado em frustum, com HZB anterior apenas quando conservador e medido;
- construir HZB atual depois do depth;
- usar HZB atual para GBuffer/late passes;
- conectar visibilidade diretamente aos counts/offsets/args;
- manter readback somente de métricas atrasadas;
- inflar bounds/frustum onde jitter e precisão exigirem;
- testar câmera dentro do bound, near-plane crossing, teleport, resize e dynamic resolution;
- evitar small-triangle culling nesta fase.

### Arquivos prováveis

- `Engine/Source/Graphics/Scene/HiZOcclusion.cpp`
- `Engine/Include/Smile/Graphics/Scene/HiZOcclusion.h`
- `Shaders/HZB/HZBTest.cs.hlsl`
- shaders de culling e draw stream
- `Engine/Source/Graphics/Renderer/RendererPasses.cpp`

### Gate de saída

- desligar readback de visibilidade não muda a imagem esperada;
- câmera em movimento rápido/teleport não produz desaparecimentos falsos;
- selected objects não precisam de exceção para mascarar atraso de readback;
- métricas separam candidates, frustum rejected, occlusion rejected e drawn;
- custo de culling + trabalho restante não piora o critical path nas cenas de gate.

## Fase 7 — Pass masks, shadows, alpha test e fallback endurecido

### Objetivo

Expandir o núcleo para passes reais sem transformar cada pass em outro pipeline completo.

### Trabalho

- calcular pass mask no culling por frame;
- gerar streams específicos para sun cascades e local shadows;
- avaliar reprojeção de receivers para descartar shadow casters somente com A/B;
- migrar masked e two-sided com classes de PSO corretas;
- preservar `IsTwoSidedForRT`/política raster coerente para foliage, grade e vidro;
- manter blend ordenado na CPU;
- contar fallback por motivo e publicar top offenders;
- migrar picking/selection para consumir stable handles/visible stream onde for seguro.

### Arquivos prováveis

- `Engine/Source/Graphics/Renderer/RendererPasses.cpp`
- `Engine/Source/Graphics/Lighting/SunShadows.cpp`
- `Engine/Source/Graphics/Lighting/LocalShadows.cpp`
- `Engine/Source/Graphics/Editor/Picking.cpp`
- shaders de depth/shadow/GBuffer

### Gate de saída

- sun/local shadows não percorrem `AllItems` inteiro para o conjunto migrado;
- **o cache estático do CSM sobrevive, ou sua remoção é paga por medição.** O `FSunShadows` já tem
  metade cacheada: DSV heap próprio, invalidação por `StaticCastersVersion` e lista ordenada com os
  estáticos primeiro ([`SunShadows.h`](../Engine/Include/Smile/Graphics/Lighting/SunShadows.h#L113)),
  de modo que a cascata estática não é redesenhada todo frame. Um stream de sombra GPU-driven que
  regenera tudo por frame **desfaz uma otimização que já existe**, e o gate acima não pega isso: a
  imagem fica idêntica e só o tempo piora. É o critério de parada nº 8 da §10 na sua forma concreta
  — aumentar o critical path sem remover custo equivalente;
- foliage/alpha test/two-sided não ganham buracos ou inversão de culling;
- picking e outline retornam o objeto certo após reuse de slots;
- fallback é pequeno, explícito e possui motivo;
- CPU path permanece disponível para bisect até a promoção da fase.

## Fase 8 — World cells, streaming e promoção do núcleo

### Objetivo

Trocar o produtor de range plano por broad phase espacial/streaming sem alterar o consumidor GPU e
promover o GPU-driven geral a caminho padrão quando os gates forem atendidos.

### Trabalho

- introduzir streaming cells e spatial nodes que mantêm **listas de handles arbitrários** e emitem
  spans em `CandidateInstanceIds` — nunca ranges de slots (ver D5). Célula não impõe contiguidade
  no allocator global;
- escolher loose octree/BVH/grid medido, sem copiar o quad-tree da Anvil por dogma;
- separar mundo lógico, residente e candidato;
- carregar/descarregar páginas respeitando handle generations e fences;
- suportar teleport e bursts de streaming;
- adicionar orçamento de residency e backpressure;
- promover GPU-driven para opaque/masked suportado;
- manter kill switch e fallback diagnóstico por uma janela definida.

### Arquivos prováveis

- novos tipos em `Engine/Include/Smile/Scene/`
- loader/streaming em `Engine/Source/Scene/`
- `GpuScene` e renderer scene import
- editor/outliner para estado lógico versus residente

### Gate de saída

- 1M residentes e milhões lógicos não causam iteração CPU integral por frame;
- trocar range único por ranges de cells não muda shaders de culling;
- teleport não usa handle stale nem mostra geometria liberada;
- budgets e evictions são observáveis;
- GPU-driven é default para o conjunto suportado e possui rollback conhecido.

## Fase 9 — Clusters planos e sidecar `.svgeo`

### Objetivo

Introduzir um consumidor real de clusters antes de congelar o formato de virtual geometry.

### Trabalho

- integrar formalmente uma versão fixada do `meshoptimizer` ou alternativa escolhida;
- registrar licença, versão e CMake;
- gerar clusters com sweep de parâmetros, não assumir 256/128;
- definir `.svgeo` v1 independente do `.smesh` base;
- armazenar bounds, normal cone, vertex/triangle ranges e encoding validado;
- rasterizar clusters selecionados com pipeline convencional/mesh shaders conforme suporte;
- medir vertex reuse, cluster count, bytes, overdraw e culling efficiency;
- manter geometria convencional como fallback e referência.

### Arquivos prováveis

- `Engine/ThirdParty/meshoptimizer/` formalmente versionado
- `Tools/Cooker/`
- novos tipos de sidecar em `Engine/Include/Smile/Scene/`
- loader de virtual geometry
- shaders de cluster culling/raster
- query de mesh shader tier em `D3D12Device`

### Gate de saída

- sidecar possui schema, versão, bounds checks, checksum e testes de corrupção;
- cluster build é determinístico;
- parâmetros são justificados por dados em pelo menos três famílias de cena;
- caminho sem `.svgeo` continua funcional;
- nenhum DAG/streaming é fingido por um array plano.

## Fase 10 — Hierarquia, LOD contínuo e geometry pages

### Objetivo

Selecionar um corte hierárquico por erro projetado e carregar somente páginas necessárias.

### Trabalho

- construir DAG/árvore de clusters com parent/child ranges;
- armazenar erro geométrico e bounds conservadores;
- selecionar clusters por erro em pixels na render resolution atual;
- tratar jitter/dynamic resolution sem cintilação;
- impedir cracks ou definir estratégia explícita de seams/stitching;
- criar geometry page cache, feedback, request dedup, prioridade e eviction;
- separar requests visíveis, prefetch e RT;
- testar memória insuficiente e páginas atrasadas com fallback conservador;
- integrar cell streaming e cluster streaming sem duas políticas concorrentes.

### Gate de saída

- transições não apresentam popping inaceitável em sequência de câmera;
- seleção respeita erro projetado em resoluções/render scales diferentes;
- miss de página nunca lê memória inválida nem remove toda representação do objeto;
- VRAM respeita orçamento e evictions não drenam a GPU;
- Moana geometry subset ou stress equivalente demonstra benefício sobre LOD tradicional.

## Fase 11 — Micropolygon, materiais deferred, RT e benchmark Moana

### Objetivo

Fechar a trilha Nanite-like somente onde o perfil justificar, integrar consumidores restantes e
validar escala de produção.

### Trabalho

- comparar hardware raster/mesh shader com software raster para microtriângulos;
- avaliar visibility buffer/material binning em vez de expandir PSOs por material;
- manter alpha/foliage/deformáveis em caminhos adequados, sem forçar solução única;
- definir política RT para BLAS por LOD/cluster, cache, proxies e TLAS residente;
- converter uma Moana Geometry subset preservando protótipos e transforms;
- avançar para ilha completa por cells/pages, sem exigir materiais/volumes offline no primeiro gate;
- medir CPU, GPU, VRAM, streaming, qualidade e estabilidade temporal;
- documentar quais partes são GPU-driven geral, virtual geometry ou conteúdo específico.

### Gate de saída

- escolha hardware/software raster é sustentada por perf e qualidade, não analogia com outra engine;
- material resolve não reintroduz custo por material/draw;
- RT não exige todas as instâncias lógicas no TLAS;
- Moana-scale preserva instancing e opera dentro de budgets declarados;
- a feature possui fallback, diagnóstico, documentação e matriz de plataformas.

## 9. Matriz de validação

### 9.1 Conteúdo

Os **gates obrigatórios** são Bistro, Emerald e as três sintéticas: as duas primeiras cobrem cena
real (material, alpha, oclusão, instancing) e as sintéticas cobrem escala com geometria e
materiais controláveis. O resto é opcional e roda quando o eixo dele interessar.

| Conteúdo | O que prova | |
|---|---|---|
| Bistro | materiais variados, transparência, alpha e regressão visual | **gate** |
| Emerald | instancing real, muitos materiais e vegetação mascarada | **gate** |
| sintética 10k/100k/1M | scaling controlado e overflows | **gate** |
| BEEPLE MEASURE_SEVEN | maior contagem do acervo (10.740), mas exercita iluminação/RT mais que submissão — o eixo de escala é melhor coberto pelas sintéticas | opcional |
| SunTemple | shadows e referência histórica do protótipo removido | opcional |
| Moana Geometry subset | instancing massivo, clusters e streaming |
| Moana completa | alvo final de escala, não gate do núcleo inicial |

### 9.2 Câmera e tempo

- bookmark estático repetível;
- flythrough com velocidade controlada;
- teleport;
- near-plane crossing;
- câmera dentro de bounds grandes;
- render scale sweep e resolução dinâmica;
- jitter TAA/DLSS/FSR;
- objeto estático, dinâmico, spawn, delete e reuse;
- resize e troca de cena.

### 9.3 Materiais e passes

- opaque single/two-sided;
- alpha test foliage e alpha test não-foliage;
- blend no fallback;
- emissivo;
- normal/MR separados e packed;
- z-prepass, GBuffer, sun/local shadow;
- picking/selection;
- motion vectors e guides temporais;
- RT hits/alpha/OMM quando aplicável.

### 9.4 Sinais obrigatórios

- tempo CPU de scene update, list/classify e submission;
- tempo GPU de expand/cull/count/prefix/scatter/args por pass;
- candidates, visible, rejected por motivo;
- batches potenciais, indirect commands e draws fallback;
- bytes persistentes, transitórios, dirty/uploaded e high-water marks;
- descriptor occupancy;
- overflows, stale handles, page misses e evictions;
- waits/drains e queue bubbles;
- imagem/capture metadata conforme protocolo.

### 9.5 Gates de qualidade

- comparação visual A/B no mesmo build e bookmark;
- sequência temporal para popping, shimmer, ghosting e motion vectors;
- bounds/culling conservador sob jitter;
- nenhum pixel perdido é aceito como “otimização” sem classificação conhecida;
- small-triangle culling, quando existir, usa render resolution e considera jitter;
- backface culling respeita a classe two-sided correta, não apenas uma flag crua.

## 10. Critérios de interrupção

Uma fase para e volta ao implementador quando:

- requer `Flush`/`WaitIdle` no frame estável;
- cria readback necessário para desenhar;
- usa índice denso como identidade persistente;
- perde geometria em overflow em vez de fallback conservador;
- mistura material identity na chave depois de bindless sem justificar PSO;
- torna cluster/virtual geometry obrigatório para conteúdo convencional;
- publica ganho sem baseline limpa/repetível;
- aumenta critical path da GPU sem remover custo equivalente;
- altera formato persistido sem versionamento e testes de bounds;
- reviewer encontra P0/P1 ainda aberto.

## 11. Sequência sugerida de commits

Cada item abaixo deve ser revisável isoladamente; uma fase pode exigir mais de um commit, mas commits
não atravessam fases sem motivo explícito.

1. `docs(rendering): add gpu-driven architecture plan`
2. `chore(cooker): version the scene recook recipe and baseline manifest` — **não** um commit de
   assets: `/Assets/Scenes/` está no `.gitignore` (linha 36) e nenhum `.sscene`/`.smesh` é tracked,
   então um commit de recook seria vazio. O que se versiona é a receita (script/manifesto por cena,
   incluindo quem usa `--opaque-glass`) e a atualização da baseline. O recook em si é uma operação
   de máquina, registrada por hash no manifesto
3. `perf(renderer): add cpu submission and gpu-driven baseline telemetry`
4. `refactor(scene): add stable gpu scene handles and consumer remaps`
5. `feat(rendering): add canonical gpu scene tables`
6. `refactor(raytracing): source instance materials from canonical table`
7. `feat(rendering): make opaque raster materials bindless`
8. `feat(rendering): add paged gpu scene uploads and instance ranges`
9. `feat(rendering): generate opaque indirect draw streams`
10. `feat(rendering): keep frustum and hiz visibility on gpu`
11. `feat(rendering): add gpu-driven shadow streams and masked materials`
12. `feat(scene): produce instance ranges from streaming cells`
13. `feat(cooker): add versioned cluster sidecar`
14. `feat(rendering): select hierarchical cluster lods`
15. `feat(rendering): stream virtual geometry pages`
16. `feat(rendering): add measured micropolygon raster path`

## 12. Pacote obrigatório para revisão

O implementador entrega, para cada milestone:

```text
Objetivo e fase
Base commit e head commit
Arquivos alterados
Invariantes afetados
Formato/ABI antes e depois
Comandos de build/test executados
Capturas e métricas com ambiente
Fallback e kill switch
Limitações conhecidas
Riscos ainda abertos
Diff completo
```

O revisor:

1. lê o diff antes da justificativa detalhada para reduzir ancoragem;
2. verifica correção, D3D12, ABI, lifetime, arquitetura e evidência;
3. classifica findings por prioridade;
4. aponta arquivo/linha, consequência, correção recomendada e teste;
5. não modifica o working tree;
6. repete a revisão depois das correções;
7. não aprova uma fase apenas porque compila.

Formato sugerido de finding:

```text
[P1] Título curto
Evidência: arquivo:linha e estado observado
Impacto: falha funcional, corrupção, stall ou dívida arquitetural
Orientação: mudança mínima esperada
Prova: teste/captura/métrica que fecha o finding
```

P0/P1 bloqueiam promoção. P2 precisa ser corrigido ou explicitamente aceito pelo proprietário. P3 é
melhoria não bloqueante e não deve expandir o milestone.

## 13. Comandos mínimos de validação

Para todo milestone com código:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Mudanças em shaders exigem compilação DXC bem-sucedida. Mudanças visuais exigem captura antes/depois
com `CAPTURE-PROTOCOL.md`. Alterações de formato exigem, além disso:

- round-trip ou load de arquivo válido;
- truncation/corruption tests;
- stride/offset assertions;
- rejeição explícita de versão incompatível;
- recook determinístico ou diferença explicada.

Mudanças GPU-driven exigem A/B no mesmo executável enquanto o fallback existir.

## 14. Decisões que não devem ser reabertas sem nova evidência

- o raster bindless precede batching material-independent;
- resultado de visibilidade não volta à CPU;
- ranges são a entrada desde o primeiro culling;
- IDs estáveis migram TLAS, temporal, HiZ e picking juntos;
- `FRTInstanceGeo` é snapshot especializado, não tabela universal;
- não será criado compilador Database/SIG;
- GPU hash map não entra no MVP;
- triangle culling tradicional não entra antes de clusters/mesh shaders medidos;
- direct queue precede async compute;
- transparência ordenada permanece fallback inicial;
- virtual geometry é sidecar e não requisito do formato-base;
- os parâmetros 256/128 do protótipo removido não são default aprovado;
- a Moana é benchmark de escala, não asset a redistribuir nem requisito para fechar o núcleo.

## 15. Proveniência técnica

- William Bussière e Nicolas Lopez, “GPU-Driven Rendering in Assassin's Creed Mirage”, em
  *GPU Zen 3: Advanced Rendering Techniques*.
- Ubisoft Anvil, “Micropolygon Rendering in Anvil”, GDC 2026.
- Disney Animation, *Moana Island Scene*, dataset de pesquisa e benchmarking.
- `meshoptimizer`, referência candidata para clusterização e simplificação; versão e integração ainda
  devem ser deliberadamente escolhidas na Fase 9.
- Implementação atual da Smile citada nas seções de evidência deste documento.

## 16. Ponto exato para começar

Não começar por shader de culling, meshlet ou `.svgeo`. O próximo trabalho autorizado é a Fase 0:

1. ~~recook v8 coerente~~ — feito; versionar a RECEITA do recook (ver Fase 0);
2. build/test/captura baseline;
3. telemetria CPU de preparação/submissão;
4. cena sintética escalável;
5. pacote de evidência para revisão read-only.

Somente depois desse gate começa a migração coordenada de identidade da Fase 1.
