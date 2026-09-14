/**
 * DALI domain helpers: address notation, the level ⇄ percentage mapping, and the status byte.
 *
 * The bus has its own vocabulary and this file is where the UI speaks it. `A3` / `G0` / `BC` are
 * the notations printed on every DALI commissioning tool, so they are what the screens show.
 */

import type { Gear, GearStatus } from './api.ts';
import type { Tone } from './ui.tsx';

export const SCENE_COUNT = 16;
export const GROUP_COUNT = 16;
export const MAX_ADDR = 63;

/** Short address as an installer writes it. */
export function addrLabel(addr: number): string {
    return `A${addr}`;
}

export function groupLabel(group: number): string {
    return `G${group}`;
}

/**
 * ADR 0003: `level_pct` is linear over the *level number*, not over light output. Level 0 is off;
 * levels 1..254 are 1..100 %. 50 % is level ~127, which is roughly 3 % of full output — surprising
 * once, and the same on every DALI tool. No second curve lives in the UI: the slider sends
 * `level_pct` and the device owns the conversion.
 */
export function levelToPct(level: number): number {
    if (level <= 0) return 0;
    return 1 + Math.round(((Math.min(level, 254) - 1) * 99) / 253);
}

/** The percentage to show for a gear, whether or not the compact list carried one. */
export function gearPct(gear: Gear): number {
    return gear.level_pct ?? levelToPct(gear.level);
}

/* ------------------------------------------------------------ status byte */

export interface StatusFlags {
    gearFailure: boolean;
    lampFailure: boolean;
    lampOn: boolean;
    limitError: boolean;
    fadeRunning: boolean;
    resetState: boolean;
    missingShortAddress: boolean;
    powerFailure: boolean;
}

/**
 * IEC 62386-102 QUERY STATUS, bit 0 first. Decoded from `raw` rather than from the named booleans
 * because the compact `/api/gears` entry carries only `raw` (SPEC §8.1) and one code path that
 * works for both lists is worth more than trusting two.
 */
export function statusFlags(status: GearStatus | undefined): StatusFlags {
    const raw = status?.raw ?? 0;
    return {
        gearFailure: (raw & 0x01) !== 0,
        lampFailure: (raw & 0x02) !== 0,
        lampOn: (raw & 0x04) !== 0,
        limitError: (raw & 0x08) !== 0,
        fadeRunning: (raw & 0x10) !== 0,
        resetState: (raw & 0x20) !== 0,
        missingShortAddress: (raw & 0x40) !== 0,
        powerFailure: (raw & 0x80) !== 0,
    };
}

/**
 * The one thing worth saying about a gear, or nothing at all.
 *
 * A healthy fitting returns `null` on purpose: 64 cards each wearing a green "OK" badge is noise,
 * and the point of the edge rail is that the one fitting in trouble is the one that is coloured.
 */
export function gearTrouble(gear: Gear): { tone: Tone; label: string } | null {
    if (!gear.present) return { tone: 'offline', label: 'Absent' };
    const f = statusFlags(gear.status);
    if (f.gearFailure) return { tone: 'error', label: 'Gear failure' };
    if (f.lampFailure) return { tone: 'error', label: 'Lamp failure' };
    if (f.missingShortAddress) return { tone: 'warn', label: 'No address' };
    if (f.powerFailure) return { tone: 'warn', label: 'Power failure' };
    if (f.resetState) return { tone: 'warn', label: 'Reset state' };
    if (f.limitError) return { tone: 'warn', label: 'Limit error' };
    if (f.fadeRunning) return { tone: 'busy', label: 'Fading' };
    return null;
}

/** Every flag that is set, for the detail screen, where the whole byte is the point. */
export function statusList(status: GearStatus | undefined): string[] {
    const f = statusFlags(status);
    const named: [boolean, string][] = [
        [f.lampOn, 'Lamp on'],
        [f.fadeRunning, 'Fade running'],
        [f.gearFailure, 'Gear failure'],
        [f.lampFailure, 'Lamp failure'],
        [f.limitError, 'Limit error'],
        [f.resetState, 'Reset state'],
        [f.missingShortAddress, 'Missing short address'],
        [f.powerFailure, 'Power failure'],
    ];
    return named.filter(([on]) => on).map(([, label]) => label);
}

/* ------------------------------------------------------------ formatting */

/**
 * Relative time for `last_scan`. The gateway may have no clock at all (no SNTP in AP mode), so a
 * timestamp that is zero, in the future, or absurdly old is reported as unknown rather than as
 * "56 years ago".
 */
export function timeAgo(unixSeconds: number | null | undefined): string {
    if (unixSeconds === null || unixSeconds === undefined || unixSeconds <= 0) return 'never';
    const delta = Math.floor(Date.now() / 1000) - unixSeconds;
    if (delta < -60 || delta > 86400 * 365) return 'at an unknown time';
    if (delta < 60) return 'just now';
    if (delta < 3600) return `${Math.floor(delta / 60)} min ago`;
    if (delta < 86400) return `${Math.floor(delta / 3600)} h ago`;
    return `${Math.floor(delta / 86400)} d ago`;
}

export function hexByte(value: number): string {
    return value.toString(16).toUpperCase().padStart(2, '0');
}

/** A hex frame is 16 or 24 bits; anything else the device will reject with `invalid_arg`. */
export function isFrame(text: string): boolean {
    return /^(?:[0-9a-fA-F]{4}|[0-9a-fA-F]{6})$/.test(text.trim());
}

/* ------------------------------------------------------------- narrowing */

/** Responses whose shape SPEC §7.4 leaves open are rendered through this rather than cast. */
export function asRecord(value: unknown): Record<string, unknown> | null {
    return typeof value === 'object' && value !== null && !Array.isArray(value)
        ? (value as Record<string, unknown>)
        : null;
}

export function asNumber(value: unknown): number | null {
    return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

/** The named queries the toolkit offers; anything else goes in as a numeric opcode. */
export const QUERIES: { value: string; label: string }[] = [
    { value: 'actual_level', label: 'Actual level' },
    { value: 'status', label: 'Status byte' },
    { value: 'control_gear_present', label: 'Control gear present' },
    { value: 'device_type', label: 'Device type' },
    { value: 'version_number', label: 'Version number' },
    { value: 'min_level', label: 'Minimum level' },
    { value: 'max_level', label: 'Maximum level' },
    { value: 'power_on_level', label: 'Power-on level' },
    { value: 'system_failure_level', label: 'System failure level' },
    { value: 'fade_time_fade_rate', label: 'Fade time / fade rate' },
    { value: 'physical_minimum', label: 'Physical minimum' },
    { value: 'content_dtr0', label: 'Content of DTR0' },
];
