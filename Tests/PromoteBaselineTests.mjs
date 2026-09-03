// Testes do publicador de baselines (Tools/SmileMCP/scripts/promote-baseline.mjs).
//
// Existe porque o promotor nunca teve teste nenhum: os 48 do ctest exercitam cooker, loader,
// bookmarks e shaders, e nenhum deles executa este script. Cada defeito dele foi encontrado por
// revisao humana lendo o codigo — caminho que nao escala e que ja deixou passar, em sequencia,
// referencias quebradas, sobrescrita silenciosa e escape de diretorio.
//
// O promotor escreve em `Docs/Baselines/` do repositorio, que e versionado. Para nao encostar
// nele, cada caso roda com SMILE_BASELINE_DIR apontando para um diretorio temporario proprio.

import { createHash } from "node:crypto";
import { execFile } from "node:child_process";
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile, chmod } from "node:fs/promises";
import { existsSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..");
const promoter = path.join(repoRoot, "Tools", "SmileMCP", "scripts", "promote-baseline.mjs");

let failures = 0;
function check(condition, what, detail = "") {
  if (condition) return;
  console.error(`[FALHA] ${what}${detail ? `\n        ${detail}` : ""}`);
  failures += 1;
}

function run(args, baselineDirectory) {
  return new Promise((resolve) => {
    execFile(
      process.execPath,
      [promoter, ...args],
      { cwd: repoRoot, env: { ...process.env, SMILE_BASELINE_DIR: baselineDirectory } },
      (error, stdout, stderr) =>
        resolve({ code: error?.code ?? 0, stdout: String(stdout), stderr: String(stderr) }),
    );
  });
}

const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");

// Um PNG minimo de verdade (1x1). Bytes reais importam: o promotor hasheia o conteudo.
const pngBytes = Buffer.from(
  "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753" +
    "de0000000c4944415408d763f8ffff3f0005fe02fea735c0000000004945" +
    "4e44ae426082",
  "hex",
);

// Relatorio sintetico com proveniencia FORTE, no formato que a regua emite hoje.
async function makeReport(capturesDirectory, options = {}) {
  const scenes = options.scenes ?? ["cena_a"];
  const slots = options.slots ?? [0, 1];
  const captures = [];
  for (const scene of scenes) {
    for (const slot of slots) {
      const stem = `${scene}_slot${slot}_longo_20260101-000000`;
      const png = path.join(capturesDirectory, `${stem}.png`);
      await writeFile(png, pngBytes);
      if (options.omitManifestFor !== `${scene}_${slot}`) {
        await writeFile(
          path.join(capturesDirectory, `${stem}.json`),
          JSON.stringify({ schema: 1, image: `${stem}.png`, scene, bookmarkSlot: slot }, null, 2),
          "utf8",
        );
      }
      captures.push({ scene, slot, capture: { pngPath: png } });
    }
  }
  const fingerprint = () => ({
    bytes: 10,
    sha256: sha(Buffer.from("x")),
  });
  return {
    schema: 1,
    passed: true,
    startedAt: "2026-01-01T00:00:00.000Z",
    buildCommit: "abc1234",
    regime: { captureOnly: true },
    gitState: { head: "a".repeat(40), workingTreeClean: true, dirtyEntries: [] },
    mcpServer: { distSha256: "b".repeat(64), sourceSha256: "c".repeat(64), stale: false },
    sceneFingerprints: Object.fromEntries(
      scenes.map((scene) => [
        scene,
        {
          [`${scene}.sscene`]: fingerprint(),
          [`${scene}.smesh`]: fingerprint(),
          [`${scene}.cameras.json`]: fingerprint(),
        },
      ]),
    ),
    summary: [],
    runs: [],
    captures,
    ...options.overrides,
  };
}

async function withSandbox(body) {
  const root = await mkdtemp(path.join(os.tmpdir(), "smile-promote-"));
  const baselines = path.join(root, "Baselines");
  const captures = path.join(root, "Captures");
  await mkdir(baselines, { recursive: true });
  await mkdir(captures, { recursive: true });
  try {
    await body({ root, baselines, captures });
  } finally {
    await rm(root, { recursive: true, force: true }).catch(() => {});
  }
}

async function writeReport(directory, report, nome = "relatorio.json") {
  const file = path.join(directory, nome);
  await writeFile(file, JSON.stringify(report), "utf8");
  return file;
}

// ---------------------------------------------------------------------------------------------
// 1. Escape de diretorio pelo --as.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  const fora = path.join(root, "vitima.json");
  await writeFile(fora, "NAO PODE SER TOCADO", "utf8");

  for (const nome of ["../vitima", "..\\vitima", "sub/dir", "..", ".", "", "con"]) {
    const result = await run([file, "--as", nome], baselines);
    check(result.code !== 0, `--as '${nome}' tem de ser recusado`, result.stdout.trim());
  }
  check(
    (await readFile(fora, "utf8")) === "NAO PODE SER TOCADO",
    "nenhum --as malicioso escreveu fora do diretorio de baselines",
  );
});

// ---------------------------------------------------------------------------------------------
// 2. Namespace imutavel: nao sobrescreve baseline ja publicada sem --replace.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  const primeira = await run([file, "--as", "fase0"], baselines);
  check(primeira.code === 0, "primeira publicacao", primeira.stderr.trim());

  const png = path.join(baselines, "Reference", "fase0", "cena_a_slot0.png");
  const hashOriginal = sha(await readFile(png));

  // Outra rodada, MESMA cena/slot, mesmo nome de baseline: e a colisao que apagaria o "antes".
  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const file2 = await writeReport(
    root,
    await makeReport(captures2, { overrides: { buildCommit: "def5678" } }),
    "relatorio2.json",
  );
  const segunda = await run([file2, "--as", "fase0"], baselines);
  check(segunda.code !== 0, "republicar no mesmo namespace sem --replace tem de ser recusado",
        segunda.stdout.trim());
  check(sha(await readFile(png)) === hashOriginal,
        "a referencia da baseline anterior continua intacta apos a recusa");

  const comReplace = await run([file2, "--as", "fase0", "--replace"], baselines);
  check(comReplace.code === 0, "--replace publica de proposito", comReplace.stderr.trim());

  // E duas baselines DIFERENTES com a mesma cena/slot convivem.
  const outra = await run([file2, "--as", "fase1"], baselines);
  check(outra.code === 0, "outra baseline com a mesma cena/slot publica em paralelo",
        outra.stderr.trim());
  check(
    existsSync(path.join(baselines, "Reference", "fase0", "cena_a_slot0.png")) &&
      existsSync(path.join(baselines, "Reference", "fase1", "cena_a_slot0.png")),
    "as duas baselines coexistem com a mesma cena/slot",
  );
});

// ---------------------------------------------------------------------------------------------
// 3. Falha no meio da escrita nao deixa a baseline pela metade.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures, { slots: [0, 1, 2, 3] }));
  const bom = await run([file, "--as", "fase0"], baselines);
  check(bom.code === 0, "publicacao de referencia", bom.stderr.trim());
  const antes = (await readdir(path.join(baselines, "Reference", "fase0"))).sort();

  // Um dos PNGs de origem some entre uma publicacao e outra: a segunda tem de abortar ANTES de
  // tocar no destino, e nao a meio caminho.
  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const relatorio2 = await makeReport(captures2, { slots: [0, 1, 2, 3] });
  await rm(relatorio2.captures[2].capture.pngPath);
  const file2 = await writeReport(root, relatorio2, "relatorio2.json");

  const ruim = await run([file2, "--as", "fase0", "--replace"], baselines);
  check(ruim.code !== 0, "PNG de origem ausente derruba a publicacao", ruim.stdout.trim());
  const depois = (await readdir(path.join(baselines, "Reference", "fase0"))).sort();
  check(
    JSON.stringify(antes) === JSON.stringify(depois),
    "a baseline anterior nao ficou metade nova e metade antiga",
    `antes=${antes.length} depois=${depois.length}`,
  );
  check(
    (await readdir(baselines)).every((entry) => !entry.startsWith(".staging-")),
    "nao sobrou diretorio de staging",
  );
});

// ---------------------------------------------------------------------------------------------
// 4. Par PNG+manifesto e obrigatorio.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures, { omitManifestFor: "cena_a_1" }));
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code !== 0, "manifesto ausente derruba a publicacao", result.stdout.trim());
  check(
    !existsSync(path.join(baselines, "Reference", "fase0")),
    "nada foi publicado quando o par estava incompleto",
  );
});

// ---------------------------------------------------------------------------------------------
// 5. O campo `image` do manifesto publicado aponta para o PNG publicado.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code === 0, "publicacao", result.stderr.trim());
  const dir = path.join(baselines, "Reference", "fase0");
  const manifesto = JSON.parse(await readFile(path.join(dir, "cena_a_slot0.json"), "utf8"));
  check(manifesto.image === "cena_a_slot0.png", "image aponta para o nome publicado",
        `image=${manifesto.image}`);
  check(existsSync(path.join(dir, manifesto.image)), "o arquivo apontado por image existe");
  check(typeof manifesto.sourceImage === "string" && manifesto.sourceImage.includes("longo"),
        "o nome de origem foi preservado em sourceImage");
});

// ---------------------------------------------------------------------------------------------
// 6. Gates de proveniencia.
await withSandbox(async ({ root, baselines, captures }) => {
  const casos = [
    ["arvore suja na medicao", { gitState: { head: "a".repeat(40), workingTreeClean: false, dirtyEntries: ["M x"] } }],
    ["git sem resposta", { gitState: { head: null, workingTreeClean: null, dirtyEntries: [] } }],
    ["HEAD invalido", { gitState: { head: "abc", workingTreeClean: true, dirtyEntries: [] } }],
    ["passed ausente", { passed: undefined }],
    ["mcpServer ausente", { mcpServer: undefined }],
    ["dist obsoleto", { mcpServer: { distSha256: "b".repeat(64), stale: true } }],
    ["fingerprints vazias", { sceneFingerprints: {} }],
    ["fingerprint de outra cena", { sceneFingerprints: { outra: {} } }],
    ["build suja no carimbo", { buildCommit: "abc1234-dirty" }],
  ];
  for (const [nome, overrides] of casos) {
    const report = await makeReport(captures, { overrides });
    for (const [key, value] of Object.entries(overrides)) {
      if (value === undefined) delete report[key];
    }
    const file = await writeReport(root, report, `${nome.replace(/\W+/g, "_")}.json`);
    const result = await run([file, "--as", "fase0"], baselines);
    check(result.code !== 0, `'${nome}' tem de ser recusado`, result.stdout.trim());
  }

  const incompletoParcial = await makeReport(captures, {
    overrides: {
      sceneFingerprints: {
        cena_a: { "cena_a.sscene": { bytes: 10, sha256: sha(Buffer.from("x")) } },
      },
    },
  });
  const file = await writeReport(root, incompletoParcial, "parcial.json");
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code !== 0, "'fingerprint com 1 de 3 arquivos' tem de ser recusado",
        result.stdout.trim());
});

// ---------------------------------------------------------------------------------------------
// 7. Caminho feliz: publica, e o MANIFEST descreve o que esta em disco.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code === 0, "caminho feliz publica", result.stderr.trim());

  const publicado = JSON.parse(
    await readFile(path.join(baselines, "fase0.json"), "utf8"),
  );
  check(publicado.capturedImages?.length === 2, "capturedImages tem as duas referencias");
  for (const imagem of publicado.capturedImages ?? []) {
    const png = path.join(baselines, imagem.file);
    const manifesto = path.join(baselines, imagem.manifest);
    check(existsSync(png), `referencia existe: ${imagem.file}`);
    check(existsSync(manifesto), `manifesto existe: ${imagem.manifest}`);
    check(sha(await readFile(png)) === imagem.sha256, `hash confere: ${imagem.file}`);
    check(imagem.file.startsWith("Reference/fase0/"), `referencia esta no namespace: ${imagem.file}`);
  }

  const manifest = await readFile(path.join(baselines, "MANIFEST.md"), "utf8");
  const linha = manifest.match(/^\| `fase0\.json` \| `([0-9a-f]{64})` \|$/m);
  check(Boolean(linha), "MANIFEST tem a linha de SHA do relatorio");
  if (linha) {
    check(
      sha(await readFile(path.join(baselines, "fase0.json"), "utf8")) === linha[1],
      "o SHA do MANIFEST confere com o arquivo em disco",
    );
  }
  check(!/ $/m.test(manifest), "MANIFEST nao tem espaco no fim de linha");
});

if (failures > 0) {
  console.error(`[PROMOTE] ${failures} falha(s)`);
  process.exit(1);
}
console.log("[PROMOTE] ok - escape de caminho, namespace, transacao, par e proveniencia");
