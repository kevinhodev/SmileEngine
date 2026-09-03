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

import { createHash } from "node:crypto";
import { execFile } from "node:child_process";
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
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
// A metade VISUAL da baseline, pelo CAPTURE-PROTOCOL.md. Sai da mesma passagem, da mesma pose e da
// mesma build que a metade de performance — separar as duas em rodadas diferentes e comparar
// imagens de cameras que so parecem iguais.
//
// Os REGIMES sao distintos de proposito, e isso nao e descuido: o profiling usa o preset de
// gameplay (gameplay_rr) porque e o custo que interessa medir, e a captura usa o cientifico, que
// desliga o que polui comparacao de imagem. Ambos ficam declarados no relatorio.
//
// A captura acontece ANTES do profiling no mesmo slot, e mexe no estado: faz reset deterministico,
// aquece N frames e fixa o ElapsedTime. Medido: a 1M com --capture deu 547,0 ms no slot 0 contra
// 539,4 ms sem, ~1,4% acima. Por isso o modo entra na CHAVE da rodada — comparar um numero tirado
// com --capture contra outro sem e comparar dois regimes.
//
// --capture-only pula o profiling (aquecimento + 60 amostras) e so captura: e a passagem rapida
// para quando a metade de performance ja esta tirada.
const captureOnly = process.argv.includes("--capture-only");
const doCapture = captureOnly || process.argv.includes("--capture");
const captureWarmupFrames = Number(argumentValue("--capture-warmup", "128"));
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

// Rede de seguranca do teardown, mantida DEPOIS de o defeito estar corrigido no bridge.
//
// O defeito: o close_editor abria com um `status(300 ms)` e engolia a falha dele como "ja parou".
// Numa cena pesada o status leva mais que isso (380 ms de p50 medidos na sintetica de 1M, a
// ~3 FPS), entao a ferramenta devolvia sucesso SEM NUNCA MANDAR o shutdown — e o editor seguia
// vivo e RENDERIZANDO por mais de 10 minutos (610 s de CPU), ate um WM_CLOSE derruba-lo em 1 s.
// O caminho nativo nunca teve culpa: com o comando entregue, o editor da 1M fecha em ~3 s. Hoje o
// shutdown() so devolve `stopped` com o PID sumido, e prazo estourado nao conclui nada.
//
// A rede continua porque a consequencia de errar aqui e cara e silenciosa: enquanto o editor nao
// morre, a VRAM dele continua tomada, e medir a cena seguinte por cima disso mistura duas cenas na
// mesma leitura — contaminacao que nao aparece no relatorio, porque cada numero isolado continua
// plausivel. `kill(pid, 0)` nao envia sinal, so pergunta se ele existe (ESRCH quando nao).
//
// `forcedKills` no relatorio deixou de ser rotina esperada: agora e sinal de defeito NOVO.
//
// Se a saida limpa nao vier, mata. Um benchmark nao tem nada a salvar, e derrubar a rodada
// inteira por um teardown que nao acontece descartaria as medicoes ja feitas. O forcado fica
// REGISTRADO no relatorio: e sintoma de um defeito no caminho de shutdown, nao rotina.
async function ensureProcessGone(pid, label, graceMilliseconds = 120_000) {
  const alive = () => {
    try {
      process.kill(pid, 0);
      return true;
    } catch (error) {
      if (error.code === "ESRCH") return false;
      if (error.code === "EPERM") return true; // existe, sem permissao para sinalizar
      throw error;
    }
  };
  const until = async (deadline) => {
    while (Date.now() < deadline) {
      if (!alive()) return true;
      await delay(500);
    }
    return !alive();
  };

  if (await until(Date.now() + graceMilliseconds)) return;

  console.warn(
    `[baseline] ${label}: o editor PID ${pid} continuou vivo ${graceMilliseconds / 1000}s depois ` +
      `de close_editor ter reportado sucesso; encerrando a forca`,
  );
  report.forcedKills ??= [];
  report.forcedKills.push({ scene: label, pid, graceSeconds: graceMilliseconds / 1000 });
  try {
    process.kill(pid);
  } catch (error) {
    if (error.code !== "ESRCH") throw error;
  }
  assert(
    await until(Date.now() + 60_000),
    `${label}: o editor PID ${pid} sobreviveu ate ao encerramento forcado; medir a proxima cena ` +
      `por cima dele misturaria as duas`,
  );
}

// O sufixo -dirty prova o estado da arvore quando o EXECUTAVEL foi compilado, e nao quando a
// medicao roda. Entre uma coisa e outra da para modificar uma .sscene ignorada pelo git, trocar um
// sidecar ou editar um script, e a rodada sai com dirtyBuild:false afirmando uma limpeza que ja
// nao existe. Entao o estado do repositorio e dos assets EFETIVAMENTE CARREGADOS e medido aqui, na
// hora, e vai para o relatorio.
async function readGitState() {
  const run = (args) =>
    new Promise((resolve) => {
      execFile("git", args, { cwd: smileRoot }, (error, stdout) =>
        resolve(error ? null : stdout.trim()),
      );
    });
  const [head, status] = await Promise.all([
    run(["rev-parse", "HEAD"]),
    run(["status", "--porcelain"]),
  ]);
  return {
    head,
    // null = o git nao respondeu; nao e o mesmo que "limpo", e o relatorio tem de distinguir.
    workingTreeClean: status === null ? null : status === "",
    dirtyEntries: status ? status.split("\n").filter(Boolean) : [],
  };
}

// Identidade do servidor MCP que esta REALMENTE sendo executado. O script chama
// `node dist/index.js`, e o dist e produto de build, ignorado pelo git — entao nem o HEAD limpo nem
// as impressoes de cena revelam um bridge obsoleto. Aconteceu: o conserto do close_editor estava
// commitado e nao compilado, e as rodadas usaram o caminho antigo sem nada acusar.
async function fingerprintMcpServer() {
  const files = { src: [], dist: [] };
  const walk = async (directory, bucket, extension) => {
    for (const entry of await readdir(directory, { withFileTypes: true }).catch(() => [])) {
      const full = path.join(directory, entry.name);
      if (entry.isDirectory()) await walk(full, bucket, extension);
      else if (entry.name.endsWith(extension)) bucket.push(full);
    }
  };
  await walk(path.join(packageRoot, "src"), files.src, ".ts");
  await walk(path.join(packageRoot, "dist"), files.dist, ".js");

  const newest = async (list) => {
    let best = null;
    for (const file of list) {
      const info = await stat(file).catch(() => null);
      if (info && (!best || info.mtimeMs > best.mtimeMs)) best = { file, mtimeMs: info.mtimeMs };
    }
    return best;
  };
  const oldest = async (list) => {
    let best = null;
    for (const file of list) {
      const info = await stat(file).catch(() => null);
      if (info && (!best || info.mtimeMs < best.mtimeMs)) best = { file, mtimeMs: info.mtimeMs };
    }
    return best;
  };
  const newestSource = await newest(files.src);
  const oldestBuilt = await oldest(files.dist);

  // Hash do que efetivamente entra no processo, para o relatorio poder ser conferido depois.
  const digest = createHash("sha256");
  for (const file of files.dist.sort()) {
    digest.update(path.relative(packageRoot, file).replace(/\\/g, "/"));
    digest.update(await readFile(file).catch(() => Buffer.alloc(0)));
  }
  return {
    distFiles: files.dist.length,
    distSha256: files.dist.length > 0 ? digest.digest("hex") : null,
    newestSource: newestSource ? path.relative(packageRoot, newestSource.file) : null,
    oldestBuilt: oldestBuilt ? path.relative(packageRoot, oldestBuilt.file) : null,
    // Sem dist nenhum o script nem roda; com src mais novo que o build, o que roda nao e o que
    // esta no fonte.
    stale:
      files.dist.length === 0 ||
      Boolean(newestSource && oldestBuilt && newestSource.mtimeMs > oldestBuilt.mtimeMs),
  };
}

// Impressao dos arquivos que a cena realmente usa. O .sscene e o .smesh das sinteticas moram em
// build/ e sao invisiveis para o `git status`; os sidecars sao versionados mas editaveis a mao.
// Sem isto, "mesma cena" e uma suposicao pelo caminho do arquivo.
async function fingerprintScene(sceneEntry) {
  const files = [
    sceneEntry.scene,
    sceneEntry.scene.replace(/\.sscene$/, ".smesh"),
    sceneEntry.sidecar,
  ];
  const out = {};
  for (const relative of files) {
    const bytes = await readFile(path.join(smileRoot, relative)).catch(() => null);
    out[relative] = bytes
      ? { bytes: bytes.length, sha256: createHash("sha256").update(bytes).digest("hex") }
      : null;
  }
  return out;
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
    captureOnly,
    capture: doCapture ? { warmupFrames: captureWarmupFrames, preset: "scientific" } : false,
    statistic: "mediana e p95 sobre amostras de frames DISTINTOS (sourceFrameIndex)",
  },
  scenes: scenes.map(({ id, label, kind, scene, sidecar }) => ({ id, label, kind, scene, sidecar })),
  runs: [],
  summary: [],
};

let currentLaunch;
// PID do editor da cena anterior, ja mandado fechar mas talvez ainda vivo.
let pendingExitPid;
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
  const fingerprints = await fingerprintScene(sceneEntry);
  // Esperar aqui, e nao depois de fechar, e o que importa: o perigo e a cena ANTERIOR ainda estar
  // segurando VRAM enquanto esta mede. Depois da ultima cena nao ha ninguem para proteger, e
  // derrubar a rodada por um teardown lento perderia 20 medicoes ja feitas -- que foi o que
  // aconteceu na primeira tentativa, com a 1M levando mais de 180 s para sair.
  if (pendingExitPid !== undefined) {
    await ensureProcessGone(pendingExitPid, sceneEntry.id);
    pendingExitPid = undefined;
  }
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


  report.sceneFingerprints ??= {};
  report.sceneFingerprints[sceneEntry.id] = fingerprints;

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

    let capture;
    if (doCapture) {
      // includeImage: false de proposito — o PNG fica em disco ao lado do manifesto; trazer 20
      // imagens pelo transporte do MCP nao ajuda ninguem.
      capture = await call("smile_capture_frame", {
        bookmarkSlot: slot,
        warmupFrames: captureWarmupFrames,
        preset: "scientific",
        pinTimeOfDay: true,
        timeOfDayHours,
        includeImage: false,
        timeoutSeconds: 900,
      });
      console.log(
        `[baseline] ${sceneEntry.id}/slot${slot}: capturado ` +
          `${capture.pngPath ?? capture.png ?? "(ver manifesto)"}`,
      );
    }

    if (captureOnly) {
      report.captures ??= [];
      report.captures.push({ scene: sceneEntry.id, slot, pose: expected, capture });
      continue;
    }

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
    // Publica no relatorio AQUI, ponto a ponto, e nao no retorno da funcao. Com a publicacao no
    // fim, qualquer excecao entre a ultima medicao e o `return` — o teardown, a auditoria de log,
    // a checagem de resolucao — descartava os quatro pontos detalhados da cena inteira, deixando
    // so a linha reduzida do summary. Foi o que aconteceu com a 1M: 20 summary e 16 runs, sem os
    // tempos de GPU por passe dela, que sao gate da Fase 0. Medido e nao gravado e o pior desfecho
    // possivel, porque o relatorio continua parecendo completo.
    const detailed = { ...row, profileConfiguration: configured, profile, capture };
    sceneRuns.push(detailed);
    report.runs.push(detailed);
    report.summary.push(row);

    console.log(
      `[baseline] ${sceneEntry.id}/slot${slot}: ` +
        `cpu-submissao=${submissionMs.toFixed(3)} ms, draws=${totalDraws}, ` +
        `visiveis=${cpu.scene.visibleItems.median}/${cpu.scene.drawItems.median}, ` +
        `frame=${frame.rawMilliseconds.median.toFixed(3)} ms, ` +
        `srv=${cpu.srvHeap.highWater}/${cpu.srvHeap.capacity}`,
    );
  }

  // No modo so-captura nao ha profiling, entao nao ha resolucao/GPU por ponto para comparar.
  if (captureOnly) {
    await auditLogs(sceneEntry.id, currentLaunch.pid);
    if (!currentLaunch.alreadyRunning) {
      await call("smile_close_editor", { timeoutSeconds: 120 });
      pendingExitPid = currentLaunch.pid;
    }
    currentLaunch = undefined;
    return sceneRuns;
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
    pendingExitPid = currentLaunch.pid;
  }
  currentLaunch = undefined;
  return sceneRuns;
}

async function writeReport() {
  const directory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Benchmarks");
  await mkdir(directory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  // Um ponto medido que nao virou run detalhado e evidencia perdida em silencio: o relatorio
  // continua com a linha no summary e parece completo. Gravado sempre, conferido no caminho verde.
  report.integrity = {
    summaryPoints: report.summary.length,
    detailedRuns: report.runs.length,
    complete: report.summary.length === report.runs.length,
  };
  const suffix = report.passed ? "" : "-FAILED";
  reportPath = path.join(directory, `gpu-driven-baseline${suffix}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  return reportPath;
}

try {
  // O -dirty do carimbo e do momento da COMPILACAO; isto e do momento da MEDICAO, e sao coisas
  // diferentes. Da para compilar limpo e depois mexer numa .sscene ignorada pelo git, num sidecar
  // ou num script, e a rodada sairia afirmando uma limpeza que ja nao existe. Conferido UMA vez,
  // antes de abrir editor nenhum: falhar depois de carregar a 1M custa minutos para dizer algo que
  // ja se sabia.
  report.mcpServer = await fingerprintMcpServer();
  // O script executa `dist/index.js` direto, entao o git limpo e as impressoes de cena NAO veem um
  // bridge obsoleto: foi assim que estas rodadas usaram o close_editor antigo durante horas, com o
  // conserto ja commitado mas nao compilado. O que roda tem de estar na proveniencia.
  // SEM escape por --allow-dirty, ao contrario das outras: arvore suja e um shakedown legitimo,
  // mas dist obsoleto significa medir codigo que nao e o do fonte — nao existe rodada em que isso
  // seja o que se quis fazer, e `npm run build` custa segundos.
  assert(
    !report.mcpServer.stale,
    `o dist/ do MCP esta atras do src/ (${report.mcpServer.newestSource} e mais novo que ` +
      `${report.mcpServer.oldestBuilt}). Rode 'npm run build' — o script executa dist/index.js, ` +
      `entao um bridge obsoleto nao aparece em git status nenhum.`,
  );

  report.gitState = await readGitState();
  // `!== false` deixava passar o `null` de "o git nao respondeu", e um relatorio verde com HEAD
  // desconhecido e pior que um vermelho: ele afirma procedencia que ninguem verificou. Exigir
  // `=== true` E um HEAD valido faz o silencio do git virar falha, que e o que ele e.
  assert(
    allowDirty || report.gitState.workingTreeClean === true,
    report.gitState.workingTreeClean === null
      ? "o git nao respondeu; sem HEAD e sem status nao da para dizer de que arvore esta rodada " +
        "saiu. Resolva, ou use --allow-dirty para uma rodada que NAO vale como baseline."
      : `a arvore esta suja AGORA (${report.gitState.dirtyEntries.length} entrada(s), ex.: ` +
        `${report.gitState.dirtyEntries.slice(0, 3).join(", ")}), embora a build tenha sido ` +
        `compilada limpa. Commite, ou use --allow-dirty para uma rodada que NAO vale como baseline.`,
  );
  assert(
    allowDirty || /^[0-9a-f]{40}$/.test(report.gitState.head ?? ""),
    `HEAD invalido ou ausente ('${report.gitState.head}')`,
  );


  await client.connect(transport);
  for (const sceneEntry of scenes) {
    // measureScene ja publicou cada ponto em report.runs assim que o mediu.
    await measureScene(sceneEntry);
  }
  assert(
    captureOnly || report.runs.length === report.summary.length,
    `relatorio incompleto: ${report.summary.length} pontos no summary contra ` +
      `${report.runs.length} runs detalhados`,
  );
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
      pendingExitPid = currentLaunch.pid;
    }
    // O editor da ULTIMA cena nao tem ninguem depois dele para proteger, mas deixa-lo vivo trava
    // o rebuild seguinte por manter o .exe aberto — foi o que aconteceu aqui. Melhor esforco: nao
    // derruba uma rodada ja concluida.
    if (pendingExitPid !== undefined) {
      await ensureProcessGone(pendingExitPid, "final", 60_000);
      pendingExitPid = undefined;
    }
  } catch (cleanupError) {
    console.error(`[baseline] cleanup: ${cleanupError}`);
  }
  await client.close();
}
