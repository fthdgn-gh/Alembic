// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's web page: Preact components over the engine's API.
//   api/         requests and the API's types
//   state/       what the page knows, as signals
//   lib/         i18n, formats, storage, where the page runs
//   input/       the controller: focus, keys and buttons, history (○ is Back)
//   components/  shared pieces: dialogs, icons, form rows, toasts
//   dialogs/     Convert, the folder picker, downloading components
//   pages/       one folder per section: files, queue, deps, settings
//   app/         the page around them: header, button hints
import "./styles/index.css";
import { render } from "preact";
import { App } from "./app/App";
import "./debug";
import { effect } from "@preact/signals";
import { info, loadInfo, notConnected } from "./state/engine";
import { refreshJobs } from "./state/jobs";
import { t, tMsg } from "./lib/i18n";
import { browse } from "./pages/files/browse";
import { applyScale } from "./pages/settings/prefs";
import { watchIncoming } from "./pages/settings/Engines";
import { noticeUpdate } from "./pages/settings/Updates";
import "./input/controller"; // the controller's keys and buttons
import { depth } from "./input/history";

applyScale();
window.addEventListener("resize", applyScale);
// A reload keeps its place in the history: the entries before it are the
// page's too, and leaving (L2 + R2) goes back through them.
history.replaceState({ view: "files", path: "/", depth: depth() }, "");
render(<App />, document.getElementById("app")!);

loadInfo().then(() => browse("/", false))
  .catch((e) => { notConnected.value = t("Engine not reachable: {error}", { error: tMsg(e.message) }); browse("/", false); });
refreshJobs();
setInterval(refreshJobs, 1000);
setInterval(() => loadInfo().catch(() => {}), 15000);
setInterval(() => watchIncoming().catch(() => {}), 2000);
// A newer release (the engine checks GitHub at start): said once per version.
effect(() => noticeUpdate(info.value));
