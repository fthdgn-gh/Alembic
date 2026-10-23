// SPDX-License-Identifier: GPL-3.0-or-later
// The page: header with the sections, the open section, button hints.
import { tab, cwd, focusEntry, info, jobs, notConnected, answered, pad, isConvertible, type Tab } from "./state";
import { t } from "./i18n";
import { IS_PS5 } from "./util";
import { Glyph } from "./ui";
import { Overlays, overlays, topOverlay, type Hint } from "./overlay";
import { Toasts } from "./toasts";
import { focusFirst, goBack, cycleTab, leaveAll, press, pushNav } from "./nav";
import { FilesView, openActions } from "./files";
import { QueueView } from "./queue";
import { DepsView } from "./deps";
import { SettingsView } from "./settings";

export { browse } from "./files";

export function showTab(next: Tab, push?: boolean) {
  if (push !== false && next !== tab.value) pushNav({ view: next, path: cwd.value });
  tab.value = next;
  if (!overlays.value.length) setTimeout(focusFirst, 0);
}

function Status() {
  const running = jobs.value.filter((j) => j.state === "running").length, queued = jobs.value.filter((j) => j.state === "queued" || j.state === "paused").length;
  const off = notConnected.value;
  const text = off || (info.value ? (running ? t("Working") : t("Idle")) + (queued ? " · " + t("{n} waiting", { n: queued }) : "") : answered.value ? t("Not connected") : t("connecting…"));
  return (
    <div class={"status" + (!off && running ? " live" : "") + (off || (answered.value && !info.value) ? " off" : "")} id="status">
      <span class="dot" /><span id="status-text">{text}</span>
    </div>
  );
}

function Header() {
  const n = jobs.value.filter((j) => j.state === "running" || j.state === "queued" || j.state === "paused").length;
  const tabButton = (id: Tab, label: string, extra?: any) => (
    <button class={"tab" + (tab.value === id ? " on" : "")} data-tab={id} data-key={"tab-" + id} onClick={() => showTab(id)}>{extra ? [<span>{label}</span>, extra] : label}</button>
  );
  return (
    <header class="top">
      <div class="logo"><img src="/icon.svg" alt="" />Alembic</div>
      <nav class="tabs" id="tabs">
        <span class="shoulder" data-pad hidden={!pad.value}>L1</span>
        {tabButton("files", t("Files"))}
        {tabButton("queue", t("Queue"), <span class="badge" id="qcount" hidden={!n}>{n}</span>)}
        {tabButton("deps", t("Components"))}
        {tabButton("settings", t("Settings"))}
        <span class="shoulder" data-pad hidden={!pad.value}>R1</span>
      </nav>
      <Status />
    </header>
  );
}

// Hints come from the screen on top: the open overlay, else the current tab.
function Hints() {
  const top = topOverlay();
  let list: Hint[];
  if (top) list = top.hints || [["x", t("Choose")], ["o", t("Back"), goBack]];
  else if (tab.value === "files") {
    const e = focusEntry.value, c = cwd.value;
    list = [["x", e ? (e.kind === "folder" ? t("Open") : isConvertible(e) ? t("Convert") : t("Options")) : t("Choose")]];
    if (c && c !== "/") list.push(["o", t("Back"), goBack]);
    if (e && c !== "/") list.push(["s", t("Options"), () => openActions(e)]);
    list.push(["lr", t("Sections"), () => cycleTab(1)]);
  } else list = [["x", t("Choose")], ["o", t("Back"), goBack], ["lr", t("Sections"), () => cycleTab(1)]];
  // Leaving is for the console's own browser (one more ○ then closes it).
  if (!top && IS_PS5) list.push(["l2r2", t("Leave"), leaveAll]);
  return (
    <footer class="hints" id="hints">
      {list.map(([b, label, fn]) => (
        <button class="hint" tabIndex={-1} data-nonav onMouseDown={(ev) => ev.preventDefault()} onClick={fn || (() => press("ok"))}>
          <span class="g"><Glyph b={b} /></span>{label}
        </button>
      ))}
    </footer>
  );
}

export function App() {
  const current = tab.value;
  return (
    <>
      <Header />
      <FilesView hidden={current !== "files"} />
      <QueueView hidden={current !== "queue"} />
      <DepsView hidden={current !== "deps"} />
      <SettingsView hidden={current !== "settings"} />
      <Hints />
      <Toasts />
      <Overlays />
    </>
  );
}
