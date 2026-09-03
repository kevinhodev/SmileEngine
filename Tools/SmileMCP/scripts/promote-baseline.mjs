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
if (report.passed === false && !process.argv.includes("--allow-failed")) {
  fail(
    `o relatorio esta marcado passed:false (${(report.error ?? "sem erro registrado")
      .split("\n")[0]
      .slice(0, 120)}). Use --allow-failed --note "<por que e aceitavel>" se for o caso.`,
  );
}
if (report.passed === false && !note) fail("--allow-failed exige --note explicando a aceitacao");

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
  const server = report.mcpServer;
  if (!server || !/^[0-9a-f]{64}$/.test(server.distSha256 ?? "") || server.stale !== false) {
    fail(
      "a rodada nao registrou um dist do MCP integro (hash de 64 hex e stale:false); " +
        "nao da para dizer que codigo de servidor foi executado",
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
  for (const [scene, entry] of Object.entries(report.sceneFingerprints ?? {})) {
    const files = Object.entries(entry ?? {});
    if (files.length !== 3) {
      fail(`${scene}: ${files.length} asset(s) com fingerprint, esperados 3 (.sscene/.smesh/.cameras.json)`);
    }
    for (const [file, value] of files) {
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

// --as entrava direto no path.join. `--as ../../Tools/cook-baseline` resolvia para fora de
// Docs/Baselines e sobrescrevia o arquivo de la — um publicador que aceita caminho arbitrario nao
// publica, ele escreve onde mandarem. Um unico componente, e sem os nomes que o Windows reserva.
if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name) || name === "." || name === "..") {
  fail(
    `nome de baseline invalido: '${name}'. Use um unico componente com letras, digitos, ponto, ` +
      `hifen ou sublinhado, comecando por letra ou digito.`,
  );
}
if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$)/i.test(name)) {
  fail(`'${name}' e um nome reservado pelo Windows`);
}

const destination = path.join(baselineDirectory, `${name}.json`);

// Cinto e suspensorio: mesmo com o nome validado, confere que os dois alvos caem DENTRO de
// Docs/Baselines. Uma regex sozinha e uma promessa; isto e a verificacao.
const insideBaselines = (candidate) => {
  const relative = path.relative(baselineDirectory, candidate);
  return relative !== "" && !relative.startsWith("..") && !path.isAbsolute(relative);
};

// Hash de cada PNG de referencia. Serve para IDENTIFICAR qual imagem gerou o relatorio — nao para
// verificar uma recaptura.
//
// A ideia de guardar so o hash e economizar os ~23 MiB de imagem foi testada e nao para em pe: a
// captura NAO reproduz byte a byte. Recapturando a Bistro do mesmo build, mesmo bookmark e mesmo
// N=128, os quatro slots deram hash diferente. A diferenca e pequena — 11,5% dos subpixels, delta
// medio 1,08 e maximo 20 em 255 — ou seja ruido residual de RT/denoiser, e nao outra imagem; mas
// comparacao exata nao sobrevive a isso.
//
// Consequencia pratica, e vale para quem for comparar na Fase 1: os PNGs de referencia ficam
// versionados em `Docs/Baselines/Reference/`, e a comparacao visual tem de ser por TOLERANCIA
// (delta maximo, fracao de pixels acima de um limiar), nunca por igualdade de hash.
// O promotor e dono da COPIA e do NOME das referencias. Na primeira versao ele so hasheava o PNG
// de origem e gravava o basename longo, enquanto a copia para Reference/ era um passo a parte que
// renomeava para <cena>_slot<N>.png: dois donos para a mesma decisao, e o pacote publicado ficou
// com 20 nomes que nao existiam em lugar nenhum. Nome e copia saem daqui, juntos, para nao poderem
// divergir de novo.
// Namespace POR BASELINE. Cena e slot nao bastam como identidade: quando a Fase 1 promover o seu
// proprio bistro_slot0.png, ele sobrescreveria o da Fase 0 e o hash guardado na baseline antiga
// passaria a divergir do arquivo — a referencia do "antes" desapareceria justamente na hora de
// comparar com ela.
const referenceDirectory = path.join(baselineDirectory, "Reference", name);
if (!insideBaselines(destination) || !insideBaselines(referenceDirectory)) {
  fail(`o destino sairia de Docs/Baselines ('${destination}')`);
}
const imageRows = [...(report.runs ?? []), ...(report.captures ?? [])]
  .map((entry) => ({ scene: entry.scene, slot: entry.slot, png: entry.capture?.pngPath }))
  .filter((entry) => entry.png);

if (imageRows.length > 0) {
  // Le e valida TUDO antes de escrever qualquer coisa. Com a escrita intercalada, um manifesto
  // ausente no meio deixava PNG novo publicado ao lado de manifesto velho — pacote pela metade,
  // e pior que pacote nenhum porque parece completo.
  for (const row of imageRows) {
    row.file = `${row.scene}_slot${row.slot}.png`;
    row.manifestFile = row.file.replace(/\.png$/, ".json");
    row.bytes = await readFile(row.png).catch(() => null);
    if (!row.bytes) fail(`PNG de origem nao encontrado: ${row.png}`);
    row.sha256 = createHash("sha256").update(row.bytes).digest("hex");

    // O manifesto que o CAPTURE-PROTOCOL emite ao lado do PNG declara preset, N, resolucao e o
    // estado inteiro do renderer. Sem ele a imagem e um pixel sem procedencia, entao o par e
    // OBRIGATORIO — antes isto era um aviso, e aviso nao impede publicar.
    const sourceManifest = row.png.replace(/\.png$/i, ".json");
    const manifestBytes = await readFile(sourceManifest, "utf8").catch(() => null);
    if (!manifestBytes) fail(`sem manifesto ao lado de ${path.basename(row.png)}; o par e exigido`);
    let manifest;
    try {
      manifest = JSON.parse(manifestBytes);
    } catch {
      fail(`manifesto ilegivel: ${sourceManifest}`);
    }
    // O campo `image` aponta para o nome LONGO de origem, que nao existe aqui dentro. Copiar o
    // manifesto verbatim deixava 20 referencias internas quebradas mesmo com os PNGs corretos.
    manifest.sourceImage = manifest.image;
    manifest.image = row.file;
    row.manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
    row.manifestSha256 = createHash("sha256").update(row.manifestText, "utf8").digest("hex");
  }

  // Publicacao em STAGING + troca no fim. Validar as entradas antes nao torna a escrita atomica:
  // os 40 artefatos iam para o destino um a um, e uma falha de disco ou permissao no meio deixava
  // a baseline metade nova e metade antiga — estado que nao existe em rodada nenhuma e que o
  // MANIFEST descreveria errado. Aqui o destino so e tocado quando tudo ja esta escrito.
  //
  // E o namespace e IMUTAVEL por default: uma baseline publicada e um registro, e sobrescreve-la
  // em silencio e a mesma perda que o namespace foi criado para impedir. --replace para reeditar
  // de proposito.
  const alreadyPublished = await stat(referenceDirectory).then(() => true).catch(() => false);
  if (alreadyPublished && !process.argv.includes("--replace")) {
    fail(
      `a baseline '${name}' ja tem referencias publicadas em Docs/Baselines/Reference/${name}/. ` +
        `Use outro --as, ou --replace para substitui-la de proposito.`,
    );
  }

  const staging = path.join(baselineDirectory, `.staging-${name}-${process.pid}`);
  await rm(staging, { recursive: true, force: true });
  await mkdir(staging, { recursive: true });
  try {
    for (const row of imageRows) {
      await writeFile(path.join(staging, row.file), row.bytes);
      await writeFile(path.join(staging, row.manifestFile), row.manifestText, "utf8");
    }
    // rename de diretorio nao sobrescreve destino existente no Windows: o antigo sai do caminho
    // primeiro, e so e apagado quando o novo ja esta no lugar.
    const retired = `${referenceDirectory}.retiring-${process.pid}`;
    if (alreadyPublished) await rename(referenceDirectory, retired);
    try {
      await mkdir(path.dirname(referenceDirectory), { recursive: true });
      await rename(staging, referenceDirectory);
    } catch (error) {
      if (alreadyPublished) await rename(retired, referenceDirectory).catch(() => {});
      throw error;
    }
    await rm(retired, { recursive: true, force: true });
  } finally {
    await rm(staging, { recursive: true, force: true });
  }

  report.capturedImages = imageRows.map((row) => ({
    scene: row.scene,
    slot: row.slot,
    file: `Reference/${name}/${row.file}`,
    manifest: `Reference/${name}/${row.manifestFile}`,
    sourceFile: path.basename(row.png),
    sha256: row.sha256,
    manifestSha256: row.manifestSha256,
  }));
  console.log(`[promote] ${imageRows.length} par(es) PNG+manifesto em Docs/Baselines/Reference/${name}/`);
}

await mkdir(baselineDirectory, { recursive: true });
const minified = JSON.stringify(report);
await writeFile(destination, minified, "utf8");
const digest = createHash("sha256").update(minified, "utf8").digest("hex");

// O manifesto e reconstruido a partir do que esta em disco, e nao acrescentado no fim: assim ele
// nao diverge quando um arquivo e removido ou regravado a mao.
const files = (await readdir(baselineDirectory))
  .filter((entry) => entry.endsWith(".json"))
  .sort();
const rows = [];
for (const file of files) {
  const bytes = await readFile(path.join(baselineDirectory, file), "utf8");
  const parsed = JSON.parse(bytes);
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
    // HEAD e `git status` NA HORA da rodada, mais o hash dos assets carregados — evidencia mais
    // forte, e a coluna existe para nao confundir as duas.
    provenance: parsed.gitState
      ? `HEAD ${(parsed.gitState.head ?? "").slice(0, 7)}` +
        (parsed.gitState.workingTreeClean === false ? " (arvore suja)" : "") +
        (parsed.sceneFingerprints ? " + assets" : "")
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

await writeFile(manifestPath, lines.join("\n"), "utf8");
console.log(`[promote] ${path.relative(smileRoot, destination)}`);
console.log(`[promote] sha256 ${digest}`);
console.log(`[promote] manifesto com ${rows.length} arquivo(s)`);
