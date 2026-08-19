// ================================================================================================
// Geracao de malha 3D por IA — a metade "de fora" do ciclo de prototipagem.
//
// O ciclo completo e: prompt -> .glb do servico -> SmileCooker (importador glTF) -> .smesh/.sscene
// -> carga aditiva no editor vivo. Este arquivo cobre a primeira seta; o resto ja existia.
//
// POR QUE AQUI, e nao em C++ dentro da engine: o passo e uma chamada HTTP de minutos com polling,
// chave de API e retentativa. Nada disso pertence a um renderer que roda a 16 ms por frame, e o
// MCP ja e a superficie por onde o agente opera o projeto. A engine continua sem saber que
// existe IA no caminho — ela recebe um .glb como receberia de qualquer DCC.
//
// Os provedores sao DADO, nao codigo: cada servico e uma entrada declarativa com endpoint, corpo
// da submissao e como ler o status. Endpoint e nome de campo mudam com a versao da API de cada
// servico, entao os dois sao sobrescreviveis por variavel de ambiente — corrigir um provedor nao
// deve exigir recompilar nem editar TypeScript.
// ================================================================================================

import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

export type ProviderId = "meshy" | "tripo" | "local";

export interface GenerateRequest {
  provider: ProviderId;
  /** Prompt de texto. Exclusivo com imagePath. */
  prompt?: string;
  /** Imagem de referencia, ja em data URI (o chamador le o arquivo). Exclusivo com prompt. */
  imageDataUri?: string;
  /** Nome do asset; vira o diretorio e o nome dos arquivos. */
  name: string;
  negativePrompt?: string;
  artStyle?: string;
  /** Orcamento total, incluindo fila e polling. */
  timeoutSeconds: number;
  pollSeconds: number;
}

export interface ProviderStatusView {
  state: "pending" | "done" | "failed";
  progress?: number;
  downloadUrl?: string;
  error?: string;
}

interface SubmitSpec {
  path: string;
  method: "POST";
  body: Record<string, unknown>;
}

interface ProviderSpec {
  id: ProviderId;
  label: string;
  docs: string;
  defaultBaseUrl: string;
  baseUrlEnv: string;
  apiKeyEnv: string;
  /** O provedor exige chave? O local (self-hosted) normalmente nao. */
  requiresApiKey: boolean;
  supportsText: boolean;
  supportsImage: boolean;
  buildSubmit(request: GenerateRequest): SubmitSpec;
  readTaskId(payload: unknown): string | undefined;
  pollPath(taskId: string, request: GenerateRequest): string;
  readStatus(payload: unknown): ProviderStatusView;
}

// --- leitura tolerante do JSON de resposta -------------------------------------------------------
// As respostas vem de fora e nao sao confiaveis: um campo ausente nao pode virar excecao de tipo
// no meio do polling, e sim um status "ainda nao sei".
function pick(payload: unknown, ...keys: string[]): unknown {
  let current: unknown = payload;
  for (const key of keys) {
    if (typeof current !== "object" || current === null) return undefined;
    current = (current as Record<string, unknown>)[key];
  }
  return current;
}

function asString(value: unknown): string | undefined {
  return typeof value === "string" && value.length > 0 ? value : undefined;
}

function asNumber(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function firstString(payload: unknown, paths: string[][]): string | undefined {
  for (const keys of paths) {
    const found = asString(pick(payload, ...keys));
    if (found) return found;
  }
  return undefined;
}

const PROVIDERS: Record<ProviderId, ProviderSpec> = {
  // Meshy: text-to-3d e image-to-3d na mesma forma (submete, devolve id, poll ate SUCCEEDED).
  meshy: {
    id: "meshy",
    label: "Meshy",
    docs: "https://docs.meshy.ai",
    defaultBaseUrl: "https://api.meshy.ai",
    baseUrlEnv: "SMILE_MESHGEN_MESHY_URL",
    apiKeyEnv: "SMILE_MESHGEN_MESHY_KEY",
    requiresApiKey: true,
    supportsText: true,
    supportsImage: true,
    buildSubmit(request) {
      if (request.imageDataUri) {
        return {
          path: "/openapi/v1/image-to-3d",
          method: "POST",
          body: {
            image_url: request.imageDataUri,
            enable_pbr: true,
          },
        };
      }
      return {
        path: "/openapi/v2/text-to-3d",
        method: "POST",
        body: {
          mode: "preview",
          prompt: request.prompt ?? "",
          ...(request.negativePrompt ? { negative_prompt: request.negativePrompt } : {}),
          ...(request.artStyle ? { art_style: request.artStyle } : {}),
        },
      };
    },
    readTaskId(payload) {
      return firstString(payload, [["result"], ["id"], ["data", "id"], ["task_id"]]);
    },
    pollPath(taskId, request) {
      return request.imageDataUri
        ? `/openapi/v1/image-to-3d/${encodeURIComponent(taskId)}`
        : `/openapi/v2/text-to-3d/${encodeURIComponent(taskId)}`;
    },
    readStatus(payload) {
      const status = (asString(pick(payload, "status")) ?? "").toUpperCase();
      const progress = asNumber(pick(payload, "progress"));
      const url = firstString(payload, [
        ["model_urls", "glb"],
        ["model_url"],
      ]);
      if (status === "SUCCEEDED" || (url && status !== "FAILED")) {
        return { state: "done", progress, downloadUrl: url };
      }
      if (status === "FAILED" || status === "CANCELED" || status === "EXPIRED") {
        return {
          state: "failed",
          error: firstString(payload, [["task_error", "message"], ["error"], ["message"]]) ?? status,
        };
      }
      return { state: "pending", progress };
    },
  },

  // Tripo3D: mesma forma, com o payload aninhado em `data` e status em minusculo.
  tripo: {
    id: "tripo",
    label: "Tripo3D",
    docs: "https://platform.tripo3d.ai/docs",
    defaultBaseUrl: "https://api.tripo3d.ai",
    baseUrlEnv: "SMILE_MESHGEN_TRIPO_URL",
    apiKeyEnv: "SMILE_MESHGEN_TRIPO_KEY",
    requiresApiKey: true,
    supportsText: true,
    // Imagem exige um upload previo com resposta propria; enquanto esse passo nao existir aqui,
    // dizer que suporta so produziria uma falha obscura no meio do polling.
    supportsImage: false,
    buildSubmit(request) {
      return {
        path: "/v2/openapi/task",
        method: "POST",
        body: {
          type: "text_to_model",
          prompt: request.prompt ?? "",
          ...(request.negativePrompt ? { negative_prompt: request.negativePrompt } : {}),
        },
      };
    },
    readTaskId(payload) {
      return firstString(payload, [["data", "task_id"], ["task_id"], ["data", "id"]]);
    },
    pollPath(taskId) {
      return `/v2/openapi/task/${encodeURIComponent(taskId)}`;
    },
    readStatus(payload) {
      const status = (asString(pick(payload, "data", "status")) ?? "").toLowerCase();
      const progress = asNumber(pick(payload, "data", "progress"));
      const url = firstString(payload, [
        ["data", "output", "pbr_model"],
        ["data", "output", "model"],
        ["data", "result", "pbr_model", "url"],
        ["data", "result", "model", "url"],
      ]);
      if (status === "success" || status === "succeeded") {
        return { state: "done", progress, downloadUrl: url };
      }
      if (status === "failed" || status === "cancelled" || status === "banned" || status === "expired") {
        return {
          state: "failed",
          error: firstString(payload, [["data", "message"], ["message"]]) ?? status,
        };
      }
      return { state: "pending", progress };
    },
  },

  // Servico self-hosted (Hunyuan3D, TRELLIS, wrapper de ComfyUI...). Contrato minimo: POST /generate
  // devolve {task_id} ou ja devolve {model_url}; GET /task/<id> devolve {status, progress, model_url}.
  // E o provedor sem custo por chamada — o que importa quando se esta iterando prompt.
  local: {
    id: "local",
    label: "Servico local (self-hosted)",
    docs: "Docs/AI-MESHGEN.md",
    defaultBaseUrl: "http://127.0.0.1:8000",
    baseUrlEnv: "SMILE_MESHGEN_LOCAL_URL",
    apiKeyEnv: "SMILE_MESHGEN_LOCAL_KEY",
    requiresApiKey: false,
    supportsText: true,
    supportsImage: true,
    buildSubmit(request) {
      return {
        path: "/generate",
        method: "POST",
        body: {
          prompt: request.prompt ?? "",
          ...(request.imageDataUri ? { image: request.imageDataUri } : {}),
          ...(request.negativePrompt ? { negative_prompt: request.negativePrompt } : {}),
          format: "glb",
        },
      };
    },
    readTaskId(payload) {
      return firstString(payload, [["task_id"], ["id"], ["result"]]);
    },
    pollPath(taskId) {
      return `/task/${encodeURIComponent(taskId)}`;
    },
    readStatus(payload) {
      const status = (asString(pick(payload, "status")) ?? "").toLowerCase();
      const progress = asNumber(pick(payload, "progress"));
      const url = firstString(payload, [["model_url"], ["glb_url"], ["output", "glb"]]);
      if (status === "failed" || status === "error") {
        return { state: "failed", error: firstString(payload, [["error"], ["message"]]) ?? status };
      }
      if (url && (status === "" || status === "done" || status === "success" || status === "completed")) {
        return { state: "done", progress, downloadUrl: url };
      }
      return { state: "pending", progress };
    },
  },
};

export const PROVIDER_IDS = Object.keys(PROVIDERS) as ProviderId[];

function providerBaseUrl(spec: ProviderSpec): string {
  const configured = process.env[spec.baseUrlEnv]?.trim();
  return (configured && configured.length > 0 ? configured : spec.defaultBaseUrl).replace(/\/+$/, "");
}

/** Descricao dos provedores para o agente escolher. NUNCA inclui o valor da chave. */
export function describeProviders(): Array<Record<string, unknown>> {
  return PROVIDER_IDS.map((id) => {
    const spec = PROVIDERS[id];
    return {
      id: spec.id,
      label: spec.label,
      docs: spec.docs,
      baseUrl: providerBaseUrl(spec),
      apiKeyEnv: spec.apiKeyEnv,
      baseUrlEnv: spec.baseUrlEnv,
      apiKeyConfigured: Boolean(process.env[spec.apiKeyEnv]?.trim()),
      requiresApiKey: spec.requiresApiKey,
      supports: { text: spec.supportsText, image: spec.supportsImage },
      ready: !spec.requiresApiKey || Boolean(process.env[spec.apiKeyEnv]?.trim()),
    };
  });
}

// --- politica de rede ---------------------------------------------------------------------------
// So https, exceto loopback. Um endpoint http remoto mandaria a chave de API em claro, e um
// endpoint arbitrario transformaria esta ferramenta num buscador de URLs para quem controlar a
// resposta de um servico.
function assertAllowedUrl(raw: string): URL {
  let url: URL;
  try {
    url = new URL(raw);
  } catch {
    throw new Error(`URL invalida: ${raw}`);
  }
  const isLoopback =
    url.hostname === "localhost" || url.hostname === "127.0.0.1" || url.hostname === "::1";
  if (url.protocol === "https:") return url;
  if (url.protocol === "http:" && isLoopback) return url;
  throw new Error(`Endpoint recusado (${url.protocol}//${url.hostname}): use https, ou http em loopback.`);
}

const MAX_MODEL_BYTES = 256 * 1024 * 1024;

async function requestJson(
  url: URL,
  init: RequestInit,
  timeoutMs: number,
): Promise<{ status: number; payload: unknown; text: string }> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, { ...init, signal: controller.signal });
    const text = await response.text();
    let payload: unknown = undefined;
    try {
      payload = text.length > 0 ? JSON.parse(text) : undefined;
    } catch {
      payload = undefined;
    }
    return { status: response.status, payload, text };
  } finally {
    clearTimeout(timer);
  }
}

/** Corta o corpo de erro: pode ser uma pagina HTML inteira, e ele vai para o log do agente. */
function briefly(text: string, limit = 400): string {
  const collapsed = text.replace(/\s+/g, " ").trim();
  return collapsed.length > limit ? `${collapsed.slice(0, limit)}…` : collapsed;
}

export interface GenerateResult {
  provider: ProviderId;
  taskId: string;
  /** Caminho relativo a raiz da SmileEngine. */
  modelPath: string;
  bytes: number;
  elapsedSeconds: number;
  pollCount: number;
  log: string[];
}

/** Nome de asset -> slug seguro. E nome de diretorio: nada de separador nem de "..". */
export function slugify(name: string): string {
  const slug = name
    .normalize("NFD")
    .replace(/[\u0300-\u036f]/g, "") // marcas de acento, ja separadas pelo NFD
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 48);
  if (slug.length === 0) throw new Error("O nome do asset precisa ter ao menos um caractere alfanumerico.");
  return slug;
}

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Submete, acompanha e baixa. Escreve em Assets/Generated/<slug>/<slug>.glb sob a raiz informada.
 * Nao cozinha nem carrega — quem encadeia isso e o chamador (tool MCP ou CLI).
 */
export async function generateMesh(root: string, request: GenerateRequest): Promise<GenerateResult> {
  const spec = PROVIDERS[request.provider];
  if (!spec) throw new Error(`Provedor desconhecido: ${request.provider}`);

  const hasPrompt = Boolean(request.prompt?.trim());
  const hasImage = Boolean(request.imageDataUri);
  if (hasPrompt === hasImage) {
    throw new Error("Informe exatamente um de prompt ou imagem.");
  }
  if (hasImage && !spec.supportsImage) {
    throw new Error(`${spec.label} nao aceita imagem por este caminho; use prompt de texto.`);
  }
  if (hasPrompt && !spec.supportsText) {
    throw new Error(`${spec.label} nao aceita prompt de texto.`);
  }

  const apiKey = process.env[spec.apiKeyEnv]?.trim();
  if (spec.requiresApiKey && !apiKey) {
    throw new Error(
      `${spec.label} precisa da chave de API em ${spec.apiKeyEnv}. ` +
        "Defina a variavel no ambiente do servidor MCP — a chave nunca e lida de arquivo do projeto.",
    );
  }

  const slug = slugify(request.name);
  const baseUrl = providerBaseUrl(spec);
  const started = Date.now();
  const deadline = started + request.timeoutSeconds * 1000;
  const log: string[] = [];
  const note = (message: string): void => {
    log.push(`[${((Date.now() - started) / 1000).toFixed(1)}s] ${message}`);
  };

  const headers: Record<string, string> = { "content-type": "application/json" };
  if (apiKey) headers.authorization = `Bearer ${apiKey}`;

  const submit = spec.buildSubmit(request);
  const submitUrl = assertAllowedUrl(`${baseUrl}${submit.path}`);
  note(`submetendo em ${spec.label} (${submit.path})`);
  const submission = await requestJson(
    submitUrl,
    { method: submit.method, headers, body: JSON.stringify(submit.body) },
    Math.min(60_000, request.timeoutSeconds * 1000),
  );
  if (submission.status >= 400) {
    throw new Error(`${spec.label} recusou a submissao (HTTP ${submission.status}): ${briefly(submission.text)}`);
  }

  // Um servico local sincrono pode ja devolver o modelo na propria submissao.
  let downloadUrl = spec.readStatus(submission.payload).downloadUrl;
  const taskId = spec.readTaskId(submission.payload) ?? "";
  if (!downloadUrl && !taskId) {
    throw new Error(`${spec.label} nao devolveu id de tarefa nem url do modelo: ${briefly(submission.text)}`);
  }
  if (taskId) note(`tarefa ${taskId} aceita`);

  let pollCount = 0;
  while (!downloadUrl) {
    if (Date.now() >= deadline) {
      throw new Error(
        `tempo esgotado apos ${request.timeoutSeconds}s aguardando ${spec.label} (tarefa ${taskId}). ` +
          "A tarefa continua no servico; aumente timeoutSeconds para acompanhar a mesma geracao.",
      );
    }
    await sleep(request.pollSeconds * 1000);
    pollCount += 1;
    const pollUrl = assertAllowedUrl(`${baseUrl}${spec.pollPath(taskId, request)}`);
    const poll = await requestJson(pollUrl, { method: "GET", headers }, 60_000);
    if (poll.status >= 400) {
      throw new Error(`${spec.label} falhou no polling (HTTP ${poll.status}): ${briefly(poll.text)}`);
    }
    const status = spec.readStatus(poll.payload);
    if (status.state === "failed") {
      throw new Error(`${spec.label} reportou falha na geracao: ${status.error ?? "sem detalhe"}`);
    }
    if (status.progress !== undefined) note(`progresso ${status.progress}%`);
    if (status.state === "done") {
      if (!status.downloadUrl) {
        throw new Error(`${spec.label} concluiu a tarefa sem url de modelo: ${briefly(poll.text)}`);
      }
      downloadUrl = status.downloadUrl;
    }
  }

  note("baixando .glb");
  const modelUrl = assertAllowedUrl(downloadUrl);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), Math.max(30_000, deadline - Date.now()));
  let bytes: Uint8Array;
  try {
    // A url de download costuma ser assinada (S3 e afins) e nao aceita o header de autorizacao —
    // manda-lo alem de inutil vazaria a chave para outro host.
    const response = await fetch(modelUrl, { signal: controller.signal });
    if (!response.ok) {
      throw new Error(`download do modelo falhou (HTTP ${response.status})`);
    }
    const buffer = await response.arrayBuffer();
    if (buffer.byteLength > MAX_MODEL_BYTES) {
      throw new Error(`modelo de ${buffer.byteLength} bytes excede o limite de 256 MB`);
    }
    bytes = new Uint8Array(buffer);
  } finally {
    clearTimeout(timer);
  }

  // Conferencia barata antes de gravar: 'glTF' (binario) ou '{' (texto). Um HTML de erro entregue
  // com HTTP 200 e o modo de falha comum destes servicos, e ele chegaria ate o Cooker como "asset".
  const isGlb = bytes.length >= 4 && bytes[0] === 0x67 && bytes[1] === 0x6c && bytes[2] === 0x54 && bytes[3] === 0x46;
  const isJson = bytes.length >= 1 && (bytes[0] === 0x7b || bytes[0] === 0x20 || bytes[0] === 0x0a);
  if (!isGlb && !isJson) {
    throw new Error("o conteudo baixado nao e um glTF/GLB (primeiros bytes nao batem com o formato).");
  }

  const directory = path.join(root, "Assets", "Generated", slug);
  await mkdir(directory, { recursive: true });
  const modelPath = path.join(directory, `${slug}.glb`);
  await writeFile(modelPath, bytes);
  note(`gravado ${(bytes.length / 1024).toFixed(0)} KB`);

  return {
    provider: spec.id,
    taskId: taskId || "(sincrono)",
    modelPath: path.relative(root, modelPath).split(path.sep).join("/"),
    bytes: bytes.length,
    elapsedSeconds: Number(((Date.now() - started) / 1000).toFixed(1)),
    pollCount,
    log,
  };
}
