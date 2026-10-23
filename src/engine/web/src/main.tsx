// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's web page: Preact components (app.tsx) over the engine's API.
import "./style.css";
import { render } from "preact";
import { App } from "./app";
import "./debug";
import { loadInfo, notConnected } from "./state";
import { t, tMsg } from "./i18n";
import { browse } from "./files";
import { refreshJobs } from "./queue";
import { applyScale, watchIncoming } from "./settings";
import "./nav"; // the controller's keys and buttons

applyScale();
window.addEventListener("resize", applyScale);
history.replaceState({ view: "files", path: "/", depth: 0 }, "");
render(<App />, document.getElementById("app")!);

loadInfo().then(() => browse("/", false))
  .catch((e) => { notConnected.value = t("Engine not reachable: {error}", { error: tMsg(e.message) }); browse("/", false); });
refreshJobs();
setInterval(refreshJobs, 1000);
setInterval(() => loadInfo().catch(() => {}), 15000);
setInterval(() => watchIncoming().catch(() => {}), 2000);
