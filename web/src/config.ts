/**
 * Configuration-document helpers shared by the wizard and the settings screen.
 *
 * Rationale for the diff: the device never sends a secret back, it sends `MASKED`, and writing
 * `MASKED` means "keep the stored value" (SPEC §6). So the safe way to build a `PUT /api/config`
 * body is to send only the leaves that actually differ from what was loaded. A password the user
 * did not touch still holds `MASKED` in the draft, compares equal, and is left out of the body —
 * there is no code path that can turn "untouched" into an empty string and wipe the credential.
 */

import type { Config, ConfigPatch } from './api.ts';

/**
 * What the device sends instead of a secret, and what "unchanged" looks like on a write. The
 * secrets are `wifi.password`, `wifi.ap_password`, `mqtt.password` and `http.auth.password`.
 */
export const MASKED = '***';

/**
 * SPEC §6: changing Wi-Fi, either DALI GPIO, the HTTP auth block or the LED GPIO needs a restart.
 * The device confirms this in `reboot_required`, but the user is told before they press Save.
 */
export function needsReboot(path: string): boolean {
    return (
        path.startsWith('wifi.') ||
        path.startsWith('http.auth') ||
        path === 'led.gpio' ||
        (path.startsWith('dali.') && path.endsWith('_gpio'))
    );
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** Dotted paths of every leaf that differs between `base` and `draft`. */
export function changedPaths(base: Config, draft: Config): string[] {
    const out: string[] = [];
    const walk = (a: unknown, b: unknown, prefix: string): void => {
        if (isPlainObject(a) && isPlainObject(b)) {
            for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) {
                walk(a[key], b[key], prefix ? `${prefix}.${key}` : key);
            }
        } else if (a !== b) {
            out.push(prefix);
        }
    };
    walk(base, draft, '');
    return out;
}

/**
 * The `PUT /api/config` body: the changed leaves only, with their enclosing objects rebuilt so the
 * device can merge subtrees. An unchanged document yields `{}`.
 */
export function patchFrom(base: Config, draft: Config): ConfigPatch {
    const build = (a: unknown, b: unknown): unknown => {
        if (!isPlainObject(a) || !isPlainObject(b)) return b;
        const out: Record<string, unknown> = {};
        for (const key of Object.keys(b)) {
            if (isPlainObject(a[key]) && isPlainObject(b[key])) {
                const nested = build(a[key], b[key]);
                if (isPlainObject(nested) && Object.keys(nested).length > 0) out[key] = nested;
            } else if (a[key] !== b[key]) {
                out[key] = b[key];
            }
        }
        return out;
    };
    return build(base, draft) as ConfigPatch;
}

/** Structured clone of the document, so a form can be edited without touching the loaded copy. */
export function cloneConfig(config: Config): Config {
    return structuredClone(config);
}

/* ------------------------------------------------------------- formatting */

export function formatUptime(seconds: number): string {
    const d = Math.floor(seconds / 86400);
    const h = Math.floor((seconds % 86400) / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    if (d > 0) return `${d} d ${h} h ${m} min`;
    if (h > 0) return `${h} h ${m} min`;
    if (m > 0) return `${m} min ${seconds % 60} s`;
    return `${seconds} s`;
}

export function formatBytes(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

/** Four quarters, the way a signal meter is read; also drives the bar glyph in the scan list. */
export function signalBars(rssi: number): number {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
}

export function signalLabel(rssi: number): string {
    return ['weak', 'fair', 'good', 'excellent'][signalBars(rssi) - 1] ?? 'weak';
}

/**
 * First-run only: drop secrets the user left empty. On a device that has never been provisioned
 * "empty" and "not set" are the same thing, and not sending the field is strictly safer than
 * sending `""` — which would clear a stored secret if the device turned out to have one.
 */
export function withoutEmptySecrets(patch: ConfigPatch): ConfigPatch {
    const out = structuredClone(patch);
    if (out.wifi?.password === '') delete out.wifi.password;
    if (out.wifi?.ap_password === '') delete out.wifi.ap_password;
    if (out.mqtt?.password === '') delete out.mqtt.password;
    if (out.http?.auth?.password === '') delete out.http.auth.password;
    return out;
}
