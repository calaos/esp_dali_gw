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

/* --------------------------------------------------------- frame reading */

export type FrameKind = 'forward' | 'backward' | 'device' | 'unknown';

export interface FrameReading {
    kind: FrameKind;
    /** What the frame says, or — when `decoded` is false — why it cannot be said. */
    text: string;
    decoded: boolean;
}

/**
 * Opcodes of IEC 62386-102, sentence case. The ranges (scenes, groups) are computed below rather
 * than listed, which keeps 96 of the 256 entries out of the bundle.
 */
const COMMANDS: Record<number, string> = {
    0x00: 'Off',
    0x01: 'Up',
    0x02: 'Down',
    0x03: 'Step up',
    0x04: 'Step down',
    0x05: 'Recall max level',
    0x06: 'Recall min level',
    0x07: 'Step down and off',
    0x08: 'On and step up',
    0x09: 'Enable DAPC sequence',
    0x0a: 'Go to last active level',
    0x20: 'Reset',
    0x21: 'Store actual level in DTR0',
    0x22: 'Save persistent variables',
    0x23: 'Set operating mode',
    0x24: 'Reset memory bank',
    0x25: 'Identify device',
    0x2a: 'Store DTR0 as max level',
    0x2b: 'Store DTR0 as min level',
    0x2c: 'Store DTR0 as system failure level',
    0x2d: 'Store DTR0 as power-on level',
    0x2e: 'Store DTR0 as fade time',
    0x2f: 'Store DTR0 as fade rate',
    0x30: 'Store DTR0 as extended fade time',
    0x80: 'Store DTR0 as short address',
    0x81: 'Enable write memory',
    0x90: 'Query status',
    0x91: 'Query control gear present',
    0x92: 'Query lamp failure',
    0x93: 'Query lamp power on',
    0x94: 'Query limit error',
    0x95: 'Query reset state',
    0x96: 'Query missing short address',
    0x97: 'Query version number',
    0x98: 'Query content DTR0',
    0x99: 'Query device type',
    0x9a: 'Query physical minimum',
    0x9b: 'Query power failure',
    0x9c: 'Query content DTR1',
    0x9d: 'Query content DTR2',
    0x9e: 'Query operating mode',
    0x9f: 'Query light source type',
    0xa0: 'Query actual level',
    0xa1: 'Query max level',
    0xa2: 'Query min level',
    0xa3: 'Query power-on level',
    0xa4: 'Query system failure level',
    0xa5: 'Query fade time / fade rate',
    0xa6: 'Query manufacturer specific mode',
    0xa7: 'Query next device type',
    0xa8: 'Query extended fade time',
    0xaa: 'Query control gear failure',
    0xc0: 'Query groups 0-7',
    0xc1: 'Query groups 8-15',
    0xc2: 'Query random address H',
    0xc3: 'Query random address M',
    0xc4: 'Query random address L',
    0xc5: 'Read memory location',
    0xc6: 'Query extended version number',
};

/**
 * Special commands, which put the command in the *address* byte. The second element says what the
 * data byte is: nothing worth printing, a plain value, or a short address in `0AAAAAA1` form —
 * the last is what makes a commissioning run readable as it happens.
 */
const SPECIAL: Record<number, readonly [string, 'none' | 'byte' | 'addr']> = {
    0xa1: ['Terminate', 'none'],
    0xa3: ['Store value in DTR0', 'byte'],
    0xa5: ['Initialise', 'byte'],
    0xa7: ['Randomise', 'none'],
    0xa9: ['Compare', 'none'],
    0xab: ['Withdraw', 'none'],
    0xb1: ['Search address H', 'byte'],
    0xb3: ['Search address M', 'byte'],
    0xb5: ['Search address L', 'byte'],
    0xb7: ['Program short address', 'addr'],
    0xb9: ['Verify short address', 'addr'],
    0xbb: ['Query short address', 'none'],
    0xc1: ['Enable device type', 'byte'],
    0xc3: ['Store value in DTR1', 'byte'],
    0xc5: ['Store value in DTR2', 'byte'],
    0xc7: ['Write memory location', 'byte'],
    0xc9: ['Write memory location, no reply', 'byte'],
};

/** Who a forward frame is talking to, or `null` for the patterns the standard reserves. */
function frameTarget(byte: number): string | null {
    if ((byte & 0x80) === 0) return addrLabel((byte >> 1) & 0x3f);
    if ((byte & 0xe0) === 0x80) return groupLabel((byte >> 1) & 0x0f);
    if ((byte & 0xfe) === 0xfe) return 'Broadcast';
    if ((byte & 0xfe) === 0xfc) return 'Broadcast, unaddressed';
    return null;
}

function commandName(opcode: number): string | null {
    if (opcode >= 0x10 && opcode <= 0x1f) return `Go to scene ${opcode - 0x10}`;
    if (opcode >= 0x40 && opcode <= 0x4f) return `Store DTR0 as scene ${opcode - 0x40}`;
    if (opcode >= 0x50 && opcode <= 0x5f) return `Remove from scene ${opcode - 0x50}`;
    if (opcode >= 0x60 && opcode <= 0x6f) return `Add to group ${opcode - 0x60}`;
    if (opcode >= 0x70 && opcode <= 0x7f) return `Remove from group ${opcode - 0x70}`;
    if (opcode >= 0xb0 && opcode <= 0xbf) return `Query scene ${opcode - 0xb0} level`;
    return COMMANDS[opcode] ?? null;
}

function readForward(address: number, data: number): FrameReading {
    const special = SPECIAL[address];
    if (special !== undefined) {
        const [name, carries] = special;
        const value =
            carries === 'none'
                ? ''
                : carries === 'addr'
                  ? ` ${addrLabel((data >> 1) & 0x3f)}`
                  : ` ${data}`;
        return { kind: 'forward', text: `${name}${value}`, decoded: true };
    }
    const target = frameTarget(address);
    if (target === null) {
        return { kind: 'unknown', text: `Reserved address byte 0x${hexByte(address)}`, decoded: false };
    }
    // Bit 0 of the address byte chooses between a level and an opcode.
    if ((address & 1) === 0) {
        const level = data === 255 ? 'level MASK, no change' : `level ${data}`;
        return { kind: 'forward', text: `${target} · ${level}`, decoded: true };
    }
    const command = commandName(data);
    return command === null
        ? {
              kind: 'unknown',
              text: `${target} · unknown opcode 0x${hexByte(data)}`,
              decoded: false,
          }
        : { kind: 'forward', text: `${target} · ${command}`, decoded: true };
}

/**
 * Read one frame off the wire (`event: rx`). Only what Part 102 fixes is decoded: a 24-bit Part 103
 * input-device message, a manufacturer opcode or a reserved address byte is reported as unread.
 * A monitor that invents a reading is worse than one that admits it has none.
 */
export function readFrame(hex: string, bits: number): FrameReading {
    const bytes: number[] = [];
    for (let i = 0; i + 1 < hex.length; i += 2) bytes.push(parseInt(hex.slice(i, i + 2), 16));
    const [first, second] = bytes;
    if (bytes.some((byte) => Number.isNaN(byte))) {
        return { kind: 'unknown', text: 'Not a hex frame', decoded: false };
    }
    if (bits === 8 && first !== undefined) {
        return { kind: 'backward', text: `Reply ${first}`, decoded: true };
    }
    if (bits === 16 && first !== undefined && second !== undefined) {
        return readForward(first, second);
    }
    if (bits === 24) {
        return { kind: 'device', text: 'Input device, Part 103 — not decoded', decoded: false };
    }
    return { kind: 'unknown', text: `${bits}-bit frame — not decoded`, decoded: false };
}

/**
 * `ts` on an `rx` event is milliseconds since the gateway booted, which is the only clock it is
 * sure of (no SNTP in AP mode). Rendered as the device's own uptime so a captured frame lines up
 * with a line in the device log.
 */
export function bootTime(ms: number): string {
    const total = Math.max(Math.floor(ms), 0);
    const pad = (value: number, width: number): string => String(value).padStart(width, '0');
    return `${Math.floor(total / 3600000)}:${pad(Math.floor(total / 60000) % 60, 2)}:${pad(
        Math.floor(total / 1000) % 60,
        2,
    )}.${pad(total % 1000, 3)}`;
}
