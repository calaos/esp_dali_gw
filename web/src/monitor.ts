/**
 * The passive bus monitor's buffer: the newest frames the gateway reported on `event: rx`.
 *
 * Deliberately not part of `store.ts`. A store commit re-renders every screen that reads the bus,
 * and foreign traffic arrives two orders of magnitude faster than a gear update: frames land in a
 * pending array and are flushed on a timer, so a burst costs a bounded number of renders in one
 * panel instead of an unbounded number everywhere.
 */

import { useEffect, useState } from 'preact/hooks';

import { bootTime, type FrameReading, readFrame } from './dali.ts';
import { onEvent } from './events.ts';

export interface MonitorFrame {
    /** Monotonic: two identical frames a millisecond apart still get stable keys. */
    seq: number;
    hex: string;
    bits: number;
    /** Milliseconds since the gateway booted. */
    ts: number;
    reading: FrameReading;
}

export interface MonitorState {
    /** Newest first. */
    frames: MonitorFrame[];
    /** Every frame the stream delivered since the last clear, kept or not. */
    seen: number;
    /** Frames that arrived while paused, and are therefore a hole in the buffer. */
    skipped: number;
    paused: boolean;
    listening: boolean;
}

/**
 * Enough to hold the burst a wall panel makes when someone leans on a button, short enough that
 * the list stays a list. Older frames are gone: the gateway drops frames under load anyway, so a
 * bigger buffer would buy completeness the protocol path cannot deliver.
 */
export const FRAME_LIMIT = 200;

/** One render per flush rather than one per frame: ~8 a second is past what anyone can read. */
const FLUSH_MS = 120;

let state: MonitorState = {
    frames: [],
    seen: 0,
    skipped: 0,
    paused: false,
    listening: false,
};

const listeners = new Set<() => void>();
let pending: MonitorFrame[] = [];
let timer: ReturnType<typeof setTimeout> | undefined;
let seq = 0;
/* Counters live outside the snapshot so that arriving traffic never commits: everything the
   stream touches is published by the flush below, one render at a time. */
let seen = 0;
let skipped = 0;

function commit(patch: Partial<MonitorState>): void {
    state = { ...state, ...patch };
    for (const listener of listeners) listener();
}

function flush(): void {
    timer = undefined;
    const arrived = pending;
    pending = [];
    commit({
        frames: [...arrived].reverse().concat(state.frames).slice(0, FRAME_LIMIT),
        seen,
        skipped,
    });
}

function schedule(): void {
    timer ??= setTimeout(flush, FLUSH_MS);
}

onEvent((event) => {
    if (event.name !== 'rx') return;
    const line = event.data as { frame?: string; bits?: number; ts?: number };
    const hex = (line.frame ?? '').toUpperCase();
    if (hex === '') return;
    const bits = line.bits ?? hex.length * 4;
    seen += 1;
    // A frame arriving is the device saying it is listening, which beats what a button believed:
    // another tab, or a page reload, can leave the monitor on with nothing here having clicked it.
    if (!state.listening) commit({ listening: true });
    if (state.paused) {
        skipped += 1;
    } else {
        seq += 1;
        pending.push({ seq, hex, bits, ts: line.ts ?? 0, reading: readFrame(hex, bits) });
        if (pending.length > FRAME_LIMIT) pending = pending.slice(-FRAME_LIMIT);
    }
    schedule();
});

/** What the device answered on `POST /api/bus/monitor` — never what the button assumed. */
export function setListening(listening: boolean): void {
    commit({ listening });
}

export function setPaused(paused: boolean): void {
    commit({ paused });
}

export function clearFrames(): void {
    pending = [];
    clearTimeout(timer);
    timer = undefined;
    seen = 0;
    skipped = 0;
    commit({ frames: [], seen, skipped });
}

/**
 * The capture as text, oldest first — the reason anyone runs this is to paste it into a bug
 * report, and a log is read the way time runs even though the screen shows the live edge on top.
 * The header says which way round it is, and what the capture is not.
 */
export function monitorText(current: MonitorState): string {
    const lines = [
        '# esp_dali_gw bus monitor',
        `# ${current.seen} frames seen, ${current.frames.length} kept${
            current.skipped > 0 ? `, ${current.skipped} skipped while paused` : ''
        }. Oldest first.`,
        '# Frames the gateway sent itself are never reported, and frames are dropped under load:',
        '# this is a sample of the bus, not a complete capture.',
        '# uptime      bits  hex     reading',
    ];
    for (const frame of [...current.frames].reverse()) {
        lines.push(
            `${bootTime(frame.ts).padEnd(13)}${String(frame.bits).padStart(2)}    ${frame.hex.padEnd(
                8,
            )}${frame.reading.text}`,
        );
    }
    return `${lines.join('\n')}\n`;
}

/**
 * The gateway is served over plain HTTP on the LAN, where Chrome and Safari leave
 * `navigator.clipboard` undefined — so a failure here is the normal case, not an edge one, and the
 * caller has to offer the file instead.
 */
export async function copyText(text: string): Promise<boolean> {
    try {
        await navigator.clipboard.writeText(text);
        return true;
    } catch {
        return false;
    }
}

export function downloadText(text: string, filename: string): void {
    const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = filename;
    link.click();
    URL.revokeObjectURL(url);
}

export function useMonitor(): MonitorState {
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
    return state;
}
