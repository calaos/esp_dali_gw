/**
 * Settings (SPEC §10 view 5, M1 subset): the whole configuration document in one scrollable form,
 * plus the maintenance actions that do not belong to any single field.
 */

import { useState } from 'preact/hooks';

import { api, ApiError, type Config } from '../api.ts';
import { changedPaths, cloneConfig, MASKED, needsReboot, patchFrom } from '../config.ts';
import {
    CheckField,
    ConfirmButton,
    Meter,
    Notice,
    NumberField,
    SecretField,
    Section,
    SelectField,
    TextField,
    TypeToConfirm,
} from '../ui.tsx';

type SaveState =
    | { phase: 'clean' }
    | { phase: 'saving' }
    | { phase: 'saved'; rebootRequired: boolean }
    | { phase: 'failed'; message: string };

export function Settings({ config, onReload }: { config: Config; onReload: () => void }) {
    const [draft, setDraft] = useState<Config>(() => cloneConfig(config));
    const [loaded, setLoaded] = useState<Config>(config);
    const [save, setSave] = useState<SaveState>({ phase: 'clean' });

    // A fresh document arrived (a save, or a restore from backup): rebase the form on it. The save
    // state is deliberately left alone so the result of what just happened stays on screen.
    if (loaded !== config) {
        setLoaded(config);
        setDraft(cloneConfig(config));
    }

    const update = (apply: (next: Config) => void) => {
        setDraft((current) => {
            const next = cloneConfig(current);
            apply(next);
            return next;
        });
        setSave({ phase: 'clean' });
    };

    const changed = changedPaths(config, draft);
    const restartNeeded = changed.some(needsReboot);

    const submit = () => {
        setSave({ phase: 'saving' });
        api.saveConfig(patchFrom(config, draft))
            .then((result) => {
                setSave({ phase: 'saved', rebootRequired: result.reboot_required });
                onReload();
            })
            .catch((cause: unknown) => {
                setSave({
                    phase: 'failed',
                    message: cause instanceof Error ? cause.message : String(cause),
                });
            });
    };

    return (
        <div class="stack">
            {save.phase === 'failed' && (
                <Notice tone="error" title="The gateway refused the settings">
                    <p class="mono">{save.message}</p>
                </Notice>
            )}
            {save.phase === 'saved' && (
                <Notice tone={save.rebootRequired ? 'warn' : 'ok'} title="Settings saved">
                    {save.rebootRequired ? (
                        <p>Some of what you changed only takes effect after a restart.</p>
                    ) : (
                        <p>They are in effect now.</p>
                    )}
                </Notice>
            )}

            <Section title="Device">
                <TextField
                    label="Name"
                    value={draft.device.name}
                    hint="Shown in Home Assistant and in this page's title."
                    onInput={(value) => {
                        update((next) => {
                            next.device.name = value;
                        });
                    }}
                />
                <TextField
                    label="Hostname"
                    value={draft.device.hostname}
                    mono
                    hint="The gateway answers at http://<hostname>.local/"
                    onInput={(value) => {
                        update((next) => {
                            next.device.hostname = value;
                        });
                    }}
                />
                <TextField
                    label="Time zone"
                    value={draft.device.timezone}
                    mono
                    placeholder="Europe/Paris"
                    onInput={(value) => {
                        update((next) => {
                            next.device.timezone = value;
                        });
                    }}
                />
            </Section>

            <Section
                title="Wi-Fi"
                restart
                description="The gateway restarts to apply anything you change here, and drops off the network while it does."
            >
                <TextField
                    label="Network name"
                    value={draft.wifi.ssid}
                    onInput={(value) => {
                        update((next) => {
                            next.wifi.ssid = value;
                        });
                    }}
                />
                <SecretField
                    label="Password"
                    value={draft.wifi.password}
                    onInput={(value) => {
                        update((next) => {
                            next.wifi.password = value;
                        });
                    }}
                />
                <CheckField
                    label="Use a fixed address instead of DHCP"
                    checked={draft.wifi.static.enabled}
                    onChange={(checked) => {
                        update((next) => {
                            next.wifi.static.enabled = checked;
                        });
                    }}
                />
                {draft.wifi.static.enabled && (
                    <>
                        <TextField
                            label="IP address"
                            value={draft.wifi.static.ip}
                            mono
                            placeholder="192.168.1.42"
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
                            onInput={(value) => {
                                update((next) => {
                                    next.wifi.static.dns = value;
                                });
                            }}
                        />
                    </>
                )}
                <SecretField
                    label="Recovery AP password"
                    value={draft.wifi.ap_password}
                    hint="Protects the ESP-DALI-GW network the gateway raises when it cannot join yours. At least 8 characters."
                    onInput={(value) => {
                        update((next) => {
                            next.wifi.ap_password = value;
                        });
                    }}
                />
                <NumberField
                    label="Give up after"
                    value={draft.wifi.fallback_ap_timeout_s}
                    min={10}
                    max={600}
                    unit="s"
                    hint="How long to keep trying your network before raising the recovery AP."
                    onInput={(value) => {
                        update((next) => {
                            next.wifi.fallback_ap_timeout_s = value;
                        });
                    }}
                />
            </Section>

            <Section title="MQTT">
                <CheckField
                    label="Publish to an MQTT broker"
                    checked={draft.mqtt.enabled}
                    onChange={(checked) => {
                        update((next) => {
                            next.mqtt.enabled = checked;
                        });
                    }}
                />
                <TextField
                    label="Broker address"
                    value={draft.mqtt.uri}
                    disabled={!draft.mqtt.enabled}
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
                    disabled={!draft.mqtt.enabled}
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
                    label="Client ID"
                    value={draft.mqtt.client_id}
                    disabled={!draft.mqtt.enabled}
                    mono
                    onInput={(value) => {
                        update((next) => {
                            next.mqtt.client_id = value;
                        });
                    }}
                />
                <TextField
                    label="Base topic"
                    value={draft.mqtt.base_topic}
                    disabled={!draft.mqtt.enabled}
                    mono
                    hint="Every topic the gateway publishes starts with this."
                    onInput={(value) => {
                        update((next) => {
                            next.mqtt.base_topic = value;
                        });
                    }}
                />
                <NumberField
                    label="Keepalive"
                    value={draft.mqtt.keepalive_s}
                    disabled={!draft.mqtt.enabled}
                    min={5}
                    max={600}
                    unit="s"
                    onInput={(value) => {
                        update((next) => {
                            next.mqtt.keepalive_s = value;
                        });
                    }}
                />
                <SelectField
                    label="Quality of service"
                    value={String(draft.mqtt.qos)}
                    disabled={!draft.mqtt.enabled}
                    options={[
                        { value: '0', label: '0 — send once, no acknowledgement' },
                        { value: '1', label: '1 — deliver at least once' },
                        { value: '2', label: '2 — deliver exactly once' },
                    ]}
                    onInput={(value) => {
                        update((next) => {
                            next.mqtt.qos = Number(value);
                        });
                    }}
                />
                <CheckField
                    label="Retain state topics"
                    checked={draft.mqtt.retain_state}
                    disabled={!draft.mqtt.enabled}
                    hint="Keeps the last known level of every fitting on the broker, so a restarting client sees it immediately."
                    onChange={(checked) => {
                        update((next) => {
                            next.mqtt.retain_state = checked;
                        });
                    }}
                />
                <CheckField
                    label="Announce fittings to Home Assistant"
                    checked={draft.mqtt.ha_discovery.enabled}
                    disabled={!draft.mqtt.enabled}
                    onChange={(checked) => {
                        update((next) => {
                            next.mqtt.ha_discovery.enabled = checked;
                        });
                    }}
                />
                <TextField
                    label="Discovery prefix"
                    value={draft.mqtt.ha_discovery.prefix}
                    disabled={!draft.mqtt.enabled || !draft.mqtt.ha_discovery.enabled}
                    mono
                    onInput={(value) => {
                        update((next) => {
                            next.mqtt.ha_discovery.prefix = value;
                        });
                    }}
                />
            </Section>

            <Section
                title="Web access"
                restart
                description="Ask for a username and password before showing this interface."
            >
                <CheckField
                    label="Require a password"
                    checked={draft.http.auth.enabled}
                    onChange={(checked) => {
                        update((next) => {
                            next.http.auth.enabled = checked;
                        });
                    }}
                />
                <TextField
                    label="Username"
                    value={draft.http.auth.username}
                    disabled={!draft.http.auth.enabled}
                    autocomplete="username"
                    onInput={(value) => {
                        update((next) => {
                            next.http.auth.username = value;
                        });
                    }}
                />
                <SecretField
                    label="Password"
                    value={draft.http.auth.password}
                    onInput={(value) => {
                        update((next) => {
                            next.http.auth.password = value;
                        });
                    }}
                />
            </Section>

            <Section
                title="DALI bus"
                description="The wiring matches the Pico-DALI2 board out of the box. Change it only if you rewired the header."
            >
                <NumberField
                    label="Transmit pin"
                    value={draft.dali.tx_gpio}
                    min={0}
                    max={30}
                    restart
                    hint="GPIO number. Driving it low asserts the bus."
                    onInput={(value) => {
                        update((next) => {
                            next.dali.tx_gpio = value;
                        });
                    }}
                />
                <NumberField
                    label="Receive pin"
                    value={draft.dali.rx_gpio}
                    min={0}
                    max={30}
                    restart
                    onInput={(value) => {
                        update((next) => {
                            next.dali.rx_gpio = value;
                        });
                    }}
                />
                <CheckField
                    label="Invert the transmit signal"
                    checked={draft.dali.invert_tx}
                    onChange={(checked) => {
                        update((next) => {
                            next.dali.invert_tx = checked;
                        });
                    }}
                />
                <CheckField
                    label="Invert the receive signal"
                    checked={draft.dali.invert_rx}
                    onChange={(checked) => {
                        update((next) => {
                            next.dali.invert_rx = checked;
                        });
                    }}
                />
                <NumberField
                    label="Poll fittings every"
                    value={draft.dali.poll_interval_s}
                    min={0}
                    max={3600}
                    unit="s"
                    hint="0 stops polling; the gateway then only reports what it is told to change."
                    onInput={(value) => {
                        update((next) => {
                            next.dali.poll_interval_s = value;
                        });
                    }}
                />
                <CheckField
                    label="Scan the bus at startup"
                    checked={draft.dali.scan_on_boot}
                    onChange={(checked) => {
                        update((next) => {
                            next.dali.scan_on_boot = checked;
                        });
                    }}
                />
                <NumberField
                    label="Identify blink"
                    value={draft.dali.identify_blink_ms}
                    min={100}
                    max={5000}
                    unit="ms"
                    onInput={(value) => {
                        update((next) => {
                            next.dali.identify_blink_ms = value;
                        });
                    }}
                />
            </Section>

            <Section title="Status light">
                <CheckField
                    label="Use the status light"
                    checked={draft.led.enabled}
                    onChange={(checked) => {
                        update((next) => {
                            next.led.enabled = checked;
                        });
                    }}
                />
                <NumberField
                    label="LED pin"
                    value={draft.led.gpio}
                    min={0}
                    max={30}
                    restart
                    disabled={!draft.led.enabled}
                    onInput={(value) => {
                        update((next) => {
                            next.led.gpio = value;
                        });
                    }}
                />
                <NumberField
                    label="Brightness"
                    value={draft.led.brightness}
                    min={0}
                    max={255}
                    disabled={!draft.led.enabled}
                    hint="0-255. The default of 32 is readable in a dark cabinet without lighting up the room."
                    onInput={(value) => {
                        update((next) => {
                            next.led.brightness = value;
                        });
                    }}
                />
            </Section>

            <BackupSection config={config} onReload={onReload} />
            <FirmwareSection />
            <MaintenanceSection />

            <SaveBar
                changed={changed}
                restartNeeded={restartNeeded}
                saving={save.phase === 'saving'}
                onSave={submit}
                onDiscard={() => {
                    setDraft(cloneConfig(config));
                    setSave({ phase: 'clean' });
                }}
            />
        </div>
    );
}

function SaveBar({
    changed,
    restartNeeded,
    saving,
    onSave,
    onDiscard,
}: {
    changed: string[];
    restartNeeded: boolean;
    saving: boolean;
    onSave: () => void;
    onDiscard: () => void;
}) {
    if (changed.length === 0) return null;
    return (
        <div class={`savebar ${restartNeeded ? 'is-warn' : 'is-busy'}`} role="region" aria-label="Unsaved changes">
            <p class="savebar__text">
                {changed.length === 1 ? '1 change' : `${changed.length} changes`}
                {restartNeeded ? ', and the gateway will restart to apply them' : ''}
            </p>
            <button type="button" class="btn btn--ghost" disabled={saving} onClick={onDiscard}>
                Discard
            </button>
            <button type="button" class="btn btn--primary" disabled={saving} onClick={onSave}>
                {saving ? 'Saving…' : restartNeeded ? 'Save and restart' : 'Save'}
            </button>
        </div>
    );
}

/* -------------------------------------------------------- backup / restore */

function BackupSection({ config, onReload }: { config: Config; onReload: () => void }) {
    const [secrets, setSecrets] = useState(false);
    const [state, setState] = useState<{ tone: 'ok' | 'error'; message: string } | null>(null);

    const download = () => {
        api.exportConfig(secrets)
            .then((document_) => {
                const blob = new Blob([JSON.stringify(document_, null, 2)], {
                    type: 'application/json',
                });
                const url = URL.createObjectURL(blob);
                const anchor = Object.assign(window.document.createElement('a'), {
                    href: url,
                    download: `${config.device.hostname || 'dali-gateway'}.json`,
                });
                anchor.click();
                URL.revokeObjectURL(url);
                setState({ tone: 'ok', message: 'Backup downloaded.' });
            })
            .catch((cause: unknown) => {
                setState({
                    tone: 'error',
                    message: cause instanceof Error ? cause.message : String(cause),
                });
            });
    };

    const restore = (file: File) => {
        file.text()
            .then((text) => api.importConfig(JSON.parse(text)))
            .then((result) => {
                setState({
                    tone: 'ok',
                    message: result.reboot_required
                        ? 'Settings restored. Restart the gateway to apply them.'
                        : 'Settings restored.',
                });
                onReload();
            })
            .catch((cause: unknown) => {
                setState({
                    tone: 'error',
                    message:
                        cause instanceof SyntaxError
                            ? 'That file is not a gateway backup.'
                            : cause instanceof Error
                              ? cause.message
                              : String(cause),
                });
            });
    };

    return (
        <section class="panel section">
            <h2>Backup and restore</h2>
            <p class="muted section__lede">
                A backup is one JSON file holding everything on this page. Restoring one replaces
                the current settings.
            </p>
            {state !== null && (
                <Notice tone={state.tone}>
                    <p>{state.message}</p>
                </Notice>
            )}
            <label class="check">
                <input
                    type="checkbox"
                    checked={secrets}
                    onChange={(event) => {
                        setSecrets(event.currentTarget.checked);
                    }}
                />
                <span>Include passwords in the backup</span>
            </label>
            <p class="muted field__hint">
                Without this the file carries{' '}
                <span class="mono">{MASKED}</span> in place of every password, and restoring it
                keeps the ones already on the device.
            </p>
            <div class="row">
                <button type="button" class="btn btn--secondary" onClick={download}>
                    Download backup
                </button>
                <label class="btn btn--secondary file-button">
                    Restore from file
                    <input
                        type="file"
                        accept="application/json,.json"
                        class="visually-hidden"
                        onChange={(event) => {
                            const file = event.currentTarget.files?.[0];
                            event.currentTarget.value = '';
                            if (file) restore(file);
                        }}
                    />
                </label>
            </div>
        </section>
    );
}

/* ------------------------------------------------------------------- OTA */

type OtaState =
    | { phase: 'idle' }
    | { phase: 'uploading'; progress: number; name: string }
    | { phase: 'verifying' }
    | { phase: 'done'; nextBoot: string | undefined }
    | { phase: 'failed'; message: string };

function FirmwareSection() {
    const [state, setState] = useState<OtaState>({ phase: 'idle' });
    const [cancel, setCancel] = useState<(() => void) | null>(null);

    const start = (file: File) => {
        setState({ phase: 'uploading', progress: 0, name: file.name });
        const transfer = api.ota(file, (fraction) => {
            setState((current) =>
                current.phase === 'uploading' ? { ...current, progress: fraction } : current,
            );
            // The device still has to verify the image once the last byte lands.
            if (fraction >= 1) setState({ phase: 'verifying' });
        });
        setCancel(() => transfer.abort);
        transfer.promise
            .then((result) => {
                setState({ phase: 'done', nextBoot: result.next_boot });
                setCancel(null);
            })
            .catch((cause: unknown) => {
                setState({
                    phase: 'failed',
                    message:
                        cause instanceof ApiError && cause.offline
                            ? 'The connection dropped part-way through. The gateway keeps running the firmware it already had; try again.'
                            : cause instanceof Error
                              ? cause.message
                              : String(cause),
                });
                setCancel(null);
            });
    };

    const busy = state.phase === 'uploading' || state.phase === 'verifying';

    return (
        <section class="panel section">
            <h2>Firmware update</h2>
            <p class="muted section__lede">
                Pick a <span class="mono">.bin</span> built for this gateway. It is written to the
                spare slot, so a failed upload leaves the running firmware untouched.
            </p>

            {state.phase === 'uploading' && (
                <>
                    <Meter value={state.progress} label={`Uploading ${state.name}`} />
                    <p class="muted">Do not close this page or power the gateway off.</p>
                </>
            )}
            {state.phase === 'verifying' && (
                <>
                    <Meter value={0} label="Checking the image" indeterminate />
                    <p class="muted">Upload finished. The gateway is checking the image.</p>
                </>
            )}
            {state.phase === 'failed' && (
                <Notice tone="error" title="The update did not complete">
                    <p>{state.message}</p>
                </Notice>
            )}
            {state.phase === 'done' && (
                <Notice tone="ok" title="Firmware written">
                    <p>
                        {state.nextBoot === undefined
                            ? 'The gateway will start the new firmware after a restart.'
                            : `The gateway will start from ${state.nextBoot} after a restart.`}
                    </p>
                </Notice>
            )}

            <div class="row">
                <label class={busy ? 'btn btn--secondary file-button is-disabled' : 'btn btn--secondary file-button'}>
                    Choose firmware file
                    <input
                        type="file"
                        accept=".bin,application/octet-stream"
                        class="visually-hidden"
                        disabled={busy}
                        onChange={(event) => {
                            const file = event.currentTarget.files?.[0];
                            event.currentTarget.value = '';
                            if (file) start(file);
                        }}
                    />
                </label>
                {cancel !== null && (
                    <button type="button" class="btn btn--ghost" onClick={cancel}>
                        Cancel upload
                    </button>
                )}
                {state.phase === 'done' && <RestartButton label="Restart into the new firmware" />}
            </div>
        </section>
    );
}

/* ----------------------------------------------------------- maintenance */

function RestartButton({ label }: { label: string }) {
    const [done, setDone] = useState(false);
    if (done) {
        return <span class="muted">Restarting. This page will reconnect on its own.</span>;
    }
    return (
        <ConfirmButton
            label={label}
            question="The gateway goes offline for about ten seconds. Any DALI operation running now is abandoned."
            confirmLabel="Restart now"
            onConfirm={() => {
                setDone(true);
                api.reboot().catch(() => {
                    /* the device often reboots before it answers */
                });
                setTimeout(() => {
                    location.reload();
                }, 12000);
            }}
        />
    );
}

function MaintenanceSection() {
    const [reset, setReset] = useState(false);

    return (
        <section class="panel panel--rail is-error section">
            <h2>Restart and reset</h2>
            <div class="stack">
                <div>
                    <RestartButton label="Restart the gateway" />
                </div>
                <div>
                    {reset ? (
                        <Notice tone="error" title="Erasing settings">
                            <p>
                                The gateway is restarting into setup mode. It will raise its own
                                <span class="mono"> ESP-DALI-GW </span> network again — join that and
                                open <span class="mono">http://192.168.4.1/</span>.
                            </p>
                        </Notice>
                    ) : (
                        <TypeToConfirm
                            word="RESET"
                            label="Factory reset"
                            question="This erases the Wi-Fi credentials, the MQTT broker, every fitting name and every password on the gateway. It cannot be undone, and you will have to set the gateway up again over its own Wi-Fi network."
                            confirmLabel="Erase everything"
                            onConfirm={() => {
                                setReset(true);
                                api.factoryReset().catch(() => {
                                    /* the device often reboots before it answers */
                                });
                            }}
                        />
                    )}
                </div>
            </div>
        </section>
    );
}
