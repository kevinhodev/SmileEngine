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

function run(args, baselineDirectory, extraEnv = {}) {
  return new Promise((resolve) => {
    execFile(
      process.execPath,
      [promoter, ...args],
      {
        cwd: repoRoot,
        env: { ...process.env, SMILE_BASELINE_DIR: baselineDirectory, ...extraEnv },
      },
      (error, stdout, stderr) =>
        resolve({
          code: error?.code ?? 0,
          stdout: String(stdout),
          stderr: String(stderr),
          // fail() do promotor escreve em STDERR e os console.log em stdout. Casar mensagem so
          // contra stdout tornava as checagens de diagnostico silenciosamente vazias — inclusive a
          // negada de ROLLBACK FALHOU, que nunca poderia casar.
          saida: String(stdout) + String(stderr),
        }),
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

// Segundo PNG (1x1 preto), com bytes DIFERENTES do primeiro: sem isso, um vazamento de conteudo
// entre duas publicacoes passaria despercebido porque o hash bateria por coincidencia.
const outroPngBytes = Buffer.from(
  "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753" +
    "de0000000c4944415408d76360000002000155a2b1180000000049454e44ae42" +
    "6082",
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
      await writeFile(png, options.pngBytes ?? pngBytes);
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
    // O promotor deriva daqui os caminhos que as fingerprints TEM de cobrir; sem isto ele recusa,
    // que e o comportamento novo (antes bastavam tres nomes quaisquer).
    scenes: scenes.map((scene) => ({
      id: scene,
      scene: `Assets/${scene}.sscene`,
      sidecar: `Assets/${scene}.cameras.json`,
    })),
    startedAt: "2026-01-01T00:00:00.000Z",
    buildCommit: "abc1234",
    regime: { captureOnly: true },
    gitState: { head: "a".repeat(40), workingTreeClean: true, dirtyEntries: [] },
    mcpServer: {
      distSha256: "b".repeat(64),
      sourceSha256: "c".repeat(64),
      distFiles: 4,
      sourceFiles: 4,
      builtInFlow: true,
      stale: false,
    },
    sceneFingerprints: Object.fromEntries(
      scenes.map((scene) => [
        scene,
        {
          [`Assets/${scene}.sscene`]: fingerprint(),
          [`Assets/${scene}.smesh`]: fingerprint(),
          [`Assets/${scene}.cameras.json`]: fingerprint(),
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
    check(result.code !== 0, `--as '${nome}' tem de ser recusado`, result.saida.trim());
  }
  check(
    (await readFile(fora, "utf8")) === "NAO PODE SER TOCADO",
    "nenhum --as malicioso escreveu fora do diretorio de baselines",
  );
});

// ---------------------------------------------------------------------------------------------
// 1b. Escape de diretorio pelo campo `scene` do RELATORIO.
//
// Proteger so o --as deixava a porta aberta do outro lado: `scene` vem do relatorio e entrava
// direto no nome do arquivo. No Win32, um scene ../../vitima resolve para fora ate de Baselines.
await withSandbox(async ({ root, baselines, captures }) => {
  // As vitimas ficam ONDE O ESCAPE CAIRIA. O staging e <baselines>/.staging-<name>-<pid>/refs,
  // entao "../../X" resolve para <baselines>/X e "../../../X" para <root>/X. Na primeira versao
  // deste teste a vitima estava so na raiz, fora da mira de "../../" — e o teste passava mesmo com
  // as duas camadas de defesa removidas.
  const vitimas = new Map([
    [path.join(baselines, "vitima_slot0.png"), "NAO PODE SER TOCADO (baselines)"],
    [path.join(root, "vitima_slot0.png"), "NAO PODE SER TOCADO (raiz)"],
  ]);
  for (const [alvo, conteudo] of vitimas) await writeFile(alvo, conteudo, "utf8");

  for (const scene of [
    "../../vitima",
    "../../../vitima",
    "..\..\vitima",
    "..\..\..\vitima",
    "a/b",
    "..",
    "",
    "con",
  ]) {
    // O relatorio tem de ser COERENTE com o nome malicioso. Se scenes/sceneFingerprints ficarem
    // com o nome antigo, o gate de proveniencia recusa por divergencia de conjunto e a publicacao
    // aborta muito antes da escrita — foi assim que a primeira versao deste teste passou mesmo
    // com as duas camadas de defesa removidas: ela verificava so que o exit era != 0, e o exit
    // vinha de outro motivo.
    const report = await makeReport(captures, { scenes: ["cena_a"] });
    report.captures = report.captures.map((entry) => ({ ...entry, scene }));
    report.scenes = [{ id: scene, scene: "Assets/x.sscene", sidecar: "Assets/x.cameras.json" }];
    report.sceneFingerprints = {
      [scene]: {
        "Assets/x.sscene": { bytes: 10, sha256: "d".repeat(64) },
        "Assets/x.smesh": { bytes: 10, sha256: "d".repeat(64) },
        "Assets/x.cameras.json": { bytes: 10, sha256: "d".repeat(64) },
      },
    };
    const file = await writeReport(root, report, `escape_${Buffer.from(scene).toString("hex")}.json`);
    const result = await run([file, "--as", "fase0"], baselines);
    check(result.code !== 0, `scene ${JSON.stringify(scene)} tem de ser recusado`, result.saida.trim());
  }
  for (const [alvo, conteudo] of vitimas) {
    check(
      (await readFile(alvo, "utf8").catch(() => null)) === conteudo,
      `nenhum scene malicioso tocou ${path.relative(root, alvo)}`,
    );
  }
  check(!existsSync(path.join(baselines, "Reference", "fase0")), "nada foi publicado");
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
        segunda.saida.trim());
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
// 2b. Imutabilidade decidida pelo PACOTE, e nao so pelo diretorio de referencias.
//
// Uma baseline so de performance nao tem imagens, entao pulava o bloco de referencias inteiro e
// sobrescrevia o <name>.json calado. Idem quando o JSON existe mas o diretorio de refs nao.
await withSandbox(async ({ root, baselines, captures }) => {
  // Relatorio SEM capturas: tem summary/runs e nenhuma imagem.
  const semImagens = async (overrides = {}) => {
    const base = await makeReport(captures);
    base.captures = [];
    base.summary = [{ scene: "cena_a", slot: 0 }];
    base.runs = [{ scene: "cena_a", slot: 0 }];
    return { ...base, ...overrides };
  };

  const file = await writeReport(root, await semImagens(), "perf.json");
  const primeira = await run([file, "--as", "soperf"], baselines);
  check(primeira.code === 0, "baseline so de performance publica", primeira.stderr.trim());
  check(
    !existsSync(path.join(baselines, "Reference", "soperf")),
    "baseline sem imagens nao cria diretorio de referencias",
  );
  const hashOriginal = sha(await readFile(path.join(baselines, "soperf.json")));

  const file2 = await writeReport(
    root,
    await semImagens({ buildCommit: "def5678" }),
    "perf2.json",
  );
  const segunda = await run([file2, "--as", "soperf"], baselines);
  check(
    segunda.code !== 0,
    "republicar baseline SEM IMAGENS sem --replace tem de ser recusado",
    segunda.saida.trim(),
  );
  check(
    sha(await readFile(path.join(baselines, "soperf.json"))) === hashOriginal,
    "o relatorio anterior sobreviveu a recusa",
  );

  const comReplace = await run([file2, "--as", "soperf", "--replace"], baselines);
  check(comReplace.code === 0, "--replace publica de proposito", comReplace.stderr.trim());

  // JSON existe, diretorio de referencias nao: a existencia tem de ser decidida pelo pacote.
  const comImagens = await writeReport(root, await makeReport(captures), "comimagens.json");
  const colisao = await run([comImagens, "--as", "soperf"], baselines);
  check(
    colisao.code !== 0,
    "publicar sobre um <name>.json existente, mesmo sem refs, tem de ser recusado",
    colisao.saida.trim(),
  );
});

// ---------------------------------------------------------------------------------------------
// 2c. `passed` ausente e recusado tambem no caminho LEGADO (sem gitState).
//
// O gate de proveniencia forte ja barra isso, entao um relatorio legado e o unico jeito de
// exercitar a checagem do topo — que aceitava o campo ausente por so testar `=== false`.
await withSandbox(async ({ root, baselines, captures }) => {
  const legado = async (overrides) => {
    const base = await makeReport(captures);
    delete base.gitState;
    delete base.mcpServer;
    delete base.sceneFingerprints;
    return { ...base, ...overrides };
  };

  const semPassed = await legado({});
  delete semPassed.passed;
  const a = await writeReport(root, semPassed, "legado_sem_passed.json");
  const semFlag = await run([a, "--as", "fase0", "--allow-legacy-provenance", "--note", "x"], baselines);
  check(semFlag.code !== 0, "legado com passed AUSENTE e recusado sem --allow-failed", semFlag.saida.trim());

  const semNota = await run(
    [a, "--as", "fase0", "--allow-legacy-provenance", "--allow-failed"],
    baselines,
  );
  check(semNota.code !== 0, "--allow-failed com passed ausente ainda exige --note", semNota.saida.trim());

  const completo = await run(
    [a, "--as", "fase0", "--allow-legacy-provenance", "--allow-failed", "--note", "aceito de proposito"],
    baselines,
  );
  check(completo.code === 0, "com as duas flags e nota, publica", completo.saida.trim());
});

// ---------------------------------------------------------------------------------------------
// 3. Falha DURANTE a escrita nao deixa o pacote pela metade.
//
// A versao anterior deste teste nao provava nada: ela removia um PNG de origem antes de invocar o
// promotor, entao a validacao antecipada abortava e a publicacao nunca chegava a escrever. Alem
// disso comparava so a LISTA DE NOMES, com as duas rodadas usando os mesmos bytes — um vazamento
// de conteudo passaria batido.
//
// Agora: bytes DIFERENTES na segunda rodada (para o hash acusar substituicao) e um failpoint
// injetado em cada etapa da publicacao, para exercitar rollback de escrita, de troca das
// referencias, do relatorio e do manifesto.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures, { slots: [0, 1, 2, 3] }));
  const bom = await run([file, "--as", "fase0"], baselines);
  check(bom.code === 0, "publicacao de referencia", bom.stderr.trim());

  // Estado que tem de sobreviver a todas as falhas, conferido por CONTEUDO.
  const alvos = [
    path.join(baselines, "fase0.json"),
    path.join(baselines, "MANIFEST.md"),
    ...(await readdir(path.join(baselines, "Reference", "fase0"))).map((f) =>
      path.join(baselines, "Reference", "fase0", f),
    ),
  ];
  const antes = new Map();
  for (const alvo of alvos) antes.set(alvo, sha(await readFile(alvo)));

  // Segunda rodada com BYTES diferentes: se algum artefato vazar, o hash muda.
  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const file2 = await writeReport(
    root,
    await makeReport(captures2, {
      slots: [0, 1, 2, 3],
      pngBytes: outroPngBytes,
      overrides: { buildCommit: "def5678" },
    }),
    "relatorio2.json",
  );
  check(sha(pngBytes) !== sha(outroPngBytes), "os dois PNGs de teste tem bytes diferentes");

  for (const ponto of [
    "apos-escrever-referencias",
    "apos-escrever-relatorio",
    "apos-trocar-referencias",
    "apos-trocar-relatorio",
    "apos-trocar-manifesto",
  ]) {
    const result = await run([file2, "--as", "fase0", "--replace"], baselines, {
      SMILE_PROMOTE_FAILPOINT: ponto,
    });
    check(result.code !== 0, `failpoint '${ponto}' derruba a publicacao`, result.saida.trim());
    check(
      !/ROLLBACK FALHOU/.test(result.saida),
      `failpoint '${ponto}': o rollback nao pode falhar`,
      result.saida.trim(),
    );

    for (const [alvo, hashOriginal] of antes) {
      const atual = await readFile(alvo).then(sha).catch(() => null);
      check(
        atual === hashOriginal,
        `failpoint '${ponto}': ${path.basename(alvo)} continua com o conteudo anterior`,
        atual === null ? "o arquivo sumiu" : "o conteudo foi substituido",
      );
    }
    const sobras = (await readdir(baselines)).filter(
      (entry) => entry.startsWith(".staging-") || entry.includes(".retiring-"),
    );
    check(sobras.length === 0, `failpoint '${ponto}': sem residuos`, sobras.join(", "));
    const sobrasRef = (await readdir(path.join(baselines, "Reference"))).filter((entry) =>
      entry.includes(".retiring-"),
    );
    check(sobrasRef.length === 0, `failpoint '${ponto}': sem residuo em Reference/`,
          sobrasRef.join(", "));
  }

  // Sem failpoint, a mesma publicacao passa e TROCA de verdade — senao o teste acima estaria
  // apenas provando que nada nunca e escrito.
  const ok = await run([file2, "--as", "fase0", "--replace"], baselines);
  check(ok.code === 0, "sem failpoint a publicacao conclui", ok.stderr.trim());
  const pngDepois = sha(
    await readFile(path.join(baselines, "Reference", "fase0", "cena_a_slot0.png")),
  );
  check(pngDepois === sha(outroPngBytes), "a publicacao bem-sucedida substituiu o conteudo");
});

// ---------------------------------------------------------------------------------------------
// 3b. Erro de filesystem nao pode virar 'nao existe'.
//
// LACUNA CONHECIDA: este teste cobre o PREFLIGHT (a consulta que decide se a baseline ja existe).
// A mesma armadilha existe dentro de trocar(), no stat do destino, e nao e alcancavel de fora: o
// preflight barra a mesma condicao antes de a troca comecar. As duas chamam o mesmo helper
// existe(), que so trata ENOENT como ausencia — mas isso e argumento de leitura, nao de teste, e
// esta escrito aqui para ninguem supor que aquele ponto esta coberto.
//
// stat(...).catch(() => false) transformava permissao negada, handle em uso ou disco fora em
// 'nao existe', e dai a imutabilidade era ignorada. Aqui o diretorio de baselines vira um ARQUIVO
// com o nome de Reference/: o readdir dele falha com ENOTDIR, que nao e ENOENT.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  // Reference existe como ARQUIVO: listar dispara ENOTDIR.
  await writeFile(path.join(baselines, "Reference"), "sou um arquivo", "utf8");
  const result = await run([file, "--as", "fase0"], baselines);
  // Conferir so o exit != 0 nao prova nada: a versao com `catch(() => [])` tambem falha, so que
  // depois, no mkdir que esbarra no arquivo. E preciso fixar o MECANISMO pelo diagnostico — foi
  // assim que esta checagem passou verde contra a implementacao quebrada na primeira tentativa.
  check(result.code !== 0, "erro de filesystem tem de abortar", result.saida.trim());
  check(
    /nao consegui (listar|consultar)/.test(result.saida),
    "a recusa tem de ser POR NAO CONSEGUIR CONSULTAR, e nao por tropecar mais adiante",
    result.saida.trim(),
  );
  check(
    (await readFile(path.join(baselines, "Reference"), "utf8")) === "sou um arquivo",
    "nada foi escrito por cima do que nao pode ser consultado",
  );
});

// ---------------------------------------------------------------------------------------------
// 3c. Nomes que o Windows considera equivalentes.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));

  // Ponto final: o Windows e o git normalizam, e este diretorio e versionado.
  for (const nome of ["fase0.", "fase0 ", "fase0.."]) {
    const result = await run([file, "--as", nome], baselines);
    check(result.code !== 0, `--as ${JSON.stringify(nome)} tem de ser recusado`, result.saida.trim());
  }

  const primeira = await run([file, "--as", "fase0"], baselines);
  check(primeira.code === 0, "publicacao de referencia", primeira.stderr.trim());
  const hashOriginal = sha(await readFile(path.join(baselines, "fase0.json")));

  // Mesma baseline com OUTRA caixa: no Windows e o mesmo caminho.
  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const file2 = await writeReport(
    root,
    await makeReport(captures2, { pngBytes: outroPngBytes, overrides: { buildCommit: "def5678" } }),
    "relatorio2.json",
  );
  const outraCaixa = await run([file2, "--as", "FASE0"], baselines);
  check(
    outraCaixa.code !== 0,
    "--as FASE0 sobre uma fase0 existente tem de ser recusado",
    outraCaixa.saida.trim(),
  );
  check(
    sha(await readFile(path.join(baselines, "fase0.json"))) === hashOriginal,
    "a baseline original sobreviveu",
  );
  // E nem com --replace, porque isso deixaria dois nomes para um diretorio.
  const comReplace = await run([file2, "--as", "FASE0", "--replace"], baselines);
  check(comReplace.code !== 0, "--replace sob outra caixa tambem e recusado", comReplace.saida.trim());
});

// ---------------------------------------------------------------------------------------------
// 3d. Manifesto tem de ser DO ponto publicado.
//
// O promotor conferia que o JSON existe, e nao que ele e daquela cena e daquele slot: uma captura
// da Emerald/slot 1 podia ser publicada como Bistro/slot 0 com hashes todos coerentes.
await withSandbox(async ({ root, baselines, captures }) => {
  for (const [rotulo, mutacao] of [
    ["slot trocado", (m) => ({ ...m, bookmarkSlot: 3 })],
    ["cena trocada", (m) => ({ ...m, scene: "OutraCena" })],
  ]) {
    const report = await makeReport(captures, { scenes: ["cena_a"], slots: [0] });
    const manifesto = report.captures[0].capture.pngPath.replace(/\.png$/, ".json");
    await writeFile(
      manifesto,
      JSON.stringify(mutacao(JSON.parse(await readFile(manifesto, "utf8"))), null, 2),
      "utf8",
    );
    const file = await writeReport(root, report, `${rotulo.replace(/\W+/g, "_")}.json`);
    const result = await run([file, "--as", "fase0"], baselines);
    check(result.code !== 0, `${rotulo} tem de ser recusado`, result.saida.trim());
    check(!existsSync(path.join(baselines, "Reference", "fase0")), `${rotulo}: nada publicado`);
  }
});

// ---------------------------------------------------------------------------------------------
// 3e. Falha DEPOIS do commit e limpeza, e nao rollback.
//
// A remocao dos backups acontecia dentro da transacao: um rm que falhasse caia no catch e o
// script dizia 'nada foi publicado' com o pacote novo ja no ar — a mensagem mais enganosa
// possivel, porque manda procurar um problema que nao existe e ignorar o que existe.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  check((await run([file, "--as", "fase0"], baselines)).code === 0, "publicacao de referencia");

  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const file2 = await writeReport(
    root,
    await makeReport(captures2, { pngBytes: outroPngBytes, overrides: { buildCommit: "def5678" } }),
    "relatorio2.json",
  );
  // DOIS mecanismos distintos, e o segundo e o que importa. O failpoint 'apos-commit' so prova
  // que uma excecao pos-commit e tratada; 'apos-commit-rm' lanca DENTRO do rm, caindo no mesmo
  // .catch em que um EPERM ou EBUSY cairia. Sem ele, a colecao de erros de limpeza podia ficar
  // sendo preenchida e nunca consultada — que era exatamente o defeito.
  for (const ponto of ["apos-commit", "apos-commit-rm"]) {
    const parcial = await run([file2, "--as", "fase0", "--replace"], baselines, {
      SMILE_PROMOTE_FAILPOINT: ponto,
    });
    check(parcial.code === 2, ponto + ": sai com codigo 2", "exit=" + parcial.code);
    check(
      /retiring-/.test(parcial.saida),
      ponto + ": nomeia onde ficaram os restos",
      parcial.saida.trim(),
    );
    check(
      (await readdir(baselines)).every((entry) => !entry.startsWith(".staging-")),
      ponto + ": o finally rodou e o staging foi removido",
    );
    // Reposiciona para o proximo ciclo comecar do mesmo estado.
    await rm(path.join(baselines, "Reference", "fase0"), { recursive: true, force: true });
    for (const sobra of (await readdir(baselines)).filter((e) => e.includes(".retiring-"))) {
      await rm(path.join(baselines, sobra), { recursive: true, force: true });
    }
    await run([file, "--as", "fase0", "--replace"], baselines);
  }

  const result = await run([file2, "--as", "fase0", "--replace"], baselines, {
    SMILE_PROMOTE_FAILPOINT: "apos-commit",
  });

  check(result.code === 2, "falha pos-commit sai com codigo proprio (2), nao 1", `exit=${result.code}`);
  check(
    /publicado, mas a limpeza/i.test(result.saida) && !/nada foi publicado/.test(result.saida),
    "a mensagem diz que PUBLICOU e que a limpeza falhou",
    result.saida.trim(),
  );
  check(
    /retiring-/.test(result.saida),
    "a mensagem nomeia onde ficaram os restos",
    result.saida.trim(),
  );
  // E o pacote novo TEM de estar ativo, porque o commit ja tinha acontecido.
  check(
    sha(await readFile(path.join(baselines, "Reference", "fase0", "cena_a_slot0.png"))) ===
      sha(outroPngBytes),
    "o pacote novo esta publicado, como a mensagem afirma",
  );
});

// ---------------------------------------------------------------------------------------------
// 3f. Consulta que falha DENTRO da transacao faz rollback, e nao process.exit.
//
// existe() e usado no trocar(), ja depois de as referencias terem sido movidas. Se ele encerrasse
// o processo (fail() -> process.exit) em vez de lancar, o Node sairia sem passar pelo catch nem
// pelo finally: pacote pela metade, sem rollback e com o staging para tras. O failpoint fica
// dentro do proprio helper, porque e a unica forma de exercitar esse ponto de fora.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  check((await run([file, "--as", "fase0"], baselines)).code === 0, "publicacao de referencia");

  const alvos = [
    path.join(baselines, "fase0.json"),
    path.join(baselines, "MANIFEST.md"),
    ...(await readdir(path.join(baselines, "Reference", "fase0"))).map((f) =>
      path.join(baselines, "Reference", "fase0", f),
    ),
  ];
  const antes = new Map();
  for (const alvo of alvos) antes.set(alvo, sha(await readFile(alvo)));

  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const file2 = await writeReport(
    root,
    await makeReport(captures2, { pngBytes: outroPngBytes, overrides: { buildCommit: "def5678" } }),
    "relatorio2.json",
  );
  // '#3' e o alvo que importa. existe() e chamado uma vez por artefato trocado, e sem alvo o
  // failpoint disparava sempre na PRIMEIRA chamada — que acontece dentro da primeira troca, antes
  // de qualquer referencia ter sido movida. O teste provava limpeza SEM MUTACAO, e nao o estado
  // parcial que o comentario dizia provar. Na terceira passagem, referencias e relatorio ja foram
  // trocados: e ali que um process.exit deixaria o pacote novo ativo e sem rollback.
  const result = await run([file2, "--as", "fase0", "--replace"], baselines, {
    SMILE_PROMOTE_FAILPOINT: "consulta-de-existencia#3",
  });

  check(result.code === 1, "consulta que falha na transacao aborta", "exit=" + result.code);
  // A propria mensagem prova ate onde a transacao chegou: passagem 3 so acontece depois das duas
  // trocas anteriores terem sido aplicadas.
  check(
    /passagem 3/.test(result.saida),
    "a falha ocorreu na TERCEIRA troca, com as duas anteriores ja aplicadas",
    result.saida.trim(),
  );
  check(
    /nada foi publicado/.test(result.saida),
    "a mensagem diz que nada foi publicado",
    result.saida.trim(),
  );
  // Com o rollback funcionando, tudo volta aos bytes antigos. Se existe() voltasse a chamar
  // fail(), o exit pularia o catch e estes artefatos ficariam com os bytes NOVOS ja movimentados.
  for (const [alvo, hashOriginal] of antes) {
    const atual = await readFile(alvo).then(sha).catch(() => null);
    check(
      atual === hashOriginal,
      "rollback preservou " + path.basename(alvo),
      atual === sha(outroPngBytes) ? "ficou com os bytes NOVOS" : "hash inesperado",
    );
  }
  // O sintoma que denuncia o process.exit: sem passar pelo finally, o staging sobra.
  check(
    (await readdir(baselines)).every((entry) => !entry.startsWith(".staging-")),
    "o finally rodou e o staging foi removido",
    (await readdir(baselines)).join(", "),
  );
});

// ---------------------------------------------------------------------------------------------
// 3g. Erro ao LER o MANIFEST nao pode virar 'manifesto ausente'.
//
// readFile(...).catch(() => "") apagaria em silencio as notas de waiver — a justificativa escrita
// de cada excecao aceita, que so existe ali. MANIFEST.md como DIRETORIO da EISDIR, que nao e
// ENOENT, sem depender de permissao (que no Windows nao e confiavel para isto).
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  await mkdir(path.join(baselines, "MANIFEST.md"), { recursive: true });
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code !== 0, "erro de leitura do MANIFEST aborta", result.saida.trim());
  check(
    /nao consegui ler o MANIFEST/.test(result.saida),
    "a recusa e POR NAO CONSEGUIR LER, e nao por tropecar adiante",
    result.saida.trim(),
  );
  check(
    !existsSync(path.join(baselines, "fase0.json")),
    "nada foi publicado quando o MANIFEST nao pode ser lido",
  );
});

// ---------------------------------------------------------------------------------------------
// 3h. Falha ao limpar o STAGING nao pode ser silenciosa.
//
// O erro do rm(staging) no finally era descartado: uma promocao BEM-SUCEDIDA saia com codigo 0
// deixando residuos, e numa falha anterior ninguem era avisado de que os artefatos novos
// continuavam no staging. E limpeza de staging, distinta da limpeza dos backups.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));

  // Caso 1: publicacao que DA CERTO, mas nao consegue limpar o staging.
  const ok = await run([file, "--as", "fase0"], baselines, {
    SMILE_PROMOTE_FAILPOINT: "limpeza-do-staging",
  });
  check(
    ok.code === 2,
    "promocao com staging nao removido sai com 2, e nao com 0",
    "exit=" + ok.code,
  );
  check(
    /staging nao pode ser removido/.test(ok.saida),
    "a mensagem distingue staging de backup",
    ok.saida.trim(),
  );
  // O pacote em si foi publicado: o problema e so o lixo.
  check(
    existsSync(path.join(baselines, "fase0.json")) &&
      existsSync(path.join(baselines, "Reference", "fase0", "cena_a_slot0.png")),
    "o pacote ficou publicado, apesar do residuo",
  );
  check(
    (await readdir(baselines)).some((entry) => entry.startsWith(".staging-")),
    "o residuo que a mensagem denuncia existe de fato",
  );

  // Caso 2: falha de publicacao E staging nao removido — a mensagem tem de dizer as duas coisas.
  for (const sobra of (await readdir(baselines)).filter((e) => e.startsWith(".staging-"))) {
    await rm(path.join(baselines, sobra), { recursive: true, force: true });
  }
  const captures2 = path.join(root, "Captures2");
  await mkdir(captures2, { recursive: true });
  const file2 = await writeReport(
    root,
    await makeReport(captures2, { pngBytes: outroPngBytes, overrides: { buildCommit: "def5678" } }),
    "relatorio2.json",
  );
  const ruim = await run([file2, "--as", "fase0", "--replace"], baselines, {
    SMILE_PROMOTE_FAILPOINT: "apos-escrever-relatorio",
  });
  // O promotor aceita um failpoint por vez, entao este caso confere apenas que a falha de
  // PUBLICACAO continua reportada como tal depois da mudanca no finally.
  check(ruim.code === 1, "falha de publicacao continua saindo com 1", "exit=" + ruim.code);
  check(
    /nada foi publicado/.test(ruim.saida),
    "e continua dizendo que nada foi publicado",
    ruim.saida.trim(),
  );
});

// ---------------------------------------------------------------------------------------------
// 4. Par PNG+manifesto e obrigatorio.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures, { omitManifestFor: "cena_a_1" }));
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code !== 0, "manifesto ausente derruba a publicacao", result.saida.trim());
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
  check(result.code === 0, "publicacao", result.saida.trim());
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
    ["mcpServer sem sourceSha256", { mcpServer: { distSha256: "b".repeat(64), distFiles: 4, sourceFiles: 4, builtInFlow: true, stale: false } }],
    ["mcpServer sem builtInFlow", { mcpServer: { distSha256: "b".repeat(64), sourceSha256: "c".repeat(64), distFiles: 4, sourceFiles: 4, stale: false } }],
    ["mcpServer sem contagem de arquivos", { mcpServer: { distSha256: "b".repeat(64), sourceSha256: "c".repeat(64), builtInFlow: true, stale: false } }],
    ["fingerprints com tres nomes ERRADOS", { sceneFingerprints: { cena_a: { "outro/a.sscene": { bytes: 10, sha256: "d".repeat(64) }, "outro/a.smesh": { bytes: 10, sha256: "d".repeat(64) }, "outro/a.cameras.json": { bytes: 10, sha256: "d".repeat(64) } } } }],
    ["report.scenes ausente", { scenes: undefined }],
  ];
  for (const [nome, overrides] of casos) {
    const report = await makeReport(captures, { overrides });
    for (const [key, value] of Object.entries(overrides)) {
      if (value === undefined) delete report[key];
    }
    const file = await writeReport(root, report, `${nome.replace(/\W+/g, "_")}.json`);
    const result = await run([file, "--as", "fase0"], baselines);
    check(result.code !== 0, `'${nome}' tem de ser recusado`, result.saida.trim());
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
        result.saida.trim());
});

// ---------------------------------------------------------------------------------------------
// 7. Caminho feliz: publica, e o MANIFEST descreve o que esta em disco.
await withSandbox(async ({ root, baselines, captures }) => {
  const file = await writeReport(root, await makeReport(captures));
  const result = await run([file, "--as", "fase0"], baselines);
  check(result.code === 0, "caminho feliz publica", result.saida.trim());

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
