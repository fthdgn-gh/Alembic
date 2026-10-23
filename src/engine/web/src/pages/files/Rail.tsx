// SPDX-License-Identifier: GPL-3.0-or-later
// The left column of Files: drives, FTP servers, paired engines' drives,
// then Places (this engine's, recent folders, each paired engine's).
import { cwd } from "../../state/view";
import { space, ftp, peerDrives, locations } from "../../state/engine";
import { rootOf, peerDriveOf, ftpOf, driveName, shownPath, hereName } from "../../state/paths";
import { base, bytes } from "../../lib/format";
import { store } from "../../lib/storage";
import { t, placeLabel } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { browse } from "./browse";

function RailItem(props: { k: string; on: boolean; title: string; icon: keyof typeof ICONS; name: string; onClick: () => void; children?: any }) {
  return (
    <button class={"ritem" + (props.on ? " on" : "")} data-key={props.k} title={props.title} onClick={props.onClick}>
      <div class="nm">{ICONS[props.icon]()}<span class="ellip">{props.name}</span></div>
      {props.children}
    </button>
  );
}
function Meter(props: { free: number; total: number }) {
  const used = props.total ? 1 - props.free / props.total : 0;
  return <div class={"meter" + (props.free < 20e9 ? " low" : "")}><div style={"width:" + (used * 100).toFixed(1) + "%"} /></div>;
}
const within = (root: string) => !!cwd.value && (cwd.value === root || cwd.value.indexOf(root + "/") === 0);

export function Rail() {
  const c = cwd.value;
  // Places: this engine's, then paired engines' (the ones added by hand and
  // the ones they suggest, like a PS5's homebrew folder; not their drives,
  // listed above), each with the engine's name under it.
  const peerRoots = new Set(peerDrives.value.map((d) => d.root));
  const short = (label: string) => placeLabel(label.replace(/ · homebrew.*$/, " · homebrew").replace(/^Internal storage$/, "Internal"));
  const places = locations.value.filter((l) => l.exists && !l.ftp && !l.peer && !space.value.some((s) => s.root === l.path));
  const theirs = locations.value.filter((l) => l.peer && l.exists && !peerRoots.has(l.path));
  const recent = (store.get<string[]>("recent") || []).filter((p) => rootOf(p) && !peerDriveOf(p) && !places.some((l) => l.path === p)).slice(0, 4);
  const engines: string[] = [];
  for (const l of theirs) if (engines.indexOf(l.peer!) < 0) engines.push(l.peer!);
  return (
    <nav class="rail scroll" id="rail" data-zone="rail" data-column>
      <h3>{t("Drives")}</h3>
      {space.value.map((s) => (
        <RailItem k={"r:" + s.root} on={within(s.root)} title={s.root} icon={/usb|ext/.test(s.root) ? "usb" : "drive"} name={driveName(s.root)} onClick={() => browse(s.root)}>
          <Meter free={s.free} total={s.total} />
          <div class="free">{t("{size} free", { size: bytes(s.free) })}</div>
        </RailItem>
      ))}
      {ftp.value.map((s) => (
        <RailItem k={"r:" + s.path} on={!!c && ftpOf(c) === s} title={s.user + "@" + s.host + ":" + s.port} icon="server" name={s.name} onClick={() => browse(s.path)}>
          <div class="free ellip">{"FTP · " + s.host + (s.port === 21 ? "" : ":" + s.port)}</div>
        </RailItem>
      ))}
      {/* Paired engines' drives (Settings > Other engines). */}
      {peerDrives.value.length ? <h3>{t("Other engines")}</h3> : null}
      {peerDrives.value.map((s) => (
        <RailItem k={"r:" + s.root} on={within(s.root)} title={s.engine} icon="server" name={driveName(s.root)} onClick={() => browse(s.root)}>
          {s.total ? <Meter free={s.free} total={s.total} /> : null}
          {s.total ? <div class="free">{t("{size} free", { size: bytes(s.free) })}</div> : null}
        </RailItem>
      ))}
      {places.length || theirs.length || recent.length ? <h3>{t("Places")}</h3> : null}
      {/* With other engines' places listed too, this engine's get its name as well. */}
      {theirs.length && (places.length || recent.length) ? <h4>{hereName()}</h4> : null}
      {places.map((l) => <RailItem k={"p:" + l.path} on={c === l.path} title={l.path} icon="place" name={short(l.label)} onClick={() => browse(l.path)} />)}
      {recent.map((p) => <RailItem k={"c:" + p} on={c === p} title={p} icon="clock" name={base(p)} onClick={() => browse(p)} />)}
      {/* Each paired engine's under its name. */}
      {engines.map((id) => {
        const list = theirs.filter((l) => l.peer === id);
        return [<h4>{list[0].engine}</h4>, ...list.map((l) => (
          <RailItem k={"p:" + l.path} on={c === l.path} title={shownPath(l.path)} icon="place" name={short(l.place || "")} onClick={() => browse(l.path)} />
        ))];
      })}
    </nav>
  );
}
