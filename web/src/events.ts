/**
 * The device's event stream (`GET /api/events`), and the only one this page will ever open.
 *
 * Why this is a `fetch` reader and not an `EventSource`: the device accepts **three** concurrent
 * clients and answers the fourth with 503 (SPEC §9). `EventSource` reports every failure as one
 * opaque `error` event with no status, and retries on its own schedule — which turns "someone left
 * a tab open" into a reconnect storm that keeps the user locked out of their own gateway. Reading
 * the response by hand gives us the status code, so a refusal and a dead network are two different
 * states with two different messages and two different backoffs.
 */

import { API_BASE } from './api.ts';

export type EventName = 'gear' | 'bus' | 'progress' | 'result' | 'log' | 'rx';

export interface StreamEvent {
    name: EventName;
    data: unknown;
}

export type StreamState =
    | { phase: 'idle' }
    | { phase: 'connecting' }
    | { phase: 'open' }
    /** Transport failure: the gateway is not answering. Retries with backoff. */
    | { phase: 'retrying'; attempt: number; retryInS: number }
    /** 503: the gateway already has three listeners. A different problem with a different fix. */
    | { phase: 'refused'; retryInS: number };

const NAMES: readonly EventName[] = ['gear', 'bus', 'progress', 'result', 'log', 'rx'];

/** 1 s → 15 s. Jittered, so several tabs coming back from sleep do not knock in unison. */
const BACKOFF_MS = [1000, 2000, 4000, 8000, 15000];

/**
 * A 503 means a slot will free only when a person closes a tab, so retrying fast is pure noise on
 * a device with 400 KB of RAM. Wait, and say what to do instead.
 */
const REFUSED_RETRY_MS = 20000;

/**
 * The device sends a heartbeat comment every 15 s. A Wi-Fi association that dies without a FIN
 * leaves the reader blocked forever, so silence for three heartbeats is treated as a dead stream.
 */
const SILENCE_MS = 45000;

const events = new Set<(event: StreamEvent) => void>();
const states = new Set<(state: StreamState) => void>();

let state: StreamState = { phase: 'idle' };
let controller: AbortController | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;
let attempt = 0;
let running = false;

/**
 * Read through a call rather than the binding: an early `if (!running) return` would otherwise
 * narrow the flag for the rest of the function, across every `await` that can change it.
 */
function isRunning(): boolean {
    return running;
}

export function streamState(): StreamState {
    return state;
}

function setState(next: StreamState): void {
    state = next;
    for (const listener of states) listener(next);
}

function emit(event: StreamEvent): void {
    for (const listener of events) listener(event);
}

/** Subscribe to the parsed events. Returns the unsubscribe. */
export function onEvent(listener: (event: StreamEvent) => void): () => void {
    events.add(listener);
    return () => {
        events.delete(listener);
    };
}

export function onStateChange(listener: (state: StreamState) => void): () => void {
    states.add(listener);
    return () => {
        states.delete(listener);
    };
}

function schedule(next: StreamState, delayMs: number): void {
    setState(next);
    clearTimeout(timer);
    timer = setTimeout(() => {
        timer = undefined;
        void run();
    }, delayMs);
}

function backoff(): void {
    const base = BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)] ?? 15000;
    const delay = Math.round(base * (0.8 + Math.random() * 0.4));
    attempt += 1;
    schedule({ phase: 'retrying', attempt, retryInS: Math.round(delay / 1000) }, delay);
}

/**
 * One SSE frame per blank line. Comments (`:` first) are the heartbeat and carry nothing but the
 * proof that the socket is alive, which the watchdog above already took from the bytes arriving.
 */
function parse(block: string): StreamEvent | null {
    let name = 'message';
    const data: string[] = [];
    for (const line of block.split('\n')) {
        if (line === '' || line.startsWith(':')) continue;
        const colon = line.indexOf(':');
        const field = colon === -1 ? line : line.slice(0, colon);
        const value = colon === -1 ? '' : line.slice(colon + 1).replace(/^ /, '');
        if (field === 'event') name = value;
        else if (field === 'data') data.push(value);
    }
    if (data.length === 0) return null;
    if (!NAMES.includes(name as EventName)) return null;
    try {
        return { name: name as EventName, data: JSON.parse(data.join('\n')) as unknown };
    } catch {
        return null;
    }
}

async function pump(body: ReadableStream<Uint8Array>, signal: AbortSignal): Promise<void> {
    const reader = body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    let watchdog = setTimeout(() => {
        controller?.abort();
    }, SILENCE_MS);
    try {
        for (;;) {
            const { value, done } = await reader.read();
            if (done || signal.aborted) return;
            clearTimeout(watchdog);
            watchdog = setTimeout(() => {
                controller?.abort();
            }, SILENCE_MS);
            buffer = (buffer + decoder.decode(value, { stream: true })).replace(/\r\n/g, '\n');
            let cut = buffer.indexOf('\n\n');
            while (cut !== -1) {
                const event = parse(buffer.slice(0, cut));
                buffer = buffer.slice(cut + 2);
                if (event !== null) emit(event);
                cut = buffer.indexOf('\n\n');
            }
        }
    } finally {
        clearTimeout(watchdog);
        reader.cancel().catch(() => undefined);
    }
}

async function run(): Promise<void> {
    if (!isRunning()) return;
    const ac = new AbortController();
    controller = ac;
    setState({ phase: 'connecting' });
    try {
        const response = await fetch(`${API_BASE}/events`, {
            signal: ac.signal,
            headers: { accept: 'text/event-stream' },
            cache: 'no-store',
        });
        if (response.status === 503) {
            response.body?.cancel().catch(() => undefined);
            attempt = 0;
            schedule({ phase: 'refused', retryInS: REFUSED_RETRY_MS / 1000 }, REFUSED_RETRY_MS);
            return;
        }
        if (!response.ok || response.body === null) {
            backoff();
            return;
        }
        attempt = 0;
        setState({ phase: 'open' });
        await pump(response.body, ac.signal);
        if (!isRunning()) return;
        // A clean end is still an end: the device restarted, or a proxy timed the stream out.
        backoff();
    } catch {
        if (!isRunning()) return;
        backoff();
    }
}

/** Open the stream. Idempotent: a second call while one is open does nothing. */
export function startStream(): void {
    if (isRunning()) return;
    running = true;
    attempt = 0;
    void run();
}

export function stopStream(): void {
    running = false;
    clearTimeout(timer);
    timer = undefined;
    controller?.abort();
    controller = null;
    setState({ phase: 'idle' });
}

/** Give up the wait and try now — the button offered when the gateway refused the connection. */
export function retryNow(): void {
    if (!isRunning()) {
        startStream();
        return;
    }
    clearTimeout(timer);
    timer = undefined;
    controller?.abort();
    attempt = 0;
    void run();
}

/**
 * Hand the slot back the moment the page goes away. The gateway only counts three listeners, and a
 * socket it has not noticed closing still occupies one.
 */
if (typeof addEventListener === 'function') {
    addEventListener('pagehide', () => {
        controller?.abort();
    });
}
