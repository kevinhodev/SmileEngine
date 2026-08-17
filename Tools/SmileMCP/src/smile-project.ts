import {
  constants as fsConstants,
  access,
  lstat,
  open,
  readFile,
  readdir,
  realpath,
  stat,
} from "node:fs/promises";
import { existsSync, realpathSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { runCommand, type CommandResult } from "./process.js";

const DEFAULT_EXCLUDED_DIRECTORIES = new Set([
  ".git",
  ".vs",
  ".codex_deps",
  "CMakeFiles",
  "node_modules",
]);

export interface ListFilesOptions {
  relativePath: string;
  maxDepth: number;
  limit: number;
  includeBuild: boolean;
}

export interface ReadFileOptions {
  relativePath: string;
  startLine: number;
  maxLines: number;
  includeLineNumbers: boolean;
}

export interface FindTextOptions {
  query: string;
  relativePath: string;
  globs: string[];
  caseSensitive: boolean;
  fixedStrings: boolean;
  includeBuild: boolean;
  maxResults: number;
}

export type BuildConfiguration = "Debug" | "Release" | "RelWithDebInfo";

export interface BuildOptions {
  buildDirectory: string;
  configuration: BuildConfiguration;
  target: string;
  timeoutSeconds: number;
}

export interface CookSceneOptions {
  sourcePath: string;
  configuration: BuildConfiguration;
  opaqueGlass: boolean;
  timeoutSeconds: number;
}

function usesBuildDirectory(name: string): boolean {
  const lower = name.toLowerCase();
  return lower === "build" || lower === "out" || lower.startsWith("cmake-build-");
}

function isSmileRoot(candidate: string): boolean {
  return (
    existsSync(path.join(candidate, "CMakeLists.txt")) &&
    existsSync(path.join(candidate, "Engine")) &&
    existsSync(path.join(candidate, "Editor")) &&
    existsSync(path.join(candidate, "Shaders"))
  );
}

function searchParents(start: string): string | undefined {
  let current = path.resolve(start);
  while (true) {
    if (isSmileRoot(current)) return current;
    const parent = path.dirname(current);
    if (parent === current) return undefined;
    current = parent;
  }
}

export function discoverSmileRoot(): string {
  const configuredRoot = process.env.SMILE_ROOT;
  if (configuredRoot) {
    const candidate = path.resolve(configuredRoot);
    if (!isSmileRoot(candidate)) {
      throw new Error(`SMILE_ROOT nao aponta para uma arvore valida da SmileEngine: ${candidate}`);
    }
    return realpathSync(candidate);
  }

  const moduleDirectory = path.dirname(fileURLToPath(import.meta.url));
  for (const start of [process.cwd(), moduleDirectory]) {
    const root = searchParents(start);
    if (root) return realpathSync(root);
  }

  throw new Error(
    "Nao foi possivel localizar a raiz da SmileEngine. Defina a variavel SMILE_ROOT.",
  );
}

function portablePath(value: string): string {
  return value.split(path.sep).join("/");
}

function isInside(parent: string, child: string): boolean {
  const relative = path.relative(parent, child);
  return relative === "" || (!relative.startsWith(`..${path.sep}`) && relative !== ".." && !path.isAbsolute(relative));
}

async function countFiles(root: string, predicate: (file: string) => boolean): Promise<number> {
  let count = 0;
  const pending = [root];

  while (pending.length > 0) {
    const current = pending.pop();
    if (!current) break;
    let entries;
    try {
      entries = await readdir(current, { withFileTypes: true });
    } catch {
      continue;
    }

    for (const entry of entries) {
      const fullPath = path.join(current, entry.name);
      if (entry.isDirectory()) pending.push(fullPath);
      else if (entry.isFile() && predicate(fullPath)) count += 1;
    }
  }

  return count;
}

export class SmileProject {
  readonly root: string;

  constructor(root = discoverSmileRoot()) {
    this.root = realpathSync(root);
  }

  async resolveProjectPath(relativePath: string): Promise<string> {
    if (path.isAbsolute(relativePath)) {
      throw new Error("Use um caminho relativo a raiz da SmileEngine.");
    }

    const candidate = path.resolve(this.root, relativePath || ".");
    if (!isInside(this.root, candidate)) {
      throw new Error("O caminho solicitado sai da raiz da SmileEngine.");
    }

    await access(candidate, fsConstants.F_OK);
    const resolved = await realpath(candidate);
    if (!isInside(this.root, resolved)) {
      throw new Error("O caminho solicitado resolve para fora da raiz da SmileEngine.");
    }
    return resolved;
  }

  relative(absolutePath: string): string {
    return portablePath(path.relative(this.root, absolutePath)) || ".";
  }

  async projectInfo(): Promise<Record<string, unknown>> {
    const cmake = await readFile(path.join(this.root, "CMakeLists.txt"), "utf8");
    const readVersionPart = (name: string): string =>
      new RegExp(`set\\(SMILE_VERSION_${name}\\s+(\\d+)\\)`).exec(cmake)?.[1] ?? "?";
    const version = ["MAJOR", "MINOR", "PATCH"].map(readVersionPart).join(".");
    const editorExecutables = await this.findEditorExecutables();
    const buildConfigurations = ["Debug", "Release", "RelWithDebInfo"].filter((configuration) =>
      existsSync(path.join(this.root, "build", "bin", configuration)),
    );

    const [shaderCount, sceneCount] = await Promise.all([
      countFiles(path.join(this.root, "Shaders"), (file) => /\.hlsl(i)?$/i.test(file)),
      countFiles(path.join(this.root, "Assets"), (file) => /\.(sscene|smap)$/i.test(file)),
    ]);

    return {
      name: "SmileEngine",
      version,
      root: this.root,
      platform: "Windows x64",
      stack: ["C++20", "DirectX 12", "DXR 1.1", "Qt 6", "HLSL", "CMake"],
      components: {
        engine: "Engine",
        editor: "Editor",
        shaders: "Shaders",
        assets: "Assets",
        tests: "Tests",
        tools: "Tools",
      },
      buildDirectory: "build",
      buildConfigurations,
      editorExecutables,
      shaderSourceCount: shaderCount,
      sceneAndMapCount: sceneCount,
      editorLogDirectory: this.editorLogDirectory(),
    };
  }

  async listFiles(options: ListFilesOptions): Promise<Record<string, unknown>> {
    const start = await this.resolveProjectPath(options.relativePath);
    const startStat = await stat(start);
    if (!startStat.isDirectory()) throw new Error("O caminho precisa apontar para um diretorio.");

    const entries: Array<Record<string, unknown>> = [];
    let truncated = false;

    const visit = async (directory: string, depth: number): Promise<void> => {
      if (entries.length >= options.limit) {
        truncated = true;
        return;
      }

      const children = await readdir(directory, { withFileTypes: true });
      children.sort((a, b) => {
        if (a.isDirectory() !== b.isDirectory()) return a.isDirectory() ? -1 : 1;
        return a.name.localeCompare(b.name, "en", { sensitivity: "base" });
      });

      for (const child of children) {
        if (entries.length >= options.limit) {
          truncated = true;
          break;
        }
        if (DEFAULT_EXCLUDED_DIRECTORIES.has(child.name)) continue;
        if (!options.includeBuild && usesBuildDirectory(child.name)) continue;

        const absolutePath = path.join(directory, child.name);
        const childStat = await lstat(absolutePath);
        entries.push({
          path: this.relative(absolutePath),
          type: child.isDirectory() ? "directory" : child.isSymbolicLink() ? "symlink" : "file",
          size: child.isFile() ? childStat.size : undefined,
          modifiedAt: childStat.mtime.toISOString(),
        });

        if (child.isDirectory() && !child.isSymbolicLink() && depth < options.maxDepth) {
          await visit(absolutePath, depth + 1);
        }
      }
    };

    await visit(start, 0);
    return {
      root: this.relative(start),
      maxDepth: options.maxDepth,
      count: entries.length,
      truncated,
      entries,
    };
  }

  async readTextFile(options: ReadFileOptions): Promise<Record<string, unknown>> {
    const absolutePath = await this.resolveProjectPath(options.relativePath);
    const fileStat = await stat(absolutePath);
    if (!fileStat.isFile()) throw new Error("O caminho precisa apontar para um arquivo.");
    if (fileStat.size > 2 * 1024 * 1024) {
      throw new Error("Arquivo maior que 2 MiB; use smile_find_text para localizar o trecho desejado.");
    }

    const buffer = await readFile(absolutePath);
    if (buffer.includes(0)) throw new Error("O arquivo parece ser binario.");

    const lines = buffer.toString("utf8").split(/\r?\n/);
    const startIndex = Math.min(options.startLine - 1, lines.length);
    const selected = lines.slice(startIndex, startIndex + options.maxLines);
    const content = options.includeLineNumbers
      ? selected.map((line, index) => `${startIndex + index + 1}: ${line}`).join("\n")
      : selected.join("\n");

    return {
      path: this.relative(absolutePath),
      totalLines: lines.length,
      startLine: selected.length > 0 ? startIndex + 1 : null,
      endLine: selected.length > 0 ? startIndex + selected.length : null,
      truncated: startIndex + selected.length < lines.length,
      content,
    };
  }

  async findText(options: FindTextOptions): Promise<Record<string, unknown>> {
    const searchRoot = await this.resolveProjectPath(options.relativePath);
    const args = ["--line-number", "--column", "--no-heading", "--color", "never"];
    if (!options.caseSensitive) args.push("--ignore-case");
    if (options.fixedStrings) args.push("--fixed-strings");
    for (const glob of options.globs) args.push("--glob", glob);
    args.push("--glob", "!.git/**", "--glob", "!node_modules/**");
    if (!options.includeBuild) {
      args.push("--glob", "!build/**", "--glob", "!out/**", "--glob", "!cmake-build-*/**");
    }
    args.push("--", options.query, searchRoot);

    const result = await runCommand("rg", args, {
      cwd: this.root,
      timeoutMs: 15_000,
      maxOutputBytes: 1024 * 1024,
    });
    if (result.exitCode !== 0 && result.exitCode !== 1) {
      throw new Error(result.stderr || "rg falhou ao pesquisar o projeto.");
    }

    const matches = result.stdout
      ? result.stdout.split(/\r?\n/).slice(0, options.maxResults)
      : [];
    return {
      query: options.query,
      root: this.relative(searchRoot),
      count: matches.length,
      truncated: result.truncated || result.stdout.split(/\r?\n/).length > options.maxResults,
      matches,
    };
  }

  async build(options: BuildOptions): Promise<CommandResult> {
    const buildDirectory = await this.resolveProjectPath(options.buildDirectory);
    const buildStat = await stat(buildDirectory);
    if (!buildStat.isDirectory()) throw new Error("O diretorio de build nao existe.");

    return runCommand(
      "cmake",
      [
        "--build",
        buildDirectory,
        "--config",
        options.configuration,
        "--target",
        options.target,
      ],
      {
        cwd: this.root,
        timeoutMs: options.timeoutSeconds * 1000,
        maxOutputBytes: 2 * 1024 * 1024,
      },
    );
  }

  async cookScene(options: CookSceneOptions): Promise<Record<string, unknown>> {
    const sourcePath = await this.resolveProjectPath(options.sourcePath);
    const sourceStat = await stat(sourcePath);
    if (!sourceStat.isFile() || path.extname(sourcePath).toLocaleLowerCase() !== ".fbx") {
      throw new Error("sourcePath precisa apontar para um arquivo .fbx dentro da SmileEngine.");
    }

    const executable = this.cookerExecutable(options.configuration);
    const outputBase = sourcePath.slice(0, -path.extname(sourcePath).length);
    const meshPath = `${outputBase}.smesh`;
    const scenePath = `${outputBase}.sscene`;
    const expectedOutputs = [meshPath, scenePath].map((outputPath) => this.relative(outputPath));
    // fopen("wb") segue symlink existente. Valide antes de executar para o tool nunca conseguir
    // sobrescrever um destino fora da raiz por meio de um .smesh/.sscene redirecionado.
    for (const outputPath of expectedOutputs) {
      if (existsSync(path.join(this.root, outputPath))) await this.resolveProjectPath(outputPath);
    }

    const args = [sourcePath];
    if (options.opaqueGlass) args.push("--opaque-glass");
    const result = await runCommand(executable, args, {
      cwd: this.root,
      timeoutMs: options.timeoutSeconds * 1000,
      maxOutputBytes: 2 * 1024 * 1024,
    });

    if (result.exitCode !== 0) {
      return {
        ...result,
        sourcePath: this.relative(sourcePath),
        expectedOutputs,
      };
    }

    const [trustedMeshPath, trustedScenePath] = await Promise.all([
      this.resolveProjectPath(expectedOutputs[0]!),
      this.resolveProjectPath(expectedOutputs[1]!),
    ]);
    const [mesh, scene] = await Promise.all([
      this.inspectCookedHeader(trustedMeshPath, 0x48534d53, "smesh"),
      this.inspectCookedHeader(trustedScenePath, 0x4e435353, "sscene"),
    ]);
    if (mesh.version !== scene.version) {
      throw new Error(
        `O cooker produziu versoes divergentes: smesh v${mesh.version} e sscene v${scene.version}.`,
      );
    }

    return {
      ...result,
      sourcePath: this.relative(sourcePath),
      opaqueGlass: options.opaqueGlass,
      cookedVersion: mesh.version,
      outputs: { mesh, scene },
    };
  }

  async findEditorExecutables(): Promise<Array<Record<string, unknown>>> {
    const binRoot = path.join(this.root, "build", "bin");
    if (!existsSync(binRoot)) return [];

    const found: Array<Record<string, unknown>> = [];
    for (const configuration of ["Debug", "Release", "RelWithDebInfo"]) {
      const executable = path.join(binRoot, configuration, "SmileEditor.exe");
      if (!existsSync(executable)) continue;
      const executableStat = await stat(executable);
      found.push({
        configuration,
        path: this.relative(executable),
        size: executableStat.size,
        modifiedAt: executableStat.mtime.toISOString(),
      });
    }
    return found;
  }

  editorExecutable(configuration: BuildConfiguration): string {
    const executable = path.join(this.root, "build", "bin", configuration, "SmileEditor.exe");
    if (!existsSync(executable)) {
      throw new Error(
        `SmileEditor ${configuration} nao encontrado. Compile o alvo SmileEditor primeiro.`,
      );
    }
    return executable;
  }

  cookerExecutable(configuration: BuildConfiguration): string {
    const executable = path.join(this.root, "build", "bin", configuration, "SmileCooker.exe");
    if (!existsSync(executable)) {
      throw new Error(
        `SmileCooker ${configuration} nao encontrado. Compile o alvo SmileCooker primeiro.`,
      );
    }
    return executable;
  }

  editorLogDirectory(): string | null {
    if (process.env.SMILE_LOG_DIR) return path.resolve(process.env.SMILE_LOG_DIR);
    if (!process.env.LOCALAPPDATA) return null;
    return path.join(process.env.LOCALAPPDATA, "SmileEngine", "SmileEditor", "Logs");
  }

  async latestLogs(sessions: number, tailLines: number, contains?: string): Promise<Record<string, unknown>> {
    const logDirectory = this.editorLogDirectory();
    if (!logDirectory || !existsSync(logDirectory)) {
      return { logDirectory, sessions: [], message: "Nenhum diretorio de logs foi encontrado." };
    }

    const entries = await readdir(logDirectory, { withFileTypes: true });
    const logs = await Promise.all(
      entries
        .filter((entry) => entry.isFile() && /^SmileEditor-.*\.log$/i.test(entry.name))
        .map(async (entry) => {
          const absolutePath = path.join(logDirectory, entry.name);
          return { absolutePath, fileStat: await stat(absolutePath) };
        }),
    );
    logs.sort((a, b) => b.fileStat.mtimeMs - a.fileStat.mtimeMs);

    const selected = [];
    for (const log of logs.slice(0, sessions)) {
      let lines = await this.readTailLines(log.absolutePath, Math.max(tailLines * 4, tailLines));
      if (contains) {
        const needle = contains.toLocaleLowerCase();
        lines = lines.filter((line) => line.toLocaleLowerCase().includes(needle));
      }
      lines = lines.slice(-tailLines);
      selected.push({
        path: log.absolutePath,
        modifiedAt: log.fileStat.mtime.toISOString(),
        size: log.fileStat.size,
        lines,
      });
    }

    return { logDirectory, count: selected.length, sessions: selected };
  }

  private async readTailLines(filePath: string, desiredLines: number): Promise<string[]> {
    const fileStat = await stat(filePath);
    const maxBytes = Math.min(fileStat.size, Math.max(256 * 1024, desiredLines * 512));
    const start = Math.max(0, fileStat.size - maxBytes);
    const handle = await open(filePath, "r");
    try {
      const buffer = Buffer.alloc(maxBytes);
      const { bytesRead } = await handle.read(buffer, 0, maxBytes, start);
      let text = buffer.subarray(0, bytesRead).toString("utf8");
      if (start > 0) text = text.slice(text.indexOf("\n") + 1);
      return text.split(/\r?\n/).filter((line) => line.length > 0).slice(-desiredLines);
    } finally {
      await handle.close();
    }
  }

  private async inspectCookedHeader(
    filePath: string,
    expectedMagic: number,
    kind: "smesh" | "sscene",
  ): Promise<Record<string, unknown> & { version: number }> {
    const fileStat = await stat(filePath);
    if (!fileStat.isFile() || fileStat.size < 16) {
      throw new Error(`O cooker nao produziu um ${kind} valido: ${this.relative(filePath)}.`);
    }

    const handle = await open(filePath, "r");
    const header = Buffer.alloc(16);
    try {
      const { bytesRead } = await handle.read(header, 0, header.length, 0);
      if (bytesRead !== header.length) throw new Error(`Cabecalho ${kind} truncado.`);
    } finally {
      await handle.close();
    }

    const magic = header.readUInt32LE(0);
    const version = header.readUInt32LE(4);
    if (magic !== expectedMagic) {
      throw new Error(`Magic invalido no ${kind} produzido: 0x${magic.toString(16)}.`);
    }

    return {
      path: this.relative(filePath),
      bytes: fileStat.size,
      modifiedAt: fileStat.mtime.toISOString(),
      version,
      ...(kind === "smesh"
        ? { meshCount: header.readUInt32LE(8) }
        : {
            materialCount: header.readUInt32LE(8),
            renderableCount: header.readUInt32LE(12),
          }),
    };
  }
}
