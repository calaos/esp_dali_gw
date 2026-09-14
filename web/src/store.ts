/**
 * The live picture of the bus: one copy, fed by one event stream, read by every screen.
 *
 * A module-level store rather than context because the stream is a process-wide resource (SPEC §9
 * caps the gateway at three listeners) and because the dashboard, the gear screen and the bus tools
 * all show the same bus — two components must never be able to disagree about whether it is busy.
 */

import { useEffect, useState } from 'preact/hooks';

import {
    api,
    type BusState,
    errorText,
    type Gear,
    type Progress,
    type Result,
} from './api.ts';
import { onEvent, onStateChange, type StreamState, streamState } from './events.ts';

export interface LogLine {
    /** Monotonic, so a list of identical messages still has stable keys. */
    seq: number;
    level: string;
    msg: string;
}

export interface Snapshot {
    bus: BusState | null;
    /** Sorted by short address, which is the order an installer walks the bus in. */
    gears: Gear[];
    progress: Progress | null;
    /** The last `result` event, plus a sequence number so a view can react to a *new* one. */
    result: Result | null;
    resultSeq: number;
    log: LogLine[];
    loaded: boolean;
    loadError: string | null;
    stream: StreamState;
}

const LOG_LIMIT = 40;

let snapshot: Snapshot = {
    bus: null,
    gears: [],
    progress: null,
    result: null,
    resultSeq: 0,
    log: [],
    loaded: false,
    loadError: null,
    stream: streamState(),
};

const listeners = new Set<() => void>();
let seq = 0;

function commit(patch: Partial<Snapshot>): void {
    snapshot = { ...snapshot, ...patch };
    for (const listener of listeners) listener();
}

function byAddr(gears: Gear[]): Gear[] {
    return [...gears].sort((a, b) => a.addr - b.addr);
}

/**
 * Merge rather than replace: a `gear` event carries the compact entry, and dropping the `config`
 * and `identity` a deep scan already read would blank the detail screen on every level change.
 */
function mergeGear(incoming: Gear): void {
    const existing = snapshot.gears.find((gear) => gear.addr === incoming.addr);
    const merged: Gear = existing === undefined ? incoming : { ...existing, ...incoming };
    // `level_pct` is derived from `level`, and a compact event carries the new level without it.
    // Left in place by the spread, the old percentage would outlive the level it came from and the
    // slider would sit at the previous value for ever.
    if (existing !== undefined && incoming.level_pct === undefined) delete merged.level_pct;
    const gears =
        existing === undefined
            ? byAddr([...snapshot.gears, merged])
            : snapshot.gears.map((gear) => (gear.addr === incoming.addr ? merged : gear));
    commit({ gears });
}

export function gearAt(addr: number): Gear | undefined {
    return snapshot.gears.find((gear) => gear.addr === addr);
}

/* ---------------------------------------------------------------- loading */

/** The initial picture. Called once by the app and again after any operation finishes. */
export async function refresh(): Promise<void> {
    try {
        const [bus, list] = await Promise.all([api.bus(), api.gears()]);
        commit({ bus, gears: byAddr(list.gears), loaded: true, loadError: null });
    } catch (error) {
        commit({ loaded: true, loadError: errorText(error) });
    }
}

/** The full entry for one gear — `config`, `identity` and the rest the compact list omits. */
export async function loadGear(addr: number): Promise<Gear> {
    const gear = await api.gear(addr);
    mergeGear(gear);
    return gear;
}

/**
 * Group membership lives in `config.groups`, which only the full entry carries, so grouping the
 * dashboard means reading every present gear once. It is a registry read, not a bus transaction,
 * but it is still 64 requests over Wi-Fi — hence on demand, four at a time, and never on load.
 */
export async function loadMembership(): Promise<void> {
    const pending = snapshot.gears.filter((gear) => gear.present && gear.config === undefined);
    const queue = [...pending];
    const worker = async (): Promise<void> => {
        for (;;) {
            const gear = queue.shift();
            if (gear === undefined) return;
            try {
                await loadGear(gear.addr);
            } catch {
                // One unreadable gear must not stop the other 63.
                return;
            }
        }
    };
    await Promise.all([worker(), worker(), worker(), worker()]);
}

/* ----------------------------------------------------------------- events */

onStateChange((stream) => {
    commit({ stream });
});

onEvent((event) => {
    switch (event.name) {
        case 'bus':
            commit({ bus: event.data as BusState });
            break;
        case 'gear':
            mergeGear(event.data as Gear);
            break;
        case 'progress':
            commit({ progress: event.data as Progress });
            break;
        case 'result': {
            seq += 1;
            commit({ result: event.data as Result, resultSeq: seq, progress: null });
            // A scan or a commissioning run rewrites the registry; the compact list is the
            // cheapest way to pick the whole thing up at once.
            void refresh();
            break;
        }
        case 'log': {
            seq += 1;
            const line = event.data as { level?: string; msg?: string };
            const entry: LogLine = {
                seq,
                level: line.level ?? 'info',
                msg: line.msg ?? '',
            };
            commit({ log: [entry, ...snapshot.log].slice(0, LOG_LIMIT) });
            break;
        }
        case 'rx':
            // `monitor.ts` owns these: they arrive far faster than a gear update and a commit here
            // would re-render every screen that reads the bus.
            break;
    }
});

/* ------------------------------------------------------------------ hooks */

export function useStore(): Snapshot {
    const [, force] = useState(0);
    useEffect(() => {
        const listener = (): void => {
            force((n) => n + 1);
        };
        listeners.add(listener);
        return () => {
            listeners.delete(listener);
        };
    }, []);
    return snapshot;
}
