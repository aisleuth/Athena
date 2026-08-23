import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { readFile, stat } from "node:fs/promises";
import { dirname, extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const guiDir = dirname(fileURLToPath(import.meta.url));
const rootDir = resolve(guiDir, "..");
const publicDir = join(guiDir, "public");
const enginePath = resolve(process.env.ATHENA_ENGINE ?? join(rootDir, "build/src/athena"));
const port = Number.parseInt(process.env.ATHENA_GUI_PORT ?? "8787", 10);

class EngineBridge {
  constructor(executable) {
    this.process = spawn(executable, [], { cwd: rootDir, stdio: ["pipe", "pipe", "pipe"] });
    this.buffer = "";
    this.waiter = null;
    this.tail = Promise.resolve();
    this.dead = null;
    this.listeners = new Set();

    this.process.stdout.setEncoding("utf8");
    this.process.stdout.on("data", (chunk) => this.consume(chunk));
    this.process.stderr.setEncoding("utf8");
    this.process.stderr.on("data", (chunk) => process.stderr.write(`[athena] ${chunk}`));
    this.process.on("error", (error) => {
      this.dead = error;
      this.waiter?.reject(error);
    });
    this.process.on("exit", (code) => {
      if (code !== 0 && !this.dead) this.dead = new Error(`Athena exited with code ${code}`);
      this.waiter?.reject(this.dead ?? new Error("Athena exited"));
    });
  }

  consume(chunk) {
    this.buffer += chunk;
    let newline;
    while ((newline = this.buffer.indexOf("\n")) !== -1) {
      const line = this.buffer.slice(0, newline).replace(/\r$/, "");
      this.buffer = this.buffer.slice(newline + 1);
      for (const listener of this.listeners) listener(line);
      if (!this.waiter) continue;
      this.waiter.lines.push(line);
      if (line === this.waiter.terminator) {
        const { resolve: done, lines } = this.waiter;
        this.waiter = null;
        done(lines);
      }
    }
  }

  onLine(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  request(command, terminator) {
    const task = this.tail.then(() => new Promise((resolveRequest, rejectRequest) => {
      if (this.dead) return rejectRequest(this.dead);
      this.waiter = { terminator, lines: [], resolve: resolveRequest, reject: rejectRequest };
      this.process.stdin.write(`${command}\n`);
    }));
    this.tail = task.catch(() => {});
    return task;
  }

  interrupt() {
    if (!this.dead) this.process.stdin.write("stop\n");
  }

  close() {
    if (!this.dead) this.process.stdin.write("quit\n");
  }
}

let engine;
try {
  await stat(enginePath);
  engine = new EngineBridge(enginePath);
} catch {
  console.error(`Athena executable not found at ${enginePath}`);
  console.error("Build it first with: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build");
  process.exit(1);
}

function parseState(lines) {
  const value = (prefix) => lines.find((line) => line.startsWith(prefix))?.slice(prefix.length) ?? "";
  const legal = value("state legal").trim();
  return {
    fen: value("state fen "),
    turn: value("state turn "),
    setup: value("state setup "),
    legalMoves: legal ? legal.split(/\s+/) : [],
    result: lines.find((line) => line.startsWith("play ") || line.startsWith("undo ")) ?? null,
  };
}

let nextAnalysisId = 1;
let analysisState = {
  id: 0, sequence: 0, status: "idle", variations: [], stats: {},
  progress: { depth: 0, completed: 0, total: 0, iterationComplete: false },
};
let analysisFrame = null;

engine.onLine((line) => {
  let match = line.match(/^analysis (\d+) update depth (\d+) completed (\d+) total (\d+) complete ([01])$/);
  if (match) {
    const id = Number(match[1]);
    if (id !== analysisState.id) return;
    analysisFrame = {
      id, sequence: analysisState.sequence + 1, status: "searching",
      variations: [], stats: {}, progress: {
        depth: Number(match[2]), completed: Number(match[3]), total: Number(match[4]),
        iterationComplete: match[5] === "1",
      },
    };
    return;
  }

  match = line.match(/^analysis (\d+) line (\d+) move (\S+) score (cp|mate) (-?\d+) depth (\d+) pv(?: (.*))?$/);
  if (match && analysisFrame?.id === Number(match[1])) {
    analysisFrame.variations.push({
      rank: Number(match[2]), move: match[3], scoreType: match[4],
      score: Number(match[5]), depth: Number(match[6]),
      pv: match[7] ? match[7].split(/\s+/) : [],
    });
    return;
  }

  match = line.match(/^analysis (\d+) stats nodes (\d+) qnodes (\d+) tthits (\d+) seldepth (\d+) time (\d+)$/);
  if (match && analysisFrame?.id === Number(match[1])) {
    analysisFrame.stats = {
      nodes: Number(match[2]), qnodes: Number(match[3]), ttHits: Number(match[4]),
      selectiveDepth: Number(match[5]), timeMs: Number(match[6]),
    };
    return;
  }

  match = line.match(/^analysis (\d+) update end$/);
  if (match && analysisFrame?.id === Number(match[1])) {
    analysisState = analysisFrame;
    analysisFrame = null;
    return;
  }

  match = line.match(/^analysis (\d+) end$/);
  if (match && Number(match[1]) === analysisState.id) {
    analysisState = { ...analysisState, status: "complete", sequence: analysisState.sequence + 1 };
  }
});

function json(response, status, body) {
  response.writeHead(status, { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" });
  response.end(JSON.stringify(body));
}

async function readJson(request) {
  let body = "";
  for await (const chunk of request) {
    body += chunk;
    if (body.length > 1_000_000) throw new Error("Request too large");
  }
  return body ? JSON.parse(body) : {};
}

const mimeTypes = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".png": "image/png",
  ".svg": "image/svg+xml",
};

async function serveStatic(request, response) {
  const requested = decodeURIComponent(new URL(request.url, "http://localhost").pathname);
  const relative = requested === "/" ? "index.html" : normalize(requested).replace(/^[/\\]+/, "");
  const path = resolve(publicDir, relative);
  if (!path.startsWith(`${publicDir}/`)) return json(response, 403, { error: "Forbidden" });
  try {
    const data = await readFile(path);
    response.writeHead(200, { "content-type": mimeTypes[extname(path)] ?? "application/octet-stream" });
    response.end(data);
  } catch {
    json(response, 404, { error: "Not found" });
  }
}

const server = createServer(async (request, response) => {
  try {
    const url = new URL(request.url, "http://localhost");
    if (url.pathname === "/api/state" && request.method === "GET") {
      return json(response, 200, parseState(await engine.request("state", "state end")));
    }
    if (url.pathname === "/api/analyze" && request.method === "POST") {
      const body = await readJson(request);
      const depth = Math.min(64, Math.max(1, Number(body.depth) || 4));
      const moveTime = Math.min(120_000, Math.max(0, Number(body.moveTime) || 0));
      const multipv = Math.min(64, Math.max(1, Number(body.multipv) || 8));
      const id = nextAnalysisId++;
      engine.interrupt();
      analysisFrame = null;
      analysisState = {
        id, sequence: 0, status: "queued", variations: [], stats: {},
        progress: { depth: 0, completed: 0, total: 0, iterationComplete: false },
      };
      const limit = moveTime === 0 ? "infinite" : `depth ${depth} movetime ${moveTime}`;
      const command = `analyze ${limit} multipv ${multipv} id ${id}`;
      engine.request(command, `analysis ${id} end`).catch((error) => {
        if (analysisState.id === id) {
          analysisState = { ...analysisState, status: "error", error: error.message };
        }
      });
      return json(response, 202, { id });
    }
    if (url.pathname === "/api/analysis" && request.method === "GET") {
      const id = Number(url.searchParams.get("id"));
      if (id !== analysisState.id) return json(response, 404, { error: "Analysis not found" });
      return json(response, 200, analysisState);
    }
    if (url.pathname === "/api/stop" && request.method === "POST") {
      engine.interrupt();
      return json(response, 200, { ok: true });
    }
    if (url.pathname === "/api/move" && request.method === "POST") {
      const body = await readJson(request);
      if (!/^[a-n](?:1[0-4]|[1-9])[a-n](?:1[0-4]|[1-9])[QRBN]?$/.test(body.move ?? "")) {
        return json(response, 400, { error: "Invalid move format" });
      }
      engine.interrupt();
      const state = parseState(await engine.request(`play ${body.move}`, "state end"));
      return json(response, state.result?.startsWith("play ok") ? 200 : 409, state);
    }
    if (url.pathname === "/api/undo" && request.method === "POST") {
      engine.interrupt();
      return json(response, 200, parseState(await engine.request("undo", "state end")));
    }
    if (url.pathname === "/api/reset" && request.method === "POST") {
      const body = await readJson(request);
      const setup = body.setup === "classic" ? "classic" : "modern";
      engine.interrupt();
      const lines = await engine.request(`position startpos ${setup}\nstate`, "state end");
      return json(response, 200, parseState(lines));
    }
    if (url.pathname === "/api/options" && request.method === "POST") {
      const body = await readJson(request);
      const hash = Math.min(1024, Math.max(1, Number(body.hash) || 16));
      engine.interrupt();
      await engine.request(`setoption name Hash value ${hash}\nstate`, "state end");
      return json(response, 200, { ok: true, hash });
    }
    if (url.pathname.startsWith("/api/")) return json(response, 404, { error: "Unknown endpoint" });
    return serveStatic(request, response);
  } catch (error) {
    console.error(error);
    return json(response, 500, { error: error.message ?? "Internal error" });
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`Athena GUI: http://127.0.0.1:${port}`);
  console.log(`Engine: ${enginePath}`);
});

for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => {
    engine.close();
    server.close(() => process.exit(0));
  });
}
