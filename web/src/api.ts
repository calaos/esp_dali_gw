/**
 * Typed client for the gateway REST API (SPEC §9).
 *
 * Every call goes through `get`/`post` so that adding an endpoint is one line in `api` plus a
 * response interface. Responses are *not* validated at runtime: the firmware and this file are
 * built from the same spec and shipped in the same image.
 */

export const API_BASE = '/api';

/**
 * The closed error set of SPEC §7.5. Every refusal the device sends names one of these; the UI
 * never parses the human message to decide what to do.
 */
export type DaliError =
    | 'invalid_arg'
    | 'bus_busy'
    | 'bus_unpowered'
    | 'no_reply'
    | 'tx_failed'
    | 'timeout'
    | 'not_present'
    | 'address_in_use'
    | 'unsupported'
    | 'cancelled'
    | 'internal';

/** Any non-2xx reply, or a transport failure (device off, wrong host, captive portal). */
export class ApiError extends Error {
    /** HTTP status, or `undefined` when the request never reached the device. */
    readonly status: number | undefined;

    /** The SPEC §7.5 code when the body carried one. */
    readonly code: DaliError | undefined;

    constructor(message: string, status?: number, code?: DaliError, options?: ErrorOptions) {
        super(message, options);
        this.name = 'ApiError';
        this.status = status;
        this.code = code;
    }

    /** True when the device could not be reached at all, as opposed to refusing the request. */
    get offline(): boolean {
        return this.status === undefined;
    }
}

/** The message a person should see for a failed call, device-worded when the device worded it. */
export function errorText(error: unknown): string {
    if (error instanceof ApiError) return error.message;
    return error instanceof Error ? error.message : String(error);
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

/* --------------------------------------------------------------- bus, gear */

export type BusOperation = 'scan' | 'commission' | 'poll_all' | 'bus_check';

/** `{done, total, found}` from SPEC §7.4; `found` only means anything during a scan. */
export interface Progress {
    operation?: BusOperation;
    done: number;
    total: number;
    found?: number;
}

/** `GET /api/bus`. `operation` and `progress` are `null` while the bus is idle. */
export interface BusState {
    powered: boolean;
    busy: boolean;
    operation: BusOperation | null;
    progress: Progress | null;
    gear_count: number;
    last_scan: number | null;
}

export interface BusCheck {
    powered?: boolean;
    replies?: boolean;
}

/**
 * QUERY STATUS. The compact `/api/gears` entry carries only `raw`, so everything else is
 * optional and `dali.ts` decodes the bits itself.
 */
export interface GearStatus {
    raw: number;
    gear_failure?: boolean;
    lamp_failure?: boolean;
    lamp_on?: boolean;
    limit_error?: boolean;
    fade_running?: boolean;
    reset_state?: boolean;
    missing_short_address?: boolean;
    power_failure?: boolean;
}

/** `null` in `scenes` is 0xFF — the scene is *not programmed*, which is not the same as level 0. */
export interface GearConfig {
    min: number;
    max: number;
    power_on: number;
    system_failure: number;
    fade_time: number;
    fade_rate: number;
    physical_min: number;
    groups: number[];
    scenes: (number | null)[];
}

/** Memory bank 0; read on a deep scan only, so every field can be missing. */
export interface GearIdentity {
    gtin?: string;
    serial?: string;
    bank0_version?: number;
}

export interface Dt8Caps {
    caps?: number;
    tc_min?: number;
    tc_max?: number;
}

/**
 * A registry entry (SPEC §7.3). `GET /api/gears` returns the compact form — addr, name, present,
 * level, on, status.raw — so anything a compact entry does not carry is optional here.
 */
export interface Gear {
    addr: number;
    name: string;
    present: boolean;
    level: number;
    on: boolean;
    status: GearStatus;
    level_pct?: number;
    last_seen?: number;
    device_types?: number[];
    version?: string;
    dt8?: Dt8Caps;
    config?: GearConfig;
    identity?: GearIdentity;
}

export interface GearList {
    gears: Gear[];
}

/** Every shape `POST …/set` accepts (SPEC §8.2); the UI only ever sends the first three. */
export type SetBody =
    | { level_pct: number }
    | { level: number }
    | { on: boolean }
    | { scene: number }
    | { cmd: string };

export type CommissionMode = 'unaddressed' | 'all';

export interface RawBody {
    frame: string;
    send_twice: boolean;
    expect_reply: boolean;
}

export type QueryBody = { addr: number; query: string } | { addr: number; opcode: number };

export interface QueryReply {
    reply?: number | null;
    raw_frame?: string;
}

/**
 * `configure` takes any subset of the parameters (SPEC §7.4). `scene` maps a scene number to a
 * level or to `null`, which removes the programming.
 */
export interface ConfigureBody {
    min?: number;
    max?: number;
    power_on?: number;
    system_failure?: number;
    fade_time?: number;
    fade_rate?: number;
    scene?: Record<string, number | null>;
    group?: { add?: number[]; remove?: number[] };
}

/**
 * SPEC §8.3, reused verbatim by HTTP. `data` is deliberately `unknown`: the per-operation shapes
 * are open (a `configure` report lists each parameter) and the UI narrows them where it renders.
 */
export interface Result<T = unknown> {
    ok: boolean;
    id?: string | number;
    action?: string;
    duration_ms?: number;
    data?: T;
    error?: DaliError;
    message?: string;
}

export type Started = Result<{ started: boolean }>;

export interface CommissionData {
    assigned?: number;
    addresses?: number[];
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
        throw new ApiError(`cannot reach the gateway (${path})`, undefined, undefined, { cause });
    }
    if (!response.ok) {
        // A refusal carries {ok:false, error, message} (SPEC §7.5). Prefer the device's own words
        // over "409 Conflict", and keep the code so callers can branch on bus_busy.
        const failure = await response
            .json()
            .then((body) => body as Partial<Failure> | null)
            .catch(() => null);
        throw new ApiError(
            failure?.message ?? `${path}: ${response.status} ${response.statusText}`,
            response.status,
            failure?.error,
        );
    }
    return (await response.json()) as T;
}

/** The body of any refusal, SPEC §7.5. */
interface Failure {
    ok: false;
    error: DaliError;
    message: string;
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

export function patch<T>(path: string, body: unknown): Promise<T> {
    return request<T>(path, {
        method: 'PATCH',
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
                reject(new ApiError(`${path}: reply was not JSON`, xhr.status, undefined, { cause }));
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

    /* ------------------------------------------------------------------- bus */

    bus: () => get<BusState>('/bus'),
    gears: () => get<GearList>('/gears'),
    gear: (addr: number) => get<Gear>(`/gears/${addr}`),

    /** Synchronous on the device: it waits for the bus result, 2 s timeout (SPEC §9). */
    setGear: (addr: number, body: SetBody) => post<Result>(`/gears/${addr}/set`, body),
    setGroup: (group: number, body: SetBody) => post<Result>(`/groups/${group}/set`, body),
    setBroadcast: (body: SetBody) => post<Result>('/broadcast/set', body),

    /** 202 + `{started:true}`; everything after that arrives on the event stream. */
    startScan: (deep: boolean) => post<Started>('/bus/scan', { deep }),
    startCommission: (mode: CommissionMode, startAddr: number) =>
        post<Started>('/bus/commission', { mode, confirm: true, start_addr: startAddr }),
    cancel: () => post<Result>('/bus/cancel', {}),
    check: () => post<Result<BusCheck>>('/bus/check', {}),

    raw: (body: RawBody) => post<Result<QueryReply>>('/bus/raw', body),
    query: (body: QueryBody) => post<Result<QueryReply>>('/bus/query', body),

    configure: (addr: number, body: ConfigureBody) =>
        post<Result>(`/gears/${addr}/configure`, body),
    identify: (addr: number) => post<Result>(`/gears/${addr}/identify`, {}),
    setAddress: (addr: number, newAddr: number) =>
        post<Result>(`/gears/${addr}/address`, { new_addr: newAddr }),
    removeAddress: (addr: number) => post<Result>(`/gears/${addr}/remove_address`, {}),
    renameGear: (addr: number, name: string) => patch<Result>(`/gears/${addr}`, { name }),
    renameGroup: (group: number, name: string) => patch<Result>(`/groups/${group}`, { name }),
};
