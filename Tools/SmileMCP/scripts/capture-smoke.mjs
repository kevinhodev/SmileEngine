import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { SmileEditorBridge } from "../dist/editor-bridge.js";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(scriptDirectory, "..");
const smileRoot = path.resolve(scriptDirectory, "../../..");
const environment = {
  ...Object.fromEntries(Object.entries(process.env).filter(([, value]) => value !== undefined)),
  SMILE_ROOT: smileRoot,
};

const bridge = new SmileEditorBridge(smileRoot);
const readyDeadline = Date.now() + 90_000;
let status;
while (Date.now() < readyDeadline) {
  try {
    status = await bridge.status(1_000);
    if (status.ready && status.scenePath) break;
  } catch {
    // O editor ainda esta subindo ou a named pipe ainda nao foi criada.
  }
  await new Promise((resolve) => setTimeout(resolve, 500));
}
if (!status?.ready) throw new Error("SmileEditor nao ficou pronto em 90 s.");

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(packageRoot, "dist", "index.js")],
  cwd: smileRoot,
  env: environment,
  stderr: "pipe",
});
const client = new Client({ name: "smile-capture-smoke", version: "0.4.0" });

try {
  await client.connect(transport);
  const callJson = async (name, args) => {
    const result = await client.callTool({ name, arguments: args });
    if (result.isError) {
      const message = result.content.find((part) => part.type === "text")?.text;
      throw new Error(message || `${name} retornou erro`);
    }
    const text = result.content.find((part) => part.type === "text")?.text;
    return JSON.parse(text || "{}");
  };

  const camera = await callJson("smile_camera_get", {});
  const gi = await callJson("smile_gi_status", {});
  if (!camera.position || !gi.policy || !Array.isArray(gi.ddgi?.cascades)) {
    throw new Error("Camera/GI nao retornaram o estado estruturado obrigatorio.");
  }
  const cameraReadback = await callJson("smile_camera_set", {
    x: camera.position.x,
    y: camera.position.y,
    z: camera.position.z,
    pitchDegrees: camera.pitchDegrees,
    yawDegrees: camera.yawDegrees,
  });
  if (!cameraReadback.position) throw new Error("smile_camera_set nao retornou readback.");
  const giReadback = await callJson("smile_gi_configure", {});
  if (!giReadback.policy || !giReadback.ddgi) {
    throw new Error("smile_gi_configure nao retornou readback pedido/efetivo.");
  }

  const result = await client.callTool({
    name: "smile_capture_frame",
    arguments: {
      bookmarkSlot: -1,
      warmupFrames: 0,
      preset: "gameplay",
      pinTimeOfDay: true,
      timeoutSeconds: 120,
      includeImage: false,
    },
  });
  if (result.isError) {
    const message = result.content.find((part) => part.type === "text")?.text;
    throw new Error(message || "smile_capture_frame retornou erro");
  }
  const text = result.content.find((part) => part.type === "text")?.text;
  const payload = JSON.parse(text || "{}");
  if (!payload.pngPath || !payload.manifestPath || !payload.manifest) {
    throw new Error("Captura nao retornou PNG, manifesto e metadados.");
  }
  console.log(`Capture smoke OK: ${payload.pngPath}`);
} finally {
  await client.close();
}
