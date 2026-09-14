/**
 * Gear detail (SPEC §10 view 3): everything one short address knows about itself, and every
 * parameter that can be written back to it.
 */

import { Fragment } from 'preact';
import { useEffect, useState } from 'preact/hooks';

import {
    api,
    type ConfigureBody,
    errorText,
    type Gear,
    type GearConfig,
    type QueryBody,
    type Result,
} from '../api.ts';
import {
    addrLabel,
    asNumber,
    asRecord,
    gearPct,
    gearTrouble,
    GROUP_COUNT,
    groupLabel,
    hexByte,
    MAX_ADDR,
    QUERIES,
    SCENE_COUNT,
    statusList,
    timeAgo,
} from '../dali.ts';
import { describeWrite, useLevel } from '../level.ts';
import { href, navigate } from '../router.ts';
import { loadGear, useStore } from '../store.ts';
import {
    ConfirmButton,
    Notice,
    NumberField,
    RenameField,
    Segmented,
    SelectField,
    Slider,
    toneClass,
    TypeToConfirm,
} from '../ui.tsx';

export function GearDetail({ addr }: { addr: number }) {
    const { gears, bus } = useStore();
    const gear = gears.find((candidate) => candidate.addr === addr);
    const [loadError, setLoadError] = useState<string | null>(null);

    // The compact list has no config and no identity; the detail screen is the whole point of the
    // full entry, so ask for it on arrival and on every address change.
    useEffect(() => {
        setLoadError(null);
        loadGear(addr).catch((error: unknown) => {
            setLoadError(errorText(error));
        });
    }, [addr]);

    if (!Number.isInteger(addr) || addr < 0 || addr > MAX_ADDR) {
        return (
            <Notice tone="error" title="No such address">
                <p>
                    Short addresses run from 0 to {MAX_ADDR}. <a href={href('dashboard')}>Go back</a>
                    .
                </p>
            </Notice>
        );
    }

    if (gear === undefined) {
        return (
            <div class="stack">
                <a class="backlink" href={href('dashboard')}>
                    All gear
                </a>
                {loadError === null ? (
                    <section class="panel panel--rail is-busy">
                        <p class="muted">Reading {addrLabel(addr)}…</p>
                    </section>
                ) : (
                    <Notice tone="error" title={`${addrLabel(addr)} did not load`}>
                        <p class="mono">{loadError}</p>
                    </Notice>
                )}
            </div>
        );
    }

    const usable = bus?.powered !== false;

    return (
        <div class="stack">
            <a class="backlink" href={href('dashboard')}>
                All gear
            </a>
            <Live gear={gear} usable={usable} />
            <Configuration gear={gear} />
            <Scenes gear={gear} />
            <Membership gear={gear} />
            <Identity gear={gear} />
            <Addressing gear={gear} gears={gears} />
            <QueryBox addr={gear.addr} />
        </div>
    );
}

/* ------------------------------------------------------------ live values */

function Live({ gear, usable }: { gear: Gear; usable: boolean }) {
    const level = useLevel((pct) => api.setGear(gear.addr, { level_pct: pct }), gearPct(gear));
    const [busy, setBusy] = useState<'name' | 'identify' | null>(null);
    const [failure, setFailure] = useState<string | null>(null);
    const [identified, setIdentified] = useState(false);

    const trouble = gearTrouble(gear);
    const flags = statusList(gear.status);
    const locked = !gear.present || !usable;

    const rename = (name: string): void => {
        setBusy('name');
        setFailure(null);
        api
            .renameGear(gear.addr, name)
            .then(() => loadGear(gear.addr))
            .catch((error: unknown) => {
                setFailure(errorText(error));
            })
            .finally(() => {
                setBusy(null);
            });
    };

    const identify = (): void => {
        setBusy('identify');
        setFailure(null);
        setIdentified(false);
        api
            .identify(gear.addr)
            .then(() => {
                setIdentified(true);
            })
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setBusy(null);
            });
    };

    return (
        <section
            class={`panel panel--rail section ${trouble === null ? 'is-ok' : toneClass(trouble.tone)}`}
        >
            <div class="section__head">
                <h2>
                    <span class="mono gear__addr">{addrLabel(gear.addr)}</span>{' '}
                    {gear.name === '' ? `Gear ${gear.addr}` : gear.name}
                </h2>
                <span class={`badge ${trouble === null ? 'is-ok' : toneClass(trouble.tone)}`}>
                    <span class="dot" />
                    {trouble?.label ?? 'On the bus'}
                </span>
            </div>

            {!gear.present && (
                <p class="muted">
                    This address did not answer the last scan ({timeAgo(gear.last_seen)}). The name
                    and the values below are the last ones the gateway saw.
                </p>
            )}

            <Slider
                label="Brightness"
                pct={level.pct}
                disabled={locked}
                onInput={level.input}
                onCommit={level.commit}
            />

            <div class="row">
                <Segmented
                    label="Power"
                    value={gear.on ? 'on' : 'off'}
                    disabled={locked}
                    options={[
                        { value: 'on', label: 'On' },
                        { value: 'off', label: 'Off' },
                    ]}
                    onSelect={(value) => {
                        level.release();
                        setFailure(null);
                        api.setGear(gear.addr, { on: value === 'on' }).catch((error: unknown) => {
                            setFailure(describeWrite(error));
                        });
                    }}
                />
                <button
                    type="button"
                    class="btn btn--secondary"
                    disabled={locked || busy === 'identify'}
                    onClick={identify}
                >
                    {busy === 'identify' ? 'Blinking' : 'Make it blink'}
                </button>
            </div>

            {identified && (
                <p class="muted">Watch the room — this fitting is identifying itself now.</p>
            )}
            {(level.error ?? failure) !== null && (
                <p class="gear__error mono" role="alert">
                    {level.error ?? failure}
                </p>
            )}

            <dl class="data-list gear__data">
                <dt>Level</dt>
                <dd class="mono">
                    {gear.level} of 254 · {gearPct(gear)} %
                </dd>
                <dt>Status byte</dt>
                <dd class="mono">0x{hexByte(gear.status.raw)}</dd>
                <dt>Reported</dt>
                <dd>{flags.length === 0 ? 'nothing set' : flags.join(', ')}</dd>
                <dt>Device type</dt>
                <dd class="mono">
                    {gear.device_types === undefined || gear.device_types.length === 0
                        ? '—'
                        : gear.device_types.map((type) => `DT${type}`).join(', ')}
                </dd>
                <dt>DALI version</dt>
                <dd class="mono">{gear.version ?? '—'}</dd>
                <dt>Last seen</dt>
                <dd>{timeAgo(gear.last_seen)}</dd>
            </dl>

            <RenameField
                label="Name"
                value={gear.name}
                saving={busy === 'name'}
                onSave={rename}
            />
        </section>
    );
}

/* ---------------------------------------------------------- configuration */

/** The parameters `configure` writes one DTR store at a time, each read back and verified. */
const SCALARS = [
    { key: 'min', label: 'Minimum level', min: 0, max: 254, hint: 'Never dims below this.' },
    { key: 'max', label: 'Maximum level', min: 0, max: 254, hint: 'Never brightens above this.' },
    {
        key: 'power_on',
        label: 'Level after power-on',
        min: 0,
        max: 255,
        hint: '255 keeps the level it had before the outage.',
    },
    {
        key: 'system_failure',
        label: 'Level on bus failure',
        min: 0,
        max: 255,
        hint: 'Where the fitting goes when the bus dies. 255 means stay put.',
    },
    { key: 'fade_time', label: 'Fade time', min: 0, max: 15, hint: '0 is an instant jump.' },
    { key: 'fade_rate', label: 'Fade rate', min: 1, max: 15, hint: 'Steps per second for dim up and down.' },
] as const;

function Configuration({ gear }: { gear: Gear }) {
    const base = gear.config;
    const [draft, setDraft] = useState<GearConfig | null>(base ?? null);
    const [seen, setSeen] = useState(base);
    const [saving, setSaving] = useState(false);
    const [report, setReport] = useState<Result | null>(null);
    const [failure, setFailure] = useState<string | null>(null);

    // The device re-sent its config: the form rebases on it rather than holding a stale draft.
    if (seen !== base) {
        setSeen(base);
        setDraft(base ?? null);
    }

    if (base === undefined || draft === null) {
        return (
            <section class="panel section">
                <h2>Configuration</h2>
                <p class="muted">
                    The gateway has not read this fitting&apos;s parameters yet. A deep scan reads
                    them for the whole bus.
                </p>
            </section>
        );
    }

    const changed = SCALARS.filter((entry) => draft[entry.key] !== base[entry.key]);

    const save = (): void => {
        const body: ConfigureBody = {};
        for (const entry of changed) body[entry.key] = draft[entry.key];
        setSaving(true);
        setFailure(null);
        setReport(null);
        api
            .configure(gear.addr, body)
            .then((result) => {
                setReport(result);
                return loadGear(gear.addr);
            })
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setSaving(false);
            });
    };

    return (
        <section class="panel section">
            <h2>Configuration</h2>
            <p class="muted section__lede">
                Levels are the DALI number, 0 to 254, not a percentage — the same scale the fitting
                stores. Physical minimum for this fitting is {base.physical_min}.
            </p>
            <div class="field-grid">
                {SCALARS.map((entry) => (
                    <NumberField
                        key={entry.key}
                        label={entry.label}
                        hint={entry.hint}
                        value={draft[entry.key]}
                        min={entry.min}
                        max={entry.max}
                        disabled={saving}
                        onInput={(value) => {
                            setDraft({ ...draft, [entry.key]: clamp(value, entry.min, entry.max) });
                        }}
                    />
                ))}
            </div>
            <WriteBar
                count={changed.length}
                saving={saving}
                onSave={save}
                onReset={() => {
                    setDraft(base);
                }}
            />
            <ConfigureReport result={report} failure={failure} />
        </section>
    );
}

function clamp(value: number, min: number, max: number): number {
    return Math.min(Math.max(Math.round(value), min), max);
}

function WriteBar({
    count,
    saving,
    onSave,
    onReset,
    label,
}: {
    count: number;
    saving: boolean;
    onSave: () => void;
    onReset: () => void;
    label?: string;
}) {
    if (count === 0) return null;
    return (
        <div class="writebar">
            <p class="savebar__text">
                {count} {label ?? (count === 1 ? 'change' : 'changes')} not written to the fitting.
            </p>
            <button type="button" class="btn btn--primary" disabled={saving} onClick={onSave}>
                {saving ? 'Writing' : 'Write to fitting'}
            </button>
            <button type="button" class="btn btn--ghost" disabled={saving} onClick={onReset}>
                Discard
            </button>
        </div>
    );
}

/**
 * `configure` verifies every parameter by reading it back (SPEC §7.4) and reports each one. The
 * exact shape of that report is open, so anything object-like is rendered as it arrives.
 */
function ConfigureReport({ result, failure }: { result: Result | null; failure: string | null }) {
    if (failure !== null) {
        return (
            <Notice tone="error" title="The write did not land">
                <p class="mono">{failure}</p>
            </Notice>
        );
    }
    if (result === null) return null;
    const data = asRecord(result.data);
    const rows = data === null ? [] : Object.entries(data);
    if (!result.ok) {
        return (
            <Notice tone="error" title="The fitting refused">
                <p class="mono">{result.message ?? (result.error ?? 'unknown error')}</p>
            </Notice>
        );
    }
    return (
        <Notice tone="ok" title="Written and read back">
            {rows.length > 0 && (
                <dl class="data-list">
                    {rows.map(([key, value]) => (
                        <Fragment key={key}>
                            <dt class="mono">{key}</dt>
                            <dd class="mono">{String(value)}</dd>
                        </Fragment>
                    ))}
                </dl>
            )}
        </Notice>
    );
}

/* ----------------------------------------------------------------- scenes */

function Scenes({ gear }: { gear: Gear }) {
    const base = gear.config?.scenes;
    const [draft, setDraft] = useState<(number | null)[] | null>(base ?? null);
    const [seen, setSeen] = useState(base);
    const [saving, setSaving] = useState(false);
    const [failure, setFailure] = useState<string | null>(null);

    if (seen !== base) {
        setSeen(base);
        setDraft(base ?? null);
    }

    if (base === undefined || draft === null) {
        return (
            <section class="panel section">
                <h2>Scenes</h2>
                <p class="muted">Scene levels are read by a deep scan.</p>
            </section>
        );
    }

    const changed = draft
        .map((value, index) => ({ index, value }))
        .filter(({ index, value }) => value !== (base[index] ?? null));

    const save = (): void => {
        const scene: Record<string, number | null> = {};
        for (const { index, value } of changed) scene[String(index)] = value;
        setSaving(true);
        setFailure(null);
        api
            .configure(gear.addr, { scene })
            .then(() => loadGear(gear.addr))
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setSaving(false);
            });
    };

    return (
        <section class="panel section">
            <h2>Scenes</h2>
            <p class="muted section__lede">
                A scene is either a stored level or not programmed at all, and the two are different
                things on the bus: not programmed means the fitting ignores the scene call, while
                level 0 means it switches off. Clearing a scene writes 255 — the not-programmed mark.
            </p>
            <div class="scenes">
                {Array.from({ length: SCENE_COUNT }, (_, index) => (
                    <SceneRow
                        key={index}
                        index={index}
                        value={draft[index] ?? null}
                        changed={(draft[index] ?? null) !== (base[index] ?? null)}
                        disabled={saving}
                        onChange={(value) => {
                            const next = [...draft];
                            next[index] = value;
                            setDraft(next);
                        }}
                    />
                ))}
            </div>
            <WriteBar
                count={changed.length}
                saving={saving}
                label={changed.length === 1 ? 'scene' : 'scenes'}
                onSave={save}
                onReset={() => {
                    setDraft(base);
                }}
            />
            {failure !== null && (
                <Notice tone="error" title="The scenes did not write">
                    <p class="mono">{failure}</p>
                </Notice>
            )}
        </section>
    );
}

function SceneRow({
    index,
    value,
    changed,
    disabled,
    onChange,
}: {
    index: number;
    value: number | null;
    changed: boolean;
    disabled: boolean;
    onChange: (value: number | null) => void;
}) {
    return (
        <div class={changed ? 'scene is-changed' : 'scene'}>
            <span class="scene__id mono">S{index}</span>
            {value === null ? (
                <>
                    <span class="muted scene__empty">Not programmed</span>
                    <button
                        type="button"
                        class="btn btn--secondary"
                        disabled={disabled}
                        onClick={() => {
                            onChange(254);
                        }}
                    >
                        Set a level
                    </button>
                </>
            ) : (
                <>
                    <input
                        class="mono scene__level"
                        type="number"
                        inputMode="numeric"
                        min={0}
                        max={254}
                        value={value}
                        disabled={disabled}
                        aria-label={`Scene ${index} level`}
                        onInput={(event) => {
                            const parsed = Number(event.currentTarget.value);
                            if (!Number.isNaN(parsed)) onChange(clamp(parsed, 0, 254));
                        }}
                    />
                    <button
                        type="button"
                        class="btn btn--ghost"
                        disabled={disabled}
                        onClick={() => {
                            onChange(null);
                        }}
                    >
                        Clear
                    </button>
                </>
            )}
        </div>
    );
}

/* ----------------------------------------------------------------- groups */

function Membership({ gear }: { gear: Gear }) {
    const base = gear.config?.groups;
    const [draft, setDraft] = useState<number[] | null>(base ?? null);
    const [seen, setSeen] = useState(base);
    const [saving, setSaving] = useState(false);
    const [failure, setFailure] = useState<string | null>(null);

    if (seen !== base) {
        setSeen(base);
        setDraft(base ?? null);
    }

    if (base === undefined || draft === null) {
        return (
            <section class="panel section">
                <h2>Groups</h2>
                <p class="muted">Group membership is read by a deep scan.</p>
            </section>
        );
    }

    const add = draft.filter((group) => !base.includes(group));
    const remove = base.filter((group) => !draft.includes(group));

    const save = (): void => {
        setSaving(true);
        setFailure(null);
        api
            .configure(gear.addr, { group: { add, remove } })
            .then(() => loadGear(gear.addr))
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setSaving(false);
            });
    };

    return (
        <section class="panel section">
            <h2>Groups</h2>
            <p class="muted section__lede">
                A fitting answers every group it belongs to, and can belong to all sixteen.
            </p>
            <div class="groups">
                {Array.from({ length: GROUP_COUNT }, (_, group) => group).map((group) => {
                    const on = draft.includes(group);
                    return (
                        <label key={group} class={on ? 'groupbox is-on' : 'groupbox'}>
                            <input
                                type="checkbox"
                                checked={on}
                                disabled={saving}
                                onChange={() => {
                                    setDraft(
                                        on
                                            ? draft.filter((value) => value !== group)
                                            : [...draft, group].sort((a, b) => a - b),
                                    );
                                }}
                            />
                            <span class="mono">{groupLabel(group)}</span>
                        </label>
                    );
                })}
            </div>
            <WriteBar
                count={add.length + remove.length}
                saving={saving}
                label={add.length + remove.length === 1 ? 'group change' : 'group changes'}
                onSave={save}
                onReset={() => {
                    setDraft(base);
                }}
            />
            {failure !== null && (
                <Notice tone="error" title="The groups did not write">
                    <p class="mono">{failure}</p>
                </Notice>
            )}
        </section>
    );
}

/* --------------------------------------------------------------- identity */

function Identity({ gear }: { gear: Gear }) {
    const identity = gear.identity;
    return (
        <section class="panel section">
            <h2>Identity</h2>
            {identity === undefined ? (
                <p class="muted">
                    The manufacturer data in memory bank 0 is read by a deep scan. Run one from bus
                    tools to fill this in.
                </p>
            ) : (
                <dl class="data-list">
                    <dt>GTIN</dt>
                    <dd class="mono">{identity.gtin ?? '—'}</dd>
                    <dt>Serial number</dt>
                    <dd class="mono">{identity.serial ?? '—'}</dd>
                    <dt>Bank 0 version</dt>
                    <dd class="mono">{identity.bank0_version ?? '—'}</dd>
                </dl>
            )}
        </section>
    );
}

/* ------------------------------------------------------------- addressing */

function Addressing({ gear, gears }: { gear: Gear; gears: Gear[] }) {
    const [target, setTarget] = useState(gear.addr);
    const [busy, setBusy] = useState(false);
    const [failure, setFailure] = useState<string | null>(null);

    const occupant = gears.find(
        (candidate) => candidate.addr === target && candidate.present && candidate.addr !== gear.addr,
    );
    const same = target === gear.addr;

    const move = (): void => {
        setBusy(true);
        setFailure(null);
        api
            .setAddress(gear.addr, target)
            .then(() => {
                navigate('gear', target);
                return loadGear(target);
            })
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setBusy(false);
            });
    };

    return (
        <section class="panel panel--rail section is-warn">
            <h2>Short address</h2>
            <p class="muted section__lede">
                The short address is how every command, group and scene on this bus reaches this
                fitting. Moving it does not move anything that refers to it: a wall switch, a
                controller or a Home Assistant entity pointed at {addrLabel(gear.addr)} will be
                pointing at whatever answers there next.
            </p>

            <NumberField
                label="Move to address"
                value={target}
                min={0}
                max={MAX_ADDR}
                disabled={busy}
                hint={`Currently ${addrLabel(gear.addr)}. Addresses run 0 to ${MAX_ADDR}.`}
                onInput={(value) => {
                    setTarget(clamp(value, 0, MAX_ADDR));
                }}
            />

            {occupant !== undefined && (
                <Notice tone="error" title={`${addrLabel(target)} is taken`}>
                    <p>
                        {occupant.name === '' ? `Gear ${occupant.addr}` : occupant.name} answers on{' '}
                        {addrLabel(target)} right now. The gateway refuses a move onto a live
                        address — give that fitting another address first.
                    </p>
                </Notice>
            )}

            {!same && occupant === undefined && (
                <TypeToConfirm
                    word={addrLabel(target)}
                    label={`Move ${addrLabel(gear.addr)} to ${addrLabel(target)}`}
                    question={`${gear.name === '' ? `Gear ${gear.addr}` : gear.name} will stop answering on ${addrLabel(gear.addr)} and start answering on ${addrLabel(target)}. Anything that addresses it by number has to be repointed by hand.`}
                    confirmLabel={`Move to ${addrLabel(target)}`}
                    cancelLabel="Keep the current address"
                    onConfirm={move}
                />
            )}

            <hr />

            <ConfirmButton
                danger
                label="Remove the short address"
                question={`${addrLabel(gear.addr)} loses its address entirely. The fitting stays on the bus but only broadcast commands reach it, and it takes a commissioning run to give it an address again.`}
                confirmLabel="Remove the address"
                disabled={busy}
                onConfirm={() => {
                    setBusy(true);
                    setFailure(null);
                    api
                        .removeAddress(gear.addr)
                        .catch((error: unknown) => {
                            setFailure(describeWrite(error));
                        })
                        .finally(() => {
                            setBusy(false);
                        });
                }}
            />

            {failure !== null && (
                <Notice tone="error" title="The address did not change">
                    <p class="mono">{failure}</p>
                </Notice>
            )}
        </section>
    );
}

/* ------------------------------------------------------------- query box */

function QueryBox({ addr }: { addr: number }) {
    const [mode, setMode] = useState<'named' | 'opcode'>('named');
    const [query, setQuery] = useState(QUERIES[0]?.value ?? 'actual_level');
    const [opcode, setOpcode] = useState(160);
    const [busy, setBusy] = useState(false);
    const [reply, setReply] = useState<Result | null>(null);
    const [failure, setFailure] = useState<string | null>(null);

    const send = (): void => {
        const body: QueryBody = mode === 'named' ? { addr, query } : { addr, opcode };
        setBusy(true);
        setFailure(null);
        setReply(null);
        api
            .query(body)
            .then(setReply)
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setBusy(false);
            });
    };

    const value = asNumber(asRecord(reply?.data)?.['reply']);
    const frame = asRecord(reply?.data)?.['raw_frame'];

    return (
        <section class="panel section">
            <h2>Ask this fitting something</h2>
            <p class="muted section__lede">
                One query, sent to {addrLabel(addr)} only. The answer is a single byte, or nothing
                at all when the fitting stays quiet.
            </p>

            <Segmented
                label="Query by"
                value={mode}
                options={[
                    { value: 'named', label: 'Named query' },
                    { value: 'opcode', label: 'Opcode' },
                ]}
                onSelect={setMode}
            />

            <div class="field-grid query__form">
                {mode === 'named' ? (
                    <SelectField
                        label="Query"
                        value={query}
                        options={QUERIES}
                        disabled={busy}
                        onInput={setQuery}
                    />
                ) : (
                    <NumberField
                        label="Opcode"
                        value={opcode}
                        min={0}
                        max={255}
                        disabled={busy}
                        hint="The second byte of the forward frame, 0 to 255."
                        onInput={(next) => {
                            setOpcode(clamp(next, 0, 255));
                        }}
                    />
                )}
            </div>

            <button type="button" class="btn btn--primary" disabled={busy} onClick={send}>
                {busy ? 'Asking' : 'Send query'}
            </button>

            {failure !== null && (
                <Notice tone="error" title="No answer">
                    <p class="mono">{failure}</p>
                </Notice>
            )}

            {reply !== null && (
                <dl class="data-list query__reply">
                    <dt>Reply</dt>
                    <dd class="mono">
                        {value === null ? 'no reply' : `${value} · 0x${hexByte(value)}`}
                    </dd>
                    {typeof frame === 'string' && (
                        <>
                            <dt>Frame</dt>
                            <dd class="mono">{frame}</dd>
                        </>
                    )}
                </dl>
            )}
        </section>
    );
}
