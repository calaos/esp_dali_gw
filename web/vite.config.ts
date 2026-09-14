import preact from '@preact/preset-vite';
import { defineConfig, type ProxyOptions } from 'vite';

// Dev-server only. Point this at a gateway on the LAN to develop against real data:
//   DEVICE_HOST=http://192.168.1.42 npm run dev
// VITE_DEVICE_HOST is accepted too so the value can live in a .env.local file.
const deviceHost =
    process.env['DEVICE_HOST'] ?? process.env['VITE_DEVICE_HOST'] ?? 'http://esp-dali-gw.local';

const proxy: ProxyOptions = { target: deviceHost, changeOrigin: true };

// SSE dies if anything between the browser and the device buffers or re-encodes the stream: ask the
// device for an unencoded body and flush every chunk instead of waiting for Nagle's timer.
const sseProxy: ProxyOptions = {
    ...proxy,
    configure: (server) => {
        server.on('proxyReq', (proxyReq) => {
            proxyReq.setHeader('accept-encoding', 'identity');
            proxyReq.setNoDelay(true);
        });
        server.on('proxyRes', (proxyRes) => {
            proxyRes.headers['cache-control'] = 'no-cache, no-transform';
            proxyRes.socket.setNoDelay(true);
        });
    },
};

export default defineConfig({
    plugins: [preact()],
    build: {
        outDir: 'dist',
        target: 'es2022',
        // The firmware serves these pre-gzipped from a SPIFFS-less embedded blob; every extra file
        // is an extra request over Wi-Fi, so keep the asset count low.
        cssCodeSplit: false,
    },
    server: {
        proxy: {
            '/api/events': sseProxy,
            '/api': proxy,
        },
    },
});
