import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
const scenePath = "Assets/Scenes/Bistro/BistroExterior.sscene";
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function parseResult(result, toolName) {
  const value = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${toolName}: ${value || "erro sem detalhe"}`);
  try { return JSON.parse(value || "{}"); }
  catch { throw new Error(`${toolName}: resposta nao e JSON`); }
}

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: { ...process.env, SMILE_ROOT: smileRoot },
  stderr: "pipe",
});
const client = new Client({ name: "smile-dlss-rain-validation", version: "0.4.0" });

async function call(toolName, args = {}) {
  const seconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const timeout = Math.max(60_000, (seconds + 30) * 1_000);
  return parseResult(await client.callTool(
    { name: toolName, arguments: args }, undefined,
    { timeout, maxTotalTimeout: timeout },
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

const report = { schema: 1, startedAt: new Date().toISOString(), scenePath, frames: [] };
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
  report.profile = await call("smile_profile_configure", {
    preset: "gameplay_rr",
    bookmarkSlot: -1,
    timeOfDayHours: 10,
    rainAmount: 1,
    backgroundThrottle: false,
    upscalerQuality: 1,
  });

  const first = await capture(96, true);
  report.frames.push({ pngPath: first.pngPath, manifestPath: first.manifestPath,
    frameIndex: first.manifest.frameIndex });
  for (let index = 0; index < 7; ++index) {
    const frame = await capture(1, false);
    report.frames.push({ pngPath: frame.pngPath, manifestPath: frame.manifestPath,
      frameIndex: frame.manifest.frameIndex });
  }
  report.passed = report.frames.length === 8 &&
    report.frames.every((frame, index) => index === 0 ||
      frame.frameIndex > report.frames[index - 1].frameIndex);
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
  const reportPath = path.join(reportDirectory, `dlss-rain-validation-${stamp}.json`);
  await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  console.log(`[dlss-rain] relatorio: ${reportPath}`);
  await client.close();
}
