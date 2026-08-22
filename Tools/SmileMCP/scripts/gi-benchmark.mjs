import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const warmupSeconds = 15;
const samples = 60;
const intervalMs = 250;
const settleMilliseconds = 4_000;
const movementStepMeters = 0.25;
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function modulo(value, divisor) {
  return ((value % divisor) + divisor) % divisor;
}

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
const client = new Client({ name: "smile-gi-benchmark", version: "0.4.0" });

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

async function waitForGI(predicate, timeoutMilliseconds = 90_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  let last;
  while (Date.now() < deadline) {
    last = await call("smile_gi_status", { timeoutSeconds: 5 });
    if (predicate(last)) return last;
    await delay(100);
  }
  throw new Error(`GI nao atingiu o estado esperado; ultimo=${JSON.stringify(last)}`);
}

function samePolicy(status, expected) {
  return status.policy?.primary?.requested === expected.primaryRequested &&
    status.policy?.primary?.effective === expected.primaryEffective &&
    status.policy?.fallback?.requested === expected.fallbackRequested &&
    status.policy?.fallback?.effective === expected.fallbackEffective;
}

function passOf(run, name, queue) {
  const pass = run.profile.passes.find((candidate) =>
    candidate.name === name && candidate.queue === queue);
  assert(pass, `${run.id}: passe '${name}' (${queue}) ausente`);
  return pass;
}

function average(values) {
  const finite = values.filter(Number.isFinite);
  assert(finite.length > 0, "media sem valores finitos");
  return finite.reduce((sum, value) => sum + value, 0) / finite.length;
}

function averagePass(runs, name, queue, statistic) {
  return average(runs.map((run) => passOf(run, name, queue).rawMilliseconds[statistic]));
}

function percentDelta(value, baseline) {
  return baseline !== 0 ? ((value - baseline) / baseline) * 100 : null;
}

const cascadeSequence = [
  {
    id: "c1_a",
    group: "cascade_cycle",
    config: { ddgiEnabled: true, indirectPrimary: "ddgi", indirectFallback: "ddgi", cascadeCount: 1 },
    expected: { primaryRequested: "ddgi", primaryEffective: "ddgi", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "c2_a",
    group: "cascade_cycle",
    config: { ddgiEnabled: true, indirectPrimary: "ddgi", indirectFallback: "ddgi", cascadeCount: 2 },
    expected: { primaryRequested: "ddgi", primaryEffective: "ddgi", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "c1_b",
    group: "cascade_cycle",
    config: { ddgiEnabled: true, indirectPrimary: "ddgi", indirectFallback: "ddgi", cascadeCount: 1 },
    expected: { primaryRequested: "ddgi", primaryEffective: "ddgi", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "c2_b",
    group: "cascade_cycle",
    config: { ddgiEnabled: true, indirectPrimary: "ddgi", indirectFallback: "ddgi", cascadeCount: 2 },
    expected: { primaryRequested: "ddgi", primaryEffective: "ddgi", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "c2_moving",
    group: "movement",
    moving: true,
    config: { ddgiEnabled: true, indirectPrimary: "ddgi", indirectFallback: "ddgi", cascadeCount: 2 },
    expected: { primaryRequested: "ddgi", primaryEffective: "ddgi", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "sharc_ddgi",
    group: "policy",
    config: { ddgiEnabled: true, indirectPrimary: "restir_sharc", indirectFallback: "ddgi", cascadeCount: 2 },
    expected: { primaryRequested: "restir_sharc", primaryEffective: "restir_sharc", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "sharc_black",
    group: "policy",
    config: { ddgiEnabled: true, indirectPrimary: "restir_sharc", indirectFallback: "black", cascadeCount: 2 },
    expected: { primaryRequested: "restir_sharc", primaryEffective: "restir_sharc", fallbackRequested: "black", fallbackEffective: "black" },
  },
  {
    id: "surface_off",
    group: "policy",
    config: { ddgiEnabled: true, indirectPrimary: "off", indirectFallback: "ddgi", cascadeCount: 2 },
    expected: { primaryRequested: "off", primaryEffective: "off", fallbackRequested: "ddgi", fallbackEffective: "ddgi" },
  },
  {
    id: "all_off",
    group: "policy",
    config: { ddgiEnabled: false, indirectPrimary: "off", indirectFallback: "black", cascadeCount: 2 },
    expected: { primaryRequested: "off", primaryEffective: "off", fallbackRequested: "black", fallbackEffective: "black" },
  },
];

const adaptiveExpected = {
  primaryRequested: "ddgi",
  primaryEffective: "ddgi",
  fallbackRequested: "ddgi",
  fallbackEffective: "ddgi",
};
const adaptiveSequence = [
  { id: "adaptive_off_a", group: "stationary", adaptiveRays: false },
  { id: "adaptive_on_a", group: "stationary", adaptiveRays: true },
  { id: "adaptive_off_b", group: "stationary", adaptiveRays: false },
  { id: "adaptive_on_b", group: "stationary", adaptiveRays: true },
  { id: "moving_off_a", group: "movement", adaptiveRays: false, moving: true },
  { id: "moving_on_a", group: "movement", adaptiveRays: true, moving: true },
  { id: "moving_off_b", group: "movement", adaptiveRays: false, moving: true },
  { id: "moving_on_b", group: "movement", adaptiveRays: true, moving: true },
].map((entry) => ({
  ...entry,
  config: {
    ddgiEnabled: true,
    indirectPrimary: "ddgi",
    indirectFallback: "ddgi",
    cascadeCount: 2,
    adaptiveRays: entry.adaptiveRays,
  },
  expected: adaptiveExpected,
}));

const suiteArgument = process.argv.indexOf("--suite");
const suite = suiteArgument >= 0 ? process.argv[suiteArgument + 1] : "cascades";
assert(suite === "cascades" || suite === "adaptive",
  `suite invalida '${suite}'; use cascades ou adaptive`);
const sequence = suite === "adaptive" ? adaptiveSequence : cascadeSequence;

let report = {
  schema: 1,
  suite,
  startedAt: new Date().toISOString(),
  scenePath,
  regime: {
    preset: "gameplay_rr",
    timeOfDayHours: 10,
    backgroundThrottle: false,
    warmupSeconds,
    samples,
    intervalMs,
    statistic: "timestamps GPU brutos; mediana e p95 por rodada",
    ...(suite === "adaptive" ? {
      adaptiveRays: {
        off: "64 raios por sonda",
        on: "16-64 raios por sonda, classificados pela proximidade de geometria",
        effectiveDistributionTelemetry: false,
        note: "a distribuicao vive em buffer GPU; a regua nao faz readback para evitar stall",
      },
    } : {}),
    movement: {
      cameraCut: false,
      axis: "world_x",
      stepMeters: movementStepMeters,
      plannedDistanceMeters: movementStepMeters * samples,
    },
  },
  sequence: sequence.map(({ id }) => id),
  runs: [],
  comparisons: {},
  logAudit: {},
};

let launch;
let originalCamera;
let originalGI;
let reportPath;

const resumeArgument = process.argv.indexOf("--resume");
if (resumeArgument >= 0) {
  const resumePath = process.argv[resumeArgument + 1];
  assert(resumePath, "--resume exige o caminho de um relatorio parcial");
  const resolvedResumePath = path.resolve(resumePath);
  report = JSON.parse(await readFile(resolvedResumePath, "utf8"));
  report.resumedFrom = resolvedResumePath;
}

async function setCamera(pose, cameraCut) {
  return call("smile_camera_set", {
    x: pose.position.x,
    y: pose.position.y,
    z: pose.position.z,
    pitchDegrees: pose.pitchDegrees,
    yawDegrees: pose.yawDegrees,
    cameraCut,
  });
}

async function runBenchmarkCase(benchmarkCase) {
  console.log(`[gi-benchmark] ${benchmarkCase.id}: configurando`);
  await setCamera(originalCamera, true);
  const profileConfiguration = await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
    backgroundThrottle: false,
  });
  assert(profileConfiguration.backgroundThrottle === false,
    `${benchmarkCase.id}: throttle de segundo plano nao foi desativado`);
  const applied = await call("smile_gi_configure", {
    ...benchmarkCase.config,
    adaptiveRays: benchmarkCase.config.adaptiveRays ?? false,
    adaptiveHysteresis: false,
    timeoutSeconds: 120,
  });
  const expectedAdaptiveRays = benchmarkCase.config.adaptiveRays ?? false;
  const status = await waitForGI((candidate) => {
    const volumeMatches = benchmarkCase.config.ddgiEnabled
      ? candidate.ddgi?.enabled === true && candidate.ddgi?.initialized === true &&
        candidate.ddgi?.desiredCascadeCount === benchmarkCase.config.cascadeCount &&
        candidate.ddgi?.actualCascadeCount === benchmarkCase.config.cascadeCount
      : candidate.ddgi?.enabled === false;
    return volumeMatches && samePolicy(candidate, benchmarkCase.expected) &&
      candidate.ddgi?.adaptiveRays === expectedAdaptiveRays &&
      candidate.frameIndex >= applied.frameIndex + 2;
  });
  if (suite === "adaptive") {
    assert(status.ddgi.raysPerProbe === 64,
      `${benchmarkCase.id}: largura do trace ${status.ddgi.raysPerProbe}, esperado 64`);
    assert(status.ddgi.adaptiveMinRays === 16 && status.ddgi.adaptiveMaxRays === 64,
      `${benchmarkCase.id}: limites adaptativos ${status.ddgi.adaptiveMinRays}-${status.ddgi.adaptiveMaxRays}, esperado 16-64`);
  }
  await delay(settleMilliseconds);

  let profile;
  let movement;
  if (benchmarkCase.moving) {
    const before = await call("smile_gi_status", { timeoutSeconds: 5 });
    const profilePromise = call("smile_profile_gpu", {
      warmupSeconds,
      samples,
      intervalMs,
      includeSamples: false,
    });
    await delay(warmupSeconds * 1_000);
    const movementStartedAt = Date.now();
    let lastCamera = originalCamera;
    for (let index = 1; index <= samples; index += 1) {
      const deadline = movementStartedAt + (index - 1) * intervalMs;
      await delay(Math.max(0, deadline - Date.now()));
      lastCamera = await call("smile_camera_set", {
        x: originalCamera.position.x + index * movementStepMeters,
        y: originalCamera.position.y,
        z: originalCamera.position.z,
        pitchDegrees: originalCamera.pitchDegrees,
        yawDegrees: originalCamera.yawDegrees,
        cameraCut: false,
      });
    }
    profile = await profilePromise;
    const after = await call("smile_gi_status", { timeoutSeconds: 5 });
    const fineBefore = before.ddgi.cascades[0];
    const fineAfter = after.ddgi.cascades[0];
    const coarseBefore = before.ddgi.cascades[1];
    const coarseAfter = after.ddgi.cascades[1];
    const fineCells = Math.round((fineAfter.gridMin.x - fineBefore.gridMin.x) / fineBefore.spacing);
    const coarseCells = Math.round((coarseAfter.gridMin.x - coarseBefore.gridMin.x) / coarseBefore.spacing);
    const fineScrollDelta = modulo(
      fineAfter.scrollCells.x - fineBefore.scrollCells.x,
      after.ddgi.gridCount.x,
    );
    const coarseScrollDelta = modulo(
      coarseAfter.scrollCells.x - coarseBefore.scrollCells.x,
      after.ddgi.gridCount.x,
    );
    assert(fineCells > 0, `${benchmarkCase.id}: percurso nao cruzou celula fina`);
    assert(fineScrollDelta === modulo(fineCells, after.ddgi.gridCount.x),
      `${benchmarkCase.id}: scroll fino ${fineScrollDelta}, esperado ${fineCells}`);
    assert(coarseScrollDelta === modulo(coarseCells, after.ddgi.gridCount.x),
      `${benchmarkCase.id}: scroll grosso ${coarseScrollDelta}, esperado ${coarseCells}`);
    movement = {
      before,
      after,
      lastCamera,
      durationMilliseconds: Date.now() - movementStartedAt,
      distanceMeters: movementStepMeters * samples,
      fineCells,
      fineScrollDelta,
      coarseCells,
      coarseScrollDelta,
    };
  } else {
    profile = await call("smile_profile_gpu", {
      warmupSeconds,
      samples,
      intervalMs,
      includeSamples: false,
    });
  }

  assert(profile.sampleCount === samples,
    `${benchmarkCase.id}: recebeu ${profile.sampleCount}/${samples} amostras`);
  assert(profile.settings?.cacheStats === false &&
    profile.settings?.cacheStatsDetail === false &&
    profile.settings?.cacheStatsSource === false,
  `${benchmarkCase.id}: instrumentacao do cache ficou ligada`);
  passOf({ id: benchmarkCase.id, profile }, "Frame (GPU)", "direct");
  if (benchmarkCase.config.ddgiEnabled) {
    passOf({ id: benchmarkCase.id, profile }, "DDGI", "asyncCompute");
  }

  const result = {
    id: benchmarkCase.id,
    group: benchmarkCase.group,
    config: benchmarkCase.config,
    expected: benchmarkCase.expected,
    profileConfiguration,
    status,
    profile,
    ...(movement ? { movement } : {}),
  };
  const frame = passOf(result, "Frame (GPU)", "direct").rawMilliseconds;
  console.log(`[gi-benchmark] ${benchmarkCase.id}: frame med=${frame.median.toFixed(3)} ms, p95=${frame.p95.toFixed(3)} ms`);
  return result;
}

function buildCascadeComparisons(runs) {
  const byId = new Map(runs.map((run) => [run.id, run]));
  const one = [byId.get("c1_a"), byId.get("c1_b")];
  const two = [byId.get("c2_a"), byId.get("c2_b")];
  assert(one.every(Boolean) && two.every(Boolean), "ciclo 1->2->1->2 incompleto");

  const metricDefinitions = [
    ["frame", "Frame (GPU)", "direct"],
    ["ddgiAsync", "DDGI", "asyncCompute"],
    ["ddgiWait", "Espera do DDGI", "direct"],
  ];
  const cascadeMetrics = Object.fromEntries(metricDefinitions.map(([key, name, queue]) => {
    const c1Median = averagePass(one, name, queue, "median");
    const c2Median = averagePass(two, name, queue, "median");
    const c1P95 = averagePass(one, name, queue, "p95");
    const c2P95 = averagePass(two, name, queue, "p95");
    return [key, {
      name,
      queue,
      c1MedianMs: c1Median,
      c2MedianMs: c2Median,
      medianDeltaMs: c2Median - c1Median,
      medianDeltaPercent: percentDelta(c2Median, c1Median),
      c1P95Ms: c1P95,
      c2P95Ms: c2P95,
      p95DeltaMs: c2P95 - c1P95,
      p95DeltaPercent: percentDelta(c2P95, c1P95),
    }];
  }));
  const c1Vram = average(one.map((run) => run.profile.localVramBytes.median));
  const c2Vram = average(two.map((run) => run.profile.localVramBytes.median));
  const c1Drift = percentDelta(
    passOf(one[1], "Frame (GPU)", "direct").rawMilliseconds.median,
    passOf(one[0], "Frame (GPU)", "direct").rawMilliseconds.median,
  );
  const c2Drift = percentDelta(
    passOf(two[1], "Frame (GPU)", "direct").rawMilliseconds.median,
    passOf(two[0], "Frame (GPU)", "direct").rawMilliseconds.median,
  );

  const moving = byId.get("c2_moving");
  const movingMetrics = Object.fromEntries(metricDefinitions.map(([key, name, queue]) => {
    const stationaryMedian = averagePass(two, name, queue, "median");
    const movingMedian = passOf(moving, name, queue).rawMilliseconds.median;
    const stationaryP95 = averagePass(two, name, queue, "p95");
    const movingP95 = passOf(moving, name, queue).rawMilliseconds.p95;
    return [key, {
      name,
      queue,
      stationaryMedianMs: stationaryMedian,
      movingMedianMs: movingMedian,
      medianDeltaMs: movingMedian - stationaryMedian,
      medianDeltaPercent: percentDelta(movingMedian, stationaryMedian),
      stationaryP95Ms: stationaryP95,
      movingP95Ms: movingP95,
      p95DeltaMs: movingP95 - stationaryP95,
      p95DeltaPercent: percentDelta(movingP95, stationaryP95),
    }];
  }));

  const allOff = byId.get("all_off");
  const policyRuns = {
    ddgi_primary: two,
    sharc_ddgi: [byId.get("sharc_ddgi")],
    sharc_black: [byId.get("sharc_black")],
    surface_off: [byId.get("surface_off")],
    all_off: [allOff],
  };
  const allOffFrame = passOf(allOff, "Frame (GPU)", "direct").rawMilliseconds.median;
  const policy = Object.fromEntries(Object.entries(policyRuns).map(([key, entries]) => {
    const frameMedian = averagePass(entries, "Frame (GPU)", "direct", "median");
    const frameP95 = averagePass(entries, "Frame (GPU)", "direct", "p95");
    return [key, {
      frameMedianMs: frameMedian,
      frameP95Ms: frameP95,
      versusAllOffMs: frameMedian - allOffFrame,
      versusAllOffPercent: percentDelta(frameMedian, allOffFrame),
      localVramMedianBytes: average(entries.map((run) => run.profile.localVramBytes.median)),
    }];
  }));

  return {
    cascadeCycle: {
      sequence: ["c1_a", "c2_a", "c1_b", "c2_b"],
      c1FrameDriftPercent: c1Drift,
      c2FrameDriftPercent: c2Drift,
      c1LocalVramMedianBytes: c1Vram,
      c2LocalVramMedianBytes: c2Vram,
      localVramDeltaBytes: c2Vram - c1Vram,
      metrics: cascadeMetrics,
    },
    movingTwoCascades: {
      movement: moving.movement,
      metrics: movingMetrics,
    },
    policy,
  };
}

function buildAdaptiveComparisons(runs) {
  const byId = new Map(runs.map((run) => [run.id, run]));
  const groups = {
    stationary: {
      off: [byId.get("adaptive_off_a"), byId.get("adaptive_off_b")],
      on: [byId.get("adaptive_on_a"), byId.get("adaptive_on_b")],
      sequence: ["adaptive_off_a", "adaptive_on_a", "adaptive_off_b", "adaptive_on_b"],
    },
    movement: {
      off: [byId.get("moving_off_a"), byId.get("moving_off_b")],
      on: [byId.get("moving_on_a"), byId.get("moving_on_b")],
      sequence: ["moving_off_a", "moving_on_a", "moving_off_b", "moving_on_b"],
    },
  };
  assert(Object.values(groups).every((group) =>
    [...group.off, ...group.on].every(Boolean)), "ciclos adaptativos incompletos");

  const metricDefinitions = [
    ["frame", "Frame (GPU)", "direct"],
    ["ddgiAsync", "DDGI", "asyncCompute"],
    ["ddgiWait", "Espera do DDGI", "direct"],
  ];
  const compareGroup = (group) => {
    const metrics = Object.fromEntries(metricDefinitions.map(([key, name, queue]) => {
      const offMedian = averagePass(group.off, name, queue, "median");
      const onMedian = averagePass(group.on, name, queue, "median");
      const offP95 = averagePass(group.off, name, queue, "p95");
      const onP95 = averagePass(group.on, name, queue, "p95");
      return [key, {
        name,
        queue,
        offMedianMs: offMedian,
        onMedianMs: onMedian,
        medianSavedMs: offMedian - onMedian,
        medianSavedPercent: offMedian !== 0 ? ((offMedian - onMedian) / offMedian) * 100 : null,
        offP95Ms: offP95,
        onP95Ms: onP95,
        p95SavedMs: offP95 - onP95,
        p95SavedPercent: offP95 !== 0 ? ((offP95 - onP95) / offP95) * 100 : null,
        offRepeatDriftPercent: percentDelta(
          passOf(group.off[1], name, queue).rawMilliseconds.median,
          passOf(group.off[0], name, queue).rawMilliseconds.median,
        ),
        onRepeatDriftPercent: percentDelta(
          passOf(group.on[1], name, queue).rawMilliseconds.median,
          passOf(group.on[0], name, queue).rawMilliseconds.median,
        ),
        pairedMedianSavedMs: group.off.map((offRun, index) =>
          passOf(offRun, name, queue).rawMilliseconds.median -
          passOf(group.on[index], name, queue).rawMilliseconds.median),
      }];
    }));
    return {
      sequence: group.sequence,
      metrics,
      frameSavingPositive: metrics.frame.medianSavedMs > 0,
      ddgiSavingPositive: metrics.ddgiAsync.medianSavedMs > 0,
      allFramePairsPositive: metrics.frame.pairedMedianSavedMs.every((value) => value > 0),
      allDdgiPairsPositive: metrics.ddgiAsync.pairedMedianSavedMs.every((value) => value > 0),
      frameRepeatDriftWithinFivePercent:
        Math.abs(metrics.frame.offRepeatDriftPercent) <= 5 &&
        Math.abs(metrics.frame.onRepeatDriftPercent) <= 5,
      ddgiRepeatDriftWithinTenPercent:
        Math.abs(metrics.ddgiAsync.offRepeatDriftPercent) <= 10 &&
        Math.abs(metrics.ddgiAsync.onRepeatDriftPercent) <= 10,
    };
  };

  const stationary = compareGroup(groups.stationary);
  const movement = compareGroup(groups.movement);
  const movementRuns = [...groups.movement.off, ...groups.movement.on];
  return {
    stationary,
    movement: {
      ...movement,
      paths: movementRuns.map((run) => ({
        id: run.id,
        distanceMeters: run.movement.distanceMeters,
        fineCells: run.movement.fineCells,
        coarseCells: run.movement.coarseCells,
      })),
    },
    verdict: {
      adaptiveRaysReducesDdgiCost:
        stationary.ddgiSavingPositive && movement.ddgiSavingPositive,
      adaptiveRaysReducesFrameCost:
        stationary.frameSavingPositive && movement.frameSavingPositive,
      frameRepetitionStable:
        stationary.frameRepeatDriftWithinFivePercent &&
        movement.frameRepeatDriftWithinFivePercent,
      ddgiRepetitionStable:
        stationary.ddgiRepeatDriftWithinTenPercent &&
        movement.ddgiRepeatDriftWithinTenPercent,
    },
  };
}

try {
  await client.connect(transport);
  if (resumeArgument < 0) {
    launch = await call("smile_run_editor", {
      configuration: "Release",
      scenePath,
      waitUntilReady: true,
      startupTimeoutSeconds: 180,
    });
    report.editor = launch;
    console.log(`[gi-benchmark] editor PID ${launch.pid}; cena=${scenePath}`);

    originalCamera = await call("smile_camera_get");
    originalGI = await call("smile_gi_status");
    report.baseline = { camera: originalCamera, gi: originalGI };

    for (const benchmarkCase of sequence) {
      report.runs.push(await runBenchmarkCase(benchmarkCase));
    }
  } else {
    assert(report.runs?.length === sequence.length,
      `relatorio parcial tem ${report.runs?.length ?? 0}/${sequence.length} bracos`);
    console.log(`[gi-benchmark] retomando ${report.runs.length} bracos ja medidos`);
  }
  report.comparisons = suite === "adaptive"
    ? buildAdaptiveComparisons(report.runs)
    : buildCascadeComparisons(report.runs);

  const resolutions = new Set(report.runs.map((run) =>
    JSON.stringify(run.profile.resolution)));
  const gpus = new Set(report.runs.map((run) => run.profile.gpu));
  assert(resolutions.size === 1, "resolucao variou entre os bracos");
  assert(gpus.size === 1, "GPU variou entre os bracos");

  const logs = await call("smile_get_latest_logs", { sessions: 5, tailLines: 1000 });
  const benchmarkPid = report.editor?.pid;
  const logSession = logs.sessions?.find((session) =>
    typeof session.path === "string" &&
    new RegExp(`-p${benchmarkPid}\\.log$`, "i").test(session.path));
  const logLines = logSession?.lines ?? [];
  assert(Number.isInteger(benchmarkPid) && logSession && logLines.length > 0,
    `auditoria nao encontrou o log persistente do editor PID ${benchmarkPid}`);
  const fatalPatterns = [
    /D3D12.*ERROR/i,
    /DXGI_ERROR/i,
    /DEVICE_REMOVED/i,
    /GPU validation.*error/i,
    /\[ERR\s*\]/i,
  ];
  const fatalMatches = logLines.filter((line) =>
    fatalPatterns.some((pattern) => pattern.test(line)));
  assert(fatalMatches.length === 0,
    `log contem erro de GPU/API: ${fatalMatches.join(", ")}`);
  report.logAudit = {
    passed: true,
    editorPid: benchmarkPid,
    logPath: logSession.path,
    fatalMatches,
  };
  report.finishedAt = new Date().toISOString();
  report.passed = true;
  delete report.error;

  const reportDirectory = path.join(
    smileRoot, "build", "bin", "Release", "Captures", "Benchmarks",
  );
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportPrefix = suite === "adaptive" ? "gi-adaptive-rays" : "gi-cascades";
  reportPath = path.join(reportDirectory, `${reportPrefix}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-benchmark] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-benchmark] FAIL: ${report.error}`);
  try {
    const reportDirectory = path.join(
      smileRoot, "build", "bin", "Release", "Captures", "Benchmarks",
    );
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    const reportPrefix = suite === "adaptive" ? "gi-adaptive-rays" : "gi-cascades";
    reportPath = path.join(reportDirectory, `${reportPrefix}-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
    console.error(`[gi-benchmark] relatorio parcial: ${reportPath}`);
  } catch (reportError) {
    console.error(`[gi-benchmark] relatorio de falha: ${reportError}`);
  }
  process.exitCode = 1;
} finally {
  try {
    const editorStatus = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (editorStatus.connected && !editorStatus.captureBusy && originalCamera) {
      await setCamera(originalCamera, true);
    }
    if (editorStatus.connected && !editorStatus.captureBusy && originalGI) {
      await call("smile_gi_configure", {
        ddgiEnabled: originalGI.ddgi.enabled,
        indirectPrimary: originalGI.policy.primary.requested,
        indirectFallback: originalGI.policy.fallback.requested,
        cascadeCount: originalGI.ddgi.desiredCascadeCount,
        adaptiveRays: originalGI.ddgi.adaptiveRays,
        adaptiveHysteresis: originalGI.ddgi.adaptiveHysteresis,
        timeoutSeconds: 120,
      });
    }
    if (editorStatus.connected && launch && !launch.alreadyRunning) {
      await call("smile_close_editor", { timeoutSeconds: 60 });
    }
  } catch (cleanupError) {
    console.error(`[gi-benchmark] cleanup: ${cleanupError}`);
  }
  await client.close();
}
