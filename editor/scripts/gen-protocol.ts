import { spawn } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const editorDir = dirname(scriptDir);
const repoRoot = dirname(editorDir);
const generator = join(repoRoot, "tools", "gen-control-dto", "gen.ts");

const child = spawn("bun", ["run", generator], {
  cwd: repoRoot,
  env: process.env,
  stdio: "inherit",
});

child.on("error", (err) => {
  console.error(err);
  process.exit(1);
});

child.on("exit", (code, signal) => {
  if (signal) {
    console.error(`DTO protocol generator exited with signal ${signal}`);
    process.exit(1);
  }
  process.exit(code ?? 0);
});
