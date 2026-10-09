# Revisão do ambient occlusion — 9 de outubro de 2026

A Smile mantém GTAO full-res como padrão, com opção half-res. A revisão corrige a reconstrução da projeção, a integração fora do centro da câmera, os filtros e a reconstrução da meia resolução. Não importa código das engines de referência.

## Problemas corrigidos

1. **Projeção incompleta.** A posição em view-space ignorava M20/M21, embora o depth fosse produzido pela matriz com jitter de TAA/FSR/DLSS. O passe agora recebe a projeção usada no frame e subtrai seus offsets na reconstrução. O layout CPU/HLSL continua em um CBV de 256 bytes, com asserts de tamanho e offset.
2. **Sinal incorreto da normal projetada.** O sinal do ângulo usava a direção da tela sem remover sua componente ao longo de V. Agora usa uma base ortogonal à direção de visão e `atan2`.
3. **Escurecimento de uma superfície aberta.** Poucas direções uniformes em tela não são uniformes em torno de V, sobretudo fora do centro. A soma da visibilidade é normalizada pela soma da mesma integral com o hemisfério aberto. Isso preserva a referência branca em vez de confundir erro de quadratura com oclusão.
4. **Amostragem nas bordas.** O clamp dos taps para dentro da imagem repetia a última coluna/linha e podia amostrar o próprio receptor. Taps fora da tela são descartados, mantendo o horizonte aberto quando falta informação.
5. **Raio e heurística de espessura.** O falloff anterior terminava em `radius/sqrt(2)` e misturava amostras de fora do raio ao horizonte acumulado. O limite agora corresponde ao raio configurado. A compensação de objetos finos aumenta a distância de falloff na componente de profundidade; o horizonte não decai a cada tap que reencontra o receptor. A distribuição quadrática concentra amostras perto do contato.
6. **Blur atravessando superfícies.** O peso relativo de depth aceitava superfícies diferentes, especialmente ao longe, e ignorava normais. Blur e upsample usam normal geométrica e distância simétrica aos planos tangentes. O blur acumula conectividade entre taps para não saltar sobre uma silhueta fina. Céu é explicitamente branco e não entra na média.
7. **Half-res deslocado.** O texel reduzido h representa o pixel full-res `2*h`; o upsample usava a fórmula de um grid centrado diferente. A coordenada agora é `pixel/scale`, preservando exatamente os anchors pares, inclusive em dimensões ímpares. Sem uma amostra da mesma superfície, retorna branco.
8. **Ruído sem acumulação.** Sem TAA/upscaler ativo, o índice de ruído fica fixo. O ciclo de 12 frames permanece disponível quando há um reconstrutor temporal.
9. **Raio/intensidade nulos e fade.** Saídas neutras têm early-out, antes das buscas de horizonte.

As quatro tabelas de filtro passaram de dois para três SRVs (`depth`, `AO`, `normal`); alocação, cópia, root signatures e liberação acompanham a mudança. A cadeia de barreiras continua UAV → compute SRV entre passes e pixel SRV no resultado final.

## Referências examinadas

- Unreal local **UE 5.8**: `Engine/Shaders/Private/PostProcessAmbientOcclusion.usf`, especialmente `SearchForLargestAngleDual`, `ComputeInnerIntegral`, filtros espacial/temporal e upsample; `Engine/Source/Runtime/Renderer/Private/CompositionLighting/PostProcessAmbientOcclusion.cpp`, parâmetros de raio, espessura e ciclos temporais. A referência confirma a necessidade de uma base ortogonal para projetar a normal e de separar busca, filtro e reconstrução.
- Cry local: `Code/CryEngine/RenderDll/XRenderD3D9/GraphicsPipeline/ScreenSpaceObscurance.cpp` e `Engine/Shaders/HWScripts/CryFX/DeferredShadingcfx.txt`, blocos `SSDO_Sampling_PS` e `SSDO_Filter_PS`. SSDO também guarda informação direcional; não é um GTAO intercambiável com a saída escalar da Smile. O filtro guiado por depth é útil como referência de integração, mas não resolve sozinho superfícies coincidentes com normais diferentes.
- [XeGTAO, implementação e discussão dos autores](https://github.com/GameTechDev/XeGTAO): distribuição quadrática, falloff relativo ao horizonte da normal e compensação de objetos finos. A Smile mantém seu orçamento de 3 direções × 8 passos × 2 lados; não assume os timings publicados para outro algoritmo/hardware.
- [Jimenez et al., Practical Realtime Strategies for Accurate Indirect Occlusion](https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf): integral GTAO, diffuse multi-bounce e specular occlusion.

## Integração de iluminação revisada

`DeferredLighting.ps.hlsl` já combina AO de material com GTAO, usa o polinômio multi-bounce no difuso e specular occlusion dependente de roughness/NoV no IBL. Luz direta e emissivo não são multiplicados indiscriminadamente pelo GTAO. Folhagem tem o bypass existente para evitar auto-oclusão das cards; subsurface sólido segue o caminho normal.

O GTAO continua sendo um detalhe de contato aplicado também ao indireto de DDGI/ReSTIR. No ReSTIR por pixel, que já mede visibilidade, isso é uma heurística adicional e não uma identidade física. A revisão não redefine essa política sem referência ray-traced do composto. Tampouco aplica AO escalar novamente aos reflexos DXR já resolvidos.

## Testes e reprodução

`SmileAmbientOcclusionPipelineTests` executa o passe C++ real e os shaders de produção em WARP, com readback R8 e checagem de erros da debug layer quando disponível. Usa reverse-Z, normais R10G10B10A2 como o prepass, câmera rotacionada, projeção com jitter e dimensões ímpares. Cobre plano aberto, contato, céu, fade, raio zero, repetibilidade sem AA, preservação de normais e inclinações, silhueta de um pixel, anchors half-res, superfície ausente no grid, viewport 1×1 e ocupação de descritores após resize.

```powershell
cmake --build build --config Release --target SmileAmbientOcclusionPipelineTests
ctest --test-dir build -C Release -R '^Smile.AmbientOcclusionPipeline$' --output-on-failure
.\build\bin\Release\SmileAmbientOcclusionPipelineTests.exe --hardware
```

O alvo opcional `SmileAmbientOcclusionRendererTests`, habilitado junto de `SMILE_ENABLE_RENDERER_IDENTITY_TESTS`, carrega uma cena real em uma janela oculta, captura com 128 frames de aquecimento e mede 32 frames distintos por modo com timestamps da GPU. Exercita off/full/half, visualização do alvo GTAO, TAA, resize ímpar, DLSS quando disponível e shutdown. Espera a publicação inicial dos recursos de cena antes de iniciar a captura determinística.

```powershell
cmake --build build --config Release --target SmileAmbientOcclusionRendererTests
.\build\bin\Release\SmileAmbientOcclusionRendererTests.exe `
  Assets\Scenes\Bistro\BistroExterior.sscene build\AO-Bistro
```

O controle negativo compila os três shaders originais de `HEAD`, carregados pela orquestração C++ revisada. Portanto é um A/B dos shaders, não uma reconstrução de todo o executável anterior. O original reproduziu 12 falhas; a revisão passou. A alteração do ruído sem AA pertence à orquestração atual em ambos os braços e não é medida por esse A/B.

## Resultados medidos

Build Release do engine, editor e dos dois novos alvos concluído. Os três shaders também compilaram diretamente com DXC `cs_6_0`, `-WX`. CTest/WARP e a execução na **NVIDIA GeForce RTX 3060 Ti** passaram com zero falhas. Os logs [original](original-warp.log), [revisado WARP](revised-warp.log) e [revisado hardware](revised-hardware.log) ficam junto desta auditoria.

No plano aberto, a visibilidade mínima do original era **0,349 full / 0,361 half**; a revisão retorna **0,988 em ambos**, incluindo as bordas da tela. Branco é 1. No plano inclinado com câmera rotacionada e jitter, a média revisada fica em aproximadamente **0,987**. O teste de contato continua produzindo oclusão: mínimos **0,851 full / 0,882 half**, portanto a correção não se resume a clarear toda a saída. As 12 falhas do controle negativo incluem auto-oclusão, raio zero, cruzamento de normais/silhuetas, céu e reconstrução half-res.

Foram capturadas **32 imagens**, oito por braço em cada cena: `BistroExterior.sscene` e `EmeraldSquare_Day.sscene`. O composto com TAA preserva os contatos arquitetônicos, enquanto a visualização isolada mostra menos borrão e mais detalhe. A folhagem aparece ruidosa no alvo AO sem acumulação; o composto conserva o bypass de folhagem já existente. As pranchas têm original à esquerda e revisão à direita:

- [Bistro: AO full, half e composto com TAA](bistro-comparison.png).
- [Emerald: AO full, half e composto com TAA](emerald-comparison.png).

Cada diretório de captura contém PNGs, manifests, `gpu.csv`, logs e marcador de shutdown concluído. Também há capturas de AO desligado, composto sem AA, TAA half-res após resize **961×541** e DLSS half-res. `analyze.py` verifica os campos explícitos de câmera, iluminação, resolução e reconstrução de cada par; [analysis.json](analysis.json) contém essas verificações, hashes dos shaders e estatísticas brutas resumidas. A iluminação estática da cena foi mantida; o pedido de pin em 10h não é aplicado quando o ciclo de horário está desabilitado, como os manifests registram.

O custo aumentou. Médias do escopo GPU `GTAO`, em **1280×720 nativo**, incluindo main e filtros:

- **Bistro full:** 0,681 → **0,847 ms**; **half:** 0,296 → **0,385 ms**.
- **Emerald full:** 0,609 → **0,851 ms**; **half:** 0,181 → **0,367 ms**.

São blocos sequenciais de **32 frames distintos por modo e braço**, sem AA, após aquecimento. Não constituem benchmark estatístico entre várias rodadas; frequências, carga e outliers afetam as médias. As medianas e extremos estão no JSON. O filtro por normais/planos e sua conectividade têm custo real, comprado aqui para corrigir vazamento e preservar geometria. Full-res permanece padrão; half-res oferece a alternativa de custo menor.

O controle com AO desligado ainda difere levemente entre execuções: RMSE RGB de **0,375/255 na Bistro** e **0,539/255 na Emerald**, com diferenças locais maiores. Outros sistemas de GI possuem estado temporal; por isso, pequenas diferenças no composto não são atribuídas exclusivamente ao GTAO. Os testes sintéticos e o alvo AO isolado são a evidência direta das correções.

Base de referência: `a45087c607a4501839f0a8779d4b0543e3717186`, workspace com alterações preexistentes preservadas. Os CSOs revisados foram restaurados após cada A/B e seus hashes conferidos; a suíte final passou novamente com esses binários.

## Limites

O resultado continua limitado ao depth visível: falta informação atrás da câmera e fora da tela. Half-res pode perder geometria menor que o grid; branco no fallback impede escurecimento de outra superfície, sem reconstruir o oclusor perdido. Não foram adicionados bent normals, depth mips próprios nem histórico temporal exclusivo de AO. O HZB de culling reverse-Z não substitui automaticamente a pirâmide de view-depth filtrada exigida por GTAO hierárquico. Esses recursos exigem integração e medição próprias.
