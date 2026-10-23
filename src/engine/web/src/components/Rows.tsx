// SPDX-License-Identifier: GPL-3.0-or-later
// Form rows: a value changed with ← and →, a switch, label/value facts.
import type { ComponentChildren } from "preact";
import { useEffect, useRef } from "preact/hooks";
import { steppers } from "../input/focus";

export interface Option<T> { value: T; label: string }

// A row whose value changes with ← and → (and a click on its arrows or the row).
export function ChoiceRow<T>(props: { label: string; hint?: string | null; options: Option<T>[]; value: T; onChange: (v: T) => void; hidden?: boolean; dataKey?: string }) {
  const { options, value, onChange } = props;
  const i = Math.max(0, options.findIndex((o) => o.value === value));
  const ref = useRef<HTMLDivElement>(null);
  const step = (d: number) => { const n = i + d; if (n < 0 || n >= options.length) return; onChange(options[n].value); };
  useEffect(() => { if (ref.current) steppers.set(ref.current, step); });
  return (
    <div ref={ref} class="frow fx" tabIndex={0} hidden={props.hidden} data-key={props.dataKey} onClick={() => step(i === options.length - 1 ? -(options.length - 1) : 1)}>
      <div class="fl"><b>{props.label}</b>{props.hint ? <small>{props.hint}</small> : null}</div>
      <div class="val">
        <span class="arr" onClick={(e) => { e.stopPropagation(); step(-1); }}>‹</span>
        <span class="valtext">{options[i] ? options[i].label : ""}</span>
        <span class="arr" onClick={(e) => { e.stopPropagation(); step(1); }}>›</span>
      </div>
    </div>
  );
}

export function SwitchRow(props: { label: string; hint?: string | null; on: boolean; onChange: (on: boolean) => void; hidden?: boolean; dataKey?: string }) {
  return (
    <button class={"frow" + (props.on ? " on" : "")} role="switch" aria-checked={!!props.on} hidden={props.hidden} data-key={props.dataKey} onClick={() => props.onChange(!props.on)}>
      <div class="fl"><b>{props.label}</b>{props.hint ? <small>{props.hint}</small> : null}</div>
      <span class="sw" />
    </button>
  );
}

// Two columns of label and value.
export function Facts(props: { rows: [string, ComponentChildren, string?][] }) {
  return <div class="facts">{props.rows.map(([k, v, cls]) => [<div>{k}</div>, <div class={cls || undefined}>{v}</div>])}</div>;
}

