import { spawn } from "node:child_process";

export interface CommandOptions {
  cwd: string;
  timeoutMs?: number;
  maxOutputBytes?: number;
  env?: NodeJS.ProcessEnv;
}
export interface CommandResult {
  command: string[];
  cwd: string;
  exitCode: number | null;
  signal: NodeJS.Signals | null;
  durationMs: number;
  timedOut: boolean;
  truncated: boolean;
  stdout: string;
  stderr: string;
}

export function runCommand(
  command: string,
  args: string[],
  options: CommandOptions,
): Promise<CommandResult> {
  const startedAt = performance.now();
  const maxOutputBytes = options.maxOutputBytes ?? 1024 * 1024;

  return new Promise((resolve) => {
    let stdout = "";
    let stderr = "";
    let outputBytes = 0;
    let truncated = false;
    let timedOut = false;
    let settled = false;

    const child = spawn(command, args, {
      cwd: options.cwd,
      env: { ...process.env, ...options.env },
      shell: false,
      windowsHide: true,
      stdio: ["ignore", "pipe", "pipe"],
    });

    const append = (target: "stdout" | "stderr", chunk: Buffer): void => {
      if (outputBytes >= maxOutputBytes) {
        truncated = true;
        return;
      }

      const remaining = maxOutputBytes - outputBytes;
      const accepted = chunk.subarray(0, remaining);
      const text = accepted.toString("utf8");
      outputBytes += accepted.byteLength;
      if (accepted.byteLength < chunk.byteLength) truncated = true;
      if (target === "stdout") stdout += text;
      else stderr += text;
    };

    child.stdout.on("data", (chunk: Buffer) => append("stdout", chunk));
    child.stderr.on("data", (chunk: Buffer) => append("stderr", chunk));

    const finish = (exitCode: number | null, signal: NodeJS.Signals | null): void => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve({
        command: [command, ...args],
        cwd: options.cwd,
        exitCode,
        signal,
        durationMs: Math.round(performance.now() - startedAt),
        timedOut,
        truncated,
        stdout: stdout.trimEnd(),
        stderr: stderr.trimEnd(),
      });
    };

    child.on("error", (error) => {
      stderr += `${stderr ? "\n" : ""}${error.message}`;
      finish(null, null);
    });
    child.on("close", finish);

    const timer = setTimeout(() => {
      timedOut = true;
      child.kill();
    }, options.timeoutMs ?? 60_000);
    timer.unref();
  });
}
