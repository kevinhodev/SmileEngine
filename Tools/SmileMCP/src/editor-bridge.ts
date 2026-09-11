import { createHash, randomUUID } from "node:crypto";
import { readFile, realpath, stat } from "node:fs/promises";
import net from "node:net";
import path from "node:path";

// v3: gpuSourceFrameIndex virou directGpuSourceFrameIndex + asyncComputeSourceFrameIndex, uma
// identidade por FILA. v2 incluiu o bloco "cpu". Ambos exigidos por este
// cliente. O bump faz o par incompativel falhar no HANDSHAKE, com causa explicita, em vez de
// so quebrar dentro do profile_snapshot. O nome do pipe segue em v1: e rendezvous, nao
// protocolo — mantendo-o, a rejeicao e "versao incompativel" e nao "editor nao encontrado".
// 4: o bloco cpu.gpuSlots. A agregacao passa a exigi-lo, entao um editor antigo tem de ser
// recusado em vez de produzir "undefined" no lugar do numero.
// 5: os blocos cpu.materialTable e cpu.meshTable (tabelas canonicas da Fase 2). Mesma regra: eles
// entraram no RESUMO, que e o que a regua le com o includeSamples default (false). Enquanto
// estavam so na amostra crua, existiam no editor e nao chegavam a quem mede.
// 6: cpu.instanceTable, com footprint e sujeira das quatro colunas canonicas por slot.
// 9: cpu.instanceTable.rowsScanned (Fase 4). Entra no RESUMO pela mesma regra do 5: e o numero
// que separa "upload incremental" de "trabalho incremental" e que julga o gate da fase. Um editor
// anterior publicaria null exatamente onde a medida decide, e um relatorio com null nesse campo
// parece completo sem ser.
// 10: transformBinds/transformSlotSets por PASSE, e o toggle bindlessTransforms (Fase 4).
//
// Os dois contadores entram no resumo porque `transformBinds == 0` e a prova de QUAL CAMINHO
// EXECUTOU — a imagem sozinha nao da isso, porque os dois caminhos produzem o mesmo pixel. Ela e
// distinta de `objectConstantBytes == 0`, que prova que o BUFFER deixou de ser escrito: um passe
// migrado zera o primeiro enquanto o segundo continua alto, e o segundo so cai quando todos os
// passes migrarem. Uma nao substitui a outra, e publicar so uma delas deixaria metade do gate
// sem evidencia.
// 11: `settings.sunShadows` e `settings.shadowCascadeCache`, com readback efetivo e override, mais
// `bindlessTransforms` no manifesto da captura.
//
// Sao PRE-REQUISITOS de leitura, e nao knobs a mais: com o cache das cascatas ligado,
// `passes.sunShadows.draws == 0` e ambiguo entre "nao desenhou" e "reusou o mapa" — os dois sao
// estados legitimos e o contador nao os separa. Um A/B de sombra que nao declara os dois nao
// prova nada, e o manifesto sem `bindlessTransforms` deixa duas capturas do MESMO par sob teste
// com procedencia identica.
// 12: `cpu.objectConstantBytesReserved`. As DUAS provas do gate do ObjectCB, separadas:
// `objectConstantBytes == 0` diz que ninguem mais ESCREVE; esta diz que o buffer deixou de
// EXISTIR. Durante seis das sete migracoes da fase a primeira caiu por passe enquanto a segunda
// ficou intacta, entao uma nao substitui a outra. Upload heap nao e itemizado pelo VramTracker
// (que cobre VRAM/DEFAULT), e por isso o numero vem do renderer e nao de la.
// 13: derivedTemporalTransforms controls all three temporal consumers as one A/B arm.
const PROTOCOL_VERSION = 13;
const DEFAULT_PIPE_NAME = "SmileEngine-MCP-v1";
const MAX_RESPONSE_BYTES = 1024 * 1024;
const MAX_INLINE_IMAGE_BYTES = 16 * 1024 * 1024;
// O renderer pode anunciar Available por um instante entre a criacao do viewport e o commit
// assincrono da cena, voltando a false enquanto troca os recursos. Uma amostra positiva fez o
// run_editor liberar a captura cedo demais na Bistro. Exija uma janela continua, nao um pulso.
const READY_STABLE_MS = 1_500;
// Comando avulso contra um editor pesado. Todo handler do McpBridge roda no event loop da GUI,
// entao o round-trip acompanha o custo do frame: medido na sintetica de 1M (~3 FPS), um status
// leva 380 ms de p50. O prazo e folgado de proposito — ele so precisa NAO expirar num editor
// vivo; quem prova encerramento e o PID, nunca um prazo estourado.
export const SLOW_EDITOR_COMMAND_TIMEOUT_MS = 10_000;
// Sonda de pipe: nao envia comando, entao nao espera frame nenhum.
const PIPE_PROBE_TIMEOUT_MS = 1_000;
const STOP_POLL_INTERVAL_MS = 250;

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
  resetHistory: boolean;
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

/**
 * Um balde de submissao de CPU. `key` e estavel em ingles e vem do engine
 * (Smile::SubmitPassKey); os rotulos traduzidos vivem so no painel do editor, entao
 * benchmark nao quebra quando um rotulo da UI e renomeado.
 *
 * `considered` e o que o laco do passe visitou; `draws` e o que virou DrawIndexedInstanced.
 * A diferenca entre os dois E o filtro daquele passe.
 */
export interface CpuSubmitPass {
  key: string;
  considered: number;
  draws: number;
  materialBinds: number;
  meshBinds: number;
  skippedInvalid: number;
  /**
   * Fase 3 do GPU-DRIVEN-PLAN. `bindlessDraws` e quanto do passe leu material da tabela
   * canonica. Nao se deriva fallback por subtracao: draws que nao consomem material pertencem
   * a um terceiro estado. `materialRowSets` mede escritas efetivas da linha;
   * `materialRowMisses` tem de ser zero — material fora da tabela desenha como o cinza da
   * linha 0 sem erro nenhum.
   */
  bindlessDraws: number;
  /**
   * Draws que CONSOMEM material e ainda o amarram por item. Nao e `draws - bindlessDraws`:
   * draw sem material — o z-prepass opaco inteiro — nao e fallback de material.
   */
  materialFallbackDraws: number;
  materialRowSets: number;
  materialRowMisses: number;
  /**
   * Fase 4. `transformBinds` conta os CBV de 256 B amarrados por item; `transformSlotSets`, as
   * constantes de root do caminho bindless. Num passe migrado o primeiro tem de ser ZERO — e essa
   * e a prova de que o passe executou o caminho novo, que a imagem sozinha nao da.
   */
  transformBinds: number;
  transformSlotSets: number;
}

export interface CpuProfileStats {
  /**
   * Frame que ESTES numeros descrevem. Difere do `frameIndex` de fora, que ja avancou quando
   * o snapshot e lido — correlacionar CPU com GPU exige este campo, nao aquele.
   */
  sourceFrameIndex: number;
  timingsMs: {
    buildDrawLists: number;
    cullSort: number;
    /** Classificacao por lote do G-buffer: o O(N log N) que a Fase 5 do GPU-driven remove. */
    batchSort: number;
    shadowLists: number;
    submit: number;
  };
  scene: {
    renderables: number;
    drawItems: number;
    visibleItems: number;
    occludedItems: number;
    materials: number;
    meshes: number;
  };
  objectConstantBytes: number;
  /**
   * Bytes RESERVADOS pelo ObjectCB. Prova de memoria, distinta da de escrita acima, e MEDIDA no
   * recurso (GetResourceAllocationInfo) em vez de derivada da capacidade.
   *
   * Inclui os buffers aposentados que ainda esperam a fence: memoria retida e memoria retida, e so
   * assim o numero fica vermelho se a coleta parar de rodar. Isso acontece apenas depois de uma
   * aposentadoria FORA de dreno (hot reload derrubando uma PSO bindless) e dura kFramesInFlight
   * frames; a troca de toggle drena e libera na hora, entao a medicao da fase le zero limpo.
   */
  objectConstantBytesReserved: number;
  /** `retired` e `overflows` entraram no protocolo 7 (Fase 3). */
  srvHeap: { used: number; highWater: number; capacity: number; retired: number;
             overflows: number };
  // Pool de residencia GPU. Retired parado com capacity acima de live e o sintoma de um Collect
  // que deixou de rodar -- crescimento que nao quebra nada e so aparece como memoria a mais.
  gpuSlots: { capacity: number; live: number; retired: number; exhausted: number };
  // Tabelas canonicas da cena (Fase 2 do GPU-DRIVEN-PLAN). rowsDirty e o numero do gate: zero em
  // todo frame sem edicao de material. residentBytes e VRAM; stagingBytes e memoria do host (os
  // slices de upload, um por frame em voo, persistentes).
  materialTable: {
    rows: number;
    residentBytes: number;
    stagingBytes: number;
    rowsDirty: number;
    copyRegions: number;
    rowsUploadedTotal: number;
  };
  // Sem rowsDirty: geometria nao e editavel em runtime, entao as linhas so mudam num Setup.
  meshTable: { rows: number; residentBytes: number; stagingBytes: number };
  instanceTable: {
    rows: number;
    residentBytes: number;
    stagingBytes: number;
    rowsDirty: number;
    bytesDirty: number;
    copyRegions: number;
    // Slots que o diff comparou para achar as sujas acima (Fase 4).
    rowsScanned: number;
    bytesUploadedTotal: number;
  };
  passes: CpuSubmitPass[];
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
  /**
   * Frame que produziu os timestamps de `direct`. ATRASADO em relacao a `frameIndex` e a
   * `cpu.sourceFrameIndex`: o slot so e legivel depois do fence.
   */
  directGpuSourceFrameIndex: number;
  /**
   * Frame que produziu os timestamps de `asyncCompute`. Serie PROPRIA: a fila tem ring proprio
   * e pode ficar suspensa por frames, entao pode pertencer a outro frame que a direct.
   * `null` quando a fila nao produziu resultado neste snapshot.
   */
  asyncComputeSourceFrameIndex: number | null;
  /** Telemetria de CPU do ultimo frame FECHADO. Ver CpuProfileStats. */
  cpu: CpuProfileStats;
}

export interface ProfileConfiguration {
  preset: "gameplay_rr" | "controlled_native" | "controlled_nrd";
  bookmarkSlot: number;
  timeOfDayHours: number;
  backgroundThrottle: boolean;
  // Readback posterior ao apply, nao eco dos argumentos: inclui pedido/efetivo da politica GI.
  settings: Record<string, unknown>;
}

export type IndirectPrimary = "restir_sharc" | "ddgi" | "off";
export type IndirectFallback = "ddgi" | "environment" | "black";

export interface CameraPose {
  position: { x: number; y: number; z: number };
  pitchDegrees: number;
  yawDegrees: number;
}

export interface SceneObjectSnapshot {
  index: number;
  id: number;
  name: string;
  visible: boolean;
  dynamic: boolean;
  position: { x: number; y: number; z: number };
  boundsMin: { x: number; y: number; z: number };
  boundsMax: { x: number; y: number; z: number };
}

export interface GIConfigureOverrides {
  ddgiEnabled?: boolean;
  indirectPrimary?: IndirectPrimary;
  indirectFallback?: IndirectFallback;
  cascadeCount?: number;
  interleavedUpdates?: boolean;
  updateRayBudget?: number;
  probeCompaction?: boolean;
  halfRes?: boolean;
  adaptiveRays?: boolean;
  adaptiveHysteresis?: boolean;
}

export interface GICascadeStatus {
  index: number;
  gridMin: { x: number; y: number; z: number };
  spacing: number;
  scrollCells: { x: number; y: number; z: number };
  updateAge: number;
}

export interface GIStatus {
  frameIndex: number;
  policy: {
    primary: { requested: IndirectPrimary; effective: IndirectPrimary };
    fallback: { requested: IndirectFallback; effective: IndirectFallback };
  };
  ddgi: {
    enabled: boolean;
    initialized: boolean;
    desiredCascadeCount: number;
    actualCascadeCount: number;
    gridCount: { x: number; y: number; z: number };
    probesPerCascade: number;
    totalProbes: number;
    raysPerProbe: number;
    adaptiveMinRays: number;
    adaptiveMaxRays: number;
    interleavedUpdates: boolean;
    updateRayBudget: number;
    updateBudgetEffective: boolean;
    effectiveProbeBudget: number;
    scheduledProbeCount: number;
    scheduledProbeStart: number;
    scheduledCascadeMask: number;
    probeCompaction: boolean;
    probeCompactionEffective: boolean;
    activeProbeCount: number;
    compactedProbeCapacity: number;
    selectedRayCount: number;
    newProbeCount: number;
    regionalProbeCount: number;
    followUpProbeCount: number;
    quotaProbeCount: number;
    roundRobinProbeCount: number;
    probeWakeInterval: number;
    lastProbeWakeSerial: number;
    scheduledCascadeCount: number;
    lastUpdatedCascadeCount: number;
    updateSerial: number;
    lastForcedUpdateSerial: number;
    scheduledFullForced: boolean;
    adaptiveRays: boolean;
    adaptiveHysteresis: boolean;
    cascades: GICascadeStatus[];
  };
  restirGI: {
    halfRes: boolean;
    halfResEffective: boolean;
    renderWidth: number;
    renderHeight: number;
  };
  settings: Record<string, unknown>;
}

export interface VolFogConfigureOverrides {
  temporal?: boolean;
}

export interface VolFogStatus {
  enabled: boolean;
  temporal: boolean;
  maxDistance: number;
  phaseG: number;
  lightsIntensity: number;
}

// Overrides da regua da Fase 0. Knob de amostragem ausente PRESERVA o valor corrente; a hora
// ausente usa 10:00 para manter o baseline historico. Todos ficam opcionais na borda para `null`
// poder significar "nao informado" em payloads montados dinamicamente.
export interface ProfileOverrides {
  timeOfDayHours?: number;
  rainAmount?: number;
  upscalerQuality?: number;
  diAnalyticCandidates?: number;
  diMeshCandidates?: number;
  meshLightsInPool?: boolean;
  diInitialVisibility?: boolean;
  diBrdfRatio?: boolean;
  meshCompactSupport?: boolean;
  bindlessMaterials?: boolean;
  bindlessTransforms?: boolean;
  derivedTemporalTransforms?: boolean;
  sunShadows?: boolean;
  shadowCascadeCache?: boolean;
  backgroundThrottle?: boolean;
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

/**
 * A named pipe nao existe: nao ha editor no ar. E a UNICA falha de transporte que autoriza
 * concluir "encerrado".
 */
export class EditorUnavailableError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "EditorUnavailableError";
  }
}

/**
 * A pipe atendeu e a resposta nao chegou no prazo. O editor esta VIVO, so lento — confundir isto
 * com o caso acima foi o defeito que fez o close_editor declarar sucesso sobre um editor que
 * seguiu renderizando por dez minutos.
 */
export class EditorTimeoutError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "EditorTimeoutError";
  }
}

// As mensagens sao as mesmas de antes das classes existirem: scripts que casam pelo texto
// continuam valendo, e quem precisa decidir passa a usar o tipo.
function bridgeUnavailableError(error: NodeJS.ErrnoException): Error {
  if (error.code === "ENOENT" || error.code === "ECONNREFUSED") {
    return new EditorUnavailableError(
      "SmileEditor nao encontrado na named pipe local. Inicie uma build que contenha o " +
        "McpBridge e aguarde o renderer ficar pronto.",
    );
  }
  return new Error(`Falha ao comunicar com o SmileEditor: ${error.message}`);
}

/**
 * Existencia do PROCESSO, e nao afirmacao do editor sobre si mesmo. `kill(pid, 0)` nao envia
 * sinal: so pergunta ao SO se o PID existe. E a unica confirmacao de encerramento que nao passa
 * pelo event loop da GUI — logo, a unica que nao fica mais fragil conforme a cena pesa mais.
 */
function processIsAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (code === "ESRCH") return false;
    if (code === "EPERM") return true; // existe; so nao da para sinalizar
    throw error;
  }
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
      resetHistory: options.resetHistory,
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
    preset: "gameplay_rr" | "controlled_native" | "controlled_nrd",
    bookmarkSlot: number,
    overrides: ProfileOverrides = {},
  ): Promise<ProfileConfiguration> {
    // Espalhados e nao enumerados: uma chave so viaja quando o chamador a definiu, e o editor
    // trata ausencia como "preserve". Enumerar aqui reintroduziria `undefined` no JSON.
    const result = await this.request(
      "profile_configure",
      { preset, bookmarkSlot, ...overrides },
      30_000,
    );
    if (!result || typeof result !== "object") {
      throw new Error("Configuracao de perfil invalida retornada pelo editor.");
    }
    const configuration = result as Partial<ProfileConfiguration>;
    if (
      configuration.preset !== preset ||
      typeof configuration.bookmarkSlot !== "number" ||
      typeof configuration.timeOfDayHours !== "number" ||
      typeof configuration.backgroundThrottle !== "boolean" ||
      !configuration.settings ||
      typeof configuration.settings !== "object" ||
      Array.isArray(configuration.settings)
    ) {
      throw new Error("Configuracao de perfil sem readback obrigatorio do estado aplicado.");
    }
    return configuration as ProfileConfiguration;
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
    // Mesma exigencia dos timings de GPU: sem o bloco de CPU o snapshot nao serve de regua,
    // e aceitar silenciosamente produziria benchmark com metade dos eixos faltando.
    const cpu = snapshot.cpu as Partial<CpuProfileStats> | undefined;
    if (!cpu || typeof cpu !== "object" ||
        typeof cpu.sourceFrameIndex !== "number" ||
        !cpu.timingsMs || typeof cpu.timingsMs !== "object" ||
        !Array.isArray(cpu.passes)) {
      throw new Error("Snapshot de perfil sem telemetria de CPU.");
    }
    // gpuSlots e OBRIGATORIO no protocolo 4 -- foi por exigi-lo que a versao subiu. Sem validar
    // aqui, a exigencia existe so no numero da versao: um payload v4 sem o bloco, ou com um campo
    // string/undefined, atravessa o cast e quebra la adiante no summarizeProfile com TypeError.
    // Erro na BORDA diz o que veio errado do editor; TypeError no agregador diz "cannot read
    // properties of undefined" numa linha que nao tem nada a ver com a causa.
    const slots = cpu.gpuSlots as Partial<CpuProfileStats["gpuSlots"]> | undefined;
    if (!slots || typeof slots !== "object") {
      throw new Error("Snapshot de perfil sem o bloco cpu.gpuSlots (protocolo 4).");
    }
    for (const campo of ["capacity", "live", "retired", "exhausted"] as const) {
      // O contrato e u32, e finito nao e o contrato: Number.isFinite aceita negativo, fracao e
      // valores acima de UINT32_MAX, nenhum deles produzivel pela origem. Se um chegar, o
      // payload nao veio de onde diz que veio, e tratar isso como numero valido propagaria a
      // mentira para dentro da agregacao.
      const valor = slots[campo];
      if (!Number.isInteger(valor) || (valor as number) < 0 || (valor as number) > 0xffffffff) {
        throw new Error(
          `Snapshot de perfil com cpu.gpuSlots.${campo} invalido ` +
            `(esperado inteiro em [0, 4294967295], veio ${typeof valor} ${String(valor)}).`,
        );
      }
    }
    // Os quatro contadores PARTICIONAM a capacidade: cada slot esta em exatamente um estado
    // (Free, Live, Retired ou Exhausted). Uma soma acima da capacidade descreve um pool que o
    // allocator nao consegue produzir -- e o tipo de incoerencia que passaria por qualquer
    // checagem campo a campo, porque cada numero isolado continua plausivel.
    const capacity = slots.capacity as number;
    const ocupados = (slots.live as number) + (slots.retired as number) +
                     (slots.exhausted as number);
    if (ocupados > capacity) {
      throw new Error(
        `Snapshot de perfil com cpu.gpuSlots incoerente: live+retired+exhausted = ${ocupados} ` +
          `acima de capacity ${capacity}.`,
      );
    }
    // materialTable/meshTable sao OBRIGATORIOS no protocolo 5 e instanceTable no 6, pela mesma
    // razao que gpuSlots foi no 4. Sem validar na borda, um editor anterior a Fase 2
    // atravessaria o cast e o resumo publicaria "null" onde deveria estar o numero do gate --
    // um relatorio que parece completo e nao e.
    //
    // DOIS contratos, e a diferenca importa. CONTAGENS (linhas, sujas, regioes) sao u32 no
    // produtor. BYTES e ACUMULADOS sao u64 -- rowsUploadedTotal soma desde o ultimo setup da
    // tabela, e nao volta a zero por frame. Dar a todos o teto de u32 recusaria um snapshot
    // legitimo de um editor rodando ha muito tempo, que e o unico momento em que o acumulado
    // serve para alguma coisa.
    //
    // O teto do u64 aqui NAO e 2^64: o editor serializa como double (QJsonValue nao tem inteiro),
    // entao acima de 2^53-1 o numero ja chega perdendo precisao. Number.isSafeInteger e
    // exatamente essa fronteira, e recusar la e recusar onde o dado deixa de ser confiavel.
    const kU32Max = 0xffffffff;
    const kU64Max = Number.MAX_SAFE_INTEGER;
    const tabelas: Array<[string, ReadonlyArray<readonly [string, number]>]> = [
      ["materialTable", [
        ["rows", kU32Max],
        ["residentBytes", kU64Max],
        ["stagingBytes", kU64Max],
        ["rowsDirty", kU32Max],
        ["copyRegions", kU32Max],
        ["rowsUploadedTotal", kU64Max],
      ]],
      ["meshTable", [
        ["rows", kU32Max],
        ["residentBytes", kU64Max],
        ["stagingBytes", kU64Max],
      ]],
      ["instanceTable", [
        ["rows", kU32Max],
        ["residentBytes", kU64Max],
        ["stagingBytes", kU64Max],
        ["rowsDirty", kU32Max],
        ["bytesDirty", kU64Max],
        ["copyRegions", kU32Max],
        ["rowsScanned", kU32Max],
        ["bytesUploadedTotal", kU64Max],
      ]],
    ];
    for (const [nome, campos] of tabelas) {
      const bloco = (cpu as unknown as Record<string, unknown>)[nome] as
        | Record<string, unknown>
        | undefined;
      if (!bloco || typeof bloco !== "object") {
        throw new Error(`Snapshot de perfil sem o bloco cpu.${nome} (protocolo 6).`);
      }
      for (const [campo, teto] of campos) {
        // isSafeInteger e nao isFinite: finito aceita negativo, fracao e o que estiver acima da
        // faixa exata do double. Nenhum deles e produzivel pela origem, e tratar um como numero
        // valido propagaria a mentira para dentro da agregacao.
        const valor = bloco[campo];
        if (!Number.isSafeInteger(valor) || (valor as number) < 0 || (valor as number) > teto) {
          throw new Error(
            `Snapshot de perfil com cpu.${nome}.${campo} invalido ` +
              `(esperado inteiro em [0, ${teto}], veio ${typeof valor} ${String(valor)}).`,
          );
        }
      }
    }
    // Os DOIS numeros do gate do ObjectCB (protocolo 12). Entraram no tipo e no resumo sem
    // entrar aqui: apagar `objectConstantBytesReserved` do payload atravessava o cast, e o
    // resumo publicava `undefined` no campo que decide a fase. Mesma falha que fez o gpuSlots
    // virar exigencia na v4.
    //
    // BYTES, entao u64/isSafeInteger e nao u32: no 1M o reservado passa de 512 MB, e o acumulado
    // por frame nao cabe no teto de 32 bits.
    for (const campo of ["objectConstantBytes", "objectConstantBytesReserved"] as const) {
      const valor = (cpu as unknown as Record<string, unknown>)[campo];
      if (!Number.isSafeInteger(valor) || (valor as number) < 0) {
        throw new Error(
          `Snapshot de perfil com cpu.${campo} invalido ` +
            `(esperado inteiro em [0, ${kU64Max}], veio ${typeof valor} ${String(valor)}).`,
        );
      }
    }
    // ESCRITA nao pode passar do RESERVADO -- e escrever num buffer que nao existe e a
    // incoerencia que interessa, porque as duas provas do gate sao independentes justamente
    // para poderem discordar. Um snapshot com `bytes > 0` e `reserved == 0` descreve um
    // renderer impossivel, e aceita-lo colocaria essa mentira dentro da agregacao.
    if ((cpu.objectConstantBytes as number) > (cpu.objectConstantBytesReserved as number)) {
      throw new Error(
        `Snapshot incoerente: cpu.objectConstantBytes = ${cpu.objectConstantBytes} acima do ` +
          `reservado ${cpu.objectConstantBytesReserved} (escrita num buffer que nao existe).`,
      );
    }

    // Heap de descritores: `retired` e `overflows` entraram no protocolo 7 pela mesma razao dos
    // contadores por passe — o resumo os le, e um editor anterior a Fase 3 os traria ausentes.
    const heap = cpu.srvHeap as Partial<CpuProfileStats["srvHeap"]> | undefined;
    if (!heap || typeof heap !== "object") {
      throw new Error("Snapshot de perfil sem o bloco cpu.srvHeap (protocolo 7).");
    }
    for (const campo of ["used", "highWater", "capacity", "retired", "overflows"] as const) {
      const valor = heap[campo];
      if (!Number.isInteger(valor) || (valor as number) < 0 || (valor as number) > 0xffffffff) {
        throw new Error(
          `Snapshot de perfil com cpu.srvHeap.${campo} invalido ` +
            `(esperado inteiro em [0, 4294967295], veio ${typeof valor} ${String(valor)}).`,
        );
      }
    }
    // Usados e aposentados PARTICIONAM o que esta indisponivel: um slot esta num estado ou no
    // outro. A soma acima da capacidade descreve um heap que o allocator nao produz.
    if ((heap.used as number) + (heap.retired as number) > (heap.capacity as number)) {
      throw new Error(
        `Snapshot incoerente: cpu.srvHeap com used+retired = ` +
          `${(heap.used as number) + (heap.retired as number)} acima de capacity ${heap.capacity}.`,
      );
    }

    // Contadores POR PASSE. Obrigatorios no protocolo 7, e a versao subiu por aqui: os tres da
    // Fase 3 (`bindlessDraws`, `materialRowSets`, `materialRowMisses`) sao lidos pelo
    // summarizeProfile, entao um editor anterior a fase os traria ausentes e o resumo publicaria
    // agregado de `undefined` — a mesma falha que fez o gpuSlots virar exigencia na v4.
    //
    // Os tres antigos entram no mesmo laco de proposito: validar so os novos e continuar lendo
    // os velhos sem checar seria duas politicas para o mesmo bloco.
    for (let i = 0; i < cpu.passes.length; ++i) {
      const passe = cpu.passes[i] as unknown as Record<string, unknown>;
      if (!passe || typeof passe !== "object" || typeof passe.key !== "string") {
        throw new Error(`Snapshot de perfil com cpu.passes[${i}] sem chave de passe.`);
      }
      for (const campo of ["considered", "draws", "materialBinds", "meshBinds", "skippedInvalid",
                           "bindlessDraws", "materialFallbackDraws", "materialRowSets",
                           "materialRowMisses", "transformBinds", "transformSlotSets"] as const) {
        const valor = passe[campo];
        if (!Number.isInteger(valor) || (valor as number) < 0 || (valor as number) > 0xffffffff) {
          throw new Error(
            `Snapshot de perfil com cpu.passes[${i}].${campo} invalido ` +
              `(esperado inteiro em [0, 4294967295], veio ${typeof valor} ${String(valor)}).`,
          );
        }
      }
      // TRES estados exclusivos por draw: leu material pela tabela, amarrou por item, ou nao
      // consome material. Os dois primeiros nao podem passar do total — e nao podem SOMAR o
      // total obrigatoriamente, porque o terceiro existe e e maioria em passes inteiros.
      const comMaterial = (passe.bindlessDraws as number) +
                          (passe.materialFallbackDraws as number);
      if (comMaterial > (passe.draws as number)) {
        throw new Error(
          `Snapshot incoerente: cpu.passes[${i}] com bindlessDraws + materialFallbackDraws = ` +
            `${comMaterial} acima de draws ${passe.draws}.`,
        );
      }
    }
    if (typeof snapshot.directGpuSourceFrameIndex !== "number") {
      throw new Error("Snapshot de perfil sem identidade da fila direct.");
    }
    // O campo da async e nullable, e por isso mesmo precisa ser validado: ausente, string ou
    // qualquer outro tipo atravessaria o cast e o resumo o leria como "fila suspensa" — uma
    // interpretacao plausivel e errada, que e o modo de falha mais caro numa regua.
    const asyncIndex = snapshot.asyncComputeSourceFrameIndex;
    if (asyncIndex !== null && typeof asyncIndex !== "number") {
      throw new Error(
        "Snapshot de perfil com identidade da fila async invalida " +
          `(esperado number ou null, veio ${typeof asyncIndex}).`,
      );
    }
    // Pareamento: timings e identidade sao a MESMA afirmacao vista de dois lados, e divergirem
    // significa que o editor publicou um estado que nao existe.
    if (snapshot.asyncCompute.length === 0 && asyncIndex !== null) {
      throw new Error(
        `Snapshot incoerente: fila async sem timings mas com identidade ${asyncIndex}.`,
      );
    }
    if (snapshot.asyncCompute.length > 0 && asyncIndex === null) {
      throw new Error(
        `Snapshot incoerente: fila async com ${snapshot.asyncCompute.length} escopos ` +
          "mas sem identidade de frame.",
      );
    }
    return snapshot as ProfileSnapshot;
  }

  async cameraPose(timeoutMs = 2_000): Promise<CameraPose> {
    const result = await this.request("camera_get", {}, timeoutMs);
    return this.validateCameraPose(result);
  }

  async setCameraPose(
    pose: {
      x: number;
      y: number;
      z: number;
      pitchDegrees: number;
      yawDegrees: number;
      cameraCut?: boolean;
    },
    timeoutMs = 5_000,
  ): Promise<CameraPose> {
    const result = await this.request("camera_set", pose, timeoutMs);
    return this.validateCameraPose(result);
  }

  async sceneObjects(
    options: { nameContains?: string; limit: number },
    timeoutMs = 5_000,
  ): Promise<{ objects: SceneObjectSnapshot[]; count: number }> {
    const result = await this.request("scene_objects", options, timeoutMs);
    if (!result || typeof result !== "object" || Array.isArray(result))
      throw new Error("Lista de objetos invalida retornada pelo editor.");
    const value = result as { objects?: unknown; count?: unknown };
    if (!Array.isArray(value.objects) || typeof value.count !== "number")
      throw new Error("Lista de objetos sem array ou contagem.");
    return result as { objects: SceneObjectSnapshot[]; count: number };
  }

  async setSceneObjectPosition(
    position: { index: number; x: number; y: number; z: number },
    timeoutMs = 5_000,
  ): Promise<SceneObjectSnapshot> {
    const result = await this.request("scene_object_set_position", position, timeoutMs);
    if (!result || typeof result !== "object" || Array.isArray(result))
      throw new Error("Objeto invalido retornado pelo editor.");
    const object = result as Partial<SceneObjectSnapshot>;
    if (typeof object.index !== "number" || typeof object.name !== "string" ||
        !object.position || typeof object.position.x !== "number" ||
        typeof object.position.y !== "number" || typeof object.position.z !== "number")
      throw new Error("Objeto sem indice, nome ou posicao efetiva.");
    return object as SceneObjectSnapshot;
  }

  async giStatus(timeoutMs = 2_000): Promise<GIStatus> {
    const result = await this.request("gi_status", {}, timeoutMs);
    return this.validateGIStatus(result);
  }

  async configureGI(
    overrides: GIConfigureOverrides,
    timeoutMs = 30_000,
  ): Promise<GIStatus> {
    const result = await this.request("gi_configure", { ...overrides }, timeoutMs);
    return this.validateGIStatus(result);
  }

  async volFogStatus(timeoutMs = 2_000): Promise<VolFogStatus> {
    const result = await this.request("volfog_status", {}, timeoutMs);
    return this.validateVolFogStatus(result);
  }

  async configureVolFog(
    overrides: VolFogConfigureOverrides,
    timeoutMs = 5_000,
  ): Promise<VolFogStatus> {
    const result = await this.request("volfog_configure", { ...overrides }, timeoutMs);
    return this.validateVolFogStatus(result);
  }

  /**
   * Pede encerramento e SO devolve `stopped` quando o encerramento foi observado de fora: o
   * processo sumiu, ou — quando o PID nao pode ser lido — a pipe deixou de existir.
   *
   * A versao anterior perguntava o status num laco e tratava a primeira falha como prova de
   * morte. Numa cena a ~3 FPS o status leva 380 ms, entao o prazo de 500 ms expirava sozinho e o
   * editor era declarado encerrado enquanto seguia vivo, com janela e renderizando. Prazo
   * estourado nao e evidencia de nada: mede a lentidao do editor, nao a existencia dele.
   */
  async shutdown(timeoutSeconds: number): Promise<Record<string, unknown>> {
    // O PID vem ANTES do pedido porque depois nao ha mais a quem perguntar; e ele a prova
    // procurada no fim. Falhar aqui nao impede o shutdown, so rebaixa a confirmacao para a pipe.
    let pid: number | undefined;
    try {
      pid = (await this.status(SLOW_EDITOR_COMMAND_TIMEOUT_MS)).pid;
    } catch (error) {
      // Pipe ausente e a unica falha que ja significa "encerrado". Lentidao nao encerra nada.
      if (error instanceof EditorUnavailableError) {
        return { alreadyStopped: true, stopped: true, confirmedBy: "pipe" };
      }
    }

    const accepted = await this.request("shutdown", {}, SLOW_EDITOR_COMMAND_TIMEOUT_MS);

    const deadline = Date.now() + timeoutSeconds * 1000;
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, STOP_POLL_INTERVAL_MS));
      if (pid !== undefined) {
        if (!processIsAlive(pid)) return { pid, accepted, stopped: true, confirmedBy: "process" };
      } else if (await this.pipeIsGone(PIPE_PROBE_TIMEOUT_MS)) {
        return { accepted, stopped: true, confirmedBy: "pipe" };
      }
    }

    throw new Error(
      `SmileEditor aceitou o shutdown mas nao encerrou em ${timeoutSeconds} s` +
        (pid !== undefined
          ? ` (PID ${pid} continua vivo)`
          : " (a named pipe continua atendendo)") +
        ".",
    );
  }

  /**
   * Conecta e desliga sem enviar comando: pergunta se a pipe ainda existe sem depender de o
   * editor conseguir RESPONDER. Conservadora de proposito — so ENOENT/ECONNREFUSED contam como
   * sumico, e qualquer outro desfecho (pipe ocupada, prazo estourado) conta como vivo, porque
   * o erro caro aqui e o falso positivo.
   */
  private pipeIsGone(timeoutMs: number): Promise<boolean> {
    return new Promise((resolve) => {
      let settled = false;
      const socket = net.createConnection(editorPipePath());

      const finish = (gone: boolean): void => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        resolve(gone);
      };

      const timer = setTimeout(() => finish(false), timeoutMs);
      timer.unref();

      socket.on("connect", () => finish(false));
      socket.on("error", (error: NodeJS.ErrnoException) =>
        finish(error.code === "ENOENT" || error.code === "ECONNREFUSED"));
    });
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
          new EditorTimeoutError(
            `SmileEditor nao concluiu '${command}' em ${(timeoutMs / 1000).toFixed(0)} s. ` +
              "A captura pode continuar dentro do editor.",
          ),
        );
      }, timeoutMs);
      timer.unref();

      socket.setEncoding("utf8");
      socket.on("connect", () => socket.write(request));
      socket.on("error", (error: NodeJS.ErrnoException) => {
        finishError(bridgeUnavailableError(error));
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

  private validateCameraPose(result: unknown): CameraPose {
    if (!result || typeof result !== "object" || Array.isArray(result)) {
      throw new Error("Pose de camera invalida retornada pelo editor.");
    }
    const pose = result as Partial<CameraPose>;
    const position = pose.position;
    if (
      !position ||
      typeof position.x !== "number" ||
      typeof position.y !== "number" ||
      typeof position.z !== "number" ||
      typeof pose.pitchDegrees !== "number" ||
      typeof pose.yawDegrees !== "number"
    ) {
      throw new Error("Pose de camera sem posicao ou orientacao numerica.");
    }
    return pose as CameraPose;
  }

  private validateGIStatus(result: unknown): GIStatus {
    if (!result || typeof result !== "object" || Array.isArray(result)) {
      throw new Error("Estado de GI invalido retornado pelo editor.");
    }
    const status = result as Partial<GIStatus>;
    if (
      typeof status.frameIndex !== "number" ||
      !status.policy ||
      !status.ddgi ||
      !status.restirGI ||
      !Array.isArray(status.ddgi.cascades) ||
      !status.settings ||
      typeof status.settings !== "object" ||
      Array.isArray(status.settings)
    ) {
      throw new Error("Estado de GI sem politica, DDGI ou readback obrigatorio.");
    }
    return status as GIStatus;
  }

  private validateVolFogStatus(result: unknown): VolFogStatus {
    if (!result || typeof result !== "object" || Array.isArray(result)) {
      throw new Error("Estado do volumetric fog invalido retornado pelo editor.");
    }
    const status = result as Partial<VolFogStatus>;
    if (
      typeof status.enabled !== "boolean" ||
      typeof status.temporal !== "boolean" ||
      typeof status.maxDistance !== "number" ||
      typeof status.phaseG !== "number" ||
      typeof status.lightsIntensity !== "number"
    ) {
      throw new Error("Estado do volumetric fog sem readback obrigatorio.");
    }
    return status as VolFogStatus;
  }

  private samePath(left: string, right: string): boolean {
    if (!left || !right) return false;
    const normalize = (value: string): string => path.resolve(value).toLocaleLowerCase("en-US");
    return normalize(left) === normalize(right);
  }
}
