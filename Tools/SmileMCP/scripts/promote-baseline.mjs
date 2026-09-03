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
import { mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const smileRoot = path.resolve(scriptDirectory, "../../..");
const baselineDirectory = path.join(smileRoot, "Docs", "Baselines");
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
const destination = path.join(baselineDirectory, `${name}.json`);

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
const referenceDirectory = path.join(baselineDirectory, "Reference");
const imageRows = [...(report.runs ?? []), ...(report.captures ?? [])]
  .map((entry) => ({ scene: entry.scene, slot: entry.slot, png: entry.capture?.pngPath }))
  .filter((entry) => entry.png);

if (imageRows.length > 0) {
  await mkdir(referenceDirectory, { recursive: true });
  for (const row of imageRows) {
    // Cena e slot SAO a identidade da referencia. O nome longo original carrega knobs e hora, que
    // ja estao no manifesto do proprio protocolo, copiado ao lado.
    row.file = `${row.scene}_slot${row.slot}.png`;
    const bytes = await readFile(row.png).catch(() => null);
    if (!bytes) {
      row.sha256 = null;
      continue;
    }
    row.sha256 = createHash("sha256").update(bytes).digest("hex");
    await writeFile(path.join(referenceDirectory, row.file), bytes);
    // O manifesto que o CAPTURE-PROTOCOL emite ao lado do PNG: e o que declara preset, N,
    // resolucao e estado. Sem ele a imagem e um pixel sem procedencia.
    const sidecar = row.png.replace(/\.png$/i, ".json");
    const sidecarBytes = await readFile(sidecar).catch(() => null);
    if (sidecarBytes) {
      await writeFile(path.join(referenceDirectory, row.file.replace(/\.png$/, ".json")),
                      sidecarBytes);
    } else {
      console.warn(`[promote] sem manifesto ao lado de ${path.basename(row.png)}`);
    }
  }
  report.capturedImages = imageRows.map((row) => ({
    scene: row.scene,
    slot: row.slot,
    file: `Reference/${row.file}`,
    sourceFile: path.basename(row.png),
    sha256: row.sha256,
  }));
  const missing = imageRows.filter((row) => !row.sha256).length;
  if (missing > 0) fail(`${missing} PNG(s) de origem nao encontrados; o pacote sairia quebrado`);
  console.log(`[promote] ${imageRows.length} referencia(s) copiadas para Docs/Baselines/Reference/`);
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
