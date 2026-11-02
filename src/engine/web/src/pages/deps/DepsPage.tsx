// SPDX-License-Identifier: GPL-3.0-or-later
// Components: third-party files the engine downloads on request, checked by
// SHA-256. The AMPR emulators follow their projects' latest releases.
import { useEffect, useState } from "preact/hooks";
import type { Dep, DepsStatus } from "../../api/types";
import { api } from "../../api/client";
import { loadInfo } from "../../state/engine";
import { tab } from "../../state/view";
import { bytes } from "../../lib/format";
import { t, tMsg } from "../../lib/i18n";
import { toast } from "../../components/Toasts";
import { overlays } from "../../components/Overlay";
import { focusFirst, zoneOf } from "../../input/focus";
import { SwitchRow } from "../../components/Rows";

const DEP_STATE: Record<string, [string, string]> = { installed: ["done", "installed"], missing: ["queued", "not installed"], partial: ["failed", "incomplete"], modified: ["failed", "changed"] };

export function DepsPage(props: { hidden: boolean }) {
  const [deps, setDeps] = useState<Dep[] | null>(null);
  const [busy, setBusy] = useState<string | null>(null); // "Downloading…" while one runs
  const [status, setStatus] = useState<DepsStatus | null>(null); // null: an engine before 0.4.3
  async function load() {
    try { setDeps(await api<Dep[]>("GET", "/api/deps")); } catch (e: any) { toast(tMsg(e.message), "bad"); }
    try { setStatus(await api<DepsStatus>("GET", "/api/deps/status")); } catch (e) { setStatus(null); }
  }
  // While the engine asks GitHub, look again every two seconds.
  useEffect(() => {
    if (!status || !status.checking || tab.value !== "deps") return;
    const id = setTimeout(() => { load(); loadInfo().catch(() => {}); }, 2000);
    return () => clearTimeout(id);
  }, [status, tab.value]);
  async function setting(path: string, body?: object) {
    try { setStatus(await api<DepsStatus>("POST", path, body)); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  }
  // Read again each time the tab opens.
  useEffect(() => { if (tab.value === "deps") load(); }, [tab.value]);
  useEffect(() => {
    if (tab.value === "deps" && deps && !overlays.value.length && zoneOf(document.activeElement) !== "deps") setTimeout(focusFirst, 0);
  }, [deps]);
  async function run(d: Dep, action: "install" | "remove" | "use") {
    setBusy(d.id + ":" + action);
    try {
      const r = await api<Dep>("POST", "/api/deps/" + d.id + "/" + action);
      toast(action === "install" && r.version ? t("{name} {version} installed", { name: t(d.label), version: r.version })
        : t(action === "install" ? "{name} installed" : action === "use" ? "{name} is now in use" : "{name} removed", { name: t(d.label) }), "ok");
    }
    catch (e: any) { toast(tMsg(e.message), "bad"); }
    await loadInfo().catch(() => {});
    await load();
    setBusy(null);
  }
  return (
    <section class="view scroll" id="view-deps" hidden={props.hidden}>
      <div class="page" data-zone="deps">
        <p class="muted" style="margin:.3rem .2rem 1rem">{status ? t("Files from other projects that Alembic doesn't ship. Each one is downloaded from the source shown and kept only if its SHA-256 matches. The AMPR emulators follow their projects' latest releases.")
          : t("Files from other projects that Alembic doesn't ship. Each one is downloaded from the source shown and kept only if its SHA-256 matches.")}</p>
        {status ? <div style="margin-bottom:1rem">
          <SwitchRow dataKey="deps:auto" label={t("Update automatically")} hint={t("When a newer AMPR emulator is out, the engine downloads it at start. Games keep the one they got until you add it again.")}
            on={status.auto_update} onChange={(on) => setting("/api/deps/auto_update", { on })} />
          <div style="text-align:right;margin-top:.5rem">
            <span class="muted" style="margin-right:.5rem">{status.checking ? t("Checking…") : ""}</span>
            <button class="btn sm" data-key="deps:check" disabled={status.checking || !!busy} onClick={() => setting("/api/deps/check")}>{t("Check for updates")}</button>
          </div>
        </div> : null}
        <div id="deps">
          {(deps || []).map((d) => {
            const st = DEP_STATE[d.state] || ["queued", d.state];
            return (
              <div class="card">
                <div class="hd"><div class="ttl">{t(d.label)}</div>{d.in_use ? <span class="state running">{t("in use")}</span> : null}<span class={"state " + st[0]}>{t(st[1])}</span></div>
                <p style="margin:.5rem 0 .8rem">{t(d.purpose)}</p>
                <div class="facts">
                  <div>{t("Source")}</div><div>{d.source}</div>
                  {d.follows !== undefined && (d.version || d.latest || d.check_error) ? [<div>{t("Version")}</div>, <div>
                    {d.version || t("not downloaded")}
                    {d.update ? <span class="state running" style="margin-left:.5rem">{t("{version} is out", { version: d.latest! })}</span>
                      : d.version && d.latest && d.version === d.latest ? <span class="muted">{" · " + t("the latest")}</span> : null}
                    {d.check_error ? <div class="muted">{t("Couldn't check for updates: {error}", { error: tMsg(d.check_error) })}</div> : null}
                  </div>] : null}
                  <div>{t("License")}</div><div>{d.license}</div>
                  <div>{t("Files")}</div><div class="mono">{d.files.map((f) => <div title={f.url + "\nSHA-256 " + f.sha256}>{f.path}</div>)}</div>
                </div>
                <div class="btns">
                  <span class="muted">{busy && busy.indexOf(d.id + ":") === 0 && busy.slice(-4) !== ":use" ? (busy.slice(-7) === "install" ? t("Downloading…") : t("Removing…")) : ""}</span>
                  {d.update && d.state === "installed" ? <button class="btn sm primary" data-key={d.id + ":update"} disabled={!!busy} onClick={() => run(d, "install")}>{t("Update to {version}", { version: d.latest! })}</button> : null}
                  {d.group && d.state === "installed" && !d.in_use ? <button class="btn sm" data-key={d.id + ":use"} disabled={!!busy} onClick={() => run(d, "use")}>{t("Use this one")}</button> : null}
                  {d.state !== "installed" ? <button class="btn sm primary" data-key={d.id + ":get"} disabled={!!busy} onClick={() => run(d, "install")}>
                    {d.state === "missing" ? t("Download ({size})", { size: bytes(d.bytes) }) : t("Download again")}</button> : null}
                  {d.state !== "missing" ? <button class="btn sm" data-key={d.id + ":rm"} disabled={!!busy} onClick={() => run(d, "remove")}>{t("Remove")}</button> : null}
                </div>
              </div>
            );
          })}
        </div>
      </div>
    </section>
  );
}
