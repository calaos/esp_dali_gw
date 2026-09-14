/**
 * Hash routing, hand-rolled. The device serves a single `index.html` from a read-only blob and has
 * no rewrite rules, so a path-based router would 404 on reload; `#/settings` never reaches it.
 */

import { useEffect, useState } from 'preact/hooks';

export type View = 'setup' | 'dashboard' | 'gear' | 'bus' | 'settings' | 'about';

/** `gear` is the only view with a parameter: the short address it is showing. */
export interface Route {
    view: View;
    addr: number | null;
}

/** The nav, in order. `gear` is reached from the dashboard, so it is deliberately not listed. */
export const NAV: { view: View; label: string }[] = [
    { view: 'dashboard', label: 'Dashboard' },
    { view: 'bus', label: 'Bus tools' },
    { view: 'settings', label: 'Settings' },
    { view: 'about', label: 'About' },
];

const VIEWS: View[] = ['setup', 'dashboard', 'gear', 'bus', 'settings', 'about'];

export function href(view: View, addr?: number): string {
    return addr === undefined ? `#/${view}` : `#/${view}/${addr}`;
}

function parse(hash: string): Route | null {
    const [name, param] = hash.replace(/^#\/?/, '').split('/');
    const view = VIEWS.find((candidate) => candidate === name);
    if (view === undefined) return null;
    const addr = param === undefined ? NaN : Number(param);
    return { view, addr: Number.isInteger(addr) ? addr : null };
}

export function navigate(view: View, addr?: number): void {
    location.hash = href(view, addr);
}

/** The route in the address bar, or `null` when there is no usable hash yet. */
export function useHashRoute(): Route | null {
    const [route, setRoute] = useState<Route | null>(() => parse(location.hash));
    useEffect(() => {
        const onChange = () => {
            setRoute(parse(location.hash));
        };
        addEventListener('hashchange', onChange);
        return () => {
            removeEventListener('hashchange', onChange);
        };
    }, []);
    return route;
}
