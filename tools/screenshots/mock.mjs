// A stand-in engine for README screenshots: serves the real page and answers
// its API with made-up games, a paired PC engine, an FTP server and jobs.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";

const REPO = process.argv[2];
const DATA = process.argv[3]; // info.json and deps.json from a real engine
const PORT = 5960;
let T0 = 0;
const now = () => Date.now() / 1000;
const GB = 1e9;

const PC = "6f3c9a1e24b74d0e8c51a7f2d9e0b318";
const SELF = "a41b7c2d9e8f40c1b5d36e7f8a9b0c12";
const PCROOT = "/peer/" + PC + "/D:/Games";

// Made-up games: title, title ID, version, icon art.
const GAMES = {
  "Quiet Orbit": ["ALEM00014", "01.004.000", "orbit"],
  "Echoes of Arden": ["ALEM00012", "01.000.003", "hills"],
  "Neon Harbor": ["ALEM00013", "02.010.000", "harbor"],
  "Paper Kingdoms": ["ALEM00015", "01.000.000", "towers"],
  "Starlight Drift": ["ALEM00011", "01.002.000", "sunset"],
  "Tidewalker": ["ALEM00016", "01.101.000", "waves"],
  "Ember Lane": ["ALEM00017", "01.003.000", "ember"],
  "Glass Meridian": ["ALEM00018", "01.000.002", "glass"],
  "Mossback": ["ALEM00019", "01.007.000", "moss"],
};
const titleOf = (p) => {
  const b = path.posix.basename(p).replace(/\.(exfat|ffpkg|ffpfsc|ffpfs|pkg)$/, "").replace(/^\./, "");
  return GAMES[b] ? b : null;
};

const mt = 1792700000;
const g = (dir, name, kind, size) => ({ name, path: dir + "/" + name, kind, ...(kind === "folder" ? { game: true } : { size }), mtime: mt + name.length * 7777 });
const DIRS = {
  "/data": [{ name: "homebrew", path: "/data/homebrew", kind: "folder", mtime: mt }],
  "/data/homebrew": [
    g("/data/homebrew", "Quiet Orbit", "folder"),
    g("/data/homebrew", "Echoes of Arden.exfat", "exfat", 42.7 * GB),
    g("/data/homebrew", "Neon Harbor.ffpkg", "ffpkg", 27.9 * GB),
    g("/data/homebrew", "Paper Kingdoms.ffpfsc", "ffpfsc", 1.3 * GB),
    g("/data/homebrew", "Starlight Drift.ffpfs", "ffpfs", 18.4 * GB),
    g("/data/homebrew", "Tidewalker.exfat", "exfat", 63.2 * GB),
  ],
  "/mnt/usb0": [{ name: "homebrew", path: "/mnt/usb0/homebrew", kind: "folder", mtime: mt }],
  "/mnt/usb0/homebrew": [g("/mnt/usb0/homebrew", "Mossback.ffpfsc", "ffpfsc", 8.2 * GB)],
  [PCROOT]: [
    g(PCROOT, "Ember Lane.pkg", "fpkg", 31.5 * GB),
    g(PCROOT, "Glass Meridian.pkg", "fpkg", 54.0 * GB),
    g(PCROOT, "Mossback.ffpfsc", "ffpfsc", 8.2 * GB),
    g(PCROOT, "Tidewalker.exfat", "exfat", 63.2 * GB),
  ],
  "/ftp/nas": [g("/ftp/nas", "Neon Harbor.pkg", "fpkg", 33.1 * GB), g("/ftp/nas", "Starlight Drift.exfat", "exfat", 19.0 * GB)],
};
DIRS["/peer/" + PC + "/D:"] = [{ name: "Games", path: PCROOT, kind: "folder", mtime: mt }];

const SPACE = [{ root: "/data", total: 825 * GB, free: 312 * GB }, { root: "/mnt/usb0", total: 2000 * GB, free: 1460 * GB }];
const PEER_DRIVES = [{ id: PC, engine: "Living room PC", platform: "host", root: PCROOT, label: "Games", free: 873 * GB, total: 2000 * GB }];
const FTP = [{ id: "nas", name: "NAS", host: "192.168.1.20", port: 21, user: "games", path: "/ftp/nas" }];
const LOCATIONS = [
  { label: "Internal · homebrew (ShadowMount+)", path: "/data/homebrew", exists: true },
  { label: "Internal storage", path: "/data", exists: true },
  { label: "USB 0 · homebrew", path: "/mnt/usb0/homebrew", exists: true },
  { label: "USB 0", path: "/mnt/usb0", exists: true },
  { label: "FTP: NAS", path: "/ftp/nas", exists: true, ftp: true },
  { label: "Living room PC · Games", path: PCROOT, exists: true, peer: PC, engine: "Living room PC", place: "Games" },
  { label: "Living room PC · Finished", path: "/peer/" + PC + "/E:/Finished", exists: true, peer: PC, engine: "Living room PC", place: "Finished", custom: true },
];
const PEERS = {
  enabled: true, listening: true, engines: [
    { id: PC, name: "Living room PC", os: "Windows 11 (x64)", platform: "host", version: "0.2.0", address: "192.168.1.50", port: 5940, url: "http://192.168.1.50:5940/", manual: false, reachable: true, seen_ago: 1, paired: true, waiting_for_code: false },
    { id: "0c8e", name: "Study Mac", os: "macOS 27.0 (arm64)", platform: "host", version: "0.2.0", address: "192.168.1.64", port: 5940, url: "http://192.168.1.64:5940/", manual: false, reachable: true, seen_ago: 2, paired: false, waiting_for_code: false },
  ],
};

const realInfo = JSON.parse(fs.readFileSync(path.join(DATA, "info.json"), "utf8"));
const INFO = Object.assign({}, realInfo, {
  id: SELF, platform: "ps5", os: "PS5", host_name: "PS5", state_dir: "/data/alembic", roots: ["/data", "/mnt/usb0"],
  network_urls: ["http://192.168.1.30:5940/"], local_url: "http://192.168.1.30:5940/", open_to_network: true,
  lz4_runtime: { path: "/data/alembic/runtime/libSceAmpr.sprx", present: true, sha256: "", pinned: true },
  launcher: { supported: true, installed: true, title_id: "ALEM00001", status: "registered", off: false },
  cache: { games: 9, bytes: 1830000 }, port: 5940, enter_button: "cross", lang: "en", busy: true, cpu_instructions: "AES, CRC32C, SHA-256",
});
const DEPS = JSON.parse(fs.readFileSync(path.join(DATA, "deps.json"), "utf8"))
  .map((d) => Object.assign(d, { state: "installed", files: d.files.map((f) => Object.assign(f, { path: f.path.replace(/^.*\/(runtime|keys)\//, "/data/alembic/$1/") })) }));

// Jobs. Running ones move along with the clock.
const spec = (o) => Object.assign({ kind: "convert", level: 6, overwrite: false, rebuild: false, cluster_size: 0, lz4: false, lz4_profile: "auto", lz4_level: 9, lz4_runtime: "", lz4_unpack: false, ampr_emu: "auto", delete_source: false, verify: true }, o);
const running = (id, s, stage, total, startPct, rate, elapsed0) => () => {
  const e = process.env.MOCK_FROZEN ? 0 : now() - T0; // frozen: for comparing two builds
  return { id, spec: s, state: "running", stage, done: Math.min(total, Math.round(total * startPct + rate * e)), total, created: mt, started: mt, finished: 0, error: "", result: null, elapsed: Math.round(elapsed0 + e), keeps_progress: true };
};
const LOCAL = [
  () => ({ id: 3, spec: spec({ input: "/data/homebrew/Paper Kingdoms", output: "/data/homebrew/Paper Kingdoms.ffpfsc", to: "ffpfsc" }), state: "done", stage: "Done", done: 1, total: 1, created: mt, started: mt, finished: mt + 3600, error: "", elapsed: 41,
    result: { from: "folder", to: "FFPFSC", output: "/data/homebrew/Paper Kingdoms.ffpfsc", method: "built a new exFAT image from the folder and compressed it", output_bytes: 1.3 * GB } }),
  () => ({ id: 4, spec: spec({ input: "/ftp/nas/Neon Harbor.pkg", output: "/data/homebrew/Neon Harbor.ffpkg", to: "ffpkg" }), state: "done", stage: "Done", done: 1, total: 1, created: mt, started: mt, finished: mt + 7200, error: "", elapsed: 1514,
    result: { from: "FPKG", to: "FFPKG", output: "/data/homebrew/Neon Harbor.ffpkg", method: "built a new UFS2 image from the file tree", output_bytes: 27.9 * GB, ampr_emulator: "/data/homebrew/Neon Harbor.ffpkg" } }),
  running(5, spec({ input: PCROOT + "/Glass Meridian.pkg", output: "/data/homebrew/Glass Meridian.ffpfsc", to: "ffpfsc" }), "Compressing PFSC", 54.0 * GB, 0.463, 160e6, 1121),
  () => ({ id: 6, spec: spec({ input: "/data/homebrew/Tidewalker.exfat", output: "/mnt/usb0/homebrew/Tidewalker.ffpfsc", to: "ffpfsc", level: 9 }), state: "paused", stage: "Paused", done: 0, total: 0, created: mt, started: 0, finished: 0, error: "", result: null, keeps_progress: true, checkpoint: { done: 24.1 * GB, total: 63.2 * GB } }),
  () => ({ id: 7, spec: spec({ input: "/data/homebrew/Echoes of Arden.exfat", output: "/ftp/nas/Echoes of Arden.exfat", kind: "copy" }), state: "queued", stage: "", done: 0, total: 0, created: mt, started: 0, finished: 0, error: "", result: null }),
];
// The PC's own jobs, with paths as it sees them.
const REMOTE = [
  running(2, spec({ input: "D:/Games/Ember Lane.pkg", output: "D:/Games/Ember Lane.exfat", to: "exfat" }), "Building exFAT", 31.5 * GB, 0.42, 380e6, 248),
];

const ICON_ART = {
  orbit: ["#1b1f4a", "#4a55c9", '<ellipse cx="256" cy="256" rx="190" ry="70" fill="none" stroke="#c9d3ff" stroke-width="14" opacity=".8"/><circle cx="256" cy="256" r="62" fill="#ffb44d"/><circle cx="420" cy="232" r="14" fill="#fff"/>'],
  hills: ["#16332a", "#3c7a55", '<circle cx="380" cy="150" r="34" fill="#f4f1dc"/><path d="M0 330 Q160 200 330 320 T512 300 V512 H0z" fill="#2f6b49"/><path d="M0 400 Q200 300 512 410 V512 H0z" fill="#1e4a33"/>'],
  harbor: ["#3a1c6b", "#1fb5d1", '<path d="M256 110 L330 330 H182z" fill="#22e0d0"/><rect x="0" y="330" width="512" height="182" fill="#0f6f8f" opacity=".7"/>'],
  towers: ["#6b3a1a", "#e09a4a", '<g fill="#3a1d10"><rect x="90" y="250" width="56" height="262"/><rect x="176" y="200" width="56" height="312"/><rect x="262" y="230" width="56" height="282"/><rect x="348" y="180" width="56" height="332"/></g><g fill="#c0392b"><path d="M90 250 l28 -50 28 50z"/><path d="M176 200 l28 -50 28 50z"/><path d="M262 230 l28 -50 28 50z"/><path d="M348 180 l28 -50 28 50z"/></g>'],
  sunset: ["#2c1d55", "#e0517a", '<circle cx="256" cy="250" r="96" fill="#ffd27a"/><g stroke="#ff8fa8" stroke-width="12"><path d="M0 360H512M0 400H512M0 440H512M0 480H512"/></g>'],
  waves: ["#0e3a63", "#7fd0f0", '<g fill="none" stroke="#e8f8ff" stroke-width="16" opacity=".85"><path d="M0 300 q64 -40 128 0 t128 0 t128 0 t128 0"/><path d="M0 370 q64 -40 128 0 t128 0 t128 0 t128 0"/><path d="M0 440 q64 -40 128 0 t128 0 t128 0 t128 0"/></g>'],
  ember: ["#2a0f0f", "#d8452c", '<path d="M256 90 C330 190 360 250 330 330 C310 390 200 400 180 330 C160 270 220 240 256 90z" fill="#ffb347"/><path d="M256 220 C290 280 300 320 270 360 C240 380 220 340 236 300z" fill="#fff1c1"/>'],
  glass: ["#0f2b3d", "#8fe3d6", '<g fill="#dffaf5" opacity=".75"><path d="M120 400 L256 110 L392 400z"/></g><path d="M256 110 L256 400" stroke="#0f2b3d" stroke-width="8"/><path d="M60 420 H452" stroke="#dffaf5" stroke-width="10"/>'],
  moss: ["#1f2e14", "#7aa63c", '<ellipse cx="256" cy="330" rx="170" ry="110" fill="#4c6b27"/><ellipse cx="256" cy="300" rx="150" ry="80" fill="#8fbf4a"/><circle cx="380" cy="300" r="12" fill="#1f2e14"/>'],
};
const iconSvg = (art) => {
  const [a, b, body] = ICON_ART[art];
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"><defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${b}"/><stop offset="1" stop-color="${a}"/></linearGradient></defs><rect width="512" height="512" fill="url(#g)"/>${body}</svg>`;
};

function browse(p) {
  if (p === "/" || !p) {
    return { path: "/", parent: null, entries: [
      ...SPACE.map((s) => ({ name: s.root, path: s.root, kind: "folder" })),
      ...FTP.map((s) => ({ name: "FTP: " + s.name, path: s.path, kind: "folder", ftp: true })),
      ...PEER_DRIVES.map((d) => ({ name: d.engine + " · " + d.label, path: d.root, kind: "folder", peer: d.id })),
    ] };
  }
  const entries = DIRS[p];
  if (!entries) return null;
  const sp = SPACE.find((s) => p === s.root || p.startsWith(s.root + "/")) || PEER_DRIVES.find((s) => p.startsWith(s.root));
  return { path: p, parent: path.posix.dirname(p), entries, ...(sp ? { free: sp.free, total: sp.total } : {}) };
}

function send(res, code, body, type) {
  res.writeHead(code, { "Content-Type": type || "application/json" });
  res.end(typeof body === "string" || Buffer.isBuffer(body) ? body : JSON.stringify(body));
}

http.createServer((req, res) => {
  const u = new URL(req.url, "http://x");
  const p = u.pathname, q = (k) => u.searchParams.get(k);
  let body = "";
  req.on("data", (c) => (body += c));
  req.on("end", () => {
    const json = body ? JSON.parse(body) : {};
    if (p === "/" || p === "/index.html") return send(res, 200, fs.readFileSync(process.env.MOCK_PAGE || REPO + "/build/web/index.html"), "text/html; charset=utf-8");
    if (p === "/i18n.js") return send(res, 200, fs.readFileSync(REPO + "/src/engine/web/i18n.js"), "text/javascript");
    if (p === "/icon.svg" || p === "/favicon.ico") return send(res, 200, fs.readFileSync(REPO + "/assets/icon/alembic.svg"), "image/svg+xml");
    if (p === "/api/info") return send(res, 200, INFO);
    if (p === "/api/fs/space") return send(res, 200, SPACE);
    if (p === "/api/locations") return send(res, 200, LOCATIONS);
    if (p === "/api/ftp") return send(res, 200, FTP);
    if (p === "/api/peer-drives") return send(res, 200, PEER_DRIVES);
    if (p === "/api/peers") return send(res, 200, PEERS);
    if (p === "/api/deps") return send(res, 200, DEPS);
    if (p === "/api/pair/incoming") return send(res, 200, {});
    if (p === "/api/lang") return send(res, 200, {});
    if (p === "/api/jobs" && !T0) T0 = now();
    if (p === "/api/jobs") return send(res, 200, LOCAL.map((f) => f()));
    if (p === "/api/peers/jobs") return send(res, 200, REMOTE.map((f) => Object.assign(f(), { engine: { id: PC, name: "Living room PC" } })));
    if (p === "/api/browse") { const b = browse(q("path")); return b ? send(res, 200, b) : send(res, 404, { error: "no such folder" }); }
    if (p === "/api/meta") {
      const t = titleOf(q("path"));
      if (!t) return send(res, 404, { error: "not a game" });
      const [id, v] = GAMES[t];
      return send(res, 200, { title: t, title_id: id, content_id: "UP0000-" + id + "_00-0000000000000000", version: v, icon: true });
    }
    if (p === "/api/icon") { const t = titleOf(q("path")); return t ? send(res, 200, iconSvg(GAMES[t][2]), "image/svg+xml") : send(res, 404, ""); }
    if (p === "/api/inspect") {
      const ip = q("path"), t = titleOf(ip), kind = (Object.values(DIRS).flat().find((e) => e.path === ip) || {}).kind;
      const fmt = { exfat: "exFAT", ffpkg: "FFPKG (UFS2)", ffpfsc: "FFPFSC", ffpfs: "FFPFS (PFS)", fpkg: "FPKG", folder: "folder" }[kind] || "folder";
      return send(res, 200, { path: ip, format: fmt, title_id: t ? GAMES[t][0] : "", files: 3184, bytes: 61.9 * GB, lz4_packed: false, lz4_profile: "scan", engine: "" });
    }
    if (p === "/api/plan") {
      const ext = { ffpfsc: ".ffpfsc", exfat: ".exfat", ffpkg: ".ffpkg", ffpfs: ".ffpfs", fpkg: ".pkg", folder: "" }[json.to] || "";
      const name = json.output_name || path.posix.basename(json.input || "").replace(/\.[a-z]+$/, "") + ext;
      const dir = json.output_dir || path.posix.dirname(json.input || "/");
      return send(res, 200, Object.assign({}, spec(json), { output: dir + "/" + name, needs: [], ampr_needed: true, ships_emulator: false, warnings: [] }));
    }
    send(res, 404, { error: "not found" });
  });
}).listen(PORT, "127.0.0.1", () => console.log("mock on http://127.0.0.1:" + PORT));
