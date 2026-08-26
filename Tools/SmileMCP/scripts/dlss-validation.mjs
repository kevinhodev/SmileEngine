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
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const modes = [
  { quality: 0, id: "dlaa" },
  { quality: 1, id: "quality" },
  { quality: 2, id: "balanced" },
  { quality: 3, id: "performance" },
];
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

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
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: environment,
  stderr: "pipe",
});
const client = new Client({ name: "smile-dlss-validation", version: "0.4.0" });

async function call(toolName, args = {}) {
  const declaredSeconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const sdkTimeout = Math.max(60_000, (declaredSeconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout: sdkTimeout, maxTotalTimeout: sdkTimeout },
  ), toolName);
}

async function capture(warmupFrames, resetHistory) {
  return call("smile_capture_frame", {
    bookmarkSlot: -1,
    warmupFrames,
    preset: "gameplay",
    resetHistory,
    pinTimeOfDay: false,
    timeoutSeconds: 300,
    includeImage: false,
  });
}

const report = {
  schema: 1,
  startedAt: new Date().toISOString(),
  scenePath,
  modes: [],
};
let editorRunning = false;

try {
  await client.connect(transport);
  const initial = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (initial.connected) {
    await call("smile_close_editor", { timeoutSeconds: 60 });
    await delay(1_000);
  }

  report.editor = await call("smile_run_editor", {
    configuration: "Release",
    scenePath,
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  editorRunning = true;

  let pose = await call("smile_camera_get", { timeoutSeconds: 5 });
  for (const mode of modes) {
    console.log(`[dlss] ${mode.id}: configurando`);
    const profile = await call("smile_profile_configure", {
      preset: "gameplay_rr",
      bookmarkSlot: -1,
      timeOfDayHours: 10,
      backgroundThrottle: false,
      upscalerQuality: mode.quality,
    });
    assert(profile.settings.denoiser === 2 && profile.settings.upscaler === 2,
      `${mode.id}: RR/DLSS nao ficou efetivo`);
    assert(profile.settings.upscalerQuality === mode.quality,
      `${mode.id}: quality readback divergente`);

    const base = await capture(96, true);
    assert(base.manifest.upscaler === "DLSS" && base.manifest.denoiser === "DLSS_RR",
      `${mode.id}: manifest nao registrou RR`);
    assert(base.manifest.upscalerQuality === mode.quality,
      `${mode.id}: manifest registrou quality incorreto`);
    if (mode.quality === 0) {
      assert(base.manifest.renderWidth === base.manifest.outputWidth &&
        base.manifest.renderHeight === base.manifest.outputHeight,
      "DLAA nao ficou em resolucao nativa");
    } else {
      assert(base.manifest.renderWidth < base.manifest.outputWidth &&
        base.manifest.renderHeight < base.manifest.outputHeight,
      `${mode.id}: modo subnativo nao reduziu a resolucao`);
    }

    pose = await call("smile_camera_set", {
      x: pose.position.x + 0.035,
      y: pose.position.y,
      z: pose.position.z,
      pitchDegrees: pose.pitchDegrees,
      yawDegrees: pose.yawDegrees + 0.45,
      cameraCut: false,
      timeoutSeconds: 30,
    });
    const motion = await capture(0, false);
    assert(motion.manifest.historyReset === false,
      `${mode.id}: captura de movimento resetou o historico`);

    report.modes.push({
      id: mode.id,
      quality: mode.quality,
      profile,
      renderSize: {
        width: base.manifest.renderWidth,
        height: base.manifest.renderHeight,
      },
      outputSize: {
        width: base.manifest.outputWidth,
        height: base.manifest.outputHeight,
      },
      captures: [
        { kind: "base", pngPath: base.pngPath, manifestPath: base.manifestPath },
        { kind: "motion", pngPath: motion.pngPath, manifestPath: motion.manifestPath },
      ],
    });
  }

  report.passed = true;
  report.finishedAt = new Date().toISOString();
} catch (error) {
  report.passed = false;
  report.finishedAt = new Date().toISOString();
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  throw error;
} finally {
  if (editorRunning) {
    try { await call("smile_close_editor", { timeoutSeconds: 60 }); }
    catch { /* preserva a falha original */ }
  }
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-");
  const reportPath = path.join(reportDirectory, `dlss-validation-${stamp}.json`);
  await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  console.log(`[dlss] relatorio: ${reportPath}`);
  await client.close();
}
