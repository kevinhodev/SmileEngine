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
const stepMeters = 0.75;
const stepCount = 8;
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
const client = new Client({ name: "smile-gi-validation", version: "0.4.0" });

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

async function waitForGI(predicate, timeoutMilliseconds = 30_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  let last;
  while (Date.now() < deadline) {
    last = await call("smile_gi_status", { timeoutSeconds: 5 });
    if (predicate(last)) return last;
    await delay(100);
  }
  throw new Error(`GI nao atingiu o estado esperado; ultimo=${JSON.stringify(last)}`);
}

async function waitForFrames(afterFrame, count = 2) {
  return waitForGI((status) => status.frameIndex >= afterFrame + count);
}

async function settleGI(afterFrame, milliseconds = 3_000) {
  // RebuildGIVolume tambem recria cache e mesh lights. A alias table e concluida por worker e
  // pode invalidar DI alguns frames depois do readback sincrono; dois frames nao provam que esse
  // trabalho lateral acabou. A janela fixa cobre o worker e depois exige frames novos.
  await delay(milliseconds);
  return waitForFrames(afterFrame, 2);
}

async function waitForCaptureBusy(timeoutMilliseconds = 10_000) {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    const status = await call("smile_editor_status", { timeoutSeconds: 2 });
    if (status.captureBusy) return status;
    await delay(25);
  }
  throw new Error("captura nao entrou em estado busy antes da mutacao concorrente");
}

function assertPolicy(status, expected, label) {
  const primary = status.policy?.primary;
  const fallback = status.policy?.fallback;
  assert(primary?.requested === expected.primaryRequested,
    `${label}: primary requested=${primary?.requested}`);
  assert(primary?.effective === expected.primaryEffective,
    `${label}: primary effective=${primary?.effective}`);
  assert(fallback?.requested === expected.fallbackRequested,
    `${label}: fallback requested=${fallback?.requested}`);
  assert(fallback?.effective === expected.fallbackEffective,
    `${label}: fallback effective=${fallback?.effective}`);
}

function validateManifest(manifest, expected, label) {
  assert(manifest.scene === path.parse(scenePath).name,
    `${label}: manifesto aponta para outra cena (${manifest.scene})`);
  assert(manifest.indirectPrimaryRequested === expected.primaryRequested,
    `${label}: manifesto primary requested=${manifest.indirectPrimaryRequested}`);
  assert(manifest.indirectPrimaryEffective === expected.primaryEffective,
    `${label}: manifesto primary effective=${manifest.indirectPrimaryEffective}`);
  assert(manifest.indirectFallbackRequested === expected.fallbackRequested,
    `${label}: manifesto fallback requested=${manifest.indirectFallbackRequested}`);
  assert(manifest.indirectFallbackEffective === expected.fallbackEffective,
    `${label}: manifesto fallback effective=${manifest.indirectFallbackEffective}`);
  assert(manifest.useGI === expected.ddgiEnabled, `${label}: useGI=${manifest.useGI}`);
  assert(manifest.cacheStatsEnabled === false, `${label}: cacheStatsEnabled ligado`);
  assert(manifest.cacheStatsDetail === false, `${label}: cacheStatsDetail ligado`);
  assert(manifest.cacheStatsSource === false, `${label}: cacheStatsSource ligado`);
}

const matrixCases = [
  {
    id: "01_sharc_ddgi",
    config: { ddgiEnabled: true, indirectPrimary: "restir_sharc", indirectFallback: "ddgi" },
    expected: {
      ddgiEnabled: true,
      primaryRequested: "restir_sharc", primaryEffective: "restir_sharc",
      fallbackRequested: "ddgi", fallbackEffective: "ddgi",
    },
  },
  {
    id: "02_sharc_black",
    config: { ddgiEnabled: true, indirectPrimary: "restir_sharc", indirectFallback: "black" },
    expected: {
      ddgiEnabled: true,
      primaryRequested: "restir_sharc", primaryEffective: "restir_sharc",
      fallbackRequested: "black", fallbackEffective: "black",
    },
  },
  {
    id: "03_ddgi_ddgi",
    config: { ddgiEnabled: true, indirectPrimary: "ddgi", indirectFallback: "ddgi" },
    expected: {
      ddgiEnabled: true,
      primaryRequested: "ddgi", primaryEffective: "ddgi",
      fallbackRequested: "ddgi", fallbackEffective: "ddgi",
    },
  },
  {
    id: "04_off",
    config: { ddgiEnabled: true, indirectPrimary: "off", indirectFallback: "ddgi" },
    expected: {
      ddgiEnabled: true,
      primaryRequested: "off", primaryEffective: "off",
      fallbackRequested: "ddgi", fallbackEffective: "ddgi",
    },
  },
  {
    id: "05_ddgi_volume_off",
    config: { ddgiEnabled: false, indirectPrimary: "ddgi", indirectFallback: "ddgi" },
    expected: {
      ddgiEnabled: false,
      primaryRequested: "ddgi", primaryEffective: "off",
      fallbackRequested: "ddgi", fallbackEffective: "black",
    },
  },
];

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  warmupFrames,
  scroll: {},
  matrix: [],
  mutationDuringCapture: {},
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
  console.log(`[gi-validation] editor PID ${launch.pid}; cena=${scenePath}`);

  originalCamera = await call("smile_camera_get");
  originalGI = await call("smile_gi_status");

  await call("smile_profile_configure", {
    preset: "controlled_native",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
  });
  const scrollApplied = await call("smile_gi_configure", {
    ddgiEnabled: true,
    indirectPrimary: "ddgi",
    indirectFallback: "ddgi",
    cascadeCount: 2,
    adaptiveRays: false,
    adaptiveHysteresis: false,
    timeoutSeconds: 120,
  });
  const scrollStart = await waitForGI((status) =>
    status.ddgi?.initialized === true && status.ddgi?.actualCascadeCount === 2 &&
    status.frameIndex >= scrollApplied.frameIndex + 2,
  60_000);
  assert(scrollStart.ddgi.cascades[0].spacing < scrollStart.ddgi.cascades[1].spacing,
    "ordem fina->grossa das cascatas nao foi preservada");

  const scrollSamples = [{ step: 0, camera: originalCamera, gi: scrollStart }];
  let previous = scrollStart;
  for (let step = 1; step <= stepCount; step += 1) {
    const camera = await call("smile_camera_set", {
      x: originalCamera.position.x + step * stepMeters,
      y: originalCamera.position.y,
      z: originalCamera.position.z,
      pitchDegrees: originalCamera.pitchDegrees,
      yawDegrees: originalCamera.yawDegrees,
    });
    const gi = await waitForFrames(previous.frameIndex, 2);
    scrollSamples.push({ step, camera, gi });
    previous = gi;
  }

  let transitions = 0;
  const transitionChecks = [];
  for (let index = 1; index < scrollSamples.length; index += 1) {
    const before = scrollSamples[index - 1].gi.ddgi.cascades[0];
    const after = scrollSamples[index].gi.ddgi.cascades[0];
    const countX = scrollSamples[index].gi.ddgi.gridCount.x;
    const cellsFloat = (after.gridMin.x - before.gridMin.x) / before.spacing;
    const cells = Math.round(cellsFloat);
    assert(Math.abs(cellsFloat - cells) < 1e-4,
      `scroll: gridMin X mudou por ${cellsFloat} celulas nao inteiras`);
    const scrollDelta = modulo(after.scrollCells.x - before.scrollCells.x, countX);
    const expectedDelta = modulo(cells, countX);
    assert(scrollDelta === expectedDelta,
      `scroll: delta toroidal ${scrollDelta}, esperado ${expectedDelta}`);
    if (cells !== 0) transitions += 1;
    transitionChecks.push({ fromStep: index - 1, toStep: index, cells, scrollDelta });
  }
  assert(transitions > 0, "percurso nao cruzou nenhuma celula da cascata fina");
  report.scroll = {
    passed: true,
    stepMeters,
    stepCount,
    transitions,
    gridCount: scrollStart.ddgi.gridCount,
    cascades: scrollStart.ddgi.cascades,
    transitionChecks,
    samples: scrollSamples,
  };

  await call("smile_camera_set", {
    x: originalCamera.position.x,
    y: originalCamera.position.y,
    z: originalCamera.position.z,
    pitchDegrees: originalCamera.pitchDegrees,
    yawDegrees: originalCamera.yawDegrees,
  });

  const matrixCascadeCount = originalGI.ddgi.desiredCascadeCount;
  for (const matrixCase of matrixCases) {
    await call("smile_profile_configure", {
      preset: "gameplay_rr",
      bookmarkSlot: -1,
      timeOfDayHours: 10,
    });
    const applied = await call("smile_gi_configure", {
      ...matrixCase.config,
      cascadeCount: matrixCascadeCount,
      adaptiveRays: false,
      adaptiveHysteresis: false,
      timeoutSeconds: 120,
    });
    const status = await settleGI(applied.frameIndex, matrixCase.id === "01_sharc_ddgi" ? 5_000 : 1_000);
    assertPolicy(status, matrixCase.expected, matrixCase.id);
    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1,
      warmupFrames,
      preset: "scientific",
      pinTimeOfDay: true,
      timeOfDayHours: 10,
      timeoutSeconds: 600,
      includeImage: false,
    });
    validateManifest(capture.manifest, matrixCase.expected, matrixCase.id);
    report.matrix.push({
      id: matrixCase.id,
      passed: true,
      expected: matrixCase.expected,
      status,
      pngPath: capture.pngPath,
      manifestPath: capture.manifestPath,
      manifest: capture.manifest,
    });
    console.log(`[gi-validation] ${matrixCase.id}: OK`);
  }

  await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
  });
  const cancellationBaseline = await call("smile_gi_configure", {
    ddgiEnabled: true,
    indirectPrimary: "restir_sharc",
    indirectFallback: "ddgi",
    cascadeCount: matrixCascadeCount,
    adaptiveRays: false,
    adaptiveHysteresis: false,
  });
  await settleGI(cancellationBaseline.frameIndex, 2_000);

  const capturePromise = client.callTool({
    name: "smile_capture_frame",
    arguments: {
      bookmarkSlot: -1,
      warmupFrames: Math.max(warmupFrames, 128),
      preset: "scientific",
      pinTimeOfDay: true,
      timeOfDayHours: 10,
      timeoutSeconds: 600,
      includeImage: false,
    },
  }, undefined, { timeout: 630_000, maxTotalTimeout: 630_000 });
  const busyStatus = await waitForCaptureBusy();
  const mutation = await call("smile_gi_configure", { indirectPrimary: "off" });
  const cancelledCapture = await capturePromise;
  const cancellationMessage = textOf(cancelledCapture);
  assert(cancelledCapture.isError === true,
    "captura concorrente terminou com sucesso depois da troca de politica");
  assert(/cancel|invalid|mudan|alter|histor|derrub/i.test(cancellationMessage),
    `captura falhou sem motivo reconhecivel: ${cancellationMessage}`);
  report.mutationDuringCapture = {
    passed: true,
    busyStatus,
    mutation,
    captureError: cancellationMessage,
  };

  const logs = await call("smile_get_latest_logs", { sessions: 1, tailLines: 1000 });
  const logLines = logs.sessions?.[0]?.lines ?? [];
  assert(logs.sessions?.length === 1 && logLines.length > 0,
    "auditoria nao encontrou o log persistente da sessao");
  const fatalPatterns = [
    /D3D12.*ERROR/i,
    /DXGI_ERROR/i,
    /DEVICE_REMOVED/i,
    /GPU validation.*error/i,
    /\[ERR\s*\]/i,
  ];
  const fatalMatches = logLines.filter((line) =>
    fatalPatterns.some((pattern) => pattern.test(line)));
  assert(fatalMatches.length === 0, `log contem erro de GPU/API: ${fatalMatches.join(", ")}`);
  report.logAudit = {
    passed: true,
    logPath: logs.sessions?.[0]?.path ?? null,
    fatalMatches,
  };
  report.finishedAt = new Date().toISOString();
  report.passed = true;

  const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  reportPath = path.join(reportDirectory, `gi-validation-${stamp}.json`);
  await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
  console.log(`[gi-validation] PASS: ${reportPath}`);
} catch (error) {
  report.finishedAt = new Date().toISOString();
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  console.error(`[gi-validation] FAIL: ${report.error}`);
  try {
    const reportDirectory = path.join(
      smileRoot, "build", "bin", "Release", "Captures", "Validation",
    );
    await mkdir(reportDirectory, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    reportPath = path.join(reportDirectory, `gi-validation-FAILED-${stamp}.json`);
    await writeFile(reportPath, JSON.stringify(report, null, 2), "utf8");
    console.error(`[gi-validation] relatorio parcial: ${reportPath}`);
  } catch (reportError) {
    console.error(`[gi-validation] relatorio de falha: ${reportError}`);
  }
  process.exitCode = 1;
} finally {
  try {
    const editorStatus = await call("smile_editor_status", { timeoutSeconds: 1 });
    if (editorStatus.connected && !editorStatus.captureBusy && originalCamera) {
      await call("smile_camera_set", {
        x: originalCamera.position.x,
        y: originalCamera.position.y,
        z: originalCamera.position.z,
        pitchDegrees: originalCamera.pitchDegrees,
        yawDegrees: originalCamera.yawDegrees,
      });
    }
    if (editorStatus.connected && !editorStatus.captureBusy && originalGI) {
      await call("smile_gi_configure", {
        ddgiEnabled: originalGI.ddgi.enabled,
        indirectPrimary: originalGI.policy.primary.requested,
        indirectFallback: originalGI.policy.fallback.requested,
        cascadeCount: originalGI.ddgi.desiredCascadeCount,
        adaptiveRays: originalGI.ddgi.adaptiveRays,
        adaptiveHysteresis: originalGI.ddgi.adaptiveHysteresis,
      });
    }
    if (editorStatus.connected && launch && !launch.alreadyRunning) {
      await call("smile_close_editor", { timeoutSeconds: 60 });
    }
  } catch (cleanupError) {
    console.error(`[gi-validation] cleanup: ${cleanupError}`);
  }
  await client.close();
}
