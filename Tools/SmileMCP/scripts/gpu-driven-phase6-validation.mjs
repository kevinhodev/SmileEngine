import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const out = path.join(root, "build/bin/Release/Captures/Validation/phase6");
const client = new Client({ name: "smile-phase6-validation", version: "1.0.0" });
const transport = new StdioClientTransport({
  command: process.execPath, args: [path.join(root, "Tools/SmileMCP/dist/index.js")],
  cwd: root, env: { ...process.env, SMILE_ROOT: root }, stderr: "pipe",
});
function require(value, message) { if (!value) throw new Error(message); }
async function call(name, args = {}) {
  const result = await client.callTool({ name, arguments: args }, undefined,
    { timeout: 660000, maxTotalTimeout: 660000 });
  const body = result.content.find(p => p.type === "text")?.text ?? "{}";
  if (result.isError) throw new Error(`${name}: ${body}`);
  return JSON.parse(body);
}
const report = { schema: 1, startedAt: new Date().toISOString(), scenes: [] };
const scenes = [
  { key: "emerald", path: "Assets/Scenes/EmeraldSquare_v4_1/EmeraldSquare_Day.sscene", slot: 0 },
  { key: "synthetic_1M", path: "build/Tests/SyntheticScenes/Synthetic_1M.sscene", slot: 0, allVisible: true },
  { key: "synthetic_1M_inside", path: "build/Tests/SyntheticScenes/Synthetic_1M.sscene", slot: 1 },
].filter(s => (!process.argv.includes("--emerald") || s.key === "emerald") &&
              (!process.argv.includes("--synthetic") || s.key.startsWith("synthetic_1M")));
async function close() {
  const status = await call("smile_editor_status", { timeoutSeconds: 1 });
  if (status.connected) await call("smile_close_editor", { timeoutSeconds: 120 });
}
try {
  await client.connect(transport);
  await close();
  for (const scene of scenes) {
    const result = { ...scene, cases: [], captures: [] };
    report.scenes.push(result);
    await call("smile_run_editor", { configuration: "Release", scenePath: scene.path,
      waitUntilReady: true, startupTimeoutSeconds: 300 });
    for (const [label, enabled] of [["off_a", false], ["on", true], ["off_b", false]]) {
      const configured = await call("smile_profile_configure", {
        preset: "controlled_native", bookmarkSlot: scene.slot, timeOfDayHours: 10,
        backgroundThrottle: false, bindlessMaterials: true, bindlessTransforms: true,
        gpuIndirectMainView: true, gpuCulling: enabled, frustumCulling: true,
        occlusionCulling: true,
      });
      require(configured.settings?.gpuCulling === enabled, `${scene.key}/${label}: toggle refused`);
      const profile = await call("smile_profile_gpu", { warmupSeconds: 5,
        samples: scene.slot === 1 ? 48 : 24,
        intervalMs: 200, includeSamples: true });
      const chain = profile.cpu?.indirectMainView;
      result.cases.push({ label, profile });
      require(chain?.active && chain.countersValid && chain.gpuStatus === 1 && chain.gpuReason === 0,
        `${scene.key}/${label}: invalid chain ${JSON.stringify(chain)}`);
      for (const sample of profile.samples ?? []) {
        const state = sample.cpu?.indirectMainView;
        require(sample.settings?.gpuCulling === enabled && state?.active && state.countersValid &&
          state.gpuStatus === 1 && state.gpuReason === 0 && state.lastReason === 0,
          `${scene.key}/${label}: invalid intermediate sample`);
      }
      if (enabled && !scene.allVisible) {
        require(chain.frustumRejected?.max + chain.occlusionRejected?.max > 0,
          `${scene.key}: no GPU rejection ${JSON.stringify(chain)}`);
        require(chain.gpuCommands?.max < chain.commands?.max, `${scene.key}: no draw reduction`);
      }
      const capture = await call("smile_capture_frame", { bookmarkSlot: scene.slot, warmupFrames: 64,
        preset: "scientific", resetHistory: true, pinTimeOfDay: true, timeOfDayHours: 10,
        timeoutSeconds: 600, includeImage: false });
      require(capture.manifest?.gpuCulling === enabled, `${label}: capture toggle differs`);
      result.captures.push({label, pngPath: capture.pngPath, manifestPath: capture.manifestPath});
    }
    const logs = await call("smile_get_latest_logs", { sessions: 1, tailLines: 1000 });
    const lines = logs.sessions?.[0]?.lines ?? [];
    const errors = lines.filter(l => /D3D12.*ERROR|DXGI_ERROR|DEVICE_REMOVED|\[ERR\s*\]/i.test(l));
    require(errors.length === 0, `${scene.key}: ${errors.join(" | ")}`);
    result.log = logs.sessions?.[0]?.path;
    console.log(`[phase6] ${scene.key}: runtime OFF/ON/OFF passed`);
    await close();
  }
  report.passed = true;
} catch (error) {
  report.passed = false; report.error = error.stack ?? String(error);
  console.error(report.error); process.exitCode = 1;
} finally {
  try { await close(); } catch {}
  report.finishedAt = new Date().toISOString();
  await mkdir(out, { recursive: true });
  const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
  const target = path.join(out, `phase6-validation-${stamp}.json`);
  await writeFile(target, JSON.stringify(report, null, 2), "utf8");
  console.log(target);
  await client.close();
}
