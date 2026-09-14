/**
 * The slider write path (SPEC §10: optimistic, debounced 150 ms, coalesced).
 *
 * Why this is a queue of exactly one and not a plain debounce: a DALI transaction takes about
 * 60 ms, the bus has a single owner (ADR 0001) and the command queue is 32 deep. A slider that
 * posts per pixel fills that queue in under a second, every later command comes back `bus_busy`,
 * and the fitting ends up at whichever value happened to survive. So:
 *
 *   1. every input event moves the slider locally and touches nothing else;
 *   2. the value replaces whatever was waiting — only the newest one is ever sent;
 *   3. it leaves after 150 ms of quiet, or 150 ms after the first unsent value whichever comes
 *      first, or immediately when the user lets go;
 *   4. at most one request is in flight per target, and a value that arrives during that request
 *      waits for it and goes out next, still coalesced.
 *
 * Rule 3 is a debounce with a ceiling rather than a plain trailing debounce: a finger that keeps
 * moving never leaves 150 ms of quiet, and a dimmer that shows nothing until the user lets go is
 * useless in a room you are standing in. Two hard limits keep it safe whatever the gesture: no two
 * requests start less than 150 ms apart, and no second request starts while one is in flight. A
 * dragged slider therefore contributes at most one command to the 32-deep queue at any instant,
 * and at most ~7 DALI transactions a second.
 */

import { useEffect, useRef, useState } from 'preact/hooks';

import { ApiError, errorText } from './api.ts';

const DEBOUNCE_MS = 150;

/** A refused write usually means the bus is mid-transaction; do not spin the queue on it. */
const RETRY_MS = 400;

/**
 * The device answers `set` before it publishes the new state. Keeping the local value for a moment
 * after the last write stops the slider snapping back to the old level and then forward again.
 */
const HOLD_MS = 900;

export class LevelPipe {
    private pending: number | null = null;
    private timer: ReturnType<typeof setTimeout> | undefined;
    private hold: ReturnType<typeof setTimeout> | undefined;
    private inflight = false;
    private disposed = false;
    /** When the value currently waiting first became unsent; 0 when nothing is waiting. */
    private queuedAt = 0;
    private lastSentAt = 0;

    private readonly send: (pct: number) => Promise<unknown>;
    /** `null` on success. Called once per settled request and once when the hold expires. */
    private readonly onSettle: (error: unknown) => void;
    /** The device's value may take the slider back. */
    private readonly onRelease: () => void;

    constructor(
        send: (pct: number) => Promise<unknown>,
        onSettle: (error: unknown) => void,
        onRelease: () => void,
    ) {
        this.send = send;
        this.onSettle = onSettle;
        this.onRelease = onRelease;
    }

    /** True while this pipe owns the slider position. */
    get owns(): boolean {
        return this.pending !== null || this.inflight || this.hold !== undefined;
    }

    /** Every `input` event. Replaces the queued value: coalescing is this one assignment. */
    queue(pct: number): void {
        if (this.disposed) return;
        this.pending = pct;
        if (this.queuedAt === 0) this.queuedAt = Date.now();
        clearTimeout(this.hold);
        this.hold = undefined;
        clearTimeout(this.timer);
        const waited = Date.now() - this.queuedAt;
        this.timer = setTimeout(
            () => {
                this.timer = undefined;
                this.pump();
            },
            Math.max(0, DEBOUNCE_MS - waited),
        );
    }

    /** Pointer release or keyboard commit: no reason to sit on the value for the rest of the window. */
    flush(): void {
        if (this.disposed) return;
        clearTimeout(this.timer);
        this.timer = undefined;
        this.pump();
    }

    /** Drop everything queued — the user pressed On or Off, which supersedes the slider. */
    reset(): void {
        this.pending = null;
        this.queuedAt = 0;
        clearTimeout(this.timer);
        clearTimeout(this.hold);
        this.timer = undefined;
        this.hold = undefined;
    }

    dispose(): void {
        this.disposed = true;
        this.reset();
    }

    private pump(): void {
        if (this.disposed || this.inflight || this.pending === null) return;
        const since = Date.now() - this.lastSentAt;
        if (since < DEBOUNCE_MS) {
            clearTimeout(this.timer);
            this.timer = setTimeout(() => {
                this.timer = undefined;
                this.pump();
            }, DEBOUNCE_MS - since);
            return;
        }
        const value = this.pending;
        this.pending = null;
        this.queuedAt = 0;
        this.lastSentAt = Date.now();
        this.inflight = true;
        this.send(value).then(
            () => {
                this.settled(null);
            },
            (error: unknown) => {
                this.settled(error);
            },
        );
    }

    private settled(error: unknown): void {
        this.inflight = false;
        if (this.disposed) return;
        this.onSettle(error);
        if (this.pending !== null) {
            clearTimeout(this.timer);
            this.timer = setTimeout(
                () => {
                    this.timer = undefined;
                    this.pump();
                },
                error === null ? 0 : RETRY_MS,
            );
            return;
        }
        if (error !== null) {
            // Nothing queued behind a failure: let the device's own state correct the slider.
            this.onRelease();
            return;
        }
        this.hold = setTimeout(() => {
            this.hold = undefined;
            this.onRelease();
        }, HOLD_MS);
    }
}

export interface LevelControl {
    /** What the slider shows: the user's value while they own it, the device's otherwise. */
    pct: number;
    error: string | null;
    /** Every `input` event. */
    input: (pct: number) => void;
    /** `change`: the user let go. */
    commit: () => void;
    /** Hand the slider back to the device, e.g. because On/Off just took over. */
    release: () => void;
}

/**
 * `send` is called with the percentage the user chose and nothing else — ADR 0003 puts the
 * percentage-to-level mapping on the device, and a second curve here would fight it.
 */
export function useLevel(send: (pct: number) => Promise<unknown>, devicePct: number): LevelControl {
    const sendRef = useRef(send);
    sendRef.current = send;

    const [local, setLocal] = useState<number | null>(null);
    const [error, setError] = useState<string | null>(null);
    const pipeRef = useRef<LevelPipe | null>(null);

    pipeRef.current ??= new LevelPipe(
        (pct) => sendRef.current(pct),
        (failure) => {
            setError(failure === null ? null : describeWrite(failure));
        },
        () => {
            setLocal(null);
        },
    );
    const pipe = pipeRef.current;

    useEffect(
        () => () => {
            pipe.dispose();
        },
        [pipe],
    );

    return {
        pct: local ?? devicePct,
        error,
        input: (pct) => {
            setLocal(pct);
            setError(null);
            pipe.queue(pct);
        },
        commit: () => {
            pipe.flush();
        },
        release: () => {
            pipe.reset();
            setLocal(null);
            setError(null);
        },
    };
}

/** Short enough to sit inside a gear card; the long message belongs on the detail screen. */
export function describeWrite(error: unknown): string {
    if (error instanceof ApiError) {
        switch (error.code) {
            case 'bus_busy':
                return 'Bus busy';
            case 'bus_unpowered':
                return 'Bus unpowered';
            case 'no_reply':
                return 'No reply';
            case 'not_present':
                return 'Not on the bus';
            case 'timeout':
                return 'Timed out';
            default:
                break;
        }
        if (error.offline) return 'No connection';
    }
    return errorText(error);
}
