/**
 * Hash routing, hand-rolled. The device serves a single `index.html` from a read-only blob and has
 * no rewrite rules, so a path-based router would 404 on reload; `#/settings` never reaches it.
 */

import { useEffect, useState } from 'preact/hooks';

export type Route = 'setup' | 'settings' | 'about';

/** The nav, in order. M2 inserts `dashboard` and M3 `bus` here; nothing else needs to change. */
export const NAV: { route: Route; label: string }[] = [
    { route: 'settings', label: 'Settings' },
    { route: 'about', label: 'About' },
];

const ROUTES: Route[] = ['setup', 'settings', 'about'];

function parse(hash: string): Route | null {
    const name = hash.replace(/^#\/?/, '');
    return ROUTES.find((route) => route === name) ?? null;
}

export function navigate(route: Route): void {
    location.hash = `#/${route}`;
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
