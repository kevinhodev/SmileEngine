import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const reportDirectory = path.join(
  smileRoot, "build", "bin", "Release", "Captures", "Validation",
);
const scenes = [
  { id: "bistro", path: "Assets/Scenes/Bistro/BistroExterior.sscene" },
  { id: "emerald", path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene" },
];
const sequence = [
  { id: "off_a", probeCompaction: false },
  { id: "on_a", probeCompaction: true },
  { id: "off_b", probeCompaction: false },
  { id: "on_b", probeCompaction: true },
];
const startupSmokeOnly = process.argv.includes("--startup-smoke");
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function parseResult(result, toolName) {
  const text = result.content.find((part) => part.type === "text")?.text ?? "";
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
const client = new Client({ name: "smile-gi-probe-compaction", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const sdkTimeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(
    await client.callTool(
      { name: toolName, arguments: args },
      undefined,
      { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
    ),
    toolName,
  );
}

async function waitForGI(predicate, timeoutMilliseconds = 120_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  let last;
  while (Date.now() < deadline) {
    last = await call("smile_gi_status", { timeoutSeconds: 5 });
    if (predicate(last)) return last;
    await delay(50);
  }
  throw new Error(`GI nao atingiu o estado esperado; ultimo=${JSON.stringify(last)}`);
}

function profilePass(profile, name) {
  return profile.passes.find((entry) => entry.name === name) ?? null;
}

function passRawMean(profile, name) {
  return Number(profilePass(profile, name)?.rawMilliseconds?.mean ?? Number.NaN);
}

function mean(values) {
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}

async function settleCompaction(enabled, appliedSerial) {
  return waitForGI((status) => {
    const ddgi = status.ddgi;
    if (!ddgi || ddgi.probeCompaction !== enabled) return false;
    if (ddgi.updateSerial < appliedSerial + (enabled ? 8 : 2)) return false;
    if (ddgi.probeCompactionEffective !== enabled) return false;
    if (!enabled) return true;
    return ddgi.compactedProbeCapacity > 0 && ddgi.activeProbeCount > 0 &&
      ddgi.activeProbeCount <= ddgi.compactedProbeCapacity;
  });
}

async function sampleCompactionStatus(count = 24) {
  const unique = new Map();
  const deadline = Date.now() + 30_000;
  while (unique.size < count && Date.now() < deadline) {
    const status = await call("smile_gi_status", { timeoutSeconds: 5 });
    unique.set(status.ddgi.updateSerial, status.ddgi);
    await delay(15);
  }
  assert(unique.size >= count, `telemetria insuficiente: ${unique.size}/${count} updates`);
  return [...unique.values()];
}

async function profileDDGI(label) {
  let profile;
  const attempts = [];
  for (let attempt = 1; attempt <= 3; ++attempt) {
    profile = await call("smile_profile_gpu", {
      warmupSeconds: 2,
      samples: 72,
      intervalMs: 125,
      includeSamples: false,
    });
    const passNames = profile.passes.map((entry) => entry.name);
    attempts.push({ attempt, firstFrameIndex: profile.firstFrameIndex,
      lastFrameIndex: profile.lastFrameIndex, passNames });
    if (passNames.includes("DDGI") && passNames.includes("DDGI (fine)") &&
        passNames.includes("DDGI (fine+coarse)")) break;
    await delay(500);
  }
  assert(profilePass(profile, "DDGI"), `${label}: scope DDGI ausente`);
  assert(profilePass(profile, "DDGI (fine)"), `${label}: scope DDGI fine ausente`);
  assert(profilePass(profile, "DDGI (fine+coarse)"),
    `${label}: scope DDGI fine+coarse ausente`);
  return { profile, attempts };
}

function performanceDecision(entries) {
  const offEntries = entries.filter((entry) => !entry.probeCompaction);
  const onEntries = entries.filter((entry) => entry.probeCompaction);
  const offDDGI = mean(offEntries.map((entry) => passRawMean(entry.profile, "DDGI")));
  const onDDGI = mean(onEntries.map((entry) => passRawMean(entry.profile, "DDGI")));
  const offFrame = mean(offEntries.map((entry) => passRawMean(entry.profile, "Frame (GPU)")));
  const onFrame = mean(onEntries.map((entry) => passRawMean(entry.profile, "Frame (GPU)")));
  const activeSamples = onEntries.flatMap((entry) => entry.telemetry)
    .filter((sample) => sample.probeCompactionEffective && sample.compactedProbeCapacity > 0);
  const activeRatio = activeSamples.length > 0
    ? mean(activeSamples.map((sample) => sample.activeProbeCount / sample.compactedProbeCapacity))
    : Number.NaN;
  return {
    offDDGIMeanMilliseconds: offDDGI,
    onDDGIMeanMilliseconds: onDDGI,
    ddgiSavedMilliseconds: offDDGI - onDDGI,
    ddgiSavedPercent: (offDDGI - onDDGI) * 100 / offDDGI,
    offFrameMeanMilliseconds: offFrame,
    onFrameMeanMilliseconds: onFrame,
    frameSavedMilliseconds: offFrame - onFrame,
    frameSavedPercent: (offFrame - onFrame) * 100 / offFrame,
    meanActiveProbeRatio: activeRatio,
    meanCulledProbePercent: (1 - activeRatio) * 100,
    wakeObservedInEveryOnProfile: onEntries.every((entry) => entry.wakeObservedDuringProfile),
    performanceSupportsDefault: onDDGI < offDDGI * 0.97,
  };
}

async function auditLogs(editorPid) {
  const logs = await call("smile_get_latest_logs", { sessions: 5, tailLines: 1000 });
  const session = logs.sessions?.find((entry) => typeof entry.path === "string" &&
    new RegExp(`-p${editorPid}\\.log$`, "i").test(entry.path));
  const fatalPatterns = [/D3D12.*ERROR/i, /DXGI_ERROR/i, /DEVICE_REMOVED/i,
    /GPU validation.*error/i, /\[ERR\s*\]/i];
  const fatalMatches = (session?.lines ?? []).filter((line) =>
    fatalPatterns.some((pattern) => pattern.test(line)));
  assert(fatalMatches.length === 0, `log contem erro GPU/API: ${fatalMatches.join(", ")}`);
  return session
    ? { passed: true, available: true, editorPid, logPath: session.path, fatalMatches }
    : { passed: null, available: false, editorPid,
        reason: "log persistente da sessao nao encontrado", fatalMatches: [] };
}

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  startupSmokeOnly,
  scenes: [],
  regime: {
    scenePaths: scenes.map((scene) => scene.path),
    cascadeCount: 2,
    adaptiveRays: true,
    adaptiveHysteresis: false,
    interleavedUpdates: true,
    policy: { primary: "ddgi", fallback: "ddgi" },
    performance: { samples: 72, intervalMs: 125, warmupSeconds: 2,
      includesPeriodicWakeCost: true },
    visual: { preset: "scientific", warmupFrames: 128,
      sequence: sequence.map((entry) => entry.id) },
  },
};
let currentLaunch;
let reportPath;

async function runScene(scene) {
  const sceneReport = {
    id: scene.id,
    scenePath: scene.path,
    performance: [],
    captures: [],
  };
  report.scenes.push(sceneReport);
  currentLaunch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: scene.path,
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  sceneReport.editor = currentLaunch;
  console.log(`[gi-probe-compaction] ${scene.id}: editor PID ${currentLaunch.pid}`);
  const startup = await call("smile_gi_status");
  sceneReport.startup = startup;
  assert(startup.ddgi?.probeCompaction === true,
    `${scene.id}: compactacao nao nasceu ligada por default`);
  if (startupSmokeOnly) {
    sceneReport.startupEffective = await waitForGI((status) =>
      status.ddgi?.probeCompaction === true &&
      status.ddgi.probeCompactionEffective === true &&
      status.ddgi.activeProbeCount > 0 &&
      status.ddgi.activeProbeCount < status.ddgi.compactedProbeCapacity);
    sceneReport.passed = true;
    await call("smile_close_editor", { timeoutSeconds: 60 });
    currentLaunch = undefined;
    return;
  }

  await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
    backgroundThrottle: false,
  });
  const initialized = await call("smile_gi_configure", {
    ddgiEnabled: true,
    indirectPrimary: "ddgi",
    indirectFallback: "ddgi",
    cascadeCount: 2,
    adaptiveRays: true,
    adaptiveHysteresis: false,
    interleavedUpdates: true,
    probeCompaction: false,
    timeoutSeconds: 120,
  });
  await delay(8_000);
  const baseline = await waitForGI((status) => status.ddgi?.initialized &&
    status.ddgi.actualCascadeCount === 2 &&
    status.ddgi.updateSerial >= initialized.ddgi.updateSerial + 180);
  sceneReport.baseline = baseline;

  for (const benchmarkCase of sequence) {
    console.log(`[gi-probe-compaction] ${scene.id}: perfil ${benchmarkCase.id}`);
    const applied = await call("smile_gi_configure", {
      probeCompaction: benchmarkCase.probeCompaction,
    });
    const steady = await settleCompaction(
      benchmarkCase.probeCompaction, applied.ddgi.updateSerial,
    );
    const telemetry = await sampleCompactionStatus();
    if (benchmarkCase.probeCompaction) {
      assert(telemetry.some((sample) => sample.activeProbeCount < sample.compactedProbeCapacity),
        `${scene.id}/${benchmarkCase.id}: nenhuma probe foi compactada`);
    }
    const beforeProfile = await call("smile_gi_status");
    const { profile, attempts } = await profileDDGI(`${scene.id}/${benchmarkCase.id}`);
    let afterProfile = await call("smile_gi_status");
    if (benchmarkCase.probeCompaction &&
        afterProfile.ddgi.lastProbeWakeSerial <= beforeProfile.ddgi.lastProbeWakeSerial) {
      afterProfile = await waitForGI((status) =>
        status.ddgi.lastProbeWakeSerial > beforeProfile.ddgi.lastProbeWakeSerial, 30_000);
    }
    const wakeObservedDuringProfile = benchmarkCase.probeCompaction &&
      afterProfile.ddgi.lastProbeWakeSerial > beforeProfile.ddgi.lastProbeWakeSerial;
    sceneReport.performance.push({ ...benchmarkCase, steady, telemetry, attempts,
      beforeProfile, afterProfile, wakeObservedDuringProfile, profile });
  }
  sceneReport.performanceDecision = performanceDecision(sceneReport.performance);

  for (const captureCase of sequence) {
    console.log(`[gi-probe-compaction] ${scene.id}: visual ${captureCase.id}`);
    const applied = await call("smile_gi_configure", {
      probeCompaction: captureCase.probeCompaction,
    });
    await settleCompaction(captureCase.probeCompaction, applied.ddgi.updateSerial);
    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1,
      warmupFrames: 128,
      preset: "scientific",
      pinTimeOfDay: true,
      timeOfDayHours: 10,
      timeoutSeconds: 600,
      includeImage: false,
    });
    assert(capture.manifest.ddgiCascadeCount === 2,
      `${scene.id}/${captureCase.id}: manifesto sem duas cascatas`);
    assert(capture.manifest.ddgiAdaptiveRays === true,
      `${scene.id}/${captureCase.id}: adaptive rays desligado no manifesto`);
    assert(capture.manifest.ddgiInterleavedUpdates === true,
      `${scene.id}/${captureCase.id}: interleaved desligado no manifesto`);
    assert(capture.manifest.ddgiProbeCompaction === captureCase.probeCompaction,
      `${scene.id}/${captureCase.id}: probeCompaction incorreto no manifesto`);
    sceneReport.captures.push({ ...captureCase, pngPath: capture.pngPath,
      manifestPath: capture.manifestPath, manifest: capture.manifest });
  }

  sceneReport.logAudit = await auditLogs(currentLaunch.pid);
  sceneReport.passed = true;
  await call("smile_close_editor", { timeoutSeconds: 60 });
  currentLaunch = undefined;
  await delay(1_000);
}

try {
  await client.connect(transport);
  const initialEditor = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (initialEditor.connected) {
    console.log("[gi-probe-compaction] fechando editor existente para controlar a cena");
    await call("smile_close_editor", { timeoutSeconds: 60 });
    await delay(1_000);
  }
  const selectedScenes = startupSmokeOnly ? scenes.slice(0, 1) : scenes;
  for (const scene of selectedScenes) await runScene(scene);
  if (!startupSmokeOnly) {
    report.performanceSupportsDefault = report.scenes.every(
      (scene) => scene.performanceDecision.performanceSupportsDefault &&
        scene.performanceDecision.wakeObservedInEveryOnProfile,
    );
  }
  report.finishedAt = new Date().toISOString();
  report.passed = true;
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const reportStem = startupSmokeOnly
    ? "gi-probe-compaction-startup-smoke"
    : "gi-probe-compaction";
  reportPath = path.join(reportDirectory, `${reportStem}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-probe-compaction] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-probe-compaction] FAIL: ${report.error}`);
  try {
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    reportPath = path.join(reportDirectory, `gi-probe-compaction-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
    console.error(`[gi-probe-compaction] relatorio parcial: ${reportPath}`);
  } catch (reportError) {
    console.error(`[gi-probe-compaction] relatorio de falha: ${reportError}`);
  }
  process.exitCode = 1;
} finally {
  try {
    const editorStatus = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (editorStatus.connected && currentLaunch)
      await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch (cleanupError) {
    console.error(`[gi-probe-compaction] cleanup: ${cleanupError}`);
  }
  await client.close();
}
