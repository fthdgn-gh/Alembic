// SPDX-License-Identifier: GPL-3.0-or-later
// The queue: jobs running or waiting, then the finished ones. Jobs that
// other engines run for this one are listed too ("on <engine>").
import { jobs, isActive, jobKey } from "../../state/jobs";
import { t } from "../../lib/i18n";
import { JobCard } from "./JobCard";
import { act } from "./jobActions";

export function QueuePage(props: { hidden: boolean }) {
  const list = jobs.value;
  const active = list.filter(isActive);
  const done = list.filter((j) => !isActive(j)).reverse();
  return (
    <section class="view scroll" id="view-queue" hidden={props.hidden}>
      <div class="page" data-zone="queue">
        <div class="head2"><div class="sect">{t("Active")}</div></div>
        <div id="active">
          {active.length ? active.map((j) => <JobCard key={jobKey(j)} j={j} />) : <div class="card empty">{t("Nothing running. Pick a package in Files and choose Convert.")}</div>}
        </div>
        <div class="head2"><div class="sect">{t("History")}</div><button class="btn sm ghost" id="btn-clear" onClick={() => act("POST", "/api/jobs/clear")}>{t("Clear history")}</button></div>
        <div id="history">
          {done.length ? done.map((j) => <JobCard key={jobKey(j)} j={j} />) : <div class="card empty">{t("No finished jobs.")}</div>}
        </div>
      </div>
    </section>
  );
}
