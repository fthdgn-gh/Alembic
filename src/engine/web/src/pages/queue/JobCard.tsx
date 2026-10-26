// SPDX-License-Identifier: GPL-3.0-or-later
// One job in the queue: what it converts, how far it is, what happened.
import { useEffect, useState } from "preact/hooks";
import type { Job, Meta } from "../../api/types";
import { getMeta, iconUrl } from "../../api/meta";
import { isActive, jobKey, rateOf } from "../../state/jobs";
import { shownPath } from "../../state/paths";
import { CONVERTIBLE, KIND_LABEL } from "../../lib/kinds";
import { base, bytes, date, duration } from "../../lib/format";
import { t, tMsg, tMethod } from "../../lib/i18n";
import { Ico } from "../../components/Icons";
import { isShared, isSharedHere } from "../files/shares";
import { jobText, cpPct, cpText } from "./jobText";
import { jobActions } from "./jobActions";

// The game's title, title ID, version and icon, as in the file list: read
// from the input, or from the output once the input is gone.
function useJobMeta(j: Job): (Meta & { from: string }) | null {
  const [m, setM] = useState<(Meta & { from: string }) | null>(null);
  const out = j.result && j.result.output ? j.result.output : null;
  const second = j.state === "done" && out && out !== j.spec.input ? out : null;
  useEffect(() => {
    let live = true;
    const tryPath = (path: string, next: string | null) => getMeta({ path, mtime: 0, size: 0, name: "", kind: "file" }).then((x) => {
      if (!live) return;
      if (x) setM(Object.assign({ from: path }, x)); else if (next) tryPath(next, null);
    });
    if (!m) tryPath(j.spec.input, second);
    return () => { live = false; };
  }, [j.spec.input, second]);
  return m;
}

export function JobCard(props: { j: Job }) {
  const j = props.j, s = j.spec, r = j.result || {}, jt = jobText(j);
  const m = useJobMeta(j);
  const pct = j.total ? Math.min(100, 100 * j.done / j.total) : 0;
  let detail = "";
  if (j.state === "running") {
    const rate = rateOf(j);
    detail = [t(j.stage), j.total ? t("{n}%", { n: pct.toFixed(1) }) : "", rate > 1 ? bytes(rate) + "/s" : "",
      rate > 1 && j.total ? t("{time} left", { time: duration((j.total - j.done) / rate) }) : "", j.elapsed != null ? t("{time} so far", { time: duration(j.elapsed) }) : ""].filter(Boolean).join("  ·  ");
  } else if (j.state === "done") {
    detail = [tMethod(r.method), r.output_bytes ? bytes(r.output_bytes) : "", j.elapsed != null ? duration(j.elapsed) : "", r.source_deleted ? t("source deleted") : ""].filter(Boolean).join("  ·  ");
  } else if (j.state === "queued") detail = cpText(j) ? t("Waiting · continues from {p}", { p: cpText(j) }) : t("Waiting for the job before it");
  else if (j.state === "paused") detail = cpText(j) ? t("Paused at {p} · resume to continue", { p: cpText(j) }) : t(j.stage || "Paused");
  else detail = j.finished ? date(j.finished) : "";
  const tags: string[] = [];
  if (j.engine) tags.push(t("on {engine}", { engine: j.engine.name }));
  if (s.kind === "convert") {
    tags.push(s.to);
    if (s.to === "ffpfsc") tags.push(t("level {n}", { n: s.level }));
    if (s.lz4) tags.push("LZ4 · " + s.lz4_profile);
    if (s.delete_source) tags.push(s.verify ? t("check, then delete source") : t("delete source"));
  }
  const notes = r.lz4 && r.lz4.profile && r.lz4.profile.notes ? r.lz4.profile.notes : [];
  const kind = s.kind === "convert" ? s.to : s.kind;
  const conv = CONVERTIBLE.indexOf(kind) >= 0;
  const sub = m ? [base(s.input), m.version ? "v" + m.version.replace(/^0+(?=\d)/, "") : "", m.title_id].filter(Boolean).join("  ·  ") : "";
  return (
    <div class="card click" tabIndex={0} data-key={"j" + jobKey(j)} onClick={() => jobActions(j)}>
      <div class="hd">
        <Ico kind={conv ? kind : "folder"} label={KIND_LABEL[kind]} icon={m && m.icon ? iconUrl({ path: m.from, mtime: 0 }) : null} />
        <div style="min-width:0;flex:1">
          <div class="ttl ellip">{m && m.title ? m.title : jt.title}</div>
          {m ? <div class="s ellip">{m.platform ? <span class={"fmt " + m.platform}>{m.platform.toUpperCase()}</span> : null}{sub}</div> : null}
          <div class="to mono ellip">{jt.to}</div>
        </div>
        <span class={"state " + j.state}>{t(j.state)}</span>
      </div>
      {j.state === "running" ? <div class={"pbar" + (j.total ? "" : " indet")}><div style={"width:" + pct + "%"} /></div>
        : cpText(j) && isActive(j) ? <div class="pbar" style="opacity:.5"><div style={"width:" + cpPct(j) + "%"} /></div> : null}
      <div class="line"><span>{detail}</span><span>{tags.map((x) => <span class="tag">{x}</span>)}</span></div>
      {r.ampr_emulator ? <div class="line">{t("AMPR emulator added: {path}", { path: shownPath(r.ampr_emulator) })}</div> : null}
      {r.source_hidden ? <div class="line">{t("Source hidden from ShadowMount+ as {name}", { name: base(r.source_hidden) })}</div> : null}
      {isShared(s.input) && isActive(j) && !isSharedHere(s.input)
        ? <div class="note">{t("Reads {name} from a computer: keep Alembic open there, or pick the file again with From this computer…", { name: base(s.input) })}</div> : null}
      {r.smp_source ? <div class="note">{t("ShadowMount+ still uses {path} for this game. Hide it (⋯ on this job) to switch to the new one.", { path: shownPath(r.smp_source) })}</div> : null}
      {r.lz4 ? <div class="line">{t("LZ4: {a} of {b} files packed", { a: r.lz4.files_packed, b: r.lz4.files_total }) + " · " + bytes(r.lz4.packed_bytes) + " → " + bytes(r.lz4.volume_bytes)}</div> : null}
      {r.source_kept ? <div class="note">{t("Source kept: {why}", { why: t(r.source_kept) })}</div> : null}
      {notes.length ? <div class="note">{notes.map(tMsg).join(" ")}</div> : null}
      {r.warnings && r.warnings.length ? <div class="note">{r.warnings.map(tMsg).join(" ")}</div> : null}
      {j.error && j.state !== "cancelled" ? <div class="err">{tMsg(j.error)}</div> : null}
    </div>
  );
}
