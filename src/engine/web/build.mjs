// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the engine's web page: src/main.tsx and its CSS, bundled by esbuild
// for the PS5's old WebKit (Safari 12), inlined into index.html as one file.
//
//   node build.mjs --out FILE                  # what CMake runs
//   node build.mjs --serve [--port 5950] [--engine http://127.0.0.1:5940]
//       rebuilds on every change and serves the page, sending /api/... to a
//       running engine (alembic-engine --listen 127.0.0.1)
//
// node_modules is installed (npm ci) when package-lock.json changed since the
// last install, so a fresh checkout needs only Node and npm.
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const arg = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : fallback; };

// npm ci, unless node_modules already holds this lock file's packages.
function install() {
  const lock = createHash("sha256").update(fs.readFileSync(path.join(here, "package-lock.json"))).digest("hex");
  const mark = path.join(here, "node_modules", ".alembic-lock");
  let have = "";
  try { have = fs.readFileSync(mark, "utf8"); } catch {}
  if (have === lock) return;
  console.log("installing the page's packages (npm ci)");
  const r = spawnSync("npm", ["ci", "--no-audit", "--no-fund"], { cwd: here, stdio: "inherit", shell: process.platform === "win32" });
  if (r.status !== 0) process.exit(r.status || 1);
  fs.writeFileSync(mark, lock);
}

install();
const esbuild = await import("esbuild");

const options = {
  entryPoints: [path.join(here, "src/main.tsx")],
  bundle: true,
  write: false,
  outdir: path.join(here, "out"), // names only: nothing is written there
  format: "iife",
  target: ["safari12"],
  // esbuild counts destructuring as missing before Safari 14.1 over an edge
  // case; the page has used it on the console all along.
  supported: { destructuring: true },
  jsx: "automatic",
  jsxImportSource: "preact",
  minify: true,
  legalComments: "inline",
  logLevel: "warning",
};

// index.html with the bundle's CSS and script in place of their markers.
function page(result) {
  const out = (ext) => result.outputFiles.filter((f) => f.path.endsWith(ext)).map((f) => f.text).join("\n");
  const html = fs.readFileSync(path.join(here, "index.html"), "utf8");
  return html.replace("<!-- alembic:style -->", () => "<style>" + out(".css") + "</style>")
    .replace("<!-- alembic:script -->", () => "<script>" + out(".js") + "</script>");
}

if (args.includes("--serve")) {
  const port = Number(arg("--port", "5950"));
  const engine = new URL(arg("--engine", "http://127.0.0.1:5940"));
  let html = "";
  const ctx = await esbuild.context({ ...options, minify: false, plugins: [{ name: "page", setup(b) {
    b.onEnd((r) => { if (!r.errors.length) { html = page(r); console.log("built " + new Date().toLocaleTimeString()); } });
  } }] });
  await ctx.watch();
  http.createServer((req, res) => {
    if (req.url === "/" || req.url === "/index.html") {
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" });
      return res.end(html);
    }
    if (req.url === "/i18n.js") {
      res.writeHead(200, { "Content-Type": "text/javascript; charset=utf-8", "Cache-Control": "no-store" });
      return res.end(fs.readFileSync(path.join(here, "i18n.js")));
    }
    // Everything else (the API, icons) comes from the engine.
    const up = http.request({ host: engine.hostname, port: engine.port, path: req.url, method: req.method, headers: req.headers }, (r) => {
      res.writeHead(r.statusCode || 502, r.headers);
      r.pipe(res);
    });
    up.on("error", (e) => { res.writeHead(502, { "Content-Type": "application/json" }); res.end(JSON.stringify({ error: "engine not reachable: " + e.message })); });
    req.pipe(up);
  }).listen(port, "127.0.0.1", () => console.log(`page on http://127.0.0.1:${port}/ (API from ${engine.origin})`));
} else {
  const out = arg("--out");
  if (!out) { console.error("usage: node build.mjs --out FILE | --serve [--port N] [--engine URL]"); process.exit(2); }
  const result = await esbuild.build(options);
  fs.mkdirSync(path.dirname(out), { recursive: true });
  // Always written: CMake goes by its time; the embedding step skips unchanged bytes.
  fs.writeFileSync(out, page(result));
}
