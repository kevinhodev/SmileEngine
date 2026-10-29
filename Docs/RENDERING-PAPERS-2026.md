# Papers de renderização 2026 — triagem e implementação

Levantamento de outubro de 2026 dos trabalhos recentes (HPG, SIGGRAPH, I3D, EGSR, Eurographics)
com aplicação direta à SmileEngine. O critério é encaixe na arquitetura que já existe: ReSTIR DI e
GI com reuso espacial, DDGI com radiance cache, NRD/RELAX e DLSS-RR.

## Implementado

### Compatibility-Guided Neighbor Selection for ReSTIR

Junkins, Kettunen, Lin, Ramamoorthi e Wyman. *Proc. ACM CGIT* 9(4), art. 52. HPG 2026, primeiro
lugar no prêmio Wolfgang Straßer de melhor paper. doi:10.1145/3820024.

O reuso espacial clássico sorteia K pixels num disco e rejeita os que falham num teste binário de
normal e posição. Perto de silhuetas, folhagem e geometria fina quase todos falham, e o pixel reusa
pouco ou nada. O paper troca esse procedimento por um pool maior de candidatas, pontuadas por um
escore contínuo de compatibilidade, com os K vizinhos escolhidos por amostragem ponderada:

```text
 score(q) = max(n·n_q, 0)^β · exp(-‖x - x_q‖ / s),   s = sqrt(Ω·d² / π)
 β = 8, Ω = 0.05 sr, d = distância da câmera ao ponto primário
```

O paper reporta de 6% a 29% menos SMAPE e de 22% a 49% menos covariância temporal, por um custo
incremental de 2% a 5%. A redução de correlação é a parte que mais importa aqui: NRD e DLSS-RR
confundem ruído correlacionado com sinal.

Na engine:

- `Shaders/Common/ReSTIRNeighborSelection.hlsli` implementa o escore e a seleção. O shader que
  inclui o header fornece `RNS_LoadSurface`.
- `ReSTIRDISpatial.cs.hlsl` e `ReSTIRGISpatial.cs.hlsl` usam a seleção quando o toggle está
  ligado. Os testes binários de normal e posição saem nesse modo. O teste do Jacobiano do GI
  continua, porque protege a reconexão e não mede semelhança de superfície.
- O pool tem 32 candidatas num raio de 3× o raio espacial configurado (48 px no default). A busca
  termina cedo quando K candidatas com escore ≥ 0,5 já apareceram.

A implementação difere da referência em dois pontos:

1. **Seleção sem reposição.** A referência roda K reservoirs independentes sobre o mesmo stream de
   candidatas. Com a busca terminando cedo, isso repete o mesmo vizinho com frequência, e cada
   repetição custa um domínio duplicado no merge e no denominador MIS. Aqui as chaves de
   Efraimidis–Spirakis (`log(u)/w`, mantendo as K maiores) fazem amostragem ponderada sem
   reposição, com um aleatório por candidata em vez de K.
2. **Piso de escore de 1e-3.** O target function do MIS no DI não inclui visibilidade. Sem o piso,
   uma superfície atrás de uma silhueta poderia ser escolhida só por falta de alternativa.

A escolha depende apenas da geometria primária e nunca do conteúdo dos reservoirs. Por isso a
correção MIS (balance heuristic) dos dois passes continua válida sem alteração.

**Controles**

| Superfície | DI | GI |
|---|---|---|
| Janela ReSTIR | "Vizinhos por compatibilidade" na seção de visibilidade e denoise | "Vizinhos por compatibilidade" na seção de reuso e validação |
| `FRenderSettings` | `SetDICompatNeighborSelection` | `SetGICompatNeighborSelection` |
| MCP `smile_profile_configure` | `diCompatNeighbors` | `giCompatNeighbors` |
| Manifesto de captura | `diCompatNeighborsRequested/Effective`, sufixo `-cns` | `restirGICompatNeighbors`, tag `cns` |

O default é OFF até o A/B nas cenas de teste. O toggle não limpa reservoirs, porque o Pass B não
realimenta o temporal. Ele invalida somente o que acumula sobre a saída: no DI, o NRD direto e o
resolve; no GI, `Dom::ScreenResolve`, o mesmo domínio do `SetGIVisibility`.

**Validação pendente (GPU)**

1. Compilar os dois shaders pelo DXC do SDK. Não houve compilação no ambiente em que o código foi
   escrito.
2. Rodar Bistro e Emerald Square com o preset `controlled_nrd`, nos dois braços de
   `diCompatNeighbors` e `giCompatNeighbors`. Comparar o heatmap de flicker (variância temporal),
   o erro contra uma referência acumulada e o tempo dos escopos `ReSTIR DI` e `ReSTIR GI` no
   profiler.
3. Inspecionar folhagem e silhuetas finas, onde o método deve render mais. Conferir também que não
   surge vazamento de luz entre objetos separados por profundidade.
4. Se o A/B confirmar o paper, ligar o default nas duas classes.

## Próximos candidatos

Em ordem de custo/benefício para esta engine:

1. **Duplication maps, de *ReSTIR PT Enhanced*.** Lin, Kettunen e Wyman, *Proc. ACM CGIT* 9(1),
   art. 13. I3D 2026, melhor paper. doi:10.1145/3804494. Um passe conta quantos pixels de uma janela
   17×17 carregam a mesma amostra e reduz o teto de M temporal onde a duplicação é alta. O paper
   aplica a técnica ao ReSTIR PT, mas o mecanismo serve ao DI (id = luz + UV) e ao GI (id = hash de
   x2). Ataca diretamente a correlação que o DLSS-RR tolera mal. A seleção de vizinhos de
   reciprocidade e o critério de reconexão por footprint do mesmo paper só se aplicam quando houver
   ReSTIR PT com pairwise MIS.
2. **Decorrelação estocástica para DLSS-RR.** Mesmo paper, versão do RTXDI 2026. Com probabilidade
   p, o pixel substitui o reservoir reamostrado pela amostra inicial e independente. p escala com a
   "estagnação" (idade da amostra), suavizada espaço-temporalmente para não enviesar. Encaixa no
   caminho `FDlssRRPass`, que hoje recebe o sinal correlacionado do ReSTIR.
3. **ReSTCV, *Spatio-Temporal Control Variates with ReSTIR for Real-Time Rendering*.** Shi, Wu,
   Wu e Xu, SIGGRAPH 2026. doi:10.1145/3799902.3811113. Usa control variates em espaço de imagem
   sobre o reuso do ReSTIR para tirar ruído cromático, já que o target escalar por luminância não
   enxerga cor. O código de referência (Falcor) depende de pairwise MIS. Exigiria migrar o espacial
   da balance heuristic para pairwise.
4. **ORCA, Online Radiance Cache Acceleration.** Greenberg, EA SEED. Advances in Real-Time Rendering,
   SIGGRAPH 2026. É um radiance cache sem dependência de histórico temporal. O material publicado é
   só o PPTX do curso; vale comparar com o `FRadianceCache` (SHaRC) quando os slides forem lidos.
5. **Real-Time Level-of-Detail Rendering with ReSTIR.** Wang et al., SIGGRAPH 2026. Interessa ao
   terreno CDLOD e à folhagem densa.
6. **Real-time Path Tracing of Massive Dynamic Foliage** e **Ray Tracing Massive Amounts of Animated
   Geometry** (Gruen, Benthin, Kern, McAllister). HPG 2026, terceiro lugar empatado. São relevantes
   se a folhagem animada entrar na TLAS.

## Fontes

- RTXDI SDK, `Doc/RestirPT.md` (NVIDIA-RTX/RTXDI, setembro de 2026): descrição pública das
  constantes do escore, do duplication map e da decorrelação. O código do SDK está sob licença
  proprietária da NVIDIA e **não** foi copiado. A implementação da engine foi escrita a partir das
  equações descritas.
- Programas do HPG 2026, do SIGGRAPH 2026 (incluindo o curso Advances) e do EGSR 2026.
