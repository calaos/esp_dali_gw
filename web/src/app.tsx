import { useCallback, useEffect, useState } from 'preact/hooks';

import { api, ApiError, type Config, type Info } from './api.ts';
import { retryNow, startStream, type StreamState } from './events.ts';
import { href, NAV, type Route, useHashRoute, type View } from './router.ts';
import { refresh, useStore } from './store.ts';
import { About } from './views/about.tsx';
import { Dashboard } from './views/dashboard.tsx';
import { GearDetail } from './views/gear.tsx';
import { Settings } from './views/settings.tsx';
import { SetupWizard } from './views/setup.tsx';
import { BusTools } from './views/tools.tsx';

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

/**
 * The event stream is down, and which of the two reasons it is matters: a gateway that is not
 * answering will come back on its own, a gateway that refused has three listeners already and will
 * not come back until a person closes something.
 */
function StreamBanner({ stream }: { stream: StreamState }) {
    if (stream.phase !== 'retrying' && stream.phase !== 'refused') return null;
    const refused = stream.phase === 'refused';
    return (
        <div class={`banner notice ${refused ? 'is-warn' : 'is-offline'}`} role="status">
            <strong>{refused ? 'The gateway is full' : 'Live updates stopped'}</strong>
            <p>
                {refused
                    ? 'It keeps three browser connections at a time and already has three. Close this page in another tab, or on another phone, and the next attempt will get in.'
                    : 'The gateway is not answering. Levels and status on this page are the last ones it sent.'}{' '}
                Retrying in {stream.retryInS} s.
            </p>
            <button type="button" class="btn btn--secondary btn--sm" onClick={retryNow}>
                Try now
            </button>
        </div>
    );
}

export function App() {
    const [state, setState] = useState<State>({ phase: 'loading' });
    const store = useStore();
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

    const ready = state.phase === 'ready' ? state : null;
    const provisioning =
        ready !== null && (ready.info.mode === 'ap' || ready.config.wifi.ssid === '');

    // One stream per page, opened once the device has answered at all and never re-opened by a
    // route change: the gateway counts listeners, not tabs (SPEC §9).
    useEffect(() => {
        if (ready === null || provisioning) return;
        startStream();
        void refresh();
    }, [ready === null, provisioning]);

    const { className, label } = tone(state);

    const route: Route = hash ?? { view: provisioning ? 'setup' : 'dashboard', addr: null };
    const nav: { view: View; label: string }[] = provisioning
        ? [{ view: 'setup', label: 'Setup' }, ...NAV]
        : NAV;
    const current: View = route.view === 'gear' ? 'dashboard' : route.view;

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
                            key={entry.view}
                            class={entry.view === current ? 'nav__link is-current' : 'nav__link'}
                            href={href(entry.view)}
                            aria-current={entry.view === current ? 'page' : undefined}
                        >
                            {entry.label}
                        </a>
                    ))}
                </nav>
            )}

            {ready !== null && !provisioning && <StreamBanner stream={store.stream} />}

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

                {ready !== null && route.view === 'setup' && (
                    <SetupWizard
                        config={ready.config}
                        hostname={ready.info.hostname ?? ready.config.device.hostname}
                    />
                )}
                {ready !== null && route.view === 'dashboard' && (
                    <Dashboard groupNames={ready.config.groups ?? {}} />
                )}
                {ready !== null && route.view === 'gear' && (
                    <GearDetail addr={route.addr ?? -1} />
                )}
                {ready !== null && route.view === 'bus' && <BusTools />}
                {ready !== null && route.view === 'settings' && (
                    <Settings config={ready.config} onReload={load} />
                )}
                {ready !== null && route.view === 'about' && <About info={ready.info} />}
            </main>
        </>
    );
}
