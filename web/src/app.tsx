import { useEffect, useState } from 'preact/hooks';

import { api, ApiError, type Info, type WifiMode } from './api.ts';

type State =
    | { phase: 'loading' }
    | { phase: 'ready'; info: Info }
    | { phase: 'error'; message: string; offline: boolean };

/** `WifiMode` is how the firmware names it; these are how a person reading the panel names it. */
const WIFI_MODE: Record<WifiMode, string> = {
    sta: 'Client',
    ap: 'Access point',
    apsta: 'Client and access point',
};

function describe(error: unknown): { message: string; offline: boolean } {
    if (error instanceof ApiError) {
        return { message: error.message, offline: error.offline };
    }
    return { message: error instanceof Error ? error.message : String(error), offline: false };
}

/** One tone drives both the header badge and the panel's edge rail, so the page reads as a unit. */
function tone(state: State): { className: string; label: string } {
    switch (state.phase) {
        case 'loading':
            return { className: 'is-busy', label: 'Connecting' };
        case 'ready':
            return state.info.status.state === 'online'
                ? { className: 'is-ok', label: 'Online' }
                : { className: 'is-warn', label: 'Degraded' };
        case 'error':
            return state.offline
                ? { className: 'is-offline', label: 'Offline' }
                : { className: 'is-error', label: 'Error' };
    }
}

export function App() {
    const [state, setState] = useState<State>({ phase: 'loading' });

    useEffect(() => {
        const controller = new AbortController();
        api.info()
            .then((info) => {
                if (!controller.signal.aborted) setState({ phase: 'ready', info });
            })
            .catch((error: unknown) => {
                if (!controller.signal.aborted) setState({ phase: 'error', ...describe(error) });
            });
        return () => {
            controller.abort();
        };
    }, []);

    const { className, label } = tone(state);

    return (
        <>
            <header class="appbar">
                <span class="mark" aria-hidden="true" />
                <h1>DALI Gateway</h1>
                <span class={`badge ${className}`} role="status">
                    <span class="dot" />
                    {label}
                </span>
            </header>
            <main class="page">
                <section class={`panel panel--rail ${className}`}>
                    <Body state={state} />
                </section>
            </main>
        </>
    );
}

function Body({ state }: { state: State }) {
    switch (state.phase) {
        case 'loading':
            return <p class="muted">Reading device information…</p>;
        case 'ready':
            return (
                <dl class="data-list">
                    <dt>Firmware</dt>
                    <dd class="mono">{state.info.status.fw}</dd>
                    <dt>ESP-IDF</dt>
                    <dd class="mono">{state.info.status.idf}</dd>
                    <dt>Wi-Fi</dt>
                    <dd>{WIFI_MODE[state.info.mode]}</dd>
                    <dt>Address</dt>
                    <dd class="mono">{state.info.status.ip}</dd>
                </dl>
            );
        case 'error':
            return (
                <>
                    <h2>{state.offline ? 'No answer from the gateway' : 'The gateway refused'}</h2>
                    <p class="muted mono">{state.message}</p>
                    <p class="muted">
                        On the dev server this means the proxy has no device to talk to. Set{' '}
                        <code>DEVICE_HOST</code> to a gateway on your network and reload.
                    </p>
                </>
            );
    }
}
