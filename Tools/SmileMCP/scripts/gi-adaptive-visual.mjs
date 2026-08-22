import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const warmupFrames = 128;
const settleMilliseconds = 4_000;
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) {
  if (!condition) throw new Error(message);
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
const client = new Client({ name: "smile-gi-adaptive-visual", version: "0.4.0" });

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

function samePolicy(status) {
  return status.policy?.primary?.requested === "ddgi" &&
    status.policy?.primary?.effective === "ddgi" &&
    status.policy?.fallback?.requested === "ddgi" &&
    status.policy?.fallback?.effective === "ddgi";
}

async function setCamera(pose) {
  return call("smile_camera_set", {
    x: pose.position.x,
    y: pose.position.y,
    z: pose.position.z,
    pitchDegrees: pose.pitchDegrees,
    yawDegrees: pose.yawDegrees,
    cameraCut: true,
  });
}

function validateManifest(manifest, adaptiveRays, label) {
  assert(manifest.scene === path.parse(scenePath).name,
    `${label}: manifesto aponta para outra cena (${manifest.scene})`);
  assert(manifest.preset === "scientific" && manifest.warmupFrames === warmupFrames,
    `${label}: captura nao ficou em scientific N=${warmupFrames}`);
  assert(manifest.upscaler === "None" && manifest.taa === false,
    `${label}: jitter/reconstrucao ativos no preset cientifico`);
  assert(manifest.renderScale === 1 &&
    manifest.renderWidth === manifest.outputWidth &&
    manifest.renderHeight === manifest.outputHeight,
  `${label}: captura nao saiu em resolucao nativa`);
  assert(manifest.indirectPrimaryRequested === "ddgi" &&
    manifest.indirectPrimaryEffective === "ddgi" &&
    manifest.indirectFallbackRequested === "ddgi" &&
    manifest.indirectFallbackEffective === "ddgi",
  `${label}: politica GI divergiu no manifesto`);
  assert(manifest.ddgiReady === true && manifest.ddgiCascadeCount === 2,
    `${label}: manifesto nao confirma duas cascatas reais`);
  assert(manifest.ddgiRaysPerProbe === 64 &&
    manifest.ddgiAdaptiveMinRays === 16 &&
    manifest.ddgiAdaptiveMaxRays === 64,
  `${label}: limites de raios inesperados no manifesto`);
  assert(manifest.ddgiAdaptiveRays === adaptiveRays,
    `${label}: adaptive=${manifest.ddgiAdaptiveRays}, esperado ${adaptiveRays}`);
  assert(manifest.cacheStatsEnabled === false &&
    manifest.cacheStatsDetail === false &&
    manifest.cacheStatsSource === false,
  `${label}: instrumentacao de cache contaminou a captura`);
}

const sequence = [
  { id: "full64_a", adaptiveRays: false },
  { id: "adaptive_a", adaptiveRays: true },
  { id: "full64_b", adaptiveRays: false },
  { id: "adaptive_b", adaptiveRays: true },
];

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  regime: {
    preset: "scientific",
    warmupFrames,
    timeOfDayHours: 10,
    cascadeCount: 2,
    policy: { primary: "ddgi", fallback: "ddgi" },
    reference: "64 raios por sonda",
    candidate: "adaptive 16-64 raios por sonda",
    sequence: sequence.map(({ id }) => id),
  },
  captures: [],
  logAudit: {},
};

let launch;
let originalCamera;
let originalGI;
let reportPath;

try {
  await client.connect(transport);
  launch = await call("smile_run_editor", {
    configuration: "Release",
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 180,
  });
  report.editor = launch;
  console.log(`[gi-adaptive-visual] editor PID ${launch.pid}; cena=${scenePath}`);

  originalCamera = await call("smile_camera_get");
  originalGI = await call("smile_gi_status");
  report.baseline = { camera: originalCamera, gi: originalGI };

  for (const captureCase of sequence) {
    console.log(`[gi-adaptive-visual] ${captureCase.id}: configurando`);
    await setCamera(originalCamera);
    const profile = await call("smile_profile_configure", {
      preset: "gameplay_rr",
      bookmarkSlot: -1,
      timeOfDayHours: 10,
      backgroundThrottle: false,
    });
    assert(profile.backgroundThrottle === false,
      `${captureCase.id}: throttle de segundo plano ficou ligado`);
    const applied = await call("smile_gi_configure", {
      ddgiEnabled: true,
      indirectPrimary: "ddgi",
      indirectFallback: "ddgi",
      cascadeCount: 2,
      adaptiveRays: captureCase.adaptiveRays,
      adaptiveHysteresis: false,
      timeoutSeconds: 120,
    });
    const status = await waitForGI((candidate) =>
      candidate.ddgi?.enabled === true &&
      candidate.ddgi?.initialized === true &&
      candidate.ddgi?.desiredCascadeCount === 2 &&
      candidate.ddgi?.actualCascadeCount === 2 &&
      candidate.ddgi?.adaptiveRays === captureCase.adaptiveRays &&
      candidate.ddgi?.adaptiveMinRays === 16 &&
      candidate.ddgi?.adaptiveMaxRays === 64 &&
      samePolicy(candidate) &&
      candidate.frameIndex >= applied.frameIndex + 2);
    await delay(settleMilliseconds);

    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1,
      warmupFrames,
      preset: "scientific",
      pinTimeOfDay: true,
      timeOfDayHours: 10,
      timeoutSeconds: 600,
      includeImage: false,
    });
    validateManifest(capture.manifest, captureCase.adaptiveRays, captureCase.id);
    report.captures.push({
      ...captureCase,
      status,
      pngPath: capture.pngPath,
      manifestPath: capture.manifestPath,
      manifest: capture.manifest,
    });
    console.log(`[gi-adaptive-visual] ${captureCase.id}: ${capture.pngPath}`);
  }

  const resolutions = new Set(report.captures.map((capture) =>
    `${capture.manifest.outputWidth}x${capture.manifest.outputHeight}`));
  assert(resolutions.size === 1, "resolucao variou entre as capturas");

  const logs = await call("smile_get_latest_logs", { sessions: 5, tailLines: 1000 });
  const logSession = logs.sessions?.find((session) =>
    typeof session.path === "string" &&
    new RegExp(`-p${launch.pid}\\.log$`, "i").test(session.path));
  const logLines = logSession?.lines ?? [];
  assert(logSession && logLines.length > 0,
    `auditoria nao encontrou o log persistente do editor PID ${launch.pid}`);
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
    editorPid: launch.pid,
    logPath: logSession.path,
    fatalMatches,
  };
  report.finishedAt = new Date().toISOString();
  report.passed = true;

  const reportDirectory = path.join(
    smileRoot, "build", "bin", "Release", "Captures", "Validation",
  );
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  reportPath = path.join(reportDirectory, `gi-adaptive-visual-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-adaptive-visual] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-adaptive-visual] FAIL: ${report.error}`);
  try {
    const reportDirectory = path.join(
      smileRoot, "build", "bin", "Release", "Captures", "Validation",
    );
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    reportPath = path.join(reportDirectory, `gi-adaptive-visual-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
    console.error(`[gi-adaptive-visual] relatorio parcial: ${reportPath}`);
  } catch (reportError) {
    console.error(`[gi-adaptive-visual] relatorio de falha: ${reportError}`);
  }
  process.exitCode = 1;
} finally {
  try {
    const editorStatus = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (editorStatus.connected && !editorStatus.captureBusy && originalCamera) {
      await setCamera(originalCamera);
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
    console.error(`[gi-adaptive-visual] cleanup: ${cleanupError}`);
  }
  await client.close();
}
