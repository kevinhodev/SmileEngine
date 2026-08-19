#!/usr/bin/env node
// CLI do gerador de malha por IA. Mesmo caminho da tool MCP smile_generate_mesh, para quando o
// que se quer e o terminal: iterar prompt sem um cliente MCP no meio, ou chamar de um script.
//
//   npm run meshgen -- --name tocho --prompt "tocho medieval de ferro" --cook --load
//
// A chave de API sai do ambiente (SMILE_MESHGEN_<PROVEDOR>_KEY) e nunca da linha de comando: um
// argumento fica no historico do shell e na lista de processos.

import path from "node:path";

import { SmileEditorBridge } from "./editor-bridge.js";
import { PROVIDER_IDS, describeProviders, generateMesh, slugify, type ProviderId } from "./meshgen.js";
import { SmileProject, discoverSmileRoot } from "./smile-project.js";

interface CliOptions {
  name?: string;
  prompt?: string;
  imagePath?: string;
  provider: ProviderId;
  negativePrompt?: string;
  artStyle?: string;
  timeoutSeconds: number;
  pollSeconds: number;
  cook: boolean;
  configuration: "Debug" | "Release" | "RelWithDebInfo";
  fitMeters: number;
  dropToGround: boolean;
  centerXZ: boolean;
  load: boolean;
}

function usage(): void {
  console.log(
    [
      "Uso: node dist/meshgen-cli.js --name <asset> (--prompt <texto> | --image <caminho>) [opcoes]",
      "",
      `  --provider <${PROVIDER_IDS.join("|")}>   default: meshy`,
      "  --negative <texto>        prompt negativo, quando o provedor aceita",
      "  --style <nome>            estilo (realistic, sculpture, ...)",
      "  --timeout <segundos>      orcamento total, default 900",
      "  --poll <segundos>         intervalo de polling, default 5",
      "  --no-cook                 so baixa o .glb, sem gerar .smesh/.sscene",
      "  --config <Debug|Release|RelWithDebInfo>  build do SmileCooker, default Release",
      "  --fit <metros>            default 2; 0 desliga a normalizacao de escala",
      "  --no-ground / --no-center desliga o encosto no chao / a centralizacao",
      "  --load                    adiciona o cozido a cena do SmileEditor aberto",
      "  --providers               lista os provedores e sai",
      "",
      "A chave de API vem do ambiente; veja Docs/AI-MESHGEN.md.",
    ].join("\n"),
  );
}

function parseArgs(argv: string[]): CliOptions {
  const options: CliOptions = {
    provider: "meshy",
    timeoutSeconds: 900,
    pollSeconds: 5,
    cook: true,
    configuration: "Release",
    fitMeters: 2,
    dropToGround: true,
    centerXZ: true,
    load: false,
  };

  const next = (index: number, flag: string): string => {
    const value = argv[index + 1];
    if (value === undefined || value.startsWith("--")) throw new Error(`${flag} precisa de um valor.`);
    return value;
  };

  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    switch (flag) {
      case "--name":     options.name = next(index, flag); index += 1; break;
      case "--prompt":   options.prompt = next(index, flag); index += 1; break;
      case "--image":    options.imagePath = next(index, flag); index += 1; break;
      case "--negative": options.negativePrompt = next(index, flag); index += 1; break;
      case "--style":    options.artStyle = next(index, flag); index += 1; break;
      case "--provider": {
        const value = next(index, flag);
        if (!(PROVIDER_IDS as string[]).includes(value)) {
          throw new Error(`Provedor desconhecido: ${value}`);
        }
        options.provider = value as ProviderId;
        index += 1;
        break;
      }
      case "--timeout":  options.timeoutSeconds = Number(next(index, flag)); index += 1; break;
      case "--poll":     options.pollSeconds = Number(next(index, flag)); index += 1; break;
      case "--fit":      options.fitMeters = Number(next(index, flag)); index += 1; break;
      case "--config": {
        const value = next(index, flag);
        if (value !== "Debug" && value !== "Release" && value !== "RelWithDebInfo") {
          throw new Error(`Configuracao desconhecida: ${value}`);
        }
        options.configuration = value;
        index += 1;
        break;
      }
      case "--no-cook":   options.cook = false; break;
      case "--no-ground": options.dropToGround = false; break;
      case "--no-center": options.centerXZ = false; break;
      case "--load":      options.load = true; break;
      case "--providers":
        console.log(JSON.stringify(describeProviders(), null, 2));
        process.exit(0);
        break;
      case "--help":
      case "-h":
        usage();
        process.exit(0);
        break;
      default:
        throw new Error(`Argumento desconhecido: ${flag}`);
    }
  }

  if (!options.name) throw new Error("--name e obrigatorio.");
  if ((options.prompt ? 1 : 0) + (options.imagePath ? 1 : 0) !== 1) {
    throw new Error("Informe exatamente um de --prompt ou --image.");
  }
  if (!Number.isFinite(options.timeoutSeconds) || options.timeoutSeconds < 30) {
    throw new Error("--timeout precisa ser >= 30 segundos.");
  }
  if (!Number.isFinite(options.pollSeconds) || options.pollSeconds < 2) {
    throw new Error("--poll precisa ser >= 2 segundos.");
  }
  if (!Number.isFinite(options.fitMeters) || options.fitMeters < 0) {
    throw new Error("--fit precisa ser >= 0.");
  }
  return options;
}

async function main(): Promise<void> {
  const options = parseArgs(process.argv.slice(2));
  const project = new SmileProject(discoverSmileRoot());

  let imageDataUri: string | undefined;
  if (options.imagePath) {
    const { readFile, stat } = await import("node:fs/promises");
    const imagePath = await project.resolveProjectPath(options.imagePath);
    const imageStat = await stat(imagePath);
    const mime = { ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
                   ".webp": "image/webp" }[path.extname(imagePath).toLocaleLowerCase()];
    if (!imageStat.isFile() || !mime) throw new Error("--image precisa ser png/jpg/webp do projeto.");
    imageDataUri = `data:${mime};base64,${(await readFile(imagePath)).toString("base64")}`;
  }

  const generation = await generateMesh(project.root, {
    provider: options.provider,
    name: options.name!,
    prompt: options.prompt,
    imageDataUri,
    imageLabel: options.imagePath,
    negativePrompt: options.negativePrompt,
    artStyle: options.artStyle,
    timeoutSeconds: options.timeoutSeconds,
    pollSeconds: options.pollSeconds,
  });
  for (const line of generation.log) console.log(line);
  console.log(`Modelo: ${generation.modelPath} (${(generation.bytes / 1024).toFixed(0)} KB)`);

  if (!options.cook) return;

  const cooked = await project.cookScene({
    sourcePath: generation.modelPath,
    configuration: options.configuration,
    opaqueGlass: false,
    timeoutSeconds: 900,
    fitMeters: options.fitMeters > 0 ? options.fitMeters : undefined,
    dropToGround: options.dropToGround,
    centerXZ: options.centerXZ,
  });
  if (typeof cooked.stdout === "string" && cooked.stdout.length > 0) console.log(cooked.stdout.trim());
  if (cooked.exitCode !== 0) throw new Error(`SmileCooker falhou (exit ${String(cooked.exitCode)}).`);

  if (!options.load) return;
  const slug = slugify(options.name!);
  const scenePath = await project.resolveProjectPath(`Assets/Generated/${slug}/${slug}.sscene`);
  const bridge = new SmileEditorBridge(project.root);
  const result = await bridge.loadScene(scenePath, true, 120);
  console.log(`Carregado no editor: ${JSON.stringify(result)}`);
}

main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
