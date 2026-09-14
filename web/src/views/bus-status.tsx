/**
 * The bus banner (SPEC §10 view 2). Every screen that can start a bus operation shows it, so the
 * answer to "why is nothing happening" is always on the page the user is already looking at.
 */

import { useState } from 'preact/hooks';

import { api, type BusOperation, errorText } from '../api.ts';
import { timeAgo } from '../dali.ts';
import { useStore } from '../store.ts';
import { Meter } from '../ui.tsx';

const OPERATION: Record<BusOperation, string> = {
    scan: 'Scanning the bus',
    commission: 'Commissioning',
    poll_all: 'Reading every fitting',
    bus_check: 'Checking the bus',
};

export function BusBanner() {
    const { bus, progress } = useStore();
    const [cancelling, setCancelling] = useState(false);
    const [failure, setFailure] = useState<string | null>(null);

    if (bus === null) {
        return (
            <section class="panel panel--rail is-busy">
                <p class="muted">Reading the bus…</p>
            </section>
        );
    }

    // SPEC §2: the Pico-DALI2 has no internal supply. An unpowered bus is a normal site condition
    // and the fix is in the cabinet, not in this UI, so the banner says where to look.
    if (!bus.powered) {
        return (
            <section class="panel panel--rail is-error">
                <div class="section__head">
                    <h2>Bus unpowered</h2>
                    <span class="badge is-error">
                        <span class="dot" />
                        No bus
                    </span>
                </div>
                <p>
                    The gateway sees no voltage on the DALI pair. It does not supply the bus itself —
                    check the DALI power supply in the cabinet. Nothing can be switched or dimmed
                    until it is back.
                </p>
                <button
                    type="button"
                    class="btn btn--secondary"
                    onClick={() => {
                        setFailure(null);
                        api.check().catch((error: unknown) => {
                            setFailure(errorText(error));
                        });
                    }}
                >
                    Check again
                </button>
                {failure !== null && <p class="muted mono">{failure}</p>}
            </section>
        );
    }

    if (bus.busy) {
        const operation = bus.operation === null ? 'Working' : OPERATION[bus.operation];
        const live = progress ?? bus.progress;
        const total = live?.total ?? 0;
        return (
            <section class="panel panel--rail is-busy" aria-live="polite">
                <div class="section__head">
                    <h2>{operation}</h2>
                    <button
                        type="button"
                        class="btn btn--secondary"
                        disabled={cancelling}
                        onClick={() => {
                            setCancelling(true);
                            setFailure(null);
                            api
                                .cancel()
                                .catch((error: unknown) => {
                                    setFailure(errorText(error));
                                })
                                .finally(() => {
                                    setCancelling(false);
                                });
                        }}
                    >
                        {cancelling ? 'Stopping' : 'Stop'}
                    </button>
                </div>
                <Meter
                    value={total > 0 ? (live?.done ?? 0) / total : 0}
                    label={operation}
                    indeterminate={total === 0}
                />
                <p class="muted mono">
                    {total > 0 ? `${live?.done ?? 0} / ${total} addresses` : 'starting'}
                    {live?.found === undefined ? '' : ` · ${live.found} found`}
                </p>
                {failure !== null && <p class="muted mono">{failure}</p>}
            </section>
        );
    }

    return (
        <section class="panel panel--rail is-ok">
            <div class="section__head">
                <h2>Bus powered</h2>
                <span class="badge is-ok">
                    <span class="dot" />
                    Idle
                </span>
            </div>
            <p class="muted">
                {bus.gear_count} control gear · last scan {timeAgo(bus.last_scan)}
            </p>
        </section>
    );
}
