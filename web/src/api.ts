/**
 * Typed client for the gateway REST API (SPEC §9).
 *
 * Every call goes through `get`/`post` so that adding an endpoint is one line in `api` plus a
 * response interface. Responses are *not* validated at runtime: the firmware and this file are
 * built from the same spec and shipped in the same image.
 */

export const API_BASE = '/api';

/** Any non-2xx reply, or a transport failure (device off, wrong host, captive portal). */
export class ApiError extends Error {
    /** HTTP status, or `undefined` when the request never reached the device. */
    readonly status: number | undefined;

    constructor(message: string, status?: number, options?: ErrorOptions) {
        super(message, options);
        this.name = 'ApiError';
        this.status = status;
    }

    /** True when the device could not be reached at all, as opposed to refusing the request. */
    get offline(): boolean {
        return this.status === undefined;
    }
}

/** Retained `status` document, identical to the MQTT `<base>/status` payload (SPEC §8.1). */
export interface GatewayStatus {
    state: 'online' | 'offline';
    /** Firmware version, e.g. `"1.2.0"`. */
    fw: string;
    /** ESP-IDF version the firmware was built with. */
    idf: string;
    ip: string;
    /** Wi-Fi signal in dBm; absent in AP-only mode. */
    rssi?: number;
    uptime_s: number;
    mac: string;
}

export interface BuildInfo {
    version?: string;
    date?: string;
    idf?: string;
    target?: string;
    /** Short git SHA of the build. */
    sha?: string;
}

export type WifiMode = 'sta' | 'ap' | 'apsta';

/** `GET /api/info`. */
export interface Info {
    status: GatewayStatus;
    mode: WifiMode;
    free_heap: number;
    build?: BuildInfo;
}

/** Deliberately narrower than `RequestInit`: plain-object headers keep the spread below honest. */
interface RequestOptions {
    method?: string;
    headers?: Record<string, string>;
    body?: string;
}

async function request<T>(path: string, options: RequestOptions = {}): Promise<T> {
    const { headers, ...rest } = options;
    let response: Response;
    try {
        response = await fetch(`${API_BASE}${path}`, {
            headers: { accept: 'application/json', ...headers },
            ...rest,
        });
    } catch (cause) {
        throw new ApiError(`cannot reach the gateway (${path})`, undefined, { cause });
    }
    if (!response.ok) {
        throw new ApiError(`${path}: ${response.status} ${response.statusText}`, response.status);
    }
    return (await response.json()) as T;
}

export function get<T>(path: string): Promise<T> {
    return request<T>(path);
}

export function post<T>(path: string, body?: unknown): Promise<T> {
    return request<T>(path, {
        method: 'POST',
        ...(body === undefined
            ? {}
            : { headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }),
    });
}

export const api = {
    info: () => get<Info>('/info'),
};
