// SPDX-License-Identifier: GPL-3.0-or-later
// Components: third-party files the engine downloads on request, pinned by SHA-256.
import { useEffect, useState } from "preact/hooks";
import type { Dep } from "./types";
import { tab, loadInfo } from "./state";
import { api, bytes } from "./util";
import { t, tMsg } from "./i18n";
import { toast } from "./toasts";
import { overlays } from "./overlay";
import { focusFirst, zoneOf } from "./nav";

const DEP_STATE: Record<string, [string, string]> = { installed: ["done", "installed"], missing: ["queued", "not installed"], partial: ["failed", "incomplete"], modified: ["failed", "changed"] };

export function DepsView(props: { hidden: boolean }) {
  const [deps, setDeps] = useState<Dep[] | null>(null);
  const [busy, setBusy] = useState<string | null>(null); // "Downloading…" while one runs
  async function load() {
    try { setDeps(await api<Dep[]>("GET", "/api/deps")); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  }
  // Read again each time the tab opens.
  useEffect(() => { if (tab.value === "deps") load(); }, [tab.value]);
  useEffect(() => {
    if (tab.value === "deps" && deps && !overlays.value.length && zoneOf(document.activeElement) !== "deps") setTimeout(focusFirst, 0);
  }, [deps]);
  async function run(d: Dep, action: "install" | "remove") {
    setBusy(d.id + ":" + action);
    try { await api("POST", "/api/deps/" + d.id + "/" + action); toast(t(action === "install" ? "{name} installed" : "{name} removed", { name: t(d.label) }), "ok"); }
    catch (e: any) { toast(tMsg(e.message), "bad"); }
    await loadInfo().catch(() => {});
    await load();
    setBusy(null);
  }
  return (
    <section class="view scroll" id="view-deps" hidden={props.hidden}>
      <div class="page" data-zone="deps">
        <p class="muted" style="margin:.3rem .2rem 1rem">{t("Files from other projects that Alembic doesn't ship. Each one is downloaded from the source shown and kept only if its SHA-256 matches.")}</p>
        <div id="deps">
          {(deps || []).map((d) => {
            const st = DEP_STATE[d.state] || ["queued", d.state];
            return (
              <div class="card">
                <div class="hd"><div class="ttl">{t(d.label)}</div><span class={"state " + st[0]}>{t(st[1])}</span></div>
                <p style="margin:.5rem 0 .8rem">{t(d.purpose)}</p>
                <div class="facts">
                  <div>{t("Source")}</div><div>{d.source}</div>
                  <div>{t("License")}</div><div>{d.license}</div>
                  <div>{t("Files")}</div><div class="mono">{d.files.map((f) => <div title={f.url + "\nSHA-256 " + f.sha256}>{f.path}</div>)}</div>
                </div>
                <div class="btns">
                  <span class="muted">{busy && busy.indexOf(d.id + ":") === 0 ? (busy.slice(-7) === "install" ? t("Downloading…") : t("Removing…")) : ""}</span>
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
