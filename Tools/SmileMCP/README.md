# SmileMCP

Servidor MCP local da SmileEngine. O transporte inicial e STDIO: o Codex inicia o processo,
descobre as ferramentas e recebe resultados estruturados sem acoplar o protocolo ao runtime C++.

## Estado do MVP

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
- `smile_profile_configure`: fixa o regime de render, a hora e opcionalmente a camera do teste.
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

O MCP atual opera do lado de fora do processo. A extensao planejada e um bridge local no editor,
com protocolo versionado e mensagens JSON, para expor estado vivo sem incorporar o SDK MCP ao C++:

```text
Codex <-> SmileMCP (STDIO) <-> bridge local <-> SmileEditor/Renderer
```

O bridge comeca read-only para estado vivo: `smile_editor_status` expoe conexao, PID, prontidao,
cena e captura em andamento. Entidades, render graph e profiling ainda sao extensoes futuras;
operacoes mutaveis e scripting devem entrar somente com allowlist, timeout e identificador de
sessao.

O primeiro comando vivo ja esta implementado: `smile_capture_frame` conversa com o `McpBridge`
do editor por JSON delimitado por linha. A named pipe aceita somente o usuario local, permanece
aberta durante o aquecimento e responde apenas depois que PNG + manifesto foram publicados.
