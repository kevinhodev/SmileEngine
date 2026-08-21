# SmileMCP

Servidor MCP local da SmileEngine. O transporte inicial e STDIO: o Codex inicia o processo,
descobre as ferramentas e recebe resultados estruturados sem acoplar o protocolo ao runtime C++.

## Ferramentas

Ferramentas disponiveis:

- `smile_project_info`: versao, componentes, builds e executaveis encontrados.
- `smile_list_files`: navegacao limitada pela arvore do projeto.
- `smile_read_file`: leitura paginada de arquivos texto.
- `smile_find_text`: pesquisa no codigo com `rg`.
- `smile_get_latest_logs`: cauda dos logs persistentes do editor.
- `smile_build`: build de um alvo CMake.
- `smile_compile_shaders`: atalho para o alvo `Shaders`.
- `smile_cook_scene`: recozinha um FBX e valida os cabecalhos `.smesh`/`.sscene` gerados.
- `smile_editor_status`: consulta PID, executavel, commit, prontidao, cena e captura do editor vivo.
- `smile_camera_get`: le a pose efetiva da camera do viewport.
- `smile_camera_set`: define uma pose absoluta e devolve o readback; aplica camera cut por
  default ou preserva continuidade temporal com `cameraCut: false` para percursos de benchmark.
- `smile_gi_status`: le politica pedida/efetiva e telemetria do DDGI, incluindo grade, sondas,
  cascatas, espacamento, scroll toroidal em celulas, idade/contagem das cascatas atualizadas,
  serial do ultimo full forcado e os limites configurados dos raios adaptativos. A distribuicao
  efetiva por sonda permanece GPU-only para nao introduzir readback e stall na regua.
- `smile_gi_configure`: altera somente os eixos de GI informados: volume, primary/fallback,
  contagem de cascatas, update intercalado, raios adaptativos e histerese adaptativa.
- `smile_profile_configure`: fixa o regime de render, a hora (`10:00` por default ou
  `timeOfDayHours`) e opcionalmente a camera do teste. O throttle de segundo plano fica desligado
  por default para o foco de outra janela nao contaminar a regua; `backgroundThrottle: true`
  restaura o comportamento interativo.
- `smile_profile_gpu`: amostra timestamps brutos e a EMA do Mini Profiler, com percentis e VRAM.
- `smile_close_editor`: encerra o editor pelo bridge, esperando o shutdown da render thread.
- `smile_run_editor`: inicia um `SmileEditor.exe`, aceita uma `.sscene` tipada e aguarda o
  renderer ficar pronto.
- `smile_capture_frame`: captura o viewport do editor aberto e retorna PNG, manifesto e,
  opcionalmente, a imagem embutida na resposta MCP.

Os acessos a arquivos aceitam somente caminhos relativos que permanecem dentro da raiz da
SmileEngine. Processos sao iniciados sem shell, e nomes de alvo CMake sao validados.

## Fluxo de validacao

O ciclo completo pode ser feito sem voltar ao terminal:

1. `smile_build` para `SmileCooker`, `Shaders` e `SmileEditor`, conforme o escopo da mudanca.
2. `smile_cook_scene` com o caminho relativo do FBX.
3. `smile_run_editor` com `scenePath`; por default ele espera ate 90 s pelo renderer e confirma
   que a cena pedida, nao apenas algum editor, ficou pronta.
4. `smile_capture_frame` para publicar PNG e manifesto.

`smile_run_editor` detecta uma instancia ja conectada para nao abrir dois editores apontando para
a mesma named pipe. Se a instancia existente estiver com outra cena, a ferramenta falha alto em
vez de capturar silenciosamente o viewport errado.

### Matriz de politica e scrolling do DDGI

Para uma matriz controlada, use `smile_profile_configure` como baseline, altere apenas o eixo do
caso com `smile_gi_configure` e confirme o resultado em `smile_gi_status` antes de capturar. O
status publica os valores `requested` e `effective` separadamente; uma degradacao para DDGI,
Black ou Off fica visivel em vez de ser confundida com a selecao pedida.

Para validar scrolling, leia a pose com `smile_camera_get`, avance a posicao em pequenos passos
absolutos com `smile_camera_set` e espere `frameIndex` avancar em `smile_gi_status`. Compare
`gridMin` e `scrollCells` de cada cascata entre os passos. A pose e limitada a valores finitos e
o editor recusa movimento durante captura deterministica. Mudancas de politica GI continuam
permitidas durante a captura de proposito: esse e o caminho usado para provar que uma troca de
estimador cancela a sessao e invalida os historicos no frame seguinte.

Para repetir automaticamente a validacao completa na Bistro exterior:

```powershell
cd D:\Engines\SmileEngine\Tools\SmileMCP
npm run validate:gi
```

O harness abre `Assets/Scenes/Bistro/BistroExterior.sscene` se necessario, valida o scrolling de
duas cascatas, captura a matriz de cinco politicas com o preset cientifico em `N=128`, prova o
cancelamento por mutacao concorrente e audita o log persistente. O resultado canonico e um JSON
em `build/bin/Release/Captures/Validation/`, junto dos caminhos dos PNGs e manifestos usados.

Para medir o custo vivo, incluindo o ciclo `1 -> 2 -> 1 -> 2`, scrolling sem camera cuts e a
matriz de politica:

```powershell
npm run benchmark:gi
```

Cada braco usa `gameplay_rr`, 15 s de aquecimento e 60 snapshots. O profile desliga o throttle
de segundo plano, exige o log do mesmo PID e grava o JSON em
`build/bin/Release/Captures/Benchmarks/`. Se somente a publicacao final falhar depois das nove
medidas, `npm run benchmark:gi -- --resume <relatorio-parcial.json>` retoma sem repetir a GPU.

Para isolar o toggle de raios adaptativos com duas cascatas e politica DDGI fixa:

```powershell
npm run benchmark:gi:adaptive
```

O harness repete `OFF -> ON -> OFF -> ON` com camera parada e depois em movimento continuo.
Cada braco confirma pelo MCP o toggle efetivo e os limites 16-64; a distribuicao por sonda
permanece GPU-only para o benchmark nao introduzir readback e stall nos timestamps.

Para decidir o default por imagem, `npm run validate:gi:adaptive-visual` captura dois pares
alternados `full64/adaptive` no preset cientifico `N=128`. Os manifestos registram a contagem de
cascatas, o toggle e os limites de raios, de modo que cada PNG permanece autoexplicativo fora do
relatorio do harness.

O gate canônico de 19/08/2026 aprovou o candidato 16-64 (57,91 dB, SSIM 0,99953 contra full-64)
e `AdaptiveRays` passou a ser ON por default; OFF continua disponível como controle full-64.

Para validar o scheduler intercalado de duas cascatas (fina todo update, grossa a cada dois):

```powershell
npm run validate:gi:interleaved
npm run analyze:gi:interleaved -- <relatorio.json>
```

O harness repete `full -> interleaved -> full -> interleaved`, exige os scopes `DDGI (fine)` e
`DDGI (fine+coarse)`, prova idade maxima 1 da grossa, scrolling axial/diagonal/vertical e full
forcado em teleporte. Depois captura o mesmo ciclo no preset cientifico `N=128`; o analisador
compara repeticoes, pares e medias e publica montagem e diffs 8x.

O A/B de 21/08/2026 mediu 22,19% de economia no passe DDGI com compensacao temporal da grossa,
SSIM 0,99974 e somente 0,0037% dos pixels acima de cinco niveis de diferenca. O gate numerico
mais estrito sinalizou viés medio de luminancia 0,116 contra teto 0,10; a revisao perceptual nao
encontrou diferenca visivel e aceitou o candidato. `cascadeCount = 2` e `interleavedUpdates = true`
passaram a ser os defaults; uma cascata continua disponivel como modo de menor custo.

## Requisitos

- Node.js 20 ou mais recente.
- `rg` (ripgrep) no `PATH` para `smile_find_text`.
- CMake no `PATH` para as ferramentas de build.
- Uma arvore de build ja configurada em `build/`.
- Uma build recente do `SmileEditor` para as ferramentas que usam a named pipe local.

## Preparacao

No PowerShell:

```powershell
cd D:\Engines\SmileEngine\Tools\SmileMCP
npm install
npm run build
npm run smoke
```

O servidor encontra a raiz subindo a partir do diretorio atual. Para eliminar ambiguidade, defina
`SMILE_ROOT`; `SMILE_LOG_DIR` pode sobrescrever o diretorio dos logs persistentes.
`SMILE_MCP_PIPE` pode trocar o nome da named pipe nos dois processos; o default e
`SmileEngine-MCP-v1`.

## Registro no Codex

Copie a secao de [`mcp-config.example.toml`](mcp-config.example.toml) para o `.codex/config.toml`
do projeto ou adicione pela interface de MCP servers. Depois reinicie o cliente local para que o
novo processo seja descoberto.

O exemplo usa `default_tools_approval_mode = "writes"`: consultas read-only podem fluir, enquanto
build e execucao continuam sujeitos a aprovacao do cliente.

## Bridge do editor

O MCP opera do lado de fora do processo. Um bridge local no editor usa protocolo versionado e
mensagens JSON para expor estado vivo sem incorporar o SDK MCP ao C++:

```text
Codex <-> SmileMCP (STDIO) <-> bridge local <-> SmileEditor/Renderer
```

Os comandos vivos atuais cobrem status, camera, GI/DDGI, captura, configuracao/snapshot de
profiling e shutdown.
`McpBridge` fica restrito a validar e traduzir o protocolo; acesso sincronizado ao renderer,
aplicacao dos presets e snapshots tipados moram no `RenderSettingsController`. O mesmo controlador
notifica os bridges QML depois de uma mutacao externa, evitando que a engine mude pela pipe e a UI
continue exibindo valores antigos.

Entidades e render graph ainda sao extensoes futuras. Novas operacoes mutaveis e scripting devem
entrar somente com allowlist, timeout e identificador de sessao. `smile_capture_frame` mantem a
named pipe aberta durante o aquecimento e responde apenas depois que PNG + manifesto foram
publicados.
