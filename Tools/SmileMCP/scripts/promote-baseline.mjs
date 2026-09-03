// Promove um relatorio de baseline para `Docs/Baselines/`, que e versionado.
//
// Por que existe: os relatorios saem em `build/bin/Release/Captures/Benchmarks/`, que o git ignora.
// Num checkout limpo o arquivo citado no plano simplesmente nao existe — sobra a tabela de
// medianas no doc, e some a distribuicao (p10/p95/desvio), o regime declarado e toda a
// proveniencia auditavel. Uma baseline que nao sobrevive a um clone nao e uma baseline; e uma
// lembranca.
//
// Grava minificado (o pretty-print dobra o tamanho sem acrescentar informacao) e registra o
// SHA-256 num manifesto, para que "o arquivo que o plano cita" e "o arquivo que esta aqui" sejam
// verificavelmente o mesmo.
//
// Alem do JSON, copia os PNGs de referencia e os manifestos do protocolo para
// `Docs/Baselines/Reference/`, com nome canonico <cena>_slot<N>.
//
// Uso: node scripts/promote-baseline.mjs <relatorio.json> [--as <nome>] [--note "..."]
//      --allow-incomplete  aceita relatorio com menos runs que pontos (precisa de --note)
//      --allow-failed      aceita relatorio com passed:false        (precisa de --note)

import { createHash } from "node:crypto";
import { mkdir, readFile, readdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const smileRoot = path.resolve(scriptDirectory, "../../..");
// SMILE_BASELINE_DIR existe para o TESTE: o promotor escreve em Docs/Baselines, que e versionado,
// e um teste que exercitasse os caminhos de falha nele sujaria o repositorio. Fora do teste,
// ninguem passa isto.
const baselineDirectory = process.env.SMILE_BASELINE_DIR
  ? path.resolve(process.env.SMILE_BASELINE_DIR)
  : path.join(smileRoot, "Docs", "Baselines");
const manifestPath = path.join(baselineDirectory, "MANIFEST.md");

function fail(message) {
  console.error(`[promote] ${message}`);
  process.exit(1);
}

// SO ENOENT significa ausencia. `stat(...).catch(() => false)` transformava qualquer erro de
// filesystem — permissao negada, disco fora, handle em uso — em "nao existe", e dai o publicador
// concluia que nao havia nada para preservar: a imutabilidade era ignorada e o rename do Windows
// substituia o arquivo sem backup. Um erro de consulta tem de abortar ANTES da primeira mutacao,
// e nao virar uma resposta.
async function existe(alvo, oQueE) {
  try {
    await stat(alvo);
    return true;
  } catch (error) {
    if (error.code === "ENOENT") return false;
    fail(`nao consegui consultar ${oQueE} ('${alvo}'): ${error.code ?? error.message}`);
  }
}

// Mesmo motivo: um readdir que falha virava lista vazia, e o MANIFEST seria regravado APAGANDO as
// baselines que ele nao conseguiu enxergar.
async function listar(diretorio, oQueE) {
  try {
    return await readdir(diretorio);
  } catch (error) {
    if (error.code === "ENOENT") return [];
    fail(`nao consegui listar ${oQueE} ('${diretorio}'): ${error.code ?? error.message}`);
  }
}

function argumentValue(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : fallback;
}

const source = process.argv[2];
if (!source || source.startsWith("--")) {
  fail("uso: promote-baseline.mjs <relatorio.json> [--as <nome>] [--note \"...\"]");
}
const note = argumentValue("--note", "");
const allowIncomplete = process.argv.includes("--allow-incomplete");

const raw = await readFile(path.resolve(source), "utf8");
const report = JSON.parse(raw);

// Um relatorio sem identidade de build nao serve como "antes": nao da para dizer o que estava
// compilado. Sujo tambem nao, pela mesma razao.
if (!report.buildCommit) fail("o relatorio nao tem buildCommit; nao ha o que promover");
if (report.buildCommit.endsWith("-dirty")) fail(`build '${report.buildCommit}' saiu de arvore suja`);

// A validacao nao olhava report.passed. Um relatorio pode ser aceitavel apesar de failed — o
// perf-f781288 e, e o porque esta escrito nele —, mas isso tem de ser uma decisao DECLARADA. Sem
// esta checagem, qualquer relatorio failed cujo summary batesse com runs entrava calado, e a
// diferenca entre "aceito com justificativa" e "ninguem reparou" desaparecia.
// `!== true` e nao `=== false`: passed AUSENTE nao e o mesmo que passou, e entrava calado tanto
// pela porta principal quanto pela do --allow-failed, que so exigia --note quando o campo estava
// explicitamente false.
if (report.passed !== true) {
  if (!process.argv.includes("--allow-failed")) {
    fail(
      report.passed === false
        ? `o relatorio esta marcado passed:false (${(report.error ?? "sem erro registrado")
            .split("\n")[0]
            .slice(0, 120)}). Use --allow-failed --note "<por que e aceitavel>" se for o caso.`
        : 'o relatorio nao declara passed:true. Use --allow-failed --note "<por que e aceitavel>".',
    );
  }
  if (!note) fail("--allow-failed exige --note explicando a aceitacao");
}

// O carimbo da build so prova o estado na COMPILACAO. Uma rodada feita com --allow-dirty sai com
// passed:true e buildCommit limpo, e passaria por aqui sem nada acusar — foi exatamente o caso do
// smoke desta sessao (buildCommit c452b1c, gitState.workingTreeClean false).
//
// Relatorio que TEM gitState e julgado por ele; relatorio legado, que so tem o carimbo, precisa de
// excecao declarada — nao se promove evidencia fraca por omissao.
if (report.gitState) {
  if (report.gitState.workingTreeClean !== true) {
    fail(
      `a rodada foi feita com a arvore suja (${report.gitState.dirtyEntries?.length ?? "?"} ` +
        `entrada(s)) ou sem o git responder. Baseline exige arvore limpa NA MEDICAO.`,
    );
  }
  if (!/^[0-9a-f]{40}$/.test(report.gitState.head ?? "")) {
    fail("a rodada nao registrou um HEAD valido; nao da para dizer de que arvore ela saiu");
  }
  // passed AUSENTE era aceito, porque a checagem acima so barra `=== false`. Um relatorio sem o
  // campo entrava como se tivesse passado.
  if (report.passed !== true && !process.argv.includes("--allow-failed")) {
    fail("o relatorio nao declara passed:true; use --allow-failed --note se for aceitavel");
  }

  // O dist executado faz parte da proveniencia tanto quanto o HEAD: sem isto, um bridge obsoleto
  // continua invisivel no pacote.
  // Nao basta ter distSha256: sem sourceSha256 e builtInFlow, o hash diz QUAL dist rodou mas nao
  // que ele corresponde a estes fontes — que e justamente o furo que o mtime deixava.
  const server = report.mcpServer;
  const hex64 = (value) => /^[0-9a-f]{64}$/.test(value ?? "");
  if (
    !server ||
    !hex64(server.distSha256) ||
    !hex64(server.sourceSha256) ||
    server.builtInFlow !== true ||
    server.stale !== false ||
    !Number.isInteger(server.distFiles) || server.distFiles <= 0 ||
    !Number.isInteger(server.sourceFiles) || server.sourceFiles <= 0
  ) {
    fail(
      "a rodada nao registrou um dist do MCP integro (distSha256 e sourceSha256 de 64 hex, " +
        "builtInFlow:true, stale:false e contagens positivas); nao da para dizer que codigo de " +
        "servidor foi executado nem que ele corresponde aos fontes",
    );
  }

  // Contagem e ausencia de null nao bastavam: um objeto vazio, ou chaves de OUTRAS cenas, passavam
  // pelos dois testes. Comparar os CONJUNTOS, e exigir os tres arquivos com bytes e sha valido.
  const scenesMeasured = new Set(
    [...(report.summary ?? []), ...(report.captures ?? [])].map((entry) => entry.scene),
  );
  const scenesFingerprinted = new Set(Object.keys(report.sceneFingerprints ?? {}));
  const faltando = [...scenesMeasured].filter((scene) => !scenesFingerprinted.has(scene));
  const sobrando = [...scenesFingerprinted].filter((scene) => !scenesMeasured.has(scene));
  if (faltando.length > 0 || sobrando.length > 0) {
    fail(
      `impressoes de asset nao correspondem as cenas medidas` +
        (faltando.length ? `; sem fingerprint: ${faltando.join(", ")}` : "") +
        (sobrando.length ? `; fingerprint de cena nao medida: ${sobrando.join(", ")}` : ""),
    );
  }
  // Exigir "tres entradas" deixava passar quaisquer tres nomes. Os caminhos certos sao derivaveis
  // de report.scenes, que ja guarda o .sscene e o sidecar de cada cena — entao se compara o
  // conjunto esperado, e nao a contagem.
  const declaradas = new Map((report.scenes ?? []).map((entry) => [entry.id, entry]));
  for (const [scene, entry] of Object.entries(report.sceneFingerprints ?? {})) {
    const declarada = declaradas.get(scene);
    if (!declarada?.scene || !declarada?.sidecar) {
      fail(`${scene}: o relatorio nao declara os caminhos da cena em report.scenes`);
    }
    const esperados = [
      declarada.scene,
      declarada.scene.replace(/\.sscene$/, ".smesh"),
      declarada.sidecar,
    ].sort();
    const presentes = Object.keys(entry ?? {}).sort();
    if (JSON.stringify(esperados) !== JSON.stringify(presentes)) {
      fail(
        `${scene}: fingerprints nao sao dos arquivos esperados.\n` +
          `        esperados: ${esperados.join(", ")}\n` +
          `        presentes: ${presentes.join(", ") || "(nenhum)"}`,
      );
    }
    for (const [file, value] of Object.entries(entry)) {
      if (!value || !Number.isInteger(value.bytes) || value.bytes <= 0 ||
          !/^[0-9a-f]{64}$/.test(value.sha256 ?? "")) {
        fail(`${scene}: fingerprint invalida para ${file}`);
      }
    }
  }
} else if (!process.argv.includes("--allow-legacy-provenance")) {
  fail(
    "o relatorio nao tem gitState/sceneFingerprints (rodada anterior a essa instrumentacao). " +
      'Use --allow-legacy-provenance --note "<por que>" para promove-lo mesmo assim.',
  );
} else if (!note) {
  fail("--allow-legacy-provenance exige --note");
}


const points = report.summary?.length ?? 0;
const runs = report.runs?.length ?? 0;
const captures = report.captures?.length ?? 0;
if (points === 0 && captures === 0) fail("relatorio sem pontos");
if (points !== runs && !report.regime?.captureOnly) {
  // Exatamente o defeito que ja passou uma vez: 20 pontos no summary e 16 runs detalhados, sem os
  // tempos de GPU por passe de uma cena inteira. Recusar por default, porque o relatorio continua
  // parecendo completo para quem so olha a tabela.
  if (!allowIncomplete) {
    fail(`incompleto: ${points} pontos no summary contra ${runs} runs detalhados. ` +
         `Recapture, ou use --allow-incomplete --note "<por que>" se a lacuna for aceitavel.`);
  }
  if (!note) fail("--allow-incomplete exige --note explicando a lacuna");
}

const defaultName = `${path.basename(source, ".json").replace(/^gpu-driven-baseline-?/, "") ||
  "baseline"}-${report.buildCommit}`;
const name = argumentValue("--as", defaultName).replace(/\.json$/, "");

// Um unico componente seguro. --as entrava direto no path.join, e `--as ../../Tools/cook-baseline`
// resolvia para fora de Docs/Baselines e sobrescrevia o arquivo de la: um publicador que aceita
// caminho arbitrario nao publica, escreve onde mandarem.
const SEGMENTO_SEGURO = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
const RESERVADO_WINDOWS = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$)/i;
function exigirSegmentoSeguro(valor, oQueE) {
  if (typeof valor !== "string" || !SEGMENTO_SEGURO.test(valor) || valor === "." || valor === "..") {
    fail(
      `${oQueE} invalido: ${JSON.stringify(valor)}. Use um unico componente com letras, digitos, ` +
        `ponto, hifen ou sublinhado, comecando por letra ou digito.`,
    );
  }
  if (RESERVADO_WINDOWS.test(valor)) fail(`${oQueE} '${valor}' e um nome reservado pelo Windows`);
  // Ponto ou espaco no fim passavam pela regex. As APIs Win32 e varias ferramentas (git incluido)
  // NORMALIZAM esses nomes, entao 'fase0.' e 'fase0' viram o mesmo caminho conforme quem escreve —
  // e este diretorio e versionado, entao um clone no Windows nem consegue materializar o arquivo.
  if (/[. ]$/.test(valor)) {
    fail(`${oQueE} '${valor}' termina em ponto ou espaco, que o Windows e o git normalizam`);
  }
}
exigirSegmentoSeguro(name, "nome de baseline");

const destination = path.join(baselineDirectory, `${name}.json`);
const referenceDirectory = path.join(baselineDirectory, "Reference", name);

// Cinto e suspensorio: a regex e a promessa, isto e a verificacao. Vale para TODO caminho que este
// script escreve, e nao so para os derivados de --as.
function dentroDe(raiz, candidato) {
  const relativo = path.relative(raiz, candidato);
  return relativo !== "" && !relativo.startsWith("..") && !path.isAbsolute(relativo);
}
if (!dentroDe(baselineDirectory, destination) || !dentroDe(baselineDirectory, referenceDirectory)) {
  fail(`o destino sairia de ${baselineDirectory} ('${destination}')`);
}

// Ponto de falha injetavel, so para o teste. Sem ele nao ha como provar que a publicacao volta
// atras no meio: remover um PNG antes de comecar aborta na validacao e nunca chega a escrever, e
// um teste que so exercita a validacao nao prova transacao nenhuma.
const failpoint = process.env.SMILE_PROMOTE_FAILPOINT ?? "";
function talvezFalhar(ponto) {
  if (failpoint === ponto) throw new Error(`falha injetada no ponto '${ponto}'`);
}

// ---- Montagem em memoria -----------------------------------------------------------------------
// Nada e escrito antes de tudo estar pronto e conferido.

const imageRows = [...(report.runs ?? []), ...(report.captures ?? [])]
  .map((entry) => ({ scene: entry.scene, slot: entry.slot, png: entry.capture?.pngPath }))
  .filter((entry) => entry.png);

for (const row of imageRows) {
  // `scene` e `slot` vem do RELATORIO, e nao da linha de comando: proteger so o --as deixava a
  // porta aberta do outro lado. Um scene "../../vitima" fazia o caminho final sair do staging e,
  // no Windows, ate de Docs/Baselines.
  exigirSegmentoSeguro(row.scene, "nome de cena no relatorio");
  if (!Number.isInteger(row.slot) || row.slot < 0 || row.slot > 99) {
    fail(`slot invalido no relatorio: ${JSON.stringify(row.slot)}`);
  }

  row.file = `${row.scene}_slot${row.slot}.png`;
  row.manifestFile = `${row.scene}_slot${row.slot}.json`;

  row.bytes = await readFile(row.png).catch(() => null);
  if (!row.bytes) fail(`PNG de origem nao encontrado: ${row.png}`);
  row.sha256 = createHash("sha256").update(row.bytes).digest("hex");

  // O manifesto que o CAPTURE-PROTOCOL emite ao lado do PNG declara preset, N, resolucao e o
  // estado do renderer. Sem ele a imagem e um pixel sem procedencia, entao o par e OBRIGATORIO —
  // antes isto era so um aviso, e aviso nao impede publicar.
  const sourceManifest = row.png.replace(/\.png$/i, ".json");
  const manifestBytes = await readFile(sourceManifest, "utf8").catch(() => null);
  if (!manifestBytes) fail(`sem manifesto ao lado de ${path.basename(row.png)}; o par e exigido`);
  let manifest;
  try {
    manifest = JSON.parse(manifestBytes);
  } catch {
    fail(`manifesto ilegivel: ${sourceManifest}`);
  }
  // Cruzamento SEMANTICO: o manifesto declara de que cena e de que slot ele e, e ate aqui isso
  // nunca era conferido — so a existencia do arquivo. Uma captura da Emerald/slot 1 podia ser
  // publicada como Bistro/slot 0 com o pacote inteiro coerente e os hashes todos batendo, porque
  // nada ligava o CONTEUDO ao ponto publicado. O nome esperado sai do report.scenes.
  const declarada = (report.scenes ?? []).find((entry) => entry.id === row.scene);
  if (declarada?.scene) {
    const esperada = path.basename(declarada.scene, ".sscene");
    if (manifest.scene !== esperada) {
      fail(
        `${row.file}: o manifesto e da cena '${manifest.scene}', esperada '${esperada}' ` +
          `(${path.basename(row.png)})`,
      );
    }
  }
  if (manifest.bookmarkSlot !== row.slot) {
    fail(
      `${row.file}: o manifesto e do slot ${manifest.bookmarkSlot}, esperado ${row.slot} ` +
        `(${path.basename(row.png)})`,
    );
  }

  // O campo `image` aponta para o nome LONGO de origem, que nao existe aqui dentro. Copiar cru
  // deixava as referencias internas quebradas mesmo com os PNGs corretos.
  manifest.sourceImage = manifest.image;
  manifest.image = row.file;
  row.manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
  row.manifestSha256 = createHash("sha256").update(row.manifestText, "utf8").digest("hex");
}

const duplicadas = imageRows
  .map((row) => row.file)
  .filter((file, index, todas) => todas.indexOf(file) !== index);
if (duplicadas.length > 0) fail(`cena/slot repetidos no relatorio: ${[...new Set(duplicadas)].join(", ")}`);

if (imageRows.length > 0) {
  report.capturedImages = imageRows.map((row) => ({
    scene: row.scene,
    slot: row.slot,
    file: `Reference/${name}/${row.file}`,
    manifest: `Reference/${name}/${row.manifestFile}`,
    sourceFile: path.basename(row.png),
    sha256: row.sha256,
    manifestSha256: row.manifestSha256,
  }));
}

const minified = JSON.stringify(report);
const digest = createHash("sha256").update(minified, "utf8").digest("hex");

// ---- Imutabilidade do pacote -------------------------------------------------------------------
// Decidida pelo PACOTE INTEIRO, e nao so pelo diretorio de referencias. Antes, um relatorio sem
// imagens — uma baseline so de performance — pulava o bloco de referencias e sobrescrevia o
// <name>.json calado; e o mesmo acontecia quando o JSON existia mas o diretorio nao.
await mkdir(baselineDirectory, { recursive: true });

// Identidade case-INSENSITIVE. O filesystem do Windows nao distingue 'fase0' de 'FASE0' (medido:
// existsSync do segundo acha o primeiro), entao publicar 'FASE0' por cima de 'fase0' escreveria no
// mesmo lugar passando pela checagem de existencia — e o Map que monta o manifesto trataria os
// dois como arquivos diferentes, produzindo uma tabela que descreve algo que nao esta em disco.
const mesmoNome = (a, b) =>
  process.platform === "win32" ? a.toLowerCase() === b.toLowerCase() : a === b;

const jaNaRaiz = (await listar(baselineDirectory, "o diretorio de baselines")).filter((entry) =>
  mesmoNome(entry, `${name}.json`),
);
const jaEmRefs = (
  await listar(path.join(baselineDirectory, "Reference"), "o diretorio de referencias")
).filter((entry) => mesmoNome(entry, name));

if ((jaNaRaiz.length > 0 || jaEmRefs.length > 0) && !process.argv.includes("--replace")) {
  fail(
    `a baseline '${name}' ja esta publicada` +
      (jaNaRaiz.length ? ` (${jaNaRaiz.join(", ")})` : "") +
      (jaEmRefs.length ? ` (Reference/${jaEmRefs.join(", ")}/)` : "") +
      `. Use outro --as, ou --replace para substitui-la de proposito.`,
  );
}
// Com --replace, publicar sob outra caixa deixaria o antigo no lugar e o novo apontando para ele:
// dois nomes, um diretorio. Exigir a caixa exata evita o pacote partido.
const divergentes = [...jaNaRaiz.filter((e) => e !== `${name}.json`), ...jaEmRefs.filter((e) => e !== name)];
if (divergentes.length > 0) {
  fail(
    `ja existe '${divergentes.join(", ")}', que no Windows e o MESMO caminho de '${name}'. ` +
      `Republique com a grafia existente, ou renomeie a baseline antiga primeiro.`,
  );
}

// ---- Manifesto, calculado ANTES da troca --------------------------------------------------------
// Ele e derivado do que estara em disco depois desta publicacao: os relatorios ja publicados, com
// o nosso substituindo/entrando. Calcular aqui permite que ele entre na MESMA transacao — gravado
// depois, uma falha na escrita dele deixaria referencias novas descritas por um manifesto velho.
const publicados = new Map();
for (const file of (await listar(baselineDirectory, "o diretorio de baselines"))
  .filter((entry) => entry.endsWith(".json"))
  .sort()) {
  publicados.set(file, await readFile(path.join(baselineDirectory, file), "utf8"));
}
publicados.set(`${name}.json`, minified);

const rows = [];
for (const file of [...publicados.keys()].sort()) {
  const bytes = publicados.get(file);
  let parsed;
  try {
    parsed = JSON.parse(bytes);
  } catch {
    fail(`ja existe um ${file} ilegivel em ${baselineDirectory}; resolva antes de publicar`);
  }
  rows.push({
    file,
    sha256: createHash("sha256").update(bytes, "utf8").digest("hex"),
    build: parsed.buildCommit ?? "-",
    points: parsed.summary?.length ?? 0,
    runs: parsed.runs?.length ?? 0,
    captures: parsed.captures?.length ?? 0,
    // `??` nao serve aqui: um summary VAZIO e truthy e nao cai no fallback, e o relatorio de
    // so-captura ficava sem cenas no manifesto.
    scenes: [...new Set([...(parsed.summary ?? []), ...(parsed.captures ?? [])].map((s) => s.scene))]
      .join(" "),
    when: (parsed.startedAt ?? "").slice(0, 10),
    // Relatorios anteriores a 2026-09-03 so tem o carimbo assado na build. Os novos tambem medem
    // HEAD e `git status` NA HORA da rodada, mais o hash dos assets e do dist do MCP — evidencia
    // mais forte, e a coluna existe para nao confundir as duas.
    provenance: parsed.gitState
      ? `HEAD ${(parsed.gitState.head ?? "").slice(0, 7)}` +
        (parsed.gitState.workingTreeClean === false ? " (arvore suja)" : "") +
        (parsed.sceneFingerprints ? " + assets" : "") +
        (parsed.mcpServer?.builtInFlow ? " + mcp" : "")
      : "so carimbo",
    note: file === `${name}.json` ? note : undefined,
  });
}

const previous = await readFile(manifestPath, "utf8").catch(() => "");
const preservedNotes = new Map(
  [...previous.matchAll(/^- `([^`]+)` — (.+)$/gm)].map((match) => [match[1], match[2]]),
);

const lines = [
  "# Baselines versionadas",
  "",
  "Relatorios de medicao promovidos de `build/bin/Release/Captures/Benchmarks/`, que o git ignora.",
  "Ficam aqui porque uma baseline que nao sobrevive a um clone nao e uma baseline. Sao gravados",
  "minificados; o SHA-256 e do arquivo como esta neste diretorio.",
  "",
  "Gerado por `Tools/SmileMCP/scripts/promote-baseline.mjs` — nao editar a tabela a mao.",
  "",
  "| arquivo | build | pontos | runs | capturas | cenas | proveniencia | data |",
  "|---|---|--:|--:|--:|---|---|---|",
];
for (const row of rows) {
  lines.push(
    `| \`${row.file}\` | \`${row.build}\` | ${row.points} | ${row.runs} | ${row.captures} | ` +
      `${row.scenes} | ${row.provenance} | ${row.when} |`,
  );
}
// Tabela e nao lista com hard-break: o "  \n" do markdown deixa espaco no fim da linha, que o
// `git diff --check` acusa a cada regravacao do manifesto.
lines.push("", "## SHA-256", "", "| arquivo | sha256 |", "|---|---|");
for (const row of rows) lines.push(`| \`${row.file}\` | \`${row.sha256}\` |`);

const notes = rows
  .map((row) => [row.file, row.note || preservedNotes.get(row.file)])
  .filter(([, value]) => value);
if (notes.length > 0) {
  lines.push("", "## Notas", "");
  for (const [file, value] of notes) lines.push(`- \`${file}\` — ${value}`);
}
lines.push("");
const manifestText = lines.join("\n");

// ---- Publicacao ---------------------------------------------------------------------------------
// Tres artefatos mudam juntos: as referencias, o <name>.json e o MANIFEST.md. Antes so as imagens
// eram trocadas por staging, e os outros dois eram gravados direto depois — uma falha ali deixava
// referencias novas com relatorio velho, ou manifesto descrevendo o que nao esta em disco.
//
// Aqui tudo e escrito no staging primeiro; a troca no destino acontece so no fim, e o que ja tinha
// sido movido volta se algum passo falhar.
const staging = path.join(baselineDirectory, `.staging-${name}-${process.pid}`);
const stagingRefs = path.join(staging, "refs");
const retiredSuffix = `.retiring-${process.pid}`;
const desfazer = [];
let publicado = false;
const backupsNaoRemovidos = [];

try {
  await rm(staging, { recursive: true, force: true });
  await mkdir(stagingRefs, { recursive: true });

  for (const row of imageRows) {
    const png = path.join(stagingRefs, row.file);
    const man = path.join(stagingRefs, row.manifestFile);
    // Ultima linha de defesa: mesmo com cena validada, nenhum caminho pode sair do staging.
    if (!dentroDe(stagingRefs, png) || !dentroDe(stagingRefs, man)) {
      fail(`o caminho da referencia sairia do staging ('${row.file}')`);
    }
    await writeFile(png, row.bytes);
    await writeFile(man, row.manifestText, "utf8");
  }
  talvezFalhar("apos-escrever-referencias");

  await writeFile(path.join(staging, "report.json"), minified, "utf8");
  await writeFile(path.join(staging, "MANIFEST.md"), manifestText, "utf8");
  talvezFalhar("apos-escrever-relatorio");

  // Troca. rename de diretorio nao sobrescreve destino existente no Windows, entao o antigo sai do
  // caminho primeiro e so e apagado quando o novo ja esta no lugar.
  const trocar = async (origem, destino) => {
    // Aqui o engano custa caro: concluir "nao existia" por causa de um erro de consulta faz o
    // rename do Windows substituir o destino SEM backup, e o rollback nao teria o que restaurar.
    const existia = await existe(destino, "o destino da troca");
    if (existia) {
      await rename(destino, `${destino}${retiredSuffix}`);
      desfazer.push(async () => rename(`${destino}${retiredSuffix}`, destino));
    }
    await rename(origem, destino);
    desfazer.push(async () => rename(destino, origem));
    return existia;
  };

  // O Reference/ precisa existir ANTES do rename: na primeira publicacao ele nao existe, e o
  // rename falha com ENOENT.
  if (imageRows.length > 0) await mkdir(path.dirname(referenceDirectory), { recursive: true });
  const refsExistiam = imageRows.length > 0 ? await trocar(stagingRefs, referenceDirectory) : false;
  talvezFalhar("apos-trocar-referencias");

  const jsonExistia = await trocar(path.join(staging, "report.json"), destination);
  talvezFalhar("apos-trocar-relatorio");

  const manifestoExistia = await trocar(path.join(staging, "MANIFEST.md"), manifestPath);
  talvezFalhar("apos-trocar-manifesto");

  // COMMIT. Daqui em diante o pacote novo esta ativo e nao ha mais volta.
  desfazer.length = 0;
  publicado = true;

  // A limpeza dos backups acontece DEPOIS do commit, e por isso fica fora da transacao. Antes ela
  // era feita aqui dentro: um rm que falhasse caia no catch e o script dizia "nada foi publicado",
  // com o pacote novo ja no ar — a mensagem mais enganosa possivel, porque manda o operador
  // procurar um problema que nao existe e ignorar o que existe.
  talvezFalhar("apos-commit");
  for (const [alvo, existia] of [
    [referenceDirectory, refsExistiam],
    [destination, jsonExistia],
    [manifestPath, manifestoExistia],
  ]) {
    if (!existia) continue;
    const backup = `${alvo}${retiredSuffix}`;
    await rm(backup, { recursive: true, force: true }).catch((error) =>
      backupsNaoRemovidos.push(`${backup} (${error.code ?? error.message})`),
    );
  }
} catch (error) {
  if (publicado) {
    // Falha DEPOIS do commit nao e rollback. O pacote esta publicado; o que sobrou e limpeza.
    console.error(
      `[promote] publicado, mas a limpeza dos backups falhou: ${error.message}\n` +
        `[promote] remova a mao o que sobrou de '*${retiredSuffix}' em ${baselineDirectory}`,
    );
    process.exit(2);
  }
  // Desfaz na ordem inversa. Falha ao restaurar NAO e engolida: sem isto, o antigo podia ficar so
  // sob o nome de recuperacao e ninguem saberia onde procurar.
  const naoRestaurados = [];
  for (const passo of desfazer.reverse()) {
    await passo().catch((restoreError) => naoRestaurados.push(String(restoreError)));
  }
  await rm(staging, { recursive: true, force: true }).catch(() => {});
  if (naoRestaurados.length > 0) {
    fail(
      `${error.message}\n[promote] E O ROLLBACK FALHOU: ${naoRestaurados.join("; ")}\n` +
        `[promote] o conteudo anterior pode estar sob '*${retiredSuffix}' em ${baselineDirectory}`,
    );
  }
  fail(`${error.message} — nada foi publicado`);
} finally {
  await rm(staging, { recursive: true, force: true }).catch(() => {});
}

if (imageRows.length > 0) {
  console.log(`[promote] ${imageRows.length} par(es) PNG+manifesto em Reference/${name}/`);
}
console.log(`[promote] ${path.relative(smileRoot, destination)}`);
console.log(`[promote] sha256 ${digest}`);
console.log(`[promote] manifesto com ${rows.length} arquivo(s)`);
