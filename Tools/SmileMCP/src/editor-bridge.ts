import { createHash, randomUUID } from "node:crypto";
import { readFile, realpath, stat } from "node:fs/promises";
import net from "node:net";
import path from "node:path";

const PROTOCOL_VERSION = 1;
const DEFAULT_PIPE_NAME = "SmileEngine-MCP-v1";
const MAX_RESPONSE_BYTES = 1024 * 1024;
const MAX_INLINE_IMAGE_BYTES = 16 * 1024 * 1024;
// O renderer pode anunciar Available por um instante entre a criacao do viewport e o commit
// assincrono da cena, voltando a false enquanto troca os recursos. Uma amostra positiva fez o
// run_editor liberar a captura cedo demais na Bistro. Exija uma janela continua, nao um pulso.
const READY_STABLE_MS = 1_500;

interface BridgeEnvelope {
  version: number;
  id: string;
  ok: boolean;
  result?: unknown;
  error?: string;
}
export interface EditorStatus {
  protocolVersion: number;
  pid: number;
  executablePath?: string;
  buildCommit?: string;
  engineVersion?: string;
  ready: boolean;
  captureBusy: boolean;
  scenePath: string;
}

export interface CaptureFrameOptions {
  bookmarkSlot: number;
  warmupFrames: number;
  preset: "scientific" | "gameplay";
  pinTimeOfDay: boolean;
  timeOfDayHours?: number;
  timeoutSeconds: number;
  includeImage: boolean;
}

export interface CaptureFrameResult {
  pngPath: string;
  manifestPath: string;
  warmupFrames: number;
  pngBytes: number;
  manifest: Record<string, unknown>;
  imageData?: string;
  imageOmittedReason?: string;
}

export interface ProfileTiming {
  name: string;
  queue: "direct" | "asyncCompute";
  depth: number;
  milliseconds: number;
  rawMilliseconds: number;
}

export interface ProfileSnapshot {
  frameIndex: number;
  cpuFps: number;
  outputWidth: number;
  outputHeight: number;
  renderWidth: number;
  renderHeight: number;
  gpu: string;
  direct: ProfileTiming[];
  asyncCompute: ProfileTiming[];
  vram: {
    valid: boolean;
    localUsageBytes: number;
    localBudgetBytes: number;
    nonLocalUsageBytes: number;
  };
  settings: Record<string, unknown>;
}

function isInside(parent: string, child: string): boolean {
  const relative = path.relative(parent, child);
  return (
    relative === "" ||
    (!relative.startsWith(`..${path.sep}`) && relative !== ".." && !path.isAbsolute(relative))
  );
}

function editorPipePath(): string {
  const name = process.env.SMILE_MCP_PIPE || DEFAULT_PIPE_NAME;
  if (name.includes("\\") || name.includes("/")) {
    throw new Error("SMILE_MCP_PIPE deve conter somente o nome da named pipe.");
  }
  return `\\\\.\\pipe\\${name}`;
}

function bridgeUnavailableMessage(error: NodeJS.ErrnoException): string {
  if (error.code === "ENOENT" || error.code === "ECONNREFUSED") {
    return (
      "SmileEditor nao encontrado na named pipe local. Inicie uma build que contenha o " +
      "McpBridge e aguarde o renderer ficar pronto."
    );
  }
  return `Falha ao comunicar com o SmileEditor: ${error.message}`;
}

export class SmileEditorBridge {
  private readonly projectRoot: string;

  constructor(projectRoot: string) {
    this.projectRoot = projectRoot;
  }

  async status(timeoutMs = 2_000): Promise<EditorStatus> {
    const result = await this.request("status", {}, timeoutMs);
    if (!result || typeof result !== "object") throw new Error("Status invalido retornado pelo editor.");
    return result as EditorStatus;
  }

  async waitUntilReady(timeoutSeconds: number, expectedScenePath?: string): Promise<EditorStatus> {
    const deadline = Date.now() + timeoutSeconds * 1000;
    let lastStatus: EditorStatus | undefined;
    let lastError: Error | undefined;
    let readySince: number | undefined;

    while (Date.now() < deadline) {
      try {
        lastStatus = await this.status(Math.min(1_000, Math.max(100, deadline - Date.now())));
        lastError = undefined;
        const sceneMatches =
          expectedScenePath === undefined ||
          this.samePath(lastStatus.scenePath, expectedScenePath);
        if (lastStatus.ready && sceneMatches) {
          readySince ??= Date.now();
          if (Date.now() - readySince >= READY_STABLE_MS) return lastStatus;
        } else {
          readySince = undefined;
        }
      } catch (error) {
        lastError = error instanceof Error ? error : new Error(String(error));
        readySince = undefined;
      }
      await new Promise((resolve) => setTimeout(resolve, 250));
    }

    if (lastStatus?.ready && expectedScenePath) {
      throw new Error(
        `SmileEditor ficou pronto, mas carregou '${lastStatus.scenePath || "nenhuma cena"}' ` +
          `em vez de '${expectedScenePath}'.`,
      );
    }
    throw new Error(
      `SmileEditor nao ficou pronto em ${timeoutSeconds} s.` +
        (lastError ? ` Ultimo erro: ${lastError.message}` : ""),
    );
  }

  async captureFrame(options: CaptureFrameOptions): Promise<CaptureFrameResult> {
    const argumentsObject: Record<string, unknown> = {
      bookmarkSlot: options.bookmarkSlot,
      warmupFrames: options.warmupFrames,
      preset: options.preset,
      pinTimeOfDay: options.pinTimeOfDay,
    };
    if (options.timeOfDayHours !== undefined) {
      argumentsObject.timeOfDayHours = options.timeOfDayHours;
    }

    const rawResult = await this.request(
      "capture_frame",
      argumentsObject,
      options.timeoutSeconds * 1000,
    );
    if (!rawResult || typeof rawResult !== "object") {
      throw new Error("Resultado de captura invalido retornado pelo editor.");
    }

    const result = rawResult as Record<string, unknown>;
    if (
      typeof result.pngPath !== "string" ||
      typeof result.manifestPath !== "string" ||
      typeof result.warmupFrames !== "number"
    ) {
      throw new Error("O editor retornou uma captura sem caminhos ou metadados obrigatorios.");
    }

    const [pngPath, manifestPath] = await Promise.all([
      this.trustedCapturePath(result.pngPath, ".png"),
      this.trustedCapturePath(result.manifestPath, ".json"),
    ]);
    const [pngStat, manifestText] = await Promise.all([
      stat(pngPath),
      readFile(manifestPath, "utf8"),
    ]);
    const manifest = JSON.parse(manifestText) as Record<string, unknown>;

    const capture: CaptureFrameResult = {
      pngPath,
      manifestPath,
      warmupFrames: result.warmupFrames,
      pngBytes: pngStat.size,
      manifest,
    };

    if (options.includeImage) {
      if (pngStat.size <= MAX_INLINE_IMAGE_BYTES) {
        capture.imageData = (await readFile(pngPath)).toString("base64");
      } else {
        capture.imageOmittedReason = "PNG maior que 16 MiB; use o caminho retornado para abri-lo.";
      }
    }
    return capture;
  }

  async configureProfile(
    preset: "gameplay_rr" | "controlled_native",
    bookmarkSlot: number,
  ): Promise<Record<string, unknown>> {
    const result = await this.request(
      "profile_configure",
      { preset, bookmarkSlot },
      30_000,
    );
    if (!result || typeof result !== "object") {
      throw new Error("Configuracao de perfil invalida retornada pelo editor.");
    }
    return result as Record<string, unknown>;
  }

  async profileSnapshot(timeoutMs = 2_000): Promise<ProfileSnapshot> {
    const result = await this.request("profile_snapshot", {}, timeoutMs);
    if (!result || typeof result !== "object") {
      throw new Error("Snapshot de perfil invalido retornado pelo editor.");
    }
    const snapshot = result as Partial<ProfileSnapshot>;
    if (!Array.isArray(snapshot.direct) || !Array.isArray(snapshot.asyncCompute)) {
      throw new Error("Snapshot de perfil sem timestamps de GPU.");
    }
    return snapshot as ProfileSnapshot;
  }

  async shutdown(timeoutSeconds: number): Promise<Record<string, unknown>> {
    const accepted = await this.request("shutdown", {}, 5_000);
    const deadline = Date.now() + timeoutSeconds * 1000;
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 250));
      try {
        await this.status(500);
      } catch {
        return { accepted, stopped: true };
      }
    }
    throw new Error(`SmileEditor nao encerrou em ${timeoutSeconds} s.`);
  }

  private request(command: string, argumentsObject: Record<string, unknown>, timeoutMs: number): Promise<unknown> {
    const id = createHash("sha256")
      .update(`${randomUUID()}-${performance.now()}`)
      .digest("hex")
      .slice(0, 32);
    const request = `${JSON.stringify({
      version: PROTOCOL_VERSION,
      id,
      command,
      arguments: argumentsObject,
    })}\n`;

    return new Promise((resolve, reject) => {
      let settled = false;
      let responseBuffer = "";
      const socket = net.createConnection(editorPipePath());

      const finishError = (error: Error): void => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        reject(error);
      };

      const timer = setTimeout(() => {
        finishError(
          new Error(
            `SmileEditor nao concluiu '${command}' em ${(timeoutMs / 1000).toFixed(0)} s. ` +
              "A captura pode continuar dentro do editor.",
          ),
        );
      }, timeoutMs);
      timer.unref();

      socket.setEncoding("utf8");
      socket.on("connect", () => socket.write(request));
      socket.on("error", (error: NodeJS.ErrnoException) => {
        finishError(new Error(bridgeUnavailableMessage(error)));
      });
      socket.on("data", (chunk: string) => {
        responseBuffer += chunk;
        if (Buffer.byteLength(responseBuffer, "utf8") > MAX_RESPONSE_BYTES) {
          finishError(new Error("Resposta do SmileEditor excede 1 MiB."));
          return;
        }

        const newline = responseBuffer.indexOf("\n");
        if (newline < 0) return;
        const line = responseBuffer.slice(0, newline).trim();
        let response: BridgeEnvelope;
        try {
          response = JSON.parse(line) as BridgeEnvelope;
        } catch {
          finishError(new Error("SmileEditor retornou JSON invalido."));
          return;
        }
        if (response.version !== PROTOCOL_VERSION || response.id !== id) {
          finishError(new Error("SmileEditor retornou uma resposta de protocolo incompativel."));
          return;
        }
        if (!response.ok) {
          finishError(new Error(response.error || `Comando '${command}' falhou.`));
          return;
        }

        settled = true;
        clearTimeout(timer);
        socket.end();
        resolve(response.result);
      });
      socket.on("end", () => {
        if (!settled) finishError(new Error("SmileEditor encerrou a conexao sem responder."));
      });
    });
  }

  private async trustedCapturePath(candidate: string, extension: string): Promise<string> {
    const [root, resolved] = await Promise.all([realpath(this.projectRoot), realpath(candidate)]);
    if (!isInside(root, resolved)) {
      throw new Error("SmileEditor retornou um caminho de captura fora da raiz do projeto.");
    }
    if (path.extname(resolved).toLocaleLowerCase() !== extension) {
      throw new Error(`SmileEditor retornou um arquivo que nao e ${extension}.`);
    }
    return resolved;
  }

  private samePath(left: string, right: string): boolean {
    if (!left || !right) return false;
    const normalize = (value: string): string => path.resolve(value).toLocaleLowerCase("en-US");
    return normalize(left) === normalize(right);
  }
}
