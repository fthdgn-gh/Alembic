// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Updates: the latest release on GitHub and, on a console, the
// payload files loaders start, replaced before the new engine takes over.
import { useEffect, useState } from "preact/hooks";
import type { UpdateStatus, Info } from "../../api/types";
import { api } from "../../api/client";
import { tab } from "../../state/view";
import { jobs } from "../../state/jobs";
import { store } from "../../lib/storage";
import { t, tMsg } from "../../lib/i18n";
import { Facts, SwitchRow } from "../../components/Rows";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { Head, SmallBtn } from "./parts";

const stateText = (s: string) => ({
  checking: t("Asking GitHub…"), searching: t("Looking for the payload files…"), downloading: t("Downloading…"),
  replacing: t("Replacing the payload files…"), starting: t("Starting the new engine…"),
} as Record<string, string>)[s] || s;

export function Updates() {
  const [u, setU] = useState<UpdateStatus | null>(null);
  const [off, setOff] = useState<Record<string, boolean>>({}); // files the person left out
  const [waitFor, setWaitFor] = useState<string | null>(null); // the version being started
  const [late, setLate] = useState(false);
  const load = () => api<UpdateStatus>("GET", "/api/update").then(setU, () => {});
  useEffect(() => { if (tab.value === "settings") load(); }, [tab.value]);
  // While it works, ask again every second.
  useEffect(() => {
    if (!u || u.state === "idle" || waitFor) return;
    const timer = setTimeout(load, 1000);
    return () => clearTimeout(timer);
  }, [u, waitFor]);
  // After the hand-off: this engine stops, the new one answers; then the page reloads (it is the new engine's).
  useEffect(() => {
    if (!waitFor) return;
    const started = Date.now();
    const timer = setInterval(() => {
      if (Date.now() - started > 90000) setLate(true);
      api<Info>("GET", "/api/info").then((i) => { if (i.version === waitFor) location.reload(); }, () => {});
    }, 2000);
    return () => clearInterval(timer);
  }, [waitFor]);
  // Not before v0.2.3, or the engine is busy starting.
  if (!u) return null;

  const busy = u.state !== "idle";
  const latest = u.latest;
  const status = waitFor ? t("Starting Alembic {version}… This page reloads when it is up.", { version: waitFor })
    : busy ? stateText(u.state)
    : !latest ? (u.checked ? t("Could not check") : t("Not checked yet"))
    : u.newer ? t("Version {version} is available", { version: latest.version }) : t("Up to date");
  const chosen = u.payloads.filter((p) => !off[p.path]);
  // The console app's image: replaced when the release's differs.
  const app = u.app;
  const appDiffers = !!(app && app.image && app.in_release && !app.current);
  const appOn = appDiffers && !off["app"];
  const appText = !app ? "" : !app.path ? t("not found")
    : !app.image ? t("{version}, a folder: not replaced", { version: app.version || "?" })
    : app.current ? t("{version}, up to date", { version: app.version || "?" })
    : app.in_release ? t("{version}; the release's image differs", { version: app.version || "?" }) : app.version || "?";
  const running = jobs.value.filter((j) => j.state === "running");
  const restarts = running.filter((j) => !j.keeps_progress);

  async function post(path: string, body?: unknown) {
    try { setU(await api<UpdateStatus>("POST", path, body || {})); }
    catch (e: any) { toast(tMsg(e.message), "bad"); }
  }
  async function updateApp() {
    try {
      setU(await api<UpdateStatus>("POST", "/api/update/app"));
      toast(t("The new app image is downloaded; it replaces the app once the app is closed."), "ok");
    } catch (e: any) { toast(tMsg(e.message), "bad"); }
  }
  async function install() {
    const body = <div>
      <p style="margin-top:0">{t("Alembic {version} is downloaded from GitHub and checked; the files below are replaced, and the new engine starts through the ELF loader (port 9021) and takes over from this one.", { version: latest!.version })}</p>
      {chosen.length ? <ul class="mono" style="font-size:.85rem">{chosen.map((p) => <li>{p.path}{p.rename_to ? " → " + p.rename_to : ""}</li>)}</ul>
        : <div class="warnbox">{t("No payload files are replaced: your loader still starts the old version next time.")}</div>}
      {appOn ? <p class="muted">{t("The console app's image is replaced too, once the app is closed.")}</p> : null}
      {restarts.length ? <div class="warnbox">{t("A running job can't keep its progress; it starts over after the update.")}</div>
        : running.length ? <p class="muted">{t("The running job continues after the update.")}</p> : null}
    </div>;
    if (!await ask({ title: t("Update to {version}?", { version: latest!.version }), body, ok: t("Update") })) return;
    try {
      setU(await api<UpdateStatus>("POST", "/api/update/install", { paths: chosen.map((p) => p.path), app: appOn }));
      setWaitFor(latest!.version);
    } catch (e: any) { toast(tMsg(e.message), "bad"); }
  }

  return (
    <>
      <Head text={t("Updates")} />
      <div class="card">
        <Facts rows={[
          [t("This version"), u.current],
          [t("Latest release"), latest ? latest.version : "–"],
          [t("Status"), status, u.error && !busy && !waitFor ? "" : undefined],
          ...(app ? [[t("Console app"), appText] as [string, string]] : []),
          ...(app && app.path ? [[t("App image"), app.path, "mono"] as [string, string, string]] : []),
        ]} />
        {u.app_pending ? <div class="note">{t("A new app image is waiting: close the app on the console to finish its update.")}</div> : null}
        {u.error && !busy && !waitFor ? <div class="err">{tMsg(u.error)}</div> : null}
        {late ? <div class="note">{t("The new engine hasn't answered yet. If it doesn't start, load Alembic again from your loader: its files were already updated.")}</div> : null}
        {latest && u.newer && latest.notes ? <div class="notes">{latest.notes}</div> : null}
        <div class="btns">
          <SmallBtn k="u:check" text={t("Check now")} disabled={busy || !!waitFor} onClick={() => post("/api/update/check")} />
          {appDiffers && latest && latest.version === u.current && !u.app_pending ? <SmallBtn k="u:app" text={t("Update the app")} disabled={busy || !!waitFor} onClick={updateApp} /> : null}
          {u.newer && !u.can_install && latest ? <a class="btn sm" data-key="u:get" href={latest.url} target="_blank" rel="noopener">{t("Download from GitHub")}</a> : null}
        </div>
        {u.newer && !u.can_install ? <p class="muted" style="margin:.5rem 0 0">{t("On a PC, replace alembic-engine with the new release and start it again.")}</p> : null}
      </div>
      {u.newer && u.can_install && latest ? <>
        <Head text={t("Payload files to update")} />
        <div class="card">
          {u.payloads.length ? null : <p class="muted" style="margin:0">{u.searched === "no" ? t("Not searched yet.")
            : t("No Alembic payload files in the usual places (Payload Manager, the autoloaders). If your loader keeps it elsewhere, search the drives.")}</p>}
          {appDiffers ? <SwitchRow dataKey="u:app-on" label={t("Console app")} on={appOn} hint={app!.path}
            onChange={(on) => setOff(Object.assign({}, off, { app: !on }))} /> : null}
          {u.payloads.map((p) => (
            <SwitchRow dataKey={"u:p:" + p.path} label={p.path} on={!off[p.path]}
              hint={[p.loader === "autoloader" ? t("autoloader") : p.loader, p.version ? t("version {version}", { version: p.version }) : t("an older version"),
                p.rename_to ? t("renamed to {name}", { name: p.rename_to }) : ""].filter(Boolean).join(" · ")}
              onChange={(on) => setOff(Object.assign({}, off, { [p.path]: !on }))} />
          ))}
          <div class="btns">
            <SmallBtn k="u:find" text={t("Search the drives")} disabled={busy || !!waitFor} onClick={() => post("/api/update/find", { everywhere: true })} />
            <SmallBtn k="u:install" cls="primary" text={t("Update to {version}", { version: latest.version })} disabled={busy || !!waitFor} onClick={install} />
          </div>
        </div>
      </> : null}
    </>
  );
}

// Once per release: say there is a newer one (the engine checks at start).
let told = "";
export function noticeUpdate(i: Info | null) {
  const v = i && i.update && i.update.newer ? i.update.latest : null;
  if (!v || v === told) return;
  told = v;
  if (store.get<string>("updateTold") === v) return;
  store.set("updateTold", v);
  toast(t("Alembic {version} is available: Settings > Updates", { version: v }), "ok");
}
