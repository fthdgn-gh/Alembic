// SPDX-License-Identifier: GPL-3.0-or-later
// Settings: the console shortcut, the page, Places, other engines, FTP
// servers, the engine itself and its cache.
import { useEffect, useState } from "preact/hooks";
import { api } from "../../api/client";
import { clearMeta } from "../../api/meta";
import { info, locations, peers, loadInfo, loadPeers } from "../../state/engine";
import { tab } from "../../state/view";
import { jobs } from "../../state/jobs";
import { shownPath } from "../../state/paths";
import { bytes } from "../../lib/format";
import { store } from "../../lib/storage";
import { t, tMsg, tStatus, plural, lang, LANGS } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { ChoiceRow, SwitchRow, Facts } from "../../components/Rows";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { setPlace } from "../files/actions";
import { SCALES, applyScale, setLang } from "./prefs";
import { Head, SmallBtn } from "./parts";
import { Engines } from "./Engines";
import { FtpServers } from "./FtpServers";
import { Updates } from "./Updates";
import { openControllerTest } from "./ControllerTest";

export function SettingsPage(props: { hidden: boolean }) {
  const i = info.value;
  const [scale, setScale] = useState(store.get<number>("scale") || 1);
  // Other engines come and go: asked again every few seconds while this is open.
  useEffect(() => {
    if (tab.value !== "settings") return;
    const timer = setInterval(() => { const p = peers.value; if (p && (p.enabled || p.engines.length)) loadPeers().catch(() => {}); }, 4000);
    return () => clearInterval(timer);
  }, [tab.value]);
  return (
    <section class="view scroll" id="view-settings" hidden={props.hidden}>
      <div class="page" data-zone="settings" id="settings">
        <Head text={t("Console")} />
        <Launcher />
        <Head text={t("Page")} />
        <ChoiceRow dataKey="s:lang" label={t("Language")} options={LANGS.map((l) => ({ value: l[0], label: l[1] }))} value={lang.value} onChange={setLang} />
        <ChoiceRow dataKey="s:scale" label={t("Text size")} hint={t("For reading from the couch")} options={SCALES.map((x) => ({ value: x.value, label: t(x.label) }))} value={scale}
          onChange={(v) => { setScale(v); store.set("scale", v); applyScale(); }} />
        <button class="frow" data-key="s:test" onClick={openControllerTest}>
          <div class="fl"><b>{t("Controller test")}</b><small>{t("Shows what each button sends to this page")}</small></div><span class="arr">›</span>
        </button>
        <Places />
        <Engines />
        <FtpServers />
        <Head text={t("Engine")} />
        <div class="card">
          <Facts rows={!i ? [[t("Engine"), t("not connected")]] : ([
            [t("Version"), i.version],
            [t("Running on"), i.platform === "ps5" ? t("PS5 (payload)") : i.os || t("PC")],
            i.host_name && i.platform !== "ps5" ? [t("Computer"), i.host_name] : null,
            i.cpu_instructions ? [t("CPU instructions"), i.cpu_instructions === "none" ? "–" : i.cpu_instructions] : null,
            [t("Address"), location.host, "mono"],
            [t("From other devices"), i.network_urls && i.network_urls.length ? i.network_urls.join("\n") : i.open_to_network ? "–" : t("This computer only"), "mono"],
            i.roots && i.roots.length ? [t("Shared folders"), i.roots.join("\n"), "mono"] : null,
            [t("State folder"), i.state_dir, "mono"],
            [t("LZ4 runtime"), i.lz4_runtime && i.lz4_runtime.present ? (i.lz4_runtime.pinned ? "ampr_emu 0.4.2.1" : t("present, not the pinned build")) : t("not downloaded")],
          ].filter(Boolean) as [string, string, string?][])} />
          <div class="btns"><SmallBtn k="s:quit" cls="danger" text={t("Stop the engine")} onClick={quitEngine} /></div>
        </div>
        <Updates />
        {i && i.cache ? <Cache /> : null}
      </div>
    </section>
  );
}

function Launcher() {
  const i = info.value, l = i && i.launcher;
  const [busy, setBusy] = useState(false);
  if (!(l && l.supported)) return (
    <div class="frow"><div class="fl"><b>{t("Shortcut in Media")}</b>
      <small>{l && i!.platform === "ps5" ? tStatus(l.status || "not available") : t("Only when the engine runs on a PS5.")}</small></div></div>
  );
  return (
    <>
      <SwitchRow dataKey="s:launcher" label={t("Shortcut in Media")} hint={t("Adds Alembic to the Media tab of the home screen; it opens this page in the console's browser")} on={l.installed}
        onChange={async (on) => {
          try { info.value = await api("POST", "/api/launcher", { enabled: on }); toast(on ? t("Added to Media on the home screen") : t("Shortcut removed"), "ok"); }
          catch (e: any) { toast(tMsg(e.message), "bad"); }
        }} />
      <div class="card">
        <Facts rows={[[t("Status"), tStatus(l.status || ""), /failed|not available/.test(l.status) ? "err" : ""], [t("Title ID"), l.title_id, "mono"]]} />
        <div class="btns"><SmallBtn k="s:again" text={t("Register again")} disabled={busy} onClick={async () => {
          setBusy(true);
          try { info.value = await api("POST", "/api/launcher", { enabled: true, again: true }); toast(t("Shortcut registered again; check Media on the home screen"), "ok"); }
          catch (e: any) { toast(tMsg(e.message), "bad"); }
          loadInfo().catch(() => {}).then(() => setBusy(false));
        }} /></div>
      </div>
    </>
  );
}

// Places the person added, here and on paired engines.
function Places() {
  const mine = locations.value.filter((l) => l.custom);
  return (
    <>
      <Head text={t("Places")} />
      <div class="card">
        {!mine.length ? <p class="muted" style="margin:0">{t("No places yet: in Files, open a folder's Options and choose Add to Places. They show here and on paired engines.")}</p> : null}
        {mine.map((l) => (
          <div class="ftprow">
            <div class="nm">{ICONS.place()}</div>
            <div style="min-width:0;flex:1"><div class="ellip">{l.place || l.label}</div><div class="muted mono ellip" style="font-size:.85rem">{shownPath(l.path)}</div></div>
            <SmallBtn k={"place:rm:" + l.path} cls="danger" text={t("Remove")} onClick={() => setPlace(l.path, false)} />
          </div>
        ))}
      </div>
    </>
  );
}

// Titles and icons the engine read from games; read again when needed.
function Cache() {
  const c = info.value!.cache;
  const [busy, setBusy] = useState(false);
  return (
    <>
      <Head text={t("Cache")} />
      <div class="card">
        <Facts rows={[[t("Game info"), plural(c.games, "{n} game", "{n} games") + " · " + bytes(c.bytes)]]} />
        <p class="muted" style="margin:.5rem 0 0">{t("Titles and icons, so lists open quickly. Entries unused for 30 days are removed at start, and the cache is kept under 100 MB.")}</p>
        <div class="btns"><SmallBtn k="s:cache" text={t("Clear cache")} disabled={busy || !(c.games || c.bytes)} onClick={async () => {
          setBusy(true);
          try {
            const cache = await api("POST", "/api/cache/clear");
            info.value = Object.assign({}, info.value!, { cache });
            clearMeta();
            toast(t("Cache cleared"), "ok");
          } catch (e: any) { toast(tMsg(e.message), "bad"); }
          setBusy(false);
        }} /></div>
      </div>
    </>
  );
}

async function quitEngine() {
  const running = jobs.value.some((j) => j.state === "running");
  const body = <div><p style="margin-top:0">{t("This page stops working until the engine is loaded again.")}</p>
    {running ? <div class="warnbox">{t("A job is running; it will be cancelled.")}</div> : null}</div>;
  if (!await ask({ title: t("Stop the engine?"), body, ok: t("Stop"), danger: true })) return;
  try { await api("POST", "/api/quit"); toast(t("Engine stopped"), "ok"); } catch (e: any) { toast(tMsg(e.message), "bad"); }
}
