import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(packageRoot, "../..");
const reportDirectory = path.join(smileRoot, "build", "bin", "Release", "Captures", "Validation");
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: { ...process.env, SMILE_ROOT: smileRoot },
  stderr: "pipe",
});
const client = new Client({ name: "smile-dlss-noise-ab", version: "0.1.0" });

function parse(result, name) {
  const value = result.content.find((part) => part.type === "text")?.text ?? "";
  if (result.isError) throw new Error(`${name}: ${value}`);
  return JSON.parse(value || "{}");
}

async function call(name, args = {}) {
  const seconds = Number(args.timeoutSeconds ?? args.startupTimeoutSeconds ?? 30);
  const timeout = Math.max(60_000, (seconds + 30) * 1_000);
  return parse(await client.callTool({ name, arguments: args }, undefined,
    { timeout, maxTotalTimeout: timeout }), name);
}

const cases = [
  { id: "raw_mesh8", preset: "controlled_native", mesh: 8 },
  { id: "nrd_mesh8", preset: "controlled_nrd", mesh: 8 },
  { id: "rr_dlaa_mesh8", preset: "gameplay_rr", mesh: 8, quality: 0 },
  { id: "rr_dlaa_mesh32", preset: "gameplay_rr", mesh: 32, quality: 0 },
];
const report = { schema: 1, startedAt: new Date().toISOString(), cases: [] };
let running = false;

try {
  await client.connect(transport);
  const status = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (status.connected) {
    await call("smile_close_editor", { timeoutSeconds: 60 });
    await delay(1_000);
  }
  report.editor = await call("smile_run_editor", {
    configuration: "Release",
    scenePath: "Assets/Scenes/Bistro/BistroExterior.sscene",
    waitUntilReady: true,
    startupTimeoutSeconds: 240,
  });
  running = true;

  for (const test of cases) {
    console.log(`[noise-ab] ${test.id}`);
    const profile = await call("smile_profile_configure", {
      preset: test.preset,
      bookmarkSlot: -1,
      timeOfDayHours: 10,
      backgroundThrottle: false,
      diMeshCandidates: test.mesh,
      ...(test.quality === undefined ? {} : { upscalerQuality: test.quality }),
    });
    const capture = await call("smile_capture_frame", {
      bookmarkSlot: -1,
      warmupFrames: 128,
      preset: "gameplay",
      resetHistory: true,
      pinTimeOfDay: false,
      timeoutSeconds: 300,
      includeImage: false,
    });
    report.cases.push({ ...test, profile, pngPath: capture.pngPath,
      manifestPath: capture.manifestPath, manifest: capture.manifest });
  }
  report.passed = true;
} catch (error) {
  report.passed = false;
  report.error = error instanceof Error ? error.stack ?? error.message : String(error);
  throw error;
} finally {
  report.finishedAt = new Date().toISOString();
  if (running) {
    try { await call("smile_close_editor", { timeoutSeconds: 60 }); } catch { }
  }
  await mkdir(reportDirectory, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-");
  const reportPath = path.join(reportDirectory, `dlss-noise-ab-${stamp}.json`);
  await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  console.log(`[noise-ab] relatorio: ${reportPath}`);
  await client.close();
}
