/**
 * Bus tools (SPEC §10 view 4): the operations that talk to the whole bus at once, the raw console
 * and the passive monitor. Everything that writes to the bus is destructive-adjacent, so nothing
 * starts without a deliberate act.
 */

import { Fragment } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';

import { api, type CommissionData, type CommissionMode, type Result } from '../api.ts';
import { addrLabel, asRecord, bootTime, hexByte, isFrame, MAX_ADDR } from '../dali.ts';
import { describeWrite } from '../level.ts';
import {
    clearFrames,
    copyText,
    downloadText,
    type MonitorFrame,
    monitorText,
    setListening,
    setPaused,
    useMonitor,
} from '../monitor.ts';
import { useStore } from '../store.ts';
import {
    CheckField,
    ConfirmButton,
    Notice,
    NumberField,
    Segmented,
    TextField,
    TypeToConfirm,
} from '../ui.tsx';
import { BusBanner } from './bus-status.tsx';

export function BusTools() {
    const { bus } = useStore();
    const busy = bus?.busy === true;
    const powered = bus?.powered !== false;

    return (
        <div class="stack">
            <BusBanner />
            <Scan busy={busy} powered={powered} />
            <Commission busy={busy} powered={powered} />
            <RawConsole busy={busy} powered={powered} />
            <Monitor />
        </div>
    );
}

/* ------------------------------------------------------------------- scan */

function Scan({ busy, powered }: { busy: boolean; powered: boolean }) {
    const { result } = useStore();
    const [deep, setDeep] = useState<'normal' | 'deep'>('normal');
    const [failure, setFailure] = useState<string | null>(null);
    const [starting, setStarting] = useState(false);

    const start = (): void => {
        setStarting(true);
        setFailure(null);
        api
            .startScan(deep === 'deep')
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setStarting(false);
            });
    };

    const last = result?.action === 'scan' ? result : null;

    return (
        <section class="panel section">
            <h2>Scan the bus</h2>
            <p class="muted section__lede">
                Walks all 64 short addresses and asks each one whether anything is there. A normal
                scan reads level and status. A deep scan also reads the fade settings, the group
                membership, all sixteen scene levels and the manufacturer data — everything the
                other screens show as &ldquo;read by a deep scan&rdquo; — and takes several minutes.
            </p>

            <Segmented
                label="Scan depth"
                value={deep}
                disabled={busy}
                options={[
                    { value: 'normal', label: 'Normal' },
                    { value: 'deep', label: 'Deep' },
                ]}
                onSelect={setDeep}
            />

            <div class="row tools__actions">
                <button
                    type="button"
                    class="btn btn--primary"
                    disabled={busy || starting || !powered}
                    onClick={start}
                >
                    {busy ? 'Bus is working' : 'Start scan'}
                </button>
            </div>

            {failure !== null && (
                <Notice tone="error" title="The scan did not start">
                    <p class="mono">{failure}</p>
                </Notice>
            )}

            {last !== null && <Outcome result={last} verb="scan" />}
        </section>
    );
}

/* ----------------------------------------------------------- commissioning */

function Commission({ busy, powered }: { busy: boolean; powered: boolean }) {
    const { result, gears } = useStore();
    const [mode, setMode] = useState<CommissionMode>('unaddressed');
    const [startAddr, setStartAddr] = useState(0);
    const [failure, setFailure] = useState<string | null>(null);

    const start = (): void => {
        setFailure(null);
        api.startCommission(mode, startAddr).catch((error: unknown) => {
            setFailure(describeWrite(error));
        });
    };

    const last = result?.action === 'commission' ? result : null;
    const known = gears.filter((gear) => gear.present).length;

    return (
        <section class={`panel panel--rail section ${mode === 'all' ? 'is-error' : 'is-warn'}`}>
            <h2>Give fittings an address</h2>
            <p class="muted section__lede">
                Commissioning makes every fitting pick a random long address, then hands out short
                addresses one at a time. It is the only way a brand-new fitting gets onto the bus.
            </p>

            <Segmented
                label="What to address"
                value={mode}
                disabled={busy}
                options={[
                    { value: 'unaddressed', label: 'New fittings only' },
                    { value: 'all', label: 'Everything on the bus' },
                ]}
                onSelect={setMode}
            />

            {mode === 'unaddressed' ? (
                <p>
                    Only fittings that have no short address yet are touched. The {known} fitting(s)
                    already on the bus keep the addresses they have.
                </p>
            ) : (
                <Notice tone="error" title="This re-addresses the whole installation">
                    <p>
                        Every fitting on this bus, including the {known} that already work, is given
                        a new short address in whatever order the bus resolves them. Names, groups,
                        scenes and anything else that refers to a fitting by number — wall switches,
                        controllers, Home Assistant entities — will point at the wrong lights
                        afterwards, and there is no undo.
                    </p>
                </Notice>
            )}

            <div class="field-grid">
                <NumberField
                    label="First address to hand out"
                    value={startAddr}
                    min={0}
                    max={MAX_ADDR}
                    disabled={busy}
                    hint="Addressing counts up from here."
                    onInput={(value) => {
                        setStartAddr(Math.min(Math.max(Math.round(value), 0), MAX_ADDR));
                    }}
                />
            </div>

            <div class="tools__actions">
                {mode === 'all' ? (
                    <TypeToConfirm
                        word="READDRESS"
                        label="Re-address everything"
                        question={`Every fitting on this bus loses its short address and is given a new one, starting at ${addrLabel(startAddr)}. Type READDRESS to confirm you want that.`}
                        confirmLabel="Re-address the whole bus"
                        cancelLabel="Leave the bus alone"
                        onConfirm={start}
                    />
                ) : (
                    <ConfirmButton
                        label="Address new fittings"
                        question={`Fittings without a short address are given one, starting at ${addrLabel(startAddr)}. Existing fittings are left alone.`}
                        confirmLabel="Start commissioning"
                        disabled={busy || !powered}
                        onConfirm={start}
                    />
                )}
            </div>

            {failure !== null && (
                <Notice tone="error" title="Commissioning did not start">
                    <p class="mono">{failure}</p>
                </Notice>
            )}

            {last !== null && <CommissionOutcome result={last} />}
        </section>
    );
}

function CommissionOutcome({ result }: { result: Result }) {
    if (!result.ok) return <Outcome result={result} verb="commissioning" />;
    const data = (result.data ?? {}) as CommissionData;
    const addresses = data.addresses ?? [];
    return (
        <Notice tone="ok" title={`${data.assigned ?? addresses.length} fitting(s) addressed`}>
            {addresses.length > 0 ? (
                <div class="table-scroll">
                    <table class="table">
                        <thead>
                            <tr>
                                <th scope="col">Address</th>
                                <th scope="col">Result</th>
                            </tr>
                        </thead>
                        <tbody>
                            {addresses.map((addr) => (
                                <tr key={addr}>
                                    <td class="mono">{addrLabel(addr)}</td>
                                    <td>Addressed</td>
                                </tr>
                            ))}
                        </tbody>
                    </table>
                </div>
            ) : (
                <p>Nothing on the bus needed an address.</p>
            )}
        </Notice>
    );
}

/** Whatever the device reported, rendered without pretending to know the shape. */
function Outcome({ result, verb }: { result: Result; verb: string }) {
    if (!result.ok) {
        const cancelled = result.error === 'cancelled';
        return (
            <Notice
                tone={cancelled ? 'warn' : 'error'}
                title={cancelled ? `The ${verb} was stopped` : `The ${verb} failed`}
            >
                <p class="mono">{result.message ?? result.error ?? 'no reason given'}</p>
            </Notice>
        );
    }
    const data = asRecord(result.data);
    return (
        <Notice tone="ok" title={`The ${verb} finished`}>
            {data !== null && (
                <dl class="data-list">
                    {Object.entries(data).map(([key, value]) => (
                        <Fragment key={key}>
                            <dt class="mono">{key}</dt>
                            <dd class="mono">{JSON.stringify(value)}</dd>
                        </Fragment>
                    ))}
                </dl>
            )}
        </Notice>
    );
}

/* ---------------------------------------------------------- raw console */

/** One send and its answer, kept together: a console that interleaves them reads backwards. */
interface Exchange {
    seq: number;
    sent: string;
    /** `null` while the frame is still on the wire. */
    reply: string | null;
    failed: boolean;
}

let lineSeq = 0;

function RawConsole({ busy, powered }: { busy: boolean; powered: boolean }) {
    const [frame, setFrame] = useState('FF08');
    const [twice, setTwice] = useState(false);
    const [expect, setExpect] = useState(false);
    const [lines, setLines] = useState<Exchange[]>([]);
    const [sending, setSending] = useState(false);

    const valid = isFrame(frame);

    const resolve = (seq: number, reply: string, failed: boolean): void => {
        setLines((current) =>
            current.map((line) => (line.seq === seq ? { ...line, reply, failed } : line)),
        );
    };

    const send = (): void => {
        const hex = frame.trim().toUpperCase();
        const options = [twice ? 'sent twice' : '', expect ? 'reply expected' : '']
            .filter((option) => option !== '')
            .join(' · ');
        lineSeq += 1;
        const seq = lineSeq;
        setSending(true);
        setLines((current) =>
            [
                { seq, sent: options === '' ? hex : `${hex}  ${options}`, reply: null, failed: false },
                ...current,
            ].slice(0, 50),
        );
        api
            .raw({ frame: hex, send_twice: twice, expect_reply: expect })
            .then((result) => {
                const reply = asRecord(result.data)?.['reply'];
                if (typeof reply === 'number') resolve(seq, `${reply}  0x${hexByte(reply)}`, false);
                else resolve(seq, expect ? 'no reply' : 'sent, no reply expected', false);
            })
            .catch((error: unknown) => {
                resolve(seq, describeWrite(error), true);
            })
            .finally(() => {
                setSending(false);
            });
    };

    return (
        <section class="panel section">
            <h2>Raw frames</h2>
            <p class="muted section__lede">
                Sends a forward frame exactly as typed — 4 hex digits for a 16-bit frame, 6 for a
                24-bit one. Nothing here is checked against the standard: this is the toolkit, and a
                wrong frame reaches every fitting on the bus.
            </p>

            <div class="field-grid">
                <TextField
                    label="Frame"
                    value={frame}
                    mono
                    hint={
                        valid
                            ? 'Address byte first, then the opcode.'
                            : 'Four or six hexadecimal digits, for example FF08.'
                    }
                    autocomplete="off"
                    onInput={setFrame}
                />
                <CheckField
                    label="Send twice"
                    checked={twice}
                    hint="Configuration commands only take effect when the same frame arrives twice."
                    onChange={setTwice}
                />
                <CheckField
                    label="Expect a reply"
                    checked={expect}
                    hint="Wait for a backward frame instead of assuming the command is silent."
                    onChange={setExpect}
                />
            </div>

            <div class="row tools__actions">
                <button
                    type="button"
                    class="btn btn--primary"
                    disabled={!valid || sending || busy || !powered}
                    onClick={send}
                >
                    {sending ? 'Sending' : 'Send frame'}
                </button>
                {lines.length > 0 && (
                    <button
                        type="button"
                        class="btn btn--ghost"
                        onClick={() => {
                            setLines([]);
                        }}
                    >
                        Clear history
                    </button>
                )}
            </div>

            {lines.length > 0 && (
                <ol class="console" aria-label="Frame history" aria-live="polite">
                    {lines.map((line) => (
                        <li key={line.seq} class="console__row">
                            <p class="console__line">
                                <span class="console__dir console__dir--tx mono">TX</span>
                                <span class="mono console__text">{line.sent}</span>
                            </p>
                            <p class="console__line">
                                <span
                                    class={`console__dir mono ${
                                        line.reply === null
                                            ? 'console__dir--wait'
                                            : line.failed
                                              ? 'console__dir--err'
                                              : 'console__dir--rx'
                                    }`}
                                >
                                    {line.reply === null ? '··' : line.failed ? 'ERR' : 'RX'}
                                </span>
                                <span class="mono console__text">
                                    {line.reply ?? 'on the wire'}
                                </span>
                            </p>
                        </li>
                    ))}
                </ol>
            )}
        </section>
    );
}

/* ---------------------------------------------------------- bus monitor */

/**
 * The passive monitor (SPEC §16 M5). It shows what *other* controllers put on the bus; the
 * gateway's own frames never come back to it, which is the one thing a user will otherwise read
 * as a bug, so the panel says it in three places: the lede, the empty state, and while the
 * gateway is busy on the bus itself.
 */
function Monitor() {
    const { bus } = useStore();
    const monitor = useMonitor();
    const { frames, seen, skipped, paused, listening } = monitor;
    const [pending, setPending] = useState(false);
    const [failure, setFailure] = useState<string | null>(null);
    const [copied, setCopied] = useState<'no' | 'yes' | 'blocked'>('no');
    const flash = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

    useEffect(
        () => () => {
            clearTimeout(flash.current);
        },
        [],
    );

    const toggle = (): void => {
        setPending(true);
        setFailure(null);
        api
            .monitor(!listening)
            .then((reply) => {
                setListening(reply.listening);
                // Stopping leaves the buffer but not a pause: a paused monitor that has also been
                // stopped has two reasons to show nothing and no way to tell them apart.
                if (!reply.listening) setPaused(false);
            })
            .catch((error: unknown) => {
                setFailure(describeWrite(error));
            })
            .finally(() => {
                setPending(false);
            });
    };

    const copy = (): void => {
        void copyText(monitorText(monitor)).then((ok) => {
            setCopied(ok ? 'yes' : 'blocked');
            clearTimeout(flash.current);
            flash.current = setTimeout(() => {
                setCopied('no');
            }, 3000);
        });
    };

    return (
        <section class={`panel panel--rail section ${listening ? 'is-busy' : ''}`}>
            <div class="section__head">
                <h2>Watch the bus</h2>
                {listening && (
                    <span class="badge is-busy">
                        <span class="dot" />
                        Listening
                    </span>
                )}
            </div>
            <p class="muted section__lede">
                Reports the frames other controllers put on the bus — a wall panel, an occupancy
                sensor, a second gateway. Frames this gateway sends are never reported back to it,
                so nothing you do from the dashboard or from Raw frames above will show up here.
            </p>

            <div class="row tools__actions">
                <button
                    type="button"
                    class={listening ? 'btn btn--secondary' : 'btn btn--primary'}
                    disabled={pending}
                    onClick={toggle}
                >
                    {listening ? 'Stop listening' : 'Start listening'}
                </button>
                {listening && (
                    <button
                        type="button"
                        class="btn btn--ghost"
                        onClick={() => {
                            setPaused(!paused);
                        }}
                    >
                        {paused ? 'Resume' : 'Pause the list'}
                    </button>
                )}
                {frames.length > 0 && (
                    <button type="button" class="btn btn--ghost" onClick={clearFrames}>
                        Clear
                    </button>
                )}
            </div>
            <p class="monitor__note muted">
                Listening is not a setting: the gateway starts with it off after every reboot.
            </p>

            {failure !== null && (
                <Notice tone="error" title="The gateway did not change the monitor">
                    <p class="mono">{failure}</p>
                </Notice>
            )}

            {listening && bus?.busy === true && (
                <Notice tone="busy" title="The gateway is on the bus itself">
                    <p>
                        Its own frames are not reported, so the list can sit still while the
                        operation above runs. Anything another controller sends still appears.
                    </p>
                </Notice>
            )}

            {frames.length === 0 ? (
                <p class="monitor__note muted">
                    {listening
                        ? 'Listening. Nothing from another controller yet.'
                        : 'Not listening yet.'}
                </p>
            ) : (
                <>
                    <div class="toolbar monitor__bar">
                        <p class="muted monitor__count">
                            {seen === frames.length
                                ? `${seen} frames`
                                : `${seen} frames, newest ${frames.length} kept`}
                            {skipped > 0 && `, ${skipped} skipped while paused`}
                        </p>
                        <div class="row">
                            <button type="button" class="btn btn--ghost" onClick={copy}>
                                {copied === 'yes' ? 'Copied' : 'Copy'}
                            </button>
                            <button
                                type="button"
                                class="btn btn--ghost"
                                onClick={() => {
                                    downloadText(monitorText(monitor), 'dali-monitor.txt');
                                }}
                            >
                                Save as text
                            </button>
                        </div>
                    </div>

                    {copied === 'blocked' && (
                        <Notice tone="warn" title="The browser refused the clipboard">
                            <p>
                                It is only offered on a secure page, and the gateway is served over
                                plain HTTP. Use Save as text instead.
                            </p>
                        </Notice>
                    )}

                    {/*
                     * Not a live region on purpose: a burst would read hundreds of rows aloud and
                     * bury the controls. The counts above it change at the same time and are the
                     * thing worth hearing. Focusable because it scrolls and holds nothing that can
                     * take focus.
                     */}
                    <ol class="console monitor" aria-label="Frames seen on the bus" tabIndex={0}>
                        {frames.map((frame) => (
                            <FrameRow key={frame.seq} frame={frame} />
                        ))}
                    </ol>
                    <p class="monitor__note muted">
                        The gateway drops frames rather than hold up its receiver, and so does this
                        page under a burst. Treat the list as a sample of the bus, not a complete
                        capture.
                    </p>
                </>
            )}
        </section>
    );
}

function FrameRow({ frame }: { frame: MonitorFrame }) {
    const { reading } = frame;
    return (
        <li class="console__row monitor__row">
            <span class="mono monitor__time">{bootTime(frame.ts)}</span>
            <span class="mono monitor__hex">{frame.hex}</span>
            <span class={`mono monitor__bits monitor__bits--${reading.kind}`}>
                {frame.bits}
                <span class="visually-hidden"> bit</span>
            </span>
            <span class={reading.decoded ? 'monitor__text' : 'monitor__text muted'}>
                {reading.text}
            </span>
        </li>
    );
}
