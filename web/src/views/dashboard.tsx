/**
 * Dashboard (SPEC §10 view 2): the bus at a glance and every fitting within one thumb's reach.
 */

import { useEffect, useState } from 'preact/hooks';

import { api, type Gear } from '../api.ts';
import { addrLabel, gearPct, gearTrouble, groupLabel, GROUP_COUNT, timeAgo } from '../dali.ts';
import { describeWrite, useLevel } from '../level.ts';
import { href } from '../router.ts';
import { loadMembership, useStore } from '../store.ts';
import { Notice, Segmented, Slider, toneClass } from '../ui.tsx';
import { BusBanner } from './bus-status.tsx';

type Mode = 'all' | 'groups';

export function Dashboard({ groupNames }: { groupNames: Record<string, { name?: string }> }) {
    const { gears, bus, loaded, loadError } = useStore();
    const [mode, setMode] = useState<Mode>('all');
    const [reading, setReading] = useState(false);
    const [broadcast, setBroadcast] = useState<string | null>(null);
    const [sending, setSending] = useState(false);

    // Group membership lives in each fitting's config, which the compact list does not carry, so
    // it is read the first time the user actually asks to see groups.
    const missing = gears.some((gear) => gear.present && gear.config === undefined);
    useEffect(() => {
        if (mode !== 'groups' || !missing || reading) return;
        setReading(true);
        void loadMembership().finally(() => {
            setReading(false);
        });
    }, [mode, missing, reading]);

    const all = (on: boolean): void => {
        setSending(true);
        setBroadcast(null);
        api
            .setBroadcast({ on })
            .catch((error: unknown) => {
                setBroadcast(describeWrite(error));
            })
            .finally(() => {
                setSending(false);
            });
    };

    const usable = bus?.powered !== false;

    return (
        <div class="stack">
            <BusBanner />

            {loadError !== null && (
                <Notice tone="error" title="The gear list did not load">
                    <p class="mono">{loadError}</p>
                </Notice>
            )}

            <section class="panel">
                <div class="toolbar">
                    <Segmented
                        label="Arrange gear"
                        value={mode}
                        options={[
                            { value: 'all', label: 'All gear' },
                            { value: 'groups', label: 'By group' },
                        ]}
                        onSelect={setMode}
                    />
                    <div class="row">
                        <button
                            type="button"
                            class="btn btn--secondary"
                            disabled={sending || !usable}
                            onClick={() => {
                                all(true);
                            }}
                        >
                            All on
                        </button>
                        <button
                            type="button"
                            class="btn btn--secondary"
                            disabled={sending || !usable}
                            onClick={() => {
                                all(false);
                            }}
                        >
                            All off
                        </button>
                    </div>
                </div>
                {broadcast !== null && (
                    <p class="gear__error mono" role="alert">
                        {broadcast}
                    </p>
                )}
            </section>

            {loaded && gears.length === 0 && (
                <section class="panel panel--rail">
                    <h2>No control gear yet</h2>
                    <p class="muted">
                        Nothing has answered on this bus. Run a scan to find what is out there.
                    </p>
                    <a class="btn btn--primary" href={href('bus')}>
                        Go to bus tools
                    </a>
                </section>
            )}

            {mode === 'all' && gears.length > 0 && (
                <div class="gears">
                    {gears.map((gear) => (
                        <GearCard key={gear.addr} gear={gear} usable={usable} />
                    ))}
                </div>
            )}

            {mode === 'groups' && gears.length > 0 && (
                <Groups gears={gears} names={groupNames} reading={reading} usable={usable} />
            )}
        </div>
    );
}

/* ------------------------------------------------------------------- card */

function GearCard({ gear, usable }: { gear: Gear; usable: boolean }) {
    const level = useLevel((pct) => api.setGear(gear.addr, { level_pct: pct }), gearPct(gear));
    const [switching, setSwitching] = useState(false);
    const [failure, setFailure] = useState<string | null>(null);

    const trouble = gearTrouble(gear);
    const locked = !gear.present || !usable;

    const power = (on: boolean): void => {
        level.release();
        setSwitching(true);
        setFailure(null);
        api
            .setGear(gear.addr, { on })
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setSwitching(false);
            });
    };

    const name = gear.name === '' ? `Gear ${gear.addr}` : gear.name;

    return (
        <article
            class={`panel panel--rail gear ${trouble === null ? '' : toneClass(trouble.tone)} ${
                gear.present ? '' : 'gear--absent'
            }`}
        >
            <div class="gear__head">
                <a class="gear__name" href={href('gear', gear.addr)}>
                    {name}
                </a>
                <span class="gear__addr mono">{addrLabel(gear.addr)}</span>
            </div>

            <Slider
                label={`${name} brightness`}
                pct={level.pct}
                disabled={locked}
                onInput={level.input}
                onCommit={level.commit}
            />

            <div class="gear__foot">
                <Segmented
                    label={`${name} power`}
                    value={gear.on ? 'on' : 'off'}
                    disabled={locked || switching}
                    options={[
                        { value: 'on', label: 'On' },
                        { value: 'off', label: 'Off' },
                    ]}
                    onSelect={(value) => {
                        power(value === 'on');
                    }}
                />
                {trouble !== null && (
                    <span class={`badge ${toneClass(trouble.tone)}`}>
                        <span class="dot" />
                        {trouble.label}
                    </span>
                )}
            </div>

            {!gear.present && (
                <p class="muted gear__note">
                    Nothing answered here on the last scan. The name and the level are the last ones
                    the gateway saw
                    {gear.last_seen === undefined ? '' : `, ${timeAgo(gear.last_seen)}`}.
                </p>
            )}

            {(level.error ?? failure) !== null && (
                <p class="gear__error mono" role="alert">
                    {level.error ?? failure}
                </p>
            )}
        </article>
    );
}

/* ----------------------------------------------------------------- groups */

function Groups({
    gears,
    names,
    reading,
    usable,
}: {
    gears: Gear[];
    names: Record<string, { name?: string }>;
    reading: boolean;
    usable: boolean;
}) {
    const known = gears.filter((gear) => gear.config !== undefined);
    const ungrouped = known.filter((gear) => (gear.config?.groups.length ?? 0) === 0);
    const unknown = gears.filter((gear) => gear.present && gear.config === undefined);

    return (
        <div class="stack">
            {reading && (
                <Notice tone="busy" title="Reading group membership">
                    <p>
                        Which groups a fitting belongs to is stored in the fitting. Reading it once
                        per address; the list fills in as answers arrive.
                    </p>
                </Notice>
            )}

            {Array.from({ length: GROUP_COUNT }, (_, group) => group)
                .map((group) => ({
                    group,
                    members: known.filter((gear) => gear.config?.groups.includes(group) === true),
                }))
                .filter(({ members }) => members.length > 0)
                .map(({ group, members }) => (
                    <GroupSection
                        key={group}
                        group={group}
                        name={names[String(group)]?.name ?? ''}
                        members={members}
                        usable={usable}
                    />
                ))}

            {ungrouped.length > 0 && (
                <details class="panel group">
                    <summary class="group__summary">
                        <span class="group__name">In no group</span>
                        <span class="muted group__count">
                            {ungrouped.length} fitting{ungrouped.length === 1 ? '' : 's'}
                        </span>
                    </summary>
                    <div class="group__body">
                        <div class="gears">
                            {ungrouped.map((gear) => (
                                <GearCard key={gear.addr} gear={gear} usable={usable} />
                            ))}
                        </div>
                    </div>
                </details>
            )}

            {!reading && unknown.length > 0 && (
                <Notice tone="warn" title="Some fittings could not be read">
                    <p>
                        {unknown.length} address(es) did not return their group membership, so they
                        are not listed above. They are all in the All gear list.
                    </p>
                </Notice>
            )}
        </div>
    );
}

function GroupSection({
    group,
    name,
    members,
    usable,
}: {
    group: number;
    name: string;
    members: Gear[];
    usable: boolean;
}) {
    // A group has no level of its own to read back: DALI broadcasts to a group and each member
    // answers for itself. The slider starts from the brightest member, which is what an installer
    // sees in the room.
    const brightest = members.reduce((top, gear) => Math.max(top, gearPct(gear)), 0);
    const level = useLevel((pct) => api.setGroup(group, { level_pct: pct }), brightest);
    const [failure, setFailure] = useState<string | null>(null);

    const power = (on: boolean): void => {
        level.release();
        setFailure(null);
        api.setGroup(group, { on }).catch((error: unknown) => {
            setFailure(describeWrite(error));
        });
    };

    const label = name === '' ? `Group ${group}` : name;

    return (
        <details class="panel group" open>
            <summary class="group__summary">
                <span class="mono group__id">{groupLabel(group)}</span>
                <span class="group__name">{label}</span>
                <span class="muted group__count">
                    {members.length} fitting{members.length === 1 ? '' : 's'}
                </span>
            </summary>
            <div class="group__body">
                <div class="group__controls">
                    <Slider
                        label={`${label} brightness`}
                        pct={level.pct}
                        disabled={!usable}
                        onInput={level.input}
                        onCommit={level.commit}
                    />
                    <Segmented
                        label={`${label} power`}
                        value={members.some((gear) => gear.on) ? 'on' : 'off'}
                        disabled={!usable}
                        options={[
                            { value: 'on', label: 'On' },
                            { value: 'off', label: 'Off' },
                        ]}
                        onSelect={(value) => {
                            power(value === 'on');
                        }}
                    />
                </div>
                {(level.error ?? failure) !== null && (
                    <p class="gear__error mono" role="alert">
                        {level.error ?? failure}
                    </p>
                )}
                <div class="gears">
                    {members.map((gear) => (
                        <GearCard key={gear.addr} gear={gear} usable={usable} />
                    ))}
                </div>
            </div>
        </details>
    );
}
