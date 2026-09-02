// Baseline do item 9 do GPU-DRIVEN-PLAN.md: o "antes" contra o qual as Fases 1-4 vao ser medidas.
//
// O que esta regua existe para capturar e o custo de SUBMISSAO na CPU — BuildDrawLists, culling,
// ordenacao e Submit — junto das contagens que explicam esse custo (renderaveis, draw items,
// visiveis, ocluidos, e draws/binds por passe). O tempo de GPU entra como contexto, nao como
// objeto: nenhuma fase do plano promete baixa-lo diretamente.
//
// Corpus de gate: Bistro e Emerald cobrem cena real (material, alpha, oclusao, instancing); as
// tres sinteticas cobrem escala com geometria controlada. MEASURE_SEVEN ficou de fora de
// proposito (ver o item 8 do plano).
//
// Uma cena por lancamento do editor: nao existe comando de trocar cena no bridge, e o
// smile_run_editor recusa reaproveitar um editor aberto com outra .sscene — entao fechar entre
// cenas nao e zelo, e o unico caminho.

import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function argumentValue(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : fallback;
}

// As sinteticas moram no diretorio de BUILD (sao geradas pela fixture do ctest) e as reais em
// Assets. O smile_run_editor aceita qualquer caminho relativo dentro da raiz do projeto, entao os
// dois convivem sem caso especial.
const allScenes = [
  {
    id: "bistro",
    label: "Bistro Exterior",
    kind: "real",
    scene: "Assets/Scenes/Bistro/BistroExterior.sscene",
  },
  {
    id: "emerald",
    label: "Emerald Square (dia)",
    kind: "real",
    scene: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene",
  },
  {
    id: "synthetic_10k",
    label: "Sintetica 10k",
    kind: "synthetic",
    scene: "build/Tests/SyntheticScenes/Synthetic_10k.sscene",
    expectedRenderables: 10_000,
  },
  {
    id: "synthetic_100k",
    label: "Sintetica 100k",
    kind: "synthetic",
    scene: "build/Tests/SyntheticScenes/Synthetic_100k.sscene",
    expectedRenderables: 100_000,
  },
  {
    id: "synthetic_1M",
    label: "Sintetica 1M",
    kind: "synthetic",
    scene: "build/Tests/SyntheticScenes/Synthetic_1M.sscene",
    expectedRenderables: 1_000_000,
  },
].map((entry) => ({
  ...entry,
  sidecar: entry.scene.replace(/\.sscene$/, ".cameras.json"),
}));

const sceneFilter = argumentValue("--scenes", "");
const scenes = sceneFilter
  ? sceneFilter.split(",").map((id) => {
      const wanted = id.trim();
      const found = allScenes.find((scene) => scene.id === wanted);
      assert(found, `cena desconhecida '${wanted}'; use ${allScenes.map((s) => s.id).join(",")}`);
      return found;
    })
  : allScenes;

// Uma baseline tirada de arvore suja nao serve como "antes": meses depois ninguem consegue dizer
// o que estava compilado. O StampVersion sufixa `-dirty` quando `git status --porcelain` nao esta
// vazio (arquivo nao rastreado inclusive), entao a checagem e barata e nao precisa de git aqui.
// --allow-dirty existe para shakedown da propria regua, e marca o relatorio como nao-gate.
const allowDirty = process.argv.includes("--allow-dirty");
const preset = argumentValue("--preset", "gameplay_rr");
const warmupSeconds = Number(argumentValue("--warmup", "15"));
const samples = Number(argumentValue("--samples", "60"));
const intervalMs = Number(argumentValue("--interval", "250"));
const timeOfDayHours = Number(argumentValue("--tod", "10"));
const settleMilliseconds = 4_000;
const slots = [0, 1, 2, 3];

const environment = {
  ...Object.fromEntries(Object.entries(process.env).filter(([, value]) => value !== undefined)),
  SMILE_ROOT: smileRoot,
};
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: environment,
  stderr: "pipe",
});
const client = new Client({ name: "smile-gpu-driven-baseline", version: "1.0.0" });

function textOf(result) {
  return result.content.find((part) => part.type === "text")?.text ?? "";
}

function parseResult(result, toolName) {
  const text = textOf(result);
  if (result.isError) throw new Error(`${toolName}: ${text || "erro sem detalhe"}`);
  try {
    return JSON.parse(text || "{}");
  } catch {
    throw new Error(`${toolName}: resposta nao e JSON`);
  }
}

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 45);
  const sdkTimeout = Math.max(90_000, (declaredSeconds + 45) * 1_000);
  return parseResult(
    await client.callTool(
      { name: toolName, arguments: args },
      undefined,
      { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
    ),
    toolName,
  );
}

// Diferenca angular menor em modulo 360. O sidecar guarda o yaw ACUMULADO (a Emerald tem slots em
// 538 e 452 graus, porque o operador girou mais de uma volta), e o renderer devolve o valor que
// ele mantem — comparar cru acusaria diferenca onde a direcao e a mesma.
function yawDelta(left, right) {
  const raw = (((left - right) % 360) + 360) % 360;
  return Math.min(raw, 360 - raw);
}

function passOf(profile, name, queue) {
  return profile.passes.find((candidate) => candidate.name === name && candidate.queue === queue);
}

// smile_close_editor devolve quando o named pipe e a janela fecham — o PROCESSO ainda pode estar
// vivo depois disso, liberando BLAS/TLAS. Foi observado na 1M: o editor continuou vivo minutos
// apos o PASS da rodada. Enquanto ele nao morre, a VRAM dele ainda esta tomada, e medir a cena
// seguinte por cima disso mistura duas cenas na mesma leitura. `kill(pid, 0)` nao envia sinal;
// so pergunta se o processo existe (ESRCH quando nao).
async function waitForProcessExit(pid, label, timeoutMilliseconds = 180_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  for (;;) {
    try {
      process.kill(pid, 0);
    } catch (error) {
      if (error.code === "ESRCH") return;
      // EPERM: existe, mas sem permissao para sinalizar. Continua sendo "vivo".
      if (error.code !== "EPERM") throw error;
    }
    assert(
      Date.now() < deadline,
      `${label}: o editor PID ${pid} continuou vivo ${timeoutMilliseconds / 1000}s depois do ` +
        `fechamento; medir a proxima cena por cima dele misturaria as duas`,
    );
    await delay(500);
  }
}

async function readSidecar(relativePath) {
  const bytes = await readFile(path.join(smileRoot, relativePath), "utf8");
  const parsed = JSON.parse(bytes);
  assert(Array.isArray(parsed.slots), `${relativePath}: sem lista de slots`);
  return new Map(parsed.slots.map((slot) => [slot.index, slot]));
}

// A verificacao que da sentido a rodada inteira. profile_configure chama Bookmarks->Restore, mas
// Restore devolve true mesmo quando o SetCameraPose recusa o teleporte — entao, sem conferir a
// pose de volta, quatro slots poderiam medir a MESMA camera e a baseline seria um numero repetido
// quatro vezes, com aparencia de quatro regimes.
function assertPoseMatches(sceneId, slot, expected, actual) {
  const positionDelta = Math.max(
    Math.abs(actual.position.x - expected.posX),
    Math.abs(actual.position.y - expected.posY),
    Math.abs(actual.position.z - expected.posZ),
  );
  const pitchDelta = Math.abs(actual.pitchDegrees - expected.pitch);
  const yaw = yawDelta(actual.yawDegrees, expected.yaw);
  assert(
    positionDelta < 0.05 && pitchDelta < 0.05 && yaw < 0.05,
    `${sceneId}/slot${slot}: a camera nao foi para a pose do bookmark ` +
      `(dpos=${positionDelta.toFixed(3)} dpitch=${pitchDelta.toFixed(3)} dyaw=${yaw.toFixed(3)})`,
  );
}

const report = {
  schema: 1,
  purpose: "baseline pre-GPU-driven (item 9 do GPU-DRIVEN-PLAN.md)",
  startedAt: new Date().toISOString(),
  regime: {
    preset,
    timeOfDayHours,
    backgroundThrottle: false,
    warmupSeconds,
    samples,
    intervalMs,
    settleMilliseconds,
    slots,
    statistic: "mediana e p95 sobre amostras de frames DISTINTOS (sourceFrameIndex)",
  },
  scenes: scenes.map(({ id, label, kind, scene, sidecar }) => ({ id, label, kind, scene, sidecar })),
  runs: [],
  summary: [],
};

let currentLaunch;
let reportPath;

// Uma rodada verde com DEVICE_REMOVED no log nao e uma rodada verde. Mesma auditoria do
// gi-benchmark, e pela mesma razao: o profiler devolve numeros mesmo quando a API reclamou.
async function auditLogs(sceneId, pid) {
  const logs = await call("smile_get_latest_logs", { sessions: 5, tailLines: 1000 });
  const session = logs.sessions?.find(
    (candidate) =>
      typeof candidate.path === "string" && new RegExp(`-p${pid}\\.log$`, "i").test(candidate.path),
  );
  const lines = session?.lines ?? [];
  assert(
    Number.isInteger(pid) && session && lines.length > 0,
    `${sceneId}: nao encontrei o log persistente do editor PID ${pid}`,
  );
  const fatalPatterns = [
    /D3D12.*ERROR/i,
    /DXGI_ERROR/i,
    /DEVICE_REMOVED/i,
    /GPU validation.*error/i,
    /\[ERR\s*\]/i,
  ];
  const matches = lines.filter((line) => fatalPatterns.some((pattern) => pattern.test(line)));
  assert(matches.length === 0, `${sceneId}: log com erro de GPU/API: ${matches.join(" | ")}`);
  report.logAudit ??= {};
  report.logAudit[sceneId] = { passed: true, pid, logPath: session.path };
}

async function measureScene(sceneEntry) {
  const sidecar = await readSidecar(sceneEntry.sidecar);
  console.log(`[baseline] ${sceneEntry.id}: abrindo o editor`);
  currentLaunch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: sceneEntry.scene,
    waitUntilReady: true,
    // A 1M leva bem mais que as reais para montar TLAS/BLAS no boot.
    startupTimeoutSeconds: 300,
  });
  console.log(`[baseline] ${sceneEntry.id}: editor PID ${currentLaunch.pid}`);

  const status = await call("smile_editor_status", { timeoutSeconds: 10 });
  const buildCommit = status.buildCommit ?? currentLaunch.status?.buildCommit;
  assert(buildCommit, `${sceneEntry.id}: o editor nao informou buildCommit`);
  assert(
    allowDirty || !buildCommit.endsWith("-dirty"),
    `${sceneEntry.id}: build '${buildCommit}' saiu de arvore suja. O gate de saida da Fase 0 pede ` +
      `baseline com commit limpo; recompile de uma arvore limpa, ou passe --allow-dirty para uma ` +
      `rodada de shakedown (que NAO vale como baseline).`,
  );
  // A comparacao futura e "mesma cena, mesmo slot, mesmo preset, antes vs depois". Se as cenas
  // forem medidas de builds diferentes, nao ha um "antes" — ha varios.
  report.buildCommit ??= buildCommit;
  assert(
    report.buildCommit === buildCommit,
    `${sceneEntry.id}: build mudou no meio da rodada ('${report.buildCommit}' -> '${buildCommit}')`,
  );
  report.dirtyBuild = buildCommit.endsWith("-dirty");

  const sceneRuns = [];
  for (const slot of slots) {
    const expected = sidecar.get(slot);
    assert(expected, `${sceneEntry.id}: sidecar sem o slot ${slot}`);

    const configured = await call("smile_profile_configure", {
      preset,
      bookmarkSlot: slot,
      timeOfDayHours,
      backgroundThrottle: false,
    });
    assert(
      configured.backgroundThrottle === false,
      `${sceneEntry.id}/slot${slot}: throttle de segundo plano continuou ligado`,
    );

    const camera = await call("smile_camera_get");
    assertPoseMatches(sceneEntry.id, slot, expected, camera);

    await delay(settleMilliseconds);
    const profile = await call("smile_profile_gpu", {
      warmupSeconds,
      samples,
      intervalMs,
      includeSamples: false,
    });

    assert(
      profile.sampleCount === samples,
      `${sceneEntry.id}/slot${slot}: ${profile.sampleCount}/${samples} amostras`,
    );
    assert(profile.cpu, `${sceneEntry.id}/slot${slot}: resposta sem o bloco de CPU`);
    assert(
      profile.cpu.distinctFrames === samples,
      `${sceneEntry.id}/slot${slot}: ${profile.cpu.distinctFrames} frames distintos em ` +
        `${samples} amostras`,
    );
    // O que a Fase 3 vai consumir. Se ja estiver perto do teto ANTES do bindless, isso e um achado
    // do baseline e nao uma surpresa no meio da fase.
    assert(
      profile.cpu.srvHeap.highWater <= profile.cpu.srvHeap.capacity,
      `${sceneEntry.id}/slot${slot}: high-water do SRV heap acima da capacidade`,
    );
    if (sceneEntry.expectedRenderables) {
      assert(
        profile.cpu.scene.renderables.median === sceneEntry.expectedRenderables,
        `${sceneEntry.id}/slot${slot}: ${profile.cpu.scene.renderables.median} renderaveis, ` +
          `esperado ${sceneEntry.expectedRenderables}`,
      );
    }

    const frame = passOf(profile, "Frame (GPU)", "direct");
    assert(frame, `${sceneEntry.id}/slot${slot}: passe 'Frame (GPU)' ausente`);

    const cpu = profile.cpu;
    const totalDraws = cpu.passes.reduce((sum, pass) => sum + (pass.draws.median ?? 0), 0);
    const submissionMs =
      cpu.timingsMs.buildDrawLists.median +
      cpu.timingsMs.cullSort.median +
      cpu.timingsMs.batchSort.median +
      cpu.timingsMs.shadowLists.median +
      cpu.timingsMs.submit.median;

    const row = {
      scene: sceneEntry.id,
      slot,
      pose: { expected, measured: camera },
      cpuSubmissionMs: submissionMs,
      cpuTimingsMs: cpu.timingsMs,
      sceneCounts: cpu.scene,
      totalDrawsMedian: totalDraws,
      drawsByPass: cpu.passes.map((pass) => ({ key: pass.key, draws: pass.draws.median })),
      srvHeap: cpu.srvHeap,
      objectConstantBytes: cpu.objectConstantBytes,
      frameGpuMs: { median: frame.rawMilliseconds.median, p95: frame.rawMilliseconds.p95 },
      localVramBytes: profile.localVramBytes,
      resolution: profile.resolution,
      gpu: profile.gpu,
    };
    sceneRuns.push({ ...row, profileConfiguration: configured, profile });
    report.summary.push(row);

    console.log(
      `[baseline] ${sceneEntry.id}/slot${slot}: ` +
        `cpu-submissao=${submissionMs.toFixed(3)} ms, draws=${totalDraws}, ` +
        `visiveis=${cpu.scene.visibleItems.median}/${cpu.scene.drawItems.median}, ` +
        `frame=${frame.rawMilliseconds.median.toFixed(3)} ms, ` +
        `srv=${cpu.srvHeap.highWater}/${cpu.srvHeap.capacity}`,
    );
  }

  // Resolucao e GPU tem de ser as mesmas nos quatro slots: se o preset realocou alvos no meio, os
  // slots deixam de ser comparaveis entre si.
  //
  // ATENCAO ao comparar rodadas: a resolucao aqui e a do VIEWPORT do editor, que segue o tamanho
  // da janela. Ela e constante DENTRO de uma rodada, mas nao esta fixada ENTRE rodadas — a janela
  // aberta noutro tamanho muda o custo de GPU. O custo de CPU, que e o objeto desta regua, e por
  // objeto e nao por pixel, entao sobrevive; o tempo de GPU so compara entre relatorios que
  // declaram a mesma resolucao.
  const resolutions = new Set(sceneRuns.map((run) => JSON.stringify(run.resolution)));
  const gpus = new Set(sceneRuns.map((run) => run.gpu));
  assert(resolutions.size === 1, `${sceneEntry.id}: a resolucao variou entre os slots`);
  assert(gpus.size === 1, `${sceneEntry.id}: a GPU variou entre os slots`);

  await auditLogs(sceneEntry.id, currentLaunch.pid);

  if (!currentLaunch.alreadyRunning) {
    await call("smile_close_editor", { timeoutSeconds: 120 });
    await waitForProcessExit(currentLaunch.pid, sceneEntry.id);
  }
  currentLaunch = undefined;
  return sceneRuns;
}

async function writeReport() {
  const directory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Benchmarks");
  await mkdir(directory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const suffix = report.passed ? "" : "-FAILED";
  reportPath = path.join(directory, `gpu-driven-baseline${suffix}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  return reportPath;
}

try {
  await client.connect(transport);
  for (const sceneEntry of scenes) {
    report.runs.push(...(await measureScene(sceneEntry)));
  }
  report.finishedAt = new Date().toISOString();
  report.passed = true;
  console.log(`[baseline] PASS: ${await writeReport()}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? (error.stack ?? error.message) : String(error);
  console.error(`[baseline] FAIL: ${report.error}`);
  try {
    console.error(`[baseline] relatorio parcial: ${await writeReport()}`);
  } catch (reportError) {
    console.error(`[baseline] relatorio de falha: ${reportError}`);
  }
  process.exitCode = 1;
} finally {
  try {
    // Fecha o editor que ficou aberto no braco que falhou; sem isso, a proxima rodada esbarra no
    // "ja existe um SmileEditor aberto com outra cena" e o operador perde tempo com um erro que
    // nao e o defeito.
    const status = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (status.connected && currentLaunch && !currentLaunch.alreadyRunning) {
      await call("smile_close_editor", { timeoutSeconds: 120 });
      await waitForProcessExit(currentLaunch.pid, "cleanup");
    }
  } catch (cleanupError) {
    console.error(`[baseline] cleanup: ${cleanupError}`);
  }
  await client.close();
}
