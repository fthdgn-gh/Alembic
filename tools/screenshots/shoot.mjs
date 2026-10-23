// Drives a headless Chromium-based browser over DevTools and saves screenshots of the mock page.
import { spawn } from "node:child_process";
import fs from "node:fs";

const OUT = process.argv[2];
const ONLY = process.argv[3] || "";
const PROFILE = process.argv[4]; // a scratch browser profile folder
const BROWSER = process.env.BROWSER || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const UA = "Mozilla/5.0 (PlayStation; PlayStation 5/9.60) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.4 Safari/605.1.15";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const br = spawn(BROWSER, ["--headless=new", "--remote-debugging-port=9333", "--user-data-dir=" + PROFILE, "--no-first-run", "--hide-scrollbars", "--force-color-profile=srgb", "about:blank"], { stdio: "ignore" });
let targets;
for (let i = 0; i < 50; i++) {
  try { targets = await (await fetch("http://127.0.0.1:9333/json")).json(); if (targets.some((t) => t.type === "page")) break; } catch {}
  await sleep(200);
}
const ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let seq = 0;
const pending = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); } };
const cdp = (method, params = {}) => new Promise((r) => { const id = ++seq; pending.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
const run = async (expr) => { const r = await cdp("Runtime.evaluate", { expression: expr, awaitPromise: true }); if (r.result && r.result.exceptionDetails) console.error(expr, JSON.stringify(r.result.exceptionDetails)); return r.result; };
const key = async (k, code, vk) => {
  await cdp("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk });
  await cdp("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk });
  await sleep(120);
};
const down = () => key("ArrowDown", "ArrowDown", 40);
const shot = async (name) => {
  if (ONLY && ONLY !== name) return;
  await sleep(600);
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  fs.writeFileSync(OUT + "/" + name + ".png", Buffer.from(r.result.data, "base64"));
  console.log("saved", name);
};

await cdp("Network.setUserAgentOverride", { userAgent: UA });
await cdp("Emulation.setDeviceMetricsOverride", { width: 1600, height: 900, deviceScaleFactor: 2, mobile: false });
await cdp("Page.enable");
await cdp("Page.navigate", { url: "http://127.0.0.1:5960/" });
await sleep(2500);

// 1. Files: the homebrew folder, first game focused.
await run(`browse("/data/homebrew")`);
await sleep(1500);
await down(); await key("ArrowUp", "ArrowUp", 38);
await shot("files");

// 2. Convert: a package on the paired PC, opened from Files.
await run(`browse(${JSON.stringify("/peer/6f3c9a1e24b74d0e8c51a7f2d9e0b318/D:/Games")})`);
await sleep(1500);
await run(`openConvert(state.listing.entries.find((e) => e.name === "Glass Meridian.pkg"))`);
await sleep(1500);
await shot("convert");
await run(`closeOverlay(null)`);
await sleep(500);

// 3. Queue: give the speed estimate a few seconds of samples.
await run(`showTab("queue")`);
await sleep(6000);
await shot("queue");

// 4. Settings: scroll to the other engines.
await run(`showTab("settings")`);
await sleep(1200);
await run(`(() => { const s = [...document.querySelectorAll("#settings .sect")].find((x) => x.textContent === "Places"); s.scrollIntoView({ block: "start" }); document.querySelector("#view-settings").scrollTop -= 20; })()`);
await shot("engines");

ws.close();
br.kill();
