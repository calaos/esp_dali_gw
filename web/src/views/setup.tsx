/**
 * First-run provisioning (SPEC §5.2, §10 view 1). Shown when the device is in AP mode or has no
 * Wi-Fi network configured.
 */

import { useCallback, useEffect, useRef, useState } from 'preact/hooks';

import { api, ApiError, type Config, type WifiNetwork } from '../api.ts';
import {
    cloneConfig,
    MASKED,
    patchFrom,
    signalBars,
    signalLabel,
    withoutEmptySecrets,
} from '../config.ts';
import { Meter, Notice, SecretField, TextField } from '../ui.tsx';

/** The scan is specified to block for at most 5 s; give the round-trip a little more than that. */
const SCAN_TIMEOUT_MS = 8000;

type Step = 'wifi' | 'mqtt' | 'summary';
const STEPS: { id: Step; label: string }[] = [
    { id: 'wifi', label: 'Wi-Fi' },
    { id: 'mqtt', label: 'MQTT' },
    { id: 'summary', label: 'Review' },
];

type ScanState =
    | { phase: 'idle' }
    | { phase: 'scanning' }
    | { phase: 'done'; networks: WifiNetwork[] }
    | { phase: 'failed'; message: string; timedOut: boolean };

export function SetupWizard({ config, hostname }: { config: Config; hostname: string }) {
    const [draft, setDraft] = useState<Config>(() => {
        const next = cloneConfig(config);
        // With no network configured there is no stored Wi-Fi password to keep, so the field starts
        // empty rather than masked. In fallback-AP mode there is one, and it stays masked.
        if (next.wifi.ssid === '') {
            next.wifi.password = '';
            next.mqtt.password = '';
        }
        return next;
    });
    const [step, setStep] = useState<Step>('wifi');
    const [saving, setSaving] = useState(false);
    const [error, setError] = useState<string | null>(null);
    const [handoff, setHandoff] = useState(false);

    const update = useCallback((apply: (next: Config) => void) => {
        setDraft((current) => {
            const next = cloneConfig(current);
            apply(next);
            return next;
        });
    }, []);

    if (handoff) {
        const target = draft.device.hostname === '' ? hostname : draft.device.hostname;
        return <Handoff ssid={draft.wifi.ssid} hostname={target} />;
    }

    const save = () => {
        setSaving(true);
        setError(null);
        api.saveConfig(withoutEmptySecrets(patchFrom(config, draft)))
            .then(() => api.reboot())
            .then(() => {
                setHandoff(true);
            })
            .catch((cause: unknown) => {
                // A device that reboots before it answers is the expected outcome, not a failure.
                if (cause instanceof ApiError && cause.offline) {
                    setHandoff(true);
                    return;
                }
                setError(cause instanceof Error ? cause.message : String(cause));
                setSaving(false);
            });
    };

    const index = STEPS.findIndex((candidate) => candidate.id === step);
    return (
        <div class="stack">
            <section class="panel panel--rail is-busy">
                <h2>Set up the gateway</h2>
                <p class="muted">
                    The gateway has no network yet. Give it the Wi-Fi it should join, and optionally
                    the MQTT broker it should talk to. It restarts once at the end.
                </p>
            </section>

            <Steps current={index} />

            {error !== null && (
                <Notice tone="error" title="The gateway refused the settings">
                    <p class="mono">{error}</p>
                </Notice>
            )}

            {step === 'wifi' && (
                <WifiStep draft={draft} update={update} onNext={() => { setStep('mqtt'); }} />
            )}
            {step === 'mqtt' && (
                <MqttStep
                    draft={draft}
                    update={update}
                    onBack={() => { setStep('wifi'); }}
                    onNext={() => { setStep('summary'); }}
                />
            )}
            {step === 'summary' && (
                <SummaryStep
                    draft={draft}
                    hostname={hostname}
                    saving={saving}
                    onBack={() => { setStep('mqtt'); }}
                    onSave={save}
                />
            )}
        </div>
    );
}

function Steps({ current }: { current: number }) {
    return (
        <ol class="steps" aria-label={`Step ${current + 1} of ${STEPS.length}`}>
            {STEPS.map((entry, index) => (
                <li
                    key={entry.id}
                    class={index <= current ? 'steps__item is-done' : 'steps__item'}
                    aria-current={index === current ? 'step' : undefined}
                >
                    <span class="steps__mark" aria-hidden="true" />
                    {entry.label}
                </li>
            ))}
        </ol>
    );
}

/* -------------------------------------------------------------- step: wifi */

function WifiStep({
    draft,
    update,
    onNext,
}: {
    draft: Config;
    update: (apply: (next: Config) => void) => void;
    onNext: () => void;
}) {
    const [scan, setScan] = useState<ScanState>({ phase: 'idle' });
    const running = useRef<AbortController | null>(null);

    const startScan = useCallback(() => {
        running.current?.abort();
        const controller = new AbortController();
        running.current = controller;
        const timer = setTimeout(() => {
            controller.abort();
        }, SCAN_TIMEOUT_MS);
        setScan({ phase: 'scanning' });
        api.scan(controller.signal)
            .then((result) => {
                clearTimeout(timer);
                setScan({
                    phase: 'done',
                    networks: [...result.networks].sort((a, b) => b.rssi - a.rssi),
                });
            })
            .catch((cause: unknown) => {
                clearTimeout(timer);
                const timedOut = controller.signal.aborted;
                setScan({
                    phase: 'failed',
                    timedOut,
                    message: cause instanceof Error ? cause.message : String(cause),
                });
            });
    }, []);

    useEffect(() => {
        startScan();
        return () => {
            running.current?.abort();
        };
    }, [startScan]);

    const ready = draft.wifi.ssid.trim() !== '';

    return (
        <>
            <section class="panel section">
                <div class="section__head">
                    <h2>Choose a network</h2>
                    <button
                        type="button"
                        class="btn btn--secondary"
                        disabled={scan.phase === 'scanning'}
                        onClick={startScan}
                    >
                        {scan.phase === 'scanning' ? 'Scanning…' : 'Scan again'}
                    </button>
                </div>

                {scan.phase === 'scanning' && (
                    <Meter value={0} label="Scanning for networks" indeterminate />
                )}
                {scan.phase === 'failed' && (
                    <Notice tone="warn" title={scan.timedOut ? 'The scan timed out' : 'The scan failed'}>
                        <p>
                            {scan.timedOut
                                ? 'The gateway did not answer within 8 seconds. Scan again, or type the network name yourself below.'
                                : 'Scan again, or type the network name yourself below.'}
                        </p>
                        <p class="mono muted">{scan.message}</p>
                    </Notice>
                )}
                {scan.phase === 'done' && scan.networks.length === 0 && (
                    <Notice tone="warn" title="No networks in range">
                        <p>
                            Move the gateway closer to the access point and scan again, or type the
                            network name yourself below. A hidden network never appears in this list.
                        </p>
                    </Notice>
                )}
                {scan.phase === 'done' && scan.networks.length > 0 && (
                    <ul class="netlist">
                        {scan.networks.map((network) => (
                            <li key={`${network.ssid}-${network.channel}`}>
                                <button
                                    type="button"
                                    class={
                                        network.ssid === draft.wifi.ssid
                                            ? 'netlist__row is-selected'
                                            : 'netlist__row'
                                    }
                                    aria-pressed={network.ssid === draft.wifi.ssid}
                                    onClick={() => {
                                        update((next) => {
                                            next.wifi.ssid = network.ssid;
                                        });
                                    }}
                                >
                                    <span class="netlist__name">{network.ssid}</span>
                                    {network.auth !== 'open' && <Lock />}
                                    <Signal rssi={network.rssi} />
                                </button>
                            </li>
                        ))}
                    </ul>
                )}
            </section>

            <section class="panel section">
                <h2>Credentials</h2>
                <div class="field-grid">
                    <TextField
                        label="Network name"
                        value={draft.wifi.ssid}
                        hint="Type it here for a hidden network."
                        autocomplete="off"
                        onInput={(value) => {
                            update((next) => {
                                next.wifi.ssid = value;
                            });
                        }}
                    />
                    <SecretField
                        label="Password"
                        value={draft.wifi.password}
                        hint="Leave empty for an open network."
                        onInput={(value) => {
                            update((next) => {
                                next.wifi.password = value;
                            });
                        }}
                    />
                    <StaticIpFields draft={draft} update={update} />
                </div>
            </section>

            <div class="row row--end">
                <button type="button" class="btn btn--primary" disabled={!ready} onClick={onNext}>
                    Continue
                </button>
            </div>
        </>
    );
}

function StaticIpFields({
    draft,
    update,
}: {
    draft: Config;
    update: (apply: (next: Config) => void) => void;
}) {
    const fixed = draft.wifi.static.enabled;
    return (
        <>
            <div class="field field--check">
                <span class="field__spacer" aria-hidden="true" />
                <div class="field__control">
                    <label class="check">
                        <input
                            type="checkbox"
                            checked={fixed}
                            onChange={(event) => {
                                const on = event.currentTarget.checked;
                                update((next) => {
                                    next.wifi.static.enabled = on;
                                });
                            }}
                        />
                        <span>Use a fixed address instead of DHCP</span>
                    </label>
                </div>
            </div>
            {fixed && (
                <>
                    <TextField
                        label="IP address"
                        value={draft.wifi.static.ip}
                        mono
                        placeholder="192.168.1.42"
                        inputMode="numeric"
                        onInput={(value) => {
                            update((next) => {
                                next.wifi.static.ip = value;
                            });
                        }}
                    />
                    <TextField
                        label="Netmask"
                        value={draft.wifi.static.mask}
                        mono
                        placeholder="255.255.255.0"
                        inputMode="numeric"
                        onInput={(value) => {
                            update((next) => {
                                next.wifi.static.mask = value;
                            });
                        }}
                    />
                    <TextField
                        label="Gateway"
                        value={draft.wifi.static.gw}
                        mono
                        placeholder="192.168.1.1"
                        inputMode="numeric"
                        onInput={(value) => {
                            update((next) => {
                                next.wifi.static.gw = value;
                            });
                        }}
                    />
                    <TextField
                        label="DNS server"
                        value={draft.wifi.static.dns}
                        mono
                        placeholder="192.168.1.1"
                        inputMode="numeric"
                        onInput={(value) => {
                            update((next) => {
                                next.wifi.static.dns = value;
                            });
                        }}
                    />
                </>
            )}
        </>
    );
}

/* -------------------------------------------------------------- step: mqtt */

function MqttStep({
    draft,
    update,
    onBack,
    onNext,
}: {
    draft: Config;
    update: (apply: (next: Config) => void) => void;
    onBack: () => void;
    onNext: () => void;
}) {
    const on = draft.mqtt.enabled;
    return (
        <>
            <section class="panel section">
                <h2>MQTT broker</h2>
                <p class="muted section__lede">
                    Optional. Skip it and the gateway still works over its web interface; you can
                    add a broker later in Settings.
                </p>
                <div class="field-grid">
                    <div class="field field--check">
                        <span class="field__spacer" aria-hidden="true" />
                        <div class="field__control">
                            <label class="check">
                                <input
                                    type="checkbox"
                                    checked={on}
                                    onChange={(event) => {
                                        const enabled = event.currentTarget.checked;
                                        update((next) => {
                                            next.mqtt.enabled = enabled;
                                        });
                                    }}
                                />
                                <span>Publish to an MQTT broker</span>
                            </label>
                        </div>
                    </div>
                    <TextField
                        label="Broker address"
                        value={draft.mqtt.uri}
                        disabled={!on}
                        mono
                        inputMode="url"
                        placeholder="mqtt://192.168.1.10:1883"
                        hint="mqtt://, mqtts://, ws:// or wss://"
                        onInput={(value) => {
                            update((next) => {
                                next.mqtt.uri = value;
                            });
                        }}
                    />
                    <TextField
                        label="Username"
                        value={draft.mqtt.username}
                        disabled={!on}
                        autocomplete="username"
                        onInput={(value) => {
                            update((next) => {
                                next.mqtt.username = value;
                            });
                        }}
                    />
                    <SecretField
                        label="Password"
                        value={draft.mqtt.password}
                        onInput={(value) => {
                            update((next) => {
                                next.mqtt.password = value;
                            });
                        }}
                    />
                    <TextField
                        label="Base topic"
                        value={draft.mqtt.base_topic}
                        disabled={!on}
                        mono
                        hint="Every topic the gateway publishes starts with this."
                        onInput={(value) => {
                            update((next) => {
                                next.mqtt.base_topic = value;
                            });
                        }}
                    />
                </div>
            </section>

            <div class="row row--end">
                <button type="button" class="btn btn--ghost" onClick={onBack}>
                    Back
                </button>
                <button type="button" class="btn btn--primary" onClick={onNext}>
                    Continue
                </button>
            </div>
        </>
    );
}

/* ----------------------------------------------------------- step: summary */

function describeSecret(value: string): string {
    if (value === MASKED) return 'unchanged';
    return value === '' ? 'none (open network)' : 'set';
}

function SummaryStep({
    draft,
    hostname,
    saving,
    onBack,
    onSave,
}: {
    draft: Config;
    hostname: string;
    saving: boolean;
    onBack: () => void;
    onSave: () => void;
}) {
    const target = draft.device.hostname === '' ? hostname : draft.device.hostname;
    return (
        <>
            <section class="panel section">
                <h2>Review</h2>
                <dl class="data-list">
                    <dt>Network</dt>
                    <dd class="mono">{draft.wifi.ssid}</dd>
                    <dt>Password</dt>
                    <dd>{describeSecret(draft.wifi.password)}</dd>
                    <dt>Address</dt>
                    <dd class="mono">
                        {draft.wifi.static.enabled ? draft.wifi.static.ip : 'from DHCP'}
                    </dd>
                    <dt>MQTT</dt>
                    <dd class="mono">{draft.mqtt.enabled ? draft.mqtt.uri : 'not used'}</dd>
                </dl>
            </section>

            <section class="panel panel--rail is-warn">
                <h2>What happens when you save</h2>
                <p>
                    The gateway stores these settings and restarts. It leaves its own{' '}
                    <span class="mono">ESP-DALI-GW</span> network to join{' '}
                    <span class="mono">{draft.wifi.ssid}</span>, so this page stops responding a few
                    seconds from now. That is the handover, not a failure.
                </p>
                <p>
                    Afterwards the gateway answers at{' '}
                    <span class="mono">http://{target}.local/</span> on your normal network.
                </p>
            </section>

            <div class="row row--end">
                <button type="button" class="btn btn--ghost" disabled={saving} onClick={onBack}>
                    Back
                </button>
                <button type="button" class="btn btn--primary" disabled={saving} onClick={onSave}>
                    {saving ? 'Saving…' : 'Save and restart'}
                </button>
            </div>
        </>
    );
}

/* ------------------------------------------------------------- the handoff */

/**
 * The one moment where the UI has to explain a failure the user is about to see. The page loses
 * the device on purpose; the probe below only ever reports good news (a reconfigure that stayed on
 * the same network), and its silence is the documented, expected outcome.
 */
function Handoff({ ssid, hostname }: { ssid: string; hostname: string }) {
    const [elapsed, setElapsed] = useState(0);
    const [back, setBack] = useState(false);

    useEffect(() => {
        const tick = setInterval(() => {
            setElapsed((value) => value + 1);
        }, 1000);
        const probe = setInterval(() => {
            api.info()
                .then(() => {
                    setBack(true);
                })
                .catch(() => {
                    /* expected: the gateway is on another network now */
                });
        }, 3000);
        return () => {
            clearInterval(tick);
            clearInterval(probe);
        };
    }, []);

    const url = `http://${hostname}.local/`;

    if (back) {
        return (
            <section class="panel panel--rail is-ok handoff">
                <h2>The gateway is back</h2>
                <p>It answered at this address again, so nothing more is needed.</p>
                <p>
                    <a class="btn btn--primary" href={url}>
                        Open {url}
                    </a>
                </p>
            </section>
        );
    }

    return (
        <section class="panel panel--rail is-busy handoff">
            <h2>Saved. The gateway is restarting.</h2>
            <p class="handoff__url mono">{url}</p>
            <p>
                This page will stop responding within a few seconds. The gateway is shutting down its
                own Wi-Fi network to join <span class="mono">{ssid}</span> — an error here is the
                handover finishing, not something going wrong.
            </p>
            <ol class="handoff__steps">
                <li>
                    Reconnect this phone or laptop to <span class="mono">{ssid}</span>.
                </li>
                <li>
                    Open <span class="mono">{url}</span>
                </li>
            </ol>
            <p class="muted">
                Restarting and joining the network usually takes under 30 seconds.{' '}
                <span class="mono">{elapsed} s</span> so far. If the address above does not answer
                after a minute, look for the gateway&apos;s IP in your router&apos;s client list —
                some networks block <span class="mono">.local</span> names.
            </p>
        </section>
    );
}

/* ------------------------------------------------------------------- icons */

function Signal({ rssi }: { rssi: number }) {
    const bars = signalBars(rssi);
    return (
        <span class="signal" title={`${rssi} dBm`}>
            <svg width="18" height="14" viewBox="0 0 18 14" aria-hidden="true">
                {[0, 1, 2, 3].map((index) => (
                    <rect
                        key={index}
                        x={index * 4.5}
                        y={11 - index * 3.4}
                        width="3"
                        height={3 + index * 3.4}
                        rx="1"
                        fill="currentColor"
                        opacity={index < bars ? 1 : 0.25}
                    />
                ))}
            </svg>
            <span class="visually-hidden">
                Signal {signalLabel(rssi)}, {rssi} dBm
            </span>
        </span>
    );
}

function Lock() {
    return (
        <span class="netlist__lock">
            <svg width="12" height="14" viewBox="0 0 12 14" aria-hidden="true">
                <path
                    d="M3 6V4a3 3 0 0 1 6 0v2"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="1.5"
                />
                <rect x="1" y="6" width="10" height="7" rx="1.5" fill="currentColor" />
            </svg>
            <span class="visually-hidden">Secured</span>
        </span>
    );
}
