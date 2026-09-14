/**
 * About and diagnostics (SPEC §10 view 6). Live counters refresh on their own; the log tail that
 * the spec also lists arrives with the SSE stream in M2.
 */

import { useEffect, useState } from 'preact/hooks';

import { api, type Info, type WifiMode } from '../api.ts';
import { formatBytes, formatUptime, signalLabel } from '../config.ts';

const REPO = 'https://github.com/calaos/esp_dali_gw';
const REFRESH_MS = 5000;

const WIFI_MODE: Record<WifiMode, string> = {
    sta: 'Joined your network',
    ap: 'Running its own network',
    apsta: 'Own network and yours',
};

export function About({ info: initial }: { info: Info }) {
    const [info, setInfo] = useState<Info>(initial);
    const [stale, setStale] = useState(false);

    useEffect(() => {
        const timer = setInterval(() => {
            api.info()
                .then((next) => {
                    setInfo(next);
                    setStale(false);
                })
                .catch(() => {
                    setStale(true);
                });
        }, REFRESH_MS);
        return () => {
            clearInterval(timer);
        };
    }, []);

    const { status, build } = info;
    return (
        <div class="stack">
            {stale && (
                <section class="panel panel--rail is-offline">
                    <p>
                        The numbers below stopped updating — the gateway is not answering. They are
                        the last values it reported.
                    </p>
                </section>
            )}

            <section class="panel section">
                <h2>Firmware</h2>
                <dl class="data-list">
                    <dt>Version</dt>
                    <dd class="mono">{build?.version ?? status.fw}</dd>
                    {build?.sha !== undefined && (
                        <>
                            <dt>Build</dt>
                            <dd class="mono">{build.sha}</dd>
                        </>
                    )}
                    {build?.date !== undefined && (
                        <>
                            <dt>Built</dt>
                            <dd class="mono">{build.date}</dd>
                        </>
                    )}
                    <dt>ESP-IDF</dt>
                    <dd class="mono">{build?.idf ?? status.idf}</dd>
                    <dt>Chip</dt>
                    <dd class="mono">{build?.target ?? 'esp32c6'}</dd>
                </dl>
            </section>

            <section class="panel section">
                <h2>Running</h2>
                <dl class="data-list">
                    <dt>Uptime</dt>
                    <dd class="mono">{formatUptime(status.uptime_s)}</dd>
                    <dt>Last restart</dt>
                    <dd class="mono">{info.reset_reason ?? 'unknown'}</dd>
                    <dt>Free memory</dt>
                    <dd class="mono">{formatBytes(info.free_heap)}</dd>
                    <dt>Lowest since boot</dt>
                    <dd class="mono">
                        {info.min_free_heap === undefined ? '—' : formatBytes(info.min_free_heap)}
                    </dd>
                </dl>
            </section>

            <section class="panel section">
                <h2>Network</h2>
                <dl class="data-list">
                    <dt>Mode</dt>
                    <dd>{WIFI_MODE[info.mode]}</dd>
                    <dt>Address</dt>
                    <dd class="mono">{status.ip}</dd>
                    <dt>Hostname</dt>
                    <dd class="mono">{info.hostname ?? '—'}</dd>
                    <dt>Signal</dt>
                    <dd class="mono">
                        {status.rssi === undefined
                            ? 'not on a network'
                            : `${status.rssi} dBm, ${signalLabel(status.rssi)}`}
                    </dd>
                    <dt>MAC</dt>
                    <dd class="mono">{status.mac}</dd>
                    <dt>Device ID</dt>
                    <dd class="mono">{info.device_id ?? '—'}</dd>
                </dl>
            </section>

            <section class="panel section">
                <h2>Source</h2>
                <p>
                    This gateway&apos;s firmware and this page are open source.{' '}
                    <a href={REPO} rel="noreferrer">
                        {REPO.replace('https://', '')}
                    </a>
                </p>
            </section>
        </div>
    );
}
