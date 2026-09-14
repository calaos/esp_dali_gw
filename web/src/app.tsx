import { useCallback, useEffect, useState } from 'preact/hooks';

import { api, ApiError, type Config, type Info } from './api.ts';
import { NAV, type Route, useHashRoute } from './router.ts';
import { About } from './views/about.tsx';
import { Settings } from './views/settings.tsx';
import { SetupWizard } from './views/setup.tsx';

type State =
    | { phase: 'loading' }
    | { phase: 'ready'; info: Info; config: Config }
    | { phase: 'error'; message: string; offline: boolean };

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
            if (state.info.mode === 'ap') return { className: 'is-warn', label: 'Setup mode' };
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
    const hash = useHashRoute();

    const load = useCallback(() => {
        Promise.all([api.info(), api.config()])
            .then(([info, config]) => {
                setState({ phase: 'ready', info, config });
            })
            .catch((error: unknown) => {
                setState({ phase: 'error', ...describe(error) });
            });
    }, []);

    useEffect(load, [load]);

    const { className, label } = tone(state);
    const ready = state.phase === 'ready' ? state : null;

    // SPEC §5.2: the wizard is the landing page while the device has no network of its own to join,
    // but the rest of the UI stays reachable in AP mode for on-site diagnostics.
    const provisioning =
        ready !== null && (ready.info.mode === 'ap' || ready.config.wifi.ssid === '');
    const route: Route = hash ?? (provisioning ? 'setup' : 'about');
    const nav = provisioning ? [{ route: 'setup' as const, label: 'Setup' }, ...NAV] : NAV;

    return (
        <>
            <header class="appbar">
                <span class="mark" aria-hidden="true" />
                <h1>{ready?.config.device.name ?? 'DALI Gateway'}</h1>
                <span class={`badge ${className}`} role="status">
                    <span class="dot" />
                    {label}
                </span>
            </header>

            {ready !== null && (
                <nav class="nav" aria-label="Sections">
                    {nav.map((entry) => (
                        <a
                            key={entry.route}
                            class={entry.route === route ? 'nav__link is-current' : 'nav__link'}
                            href={`#/${entry.route}`}
                            aria-current={entry.route === route ? 'page' : undefined}
                        >
                            {entry.label}
                        </a>
                    ))}
                </nav>
            )}

            <main class="page">
                {state.phase === 'loading' && (
                    <section class="panel panel--rail is-busy">
                        <p class="muted">Reading device information…</p>
                    </section>
                )}

                {state.phase === 'error' && (
                    <section class={`panel panel--rail ${className}`}>
                        <h2>
                            {state.offline ? 'No answer from the gateway' : 'The gateway refused'}
                        </h2>
                        <p class="muted mono">{state.message}</p>
                        <p class="muted">
                            If the gateway has just restarted, give it a few seconds. On the dev
                            server this means the proxy has no device to talk to: set{' '}
                            <code>DEVICE_HOST</code> and reload.
                        </p>
                        <button type="button" class="btn btn--secondary" onClick={load}>
                            Try again
                        </button>
                    </section>
                )}

                {ready !== null && route === 'setup' && (
                    <SetupWizard
                        config={ready.config}
                        hostname={ready.info.hostname ?? ready.config.device.hostname}
                    />
                )}
                {ready !== null && route === 'settings' && (
                    <Settings config={ready.config} onReload={load} />
                )}
                {ready !== null && route === 'about' && <About info={ready.info} />}
            </main>
        </>
    );
}
