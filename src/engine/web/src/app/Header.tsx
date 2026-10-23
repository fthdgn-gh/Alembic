// SPDX-License-Identifier: GPL-3.0-or-later
// The top bar: the sections (L1/R1 switch them) and the engine's status.
import { info, notConnected, answered } from "../state/engine";
import { tab, pad, type Tab } from "../state/view";
import { jobs, isActive } from "../state/jobs";
import { t } from "../lib/i18n";
import { showTab } from "../input/history";

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

export function Header() {
  const n = jobs.value.filter(isActive).length;
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

