import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
const scenes = [
  { id: "bistro", path: "Assets/Scenes/Bistro/BistroExterior.sscene" },
  { id: "emerald", path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene" },
];
const sequence = [
  { id: "off_a", halfRes: false },
  { id: "on_a", halfRes: true },
  { id: "on_b", halfRes: true },
  { id: "off_b", halfRes: false },
];
const startupSmokeOnly = process.argv.includes("--startup-smoke");
const gameplayRR = process.argv.includes("--gameplay-rr");
const profilePreset = gameplayRR ? "gameplay_rr" : "controlled_native";
const capturePreset = gameplayRR ? "gameplay" : "scientific";
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) { if (!condition) throw new Error(message); }
function parseResult(result, toolName) {
  const value = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${value || "erro sem detalhe"}`);
  try { return JSON.parse(value || "{}"); }
  catch { throw new Error(`${toolName}: resposta nao e JSON`); }
}
const environment = {
  ...Object.fromEntries(Object.entries(process.env).filter(([, value]) => value !== undefined)),
  SMILE_ROOT: smileRoot,
};
const transport = new StdioClientTransport({
  command: process.execPath, args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot, env: environment, stderr: "pipe",
});
const client = new Client({ name: "smile-gi-half-res", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const sdkTimeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
  ), toolName);
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
  return Number(profilePass(profile, name)?.rawMilliseconds?.mean ?? 0);
}
function mean(values) { return values.reduce((sum, value) => sum + value, 0) / values.length; }

async function settleHalfRes(enabled, renderSize, extraFrames = 128) {
  const applied = await call("smile_gi_configure", { halfRes: enabled, timeoutSeconds: 120 });
  const expectedWidth = enabled ? Math.ceil(renderSize.width / 2) : renderSize.width;
  const expectedHeight = enabled ? Math.ceil(renderSize.height / 2) : renderSize.height;
  const status = await waitForGI((candidate) => candidate.restirGI?.halfRes === enabled &&
    candidate.restirGI.halfResEffective === enabled &&
    candidate.restirGI.renderWidth === expectedWidth &&
    candidate.restirGI.renderHeight === expectedHeight &&
    candidate.frameIndex >= applied.frameIndex + extraFrames);
  return { applied, status, expectedWidth, expectedHeight };
}

async function profileGI(label, halfRes) {
  let profile;
  const attempts = [];
  for (let attempt = 1; attempt <= 3; ++attempt) {
    profile = await call("smile_profile_gpu", {
      warmupSeconds: 2, samples: 72, intervalMs: 125, includeSamples: false,
    });
    const passNames = profile.passes.map((entry) => entry.name);
    attempts.push({ attempt, firstFrameIndex: profile.firstFrameIndex,
      lastFrameIndex: profile.lastFrameIndex, passNames });
    const common = passNames.includes("ReSTIR GI") &&
      passNames.includes("Temporal + Trace Secundário") &&
      passNames.includes("Reuso Espacial + Resolve") &&
      passNames.includes("Frame (GPU)");
    if (common && (!halfRes || passNames.includes("Reconstrução temporal 4 fases"))) break;
    await delay(500);
  }
  assert(profilePass(profile, "ReSTIR GI"), `${label}: scope ReSTIR GI ausente`);
  assert(profilePass(profile, "Temporal + Trace Secundário"), `${label}: trace ausente`);
  assert(profilePass(profile, "Reuso Espacial + Resolve"), `${label}: spatial ausente`);
  assert(profilePass(profile, "Frame (GPU)"), `${label}: frame ausente`);
  if (halfRes) assert(profilePass(profile, "Reconstrução temporal 4 fases"),
    `${label}: reconstrução temporal ausente`);
  return { profile, attempts };
}

function performanceDecision(entries) {
  const off = entries.filter((entry) => !entry.halfRes);
  const on = entries.filter((entry) => entry.halfRes);
  const passes = {
    trace: "Temporal + Trace Secundário",
    spatial: "Reuso Espacial + Resolve",
    upsample: "Reconstrução temporal 4 fases",
    restir: "ReSTIR GI",
    frame: "Frame (GPU)",
  };
  const comparison = {};
  for (const [key, pass] of Object.entries(passes)) {
    const offMs = mean(off.map((entry) => passRawMean(entry.profile, pass)));
    const onMs = mean(on.map((entry) => passRawMean(entry.profile, pass)));
    comparison[key] = { pass, offMeanMilliseconds: offMs, onMeanMilliseconds: onMs,
      savedMilliseconds: offMs - onMs,
      savedPercent: offMs > 0 ? 100 * (offMs - onMs) / offMs : 0 };
  }
  return { comparison,
    performancePromising: comparison.restir.savedMilliseconds > 0.5 &&
      comparison.restir.savedPercent > 20 };
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
  return session ? { passed: true, available: true, editorPid, logPath: session.path,
    fatalMatches } : { passed: null, available: false, editorPid,
    reason: "log persistente da sessao nao encontrado", fatalMatches: [] };
}

const report = {
  schema: 1, startedAt: new Date().toISOString(), startupSmokeOnly, gameplayRR, scenes: [],
  regime: {
    policy: { primary: "restir_sharc", fallback: "ddgi" },
    halfResolution: "1/2 por eixo (1/4 dos pixels), histórico full-res em quatro fases + rejeição geométrica",
    sequence: sequence.map((entry) => entry.id),
    performance: { samples: 72, intervalMs: 125, warmupSeconds: 2,
      settleFramesAfterReallocation: 128 },
    presentation: profilePreset,
    visual: { preset: capturePreset, warmupFrames: 128 },
  },
};
let currentLaunch;
let reportPath;

async function runScene(scene) {
  const sceneReport = { id: scene.id, scenePath: scene.path, performance: [], captures: [] };
  report.scenes.push(sceneReport);
  currentLaunch = await call("smile_run_editor", {
    configuration: "Release", scenePath: scene.path,
    waitUntilReady: true, startupTimeoutSeconds: 240,
  });
  sceneReport.editor = currentLaunch;
  console.log(`[gi-half-res] ${scene.id}: editor PID ${currentLaunch.pid}`);
  sceneReport.startup = await call("smile_gi_status");
  assert(sceneReport.startup.restirGI?.halfRes === false,
    `${scene.id}: half-res nao nasceu OFF`);
  await call("smile_profile_configure", {
    preset: profilePreset, bookmarkSlot: -1,
    timeOfDayHours: 10, backgroundThrottle: false,
  });
  await call("smile_gi_configure", {
    ddgiEnabled: true, indirectPrimary: "restir_sharc", indirectFallback: "ddgi",
    cascadeCount: 2, interleavedUpdates: true, probeCompaction: true,
    halfRes: false, timeoutSeconds: 120,
  });
  const baselineGI = await waitForGI((candidate) => candidate.restirGI?.halfRes === false &&
    candidate.restirGI.halfResEffective === false &&
    candidate.restirGI.renderWidth > 0 && candidate.restirGI.renderHeight > 0);
  const renderSize = {
    width: baselineGI.restirGI.renderWidth,
    height: baselineGI.restirGI.renderHeight,
  };
  sceneReport.renderSize = renderSize;

  if (startupSmokeOnly) {
    const enabled = await settleHalfRes(true, renderSize, 16);
    sceneReport.startupEnabled = enabled;
    sceneReport.passed = true;
    await call("smile_close_editor", { timeoutSeconds: 60 });
    currentLaunch = undefined;
    return;
  }

  await delay(6_000);
  for (const benchmarkCase of sequence) {
    console.log(`[gi-half-res] ${scene.id}: perfil ${benchmarkCase.id}`);
    const steady = await settleHalfRes(benchmarkCase.halfRes, renderSize);
    const beforeProfile = await call("smile_gi_status");
    const { profile, attempts } = await profileGI(
      `${scene.id}/${benchmarkCase.id}`, benchmarkCase.halfRes);
    const afterProfile = await call("smile_gi_status");
    sceneReport.performance.push({ ...benchmarkCase, steady, beforeProfile, afterProfile,
      attempts, profile });
  }
  sceneReport.performanceDecision = performanceDecision(sceneReport.performance);

  for (const captureCase of sequence) {
    console.log(`[gi-half-res] ${scene.id}: visual ${captureCase.id}`);
    await settleHalfRes(captureCase.halfRes, renderSize, 16);
    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1, warmupFrames: 128, preset: capturePreset,
      resetHistory: true,
      pinTimeOfDay: true, timeOfDayHours: 10,
      timeoutSeconds: 600, includeImage: false,
    });
    const expectedWidth = captureCase.halfRes ? Math.ceil(renderSize.width / 2) : renderSize.width;
    const expectedHeight = captureCase.halfRes ? Math.ceil(renderSize.height / 2) : renderSize.height;
    assert(capture.manifest.restirGIHalfResRequested === captureCase.halfRes,
      `${scene.id}/${captureCase.id}: pedido incorreto no manifesto`);
    assert(capture.manifest.restirGIHalfResEffective === captureCase.halfRes,
      `${scene.id}/${captureCase.id}: efetivo incorreto no manifesto`);
    assert(capture.manifest.restirGIRenderWidth === expectedWidth &&
      capture.manifest.restirGIRenderHeight === expectedHeight,
    `${scene.id}/${captureCase.id}: resolucao interna incorreta`);
    if (gameplayRR) {
      assert(capture.manifest.preset === "gameplay" &&
        capture.manifest.denoiser === "DLSS_RR" && capture.manifest.upscaler === "DLSS",
      `${scene.id}/${captureCase.id}: gameplay_rr nao ficou efetivo`);
    }
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
    console.log("[gi-half-res] fechando editor existente para controlar a cena");
    await call("smile_close_editor", { timeoutSeconds: 60 });
    await delay(1_000);
  }
  const selectedScenes = startupSmokeOnly ? scenes.slice(0, 1) : scenes;
  for (const scene of selectedScenes) await runScene(scene);
  report.finishedAt = new Date().toISOString(); report.passed = true;
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const stem = startupSmokeOnly ? "gi-half-res-startup-smoke" :
    (gameplayRR ? "gi-half-res-gameplay-rr" : "gi-half-res");
  reportPath = path.join(reportDirectory, `${stem}-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-half-res] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString(); report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-half-res] FAIL: ${report.error}`);
  try {
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    reportPath = path.join(reportDirectory, `gi-half-res-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
    console.error(`[gi-half-res] relatorio parcial: ${reportPath}`);
  } catch (reportError) { console.error(`[gi-half-res] relatorio de falha: ${reportError}`); }
  process.exitCode = 1;
} finally {
  try {
    const editorStatus = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (editorStatus.connected && currentLaunch)
      await call("smile_close_editor", { timeoutSeconds: 60 });
  } catch (cleanupError) { console.error(`[gi-half-res] cleanup: ${cleanupError}`); }
  await client.close();
}
