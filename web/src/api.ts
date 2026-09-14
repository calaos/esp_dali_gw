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
    hostname?: string;
    device_id?: string;
    min_free_heap?: number;
    /** esp_reset_reason() rendered by the firmware, e.g. `"POWERON"`, `"SW"`, `"PANIC"`. */
    reset_reason?: string;
}

/* ------------------------------------------------------------------ config */

/**
 * The SPEC §6 configuration document. Secret fields (`wifi.password`, `wifi.ap_password`,
 * `mqtt.password`, `http.auth.password`) never carry a real value: the device sends `"***"`, and
 * writing `"***"` back means "leave unchanged". See `MASKED` in `web/src/config.ts`.
 */
export interface Config {
    schema: number;
    device: { name: string; hostname: string; timezone: string };
    wifi: {
        ssid: string;
        password: string;
        static: { enabled: boolean; ip: string; mask: string; gw: string; dns: string };
        ap_password: string;
        fallback_ap_timeout_s: number;
    };
    mqtt: {
        enabled: boolean;
        uri: string;
        username: string;
        password: string;
        client_id: string;
        base_topic: string;
        keepalive_s: number;
        qos: number;
        retain_state: boolean;
        ha_discovery: { enabled: boolean; prefix: string };
    };
    http: { auth: { enabled: boolean; username: string; password: string } };
    dali: {
        tx_gpio: number;
        rx_gpio: number;
        invert_tx: boolean;
        invert_rx: boolean;
        poll_interval_s: number;
        scan_on_boot: boolean;
        identify_blink_ms: number;
    };
    led: { enabled: boolean; gpio: number; brightness: number };
    gears?: Record<string, { name?: string }>;
    groups?: Record<string, { name?: string }>;
}

/** A `PUT /api/config` body: any subtree of `Config`, merged by the device. */
export type ConfigPatch = {
    [K in keyof Config]?: Config[K] extends object ? ConfigPatchOf<Config[K]> : Config[K];
};

type ConfigPatchOf<T> = { [K in keyof T]?: T[K] extends object ? ConfigPatchOf<T[K]> : T[K] };

export interface WriteResult {
    ok: boolean;
    reboot_required: boolean;
}

/** DALI-relevant auth strings the firmware reports; anything else renders as-is. */
export interface WifiNetwork {
    ssid: string;
    rssi: number;
    /** `"open"`, `"wpa2"`, `"wpa3"`, … — anything but `"open"` is treated as secured. */
    auth: string;
    channel: number;
}

export interface ScanResult {
    networks: WifiNetwork[];
}

export interface OtaResult {
    ok: boolean;
    /** Partition the device will boot from next, e.g. `"ota_1"`. */
    next_boot?: string;
}

/** Deliberately narrower than `RequestInit`: plain-object headers keep the spread below honest. */
interface RequestOptions {
    method?: string;
    headers?: Record<string, string>;
    body?: string;
    signal?: AbortSignal;
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

export function post<T>(path: string, body?: unknown, signal?: AbortSignal): Promise<T> {
    return request<T>(path, {
        method: 'POST',
        ...(signal ? { signal } : {}),
        ...(body === undefined
            ? {}
            : { headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }),
    });
}

export function put<T>(path: string, body: unknown): Promise<T> {
    return request<T>(path, {
        method: 'PUT',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(body),
    });
}

/**
 * `fetch` exposes no upload progress — `ReadableStream` request bodies are not supported without
 * HTTP/2 and the device speaks HTTP/1.1 — so a firmware image goes up through XMLHttpRequest,
 * whose `upload.onprogress` is the only way to drive a real progress bar.
 */
export function upload(
    path: string,
    file: Blob,
    onProgress: (fraction: number) => void,
): { promise: Promise<OtaResult>; abort: () => void } {
    const xhr = new XMLHttpRequest();
    const promise = new Promise<OtaResult>((resolve, reject) => {
        xhr.open('POST', `${API_BASE}${path}`);
        xhr.setRequestHeader('content-type', 'application/octet-stream');
        xhr.upload.addEventListener('progress', (event) => {
            if (event.lengthComputable) onProgress(event.loaded / event.total);
        });
        xhr.addEventListener('load', () => {
            if (xhr.status < 200 || xhr.status >= 300) {
                reject(new ApiError(`${path}: ${xhr.status} ${xhr.statusText}`, xhr.status));
                return;
            }
            try {
                resolve(JSON.parse(xhr.responseText) as OtaResult);
            } catch (cause) {
                reject(new ApiError(`${path}: reply was not JSON`, xhr.status, { cause }));
            }
        });
        xhr.addEventListener('error', () => {
            reject(new ApiError(`upload failed (${path})`));
        });
        xhr.addEventListener('abort', () => {
            reject(new ApiError(`upload cancelled (${path})`));
        });
        xhr.send(file);
    });
    return {
        promise,
        abort: () => {
            xhr.abort();
        },
    };
}

export const api = {
    info: () => get<Info>('/info'),
    config: () => get<Config>('/config'),
    saveConfig: (patch: ConfigPatch) => put<WriteResult>('/config', patch),
    scan: (signal?: AbortSignal) => post<ScanResult>('/wifi/scan', {}, signal),
    exportConfig: (secrets: boolean) =>
        post<Config>(`/config/export${secrets ? '?secrets=1' : ''}`, {}),
    importConfig: (document: unknown) => post<WriteResult>('/config/import', document),
    reboot: () => post<{ ok: boolean }>('/reboot', { confirm: true }),
    factoryReset: () => post<{ ok: boolean }>('/factory_reset', { confirm: true }),
    ota: (file: Blob, onProgress: (fraction: number) => void) =>
        upload('/ota', file, onProgress),
};
