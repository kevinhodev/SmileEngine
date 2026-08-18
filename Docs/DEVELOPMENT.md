# Guia de desenvolvimento

Este documento concentra as convenções que devem permanecer estáveis enquanto a engine cresce.
`ARCHITECTURE.md` descreve o estado atual; este guia descreve como alterar esse estado sem criar
novas exceções. A regra curta para agentes está em `.cursor/rules/smile-engine-standards.mdc`.

## Limites de dependência

- `Core/` e `Math/` não dependem de DirectX, Qt, cena ou renderer.
- `Graphics/` pode consumir `Core/`, `Math/` e o modelo de cena.
- Código novo em `Scene/` não inclui `Renderer.h`. O acoplamento atual de `SceneLoader.cpp` é dívida
  conhecida e deve ser removido por um resultado de importação independente do renderer.
- `Engine/` não depende de Qt. Tradução para `QObject`, signals, models e QML pertence a `Editor/`.
- Ferramentas reutilizam a API pública de `Engine/Include` sempre que possível.
- Headers públicos expõem contratos; detalhes de implementação e helpers privados ficam no `.cpp`.

## Contrato de render pass

Há dois papéis deliberadamente diferentes:

- `FPipelineOwner` possui shaders/PSOs e participa do hot reload. Bakers como IBL e ruído de nuvens
  pertencem aqui mesmo sem gravar trabalho por frame.
- `FRenderPass` acrescenta ciclo de vida de frame: atividade, resize, debug targets e invalidação de
  histórico.

O registro é um índice não-dono. Os objetos continuam membros por valor do `Renderer`, que controla
sua ordem de construção e destruição. Um passe entra nas listas por `Renderer::RegisterPasses()`;
seus shader stems ficam junto do próprio objeto que recria os PSOs.

Não há `virtual Execute()` comum. As assinaturas de gravação têm insumos diferentes e forçá-las para
um contexto universal apenas moveria o god object. A ordem do frame permanece explícita nas fases
nomeadas de `Renderer`, enquanto o registro uniformiza somente o ciclo de vida que é realmente comum.

Ao adicionar um passe:

1. derive de `FPipelineOwner` ou `FRenderPass` conforme ele grave trabalho por frame;
2. implemente `Name`, `IsInitialized`, `ShaderStems` e `OnRecreatePipelines`;
3. registre-o em `Renderer::RegisterPasses()`;
4. se possuir recursos dependentes de resolução, implemente `OnResize`;
5. se acumular entre frames, declare `HistoryTargets` e trate `OnInvalidateHistory`;
6. publique alvos de debug por `OnRegisterDebugTargets`;
7. adicione um teste CPU para qualquer regra que não precise de uma GPU.

O reload por stem visita todos os donos correspondentes. Isso é necessário porque instâncias
diferentes podem compartilhar o mesmo shader, como as cascatas do oceano.

## Política de comentários

Comentários no código devem explicar algo que os tipos e nomes não conseguem expressar:

- contrato de ownership ou lifetime;
- ordem obrigatória de comandos/barreiras;
- invariante temporal, de memória ou de layout C++/HLSL;
- motivo não evidente para uma decisão com efeito observável.

Mova para `Docs/`:

- cronologia de bugs e commits;
- comparações extensas com outras engines;
- resultados de benchmarks e auditorias;
- planos, fases concluídas e trabalho futuro.

Remova comentários que apenas traduzem a linha seguinte, repetem o nome de um campo, descrevem um
estado que já mudou ou funcionam como separador visual excessivo. Um teste com nome claro é preferível
a um parágrafo que promete uma invariante sem verificá-la.

## Estilo e alterações mecânicas

- C++20, quatro espaços e braces na mesma linha; `.clang-format` é a fonte de verdade.
- Identificadores em inglês; comentários e documentação podem permanecer em PT-BR.
- Definições usam `_` nos parâmetros; membros não usam sufixo `_`.
- `.editorconfig` define indentação e UTF-8; `.gitattributes` fixa arquivos texto em LF.
- Formate somente arquivos tocados. Uma normalização ampla deve ser um commit isolado, sem feature.
- Headers e unidades `.cpp` da engine permanecem explícitos no manifesto
  `Engine/cmake/SmileEngineSources.cmake`. O configure falha se um arquivo ficar sem domínio, for
  listado duas vezes ou deixar de existir.

## Organização de Graphics

- Headers públicos ficam em `Engine/Include/Smile/Graphics/<Domínio>`; implementações ficam no
  domínio espelhado em `Engine/Source/Graphics/<Domínio>`.
- Um include sempre carrega o domínio: `#include "Smile/Graphics/Backend/D3D12/D3D12Device.h"`.
- No Visual Studio, a separação `Include`/`Source` é removida dos filtros: `.h` e `.cpp` aparecem
  juntos em `Graphics/<Domínio>`.
- A implementação de `Renderer` é dividida por responsabilidade: lifecycle em `Renderer.cpp`,
  cena em `RendererScene.cpp`, captura/diagnóstico em `RendererCapture.cpp`, frame em
  `RendererFrame.cpp` e gravação de passes em `RendererPasses.cpp`. O mapa de ownership e fluxo
  está em [`RENDERER.md`](RENDERER.md). Estado CPU persistente de cena pertence a
  `RendererSceneState.h`; histórico e relógios entre frames pertencem a `RendererFrameState.h`.
- Escolha primeiro um dos domínios existentes: `Renderer`, `Backend`, `Resources`, `Scene`, `Lighting`,
  `GI`, `RayTracing`, `Environment`, `PostProcess`, `Water`, `Editor` ou `Debug`. Crie outro apenas
  quando houver responsabilidade própria e mais de um componente relacionado.

## Validação mínima

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Mudanças em shaders também precisam de compilação DXC bem-sucedida. Mudanças visuais devem ser
validadas no editor, idealmente com captura antes/depois usando o protocolo de `CAPTURE-PROTOCOL.md`.

## Próximas frentes arquiteturais

1. Reduzir a superfície e o ownership de `Renderer.h`, movendo estado para os subsistemas donos sem
   voltar a concentrar a implementação em uma única TU.
2. Remover a dependência `SceneLoader -> Renderer` com um resultado de importação independente.
3. Extrair telemetria, apresentação de debug targets e fila de jobs de `ViewportWidget`.
4. Aumentar a cobertura CPU de `Math`, `CookedFormat`, culling e mutações de cena.
5. Limpar comentários por subsistema junto de mudanças funcionais, movendo conhecimento durável para
   documentos temáticos e evitando um diff cosmético gigante.
