import { defineConfig } from 'vite';

// The bridge admits a browser only when Host and Origin are its own
// localhost:port. A Vite page on another port would be rejected, so the proxy
// rewrites both. Bind loopback only: the proxy is itself a path onto the bridge.
const target = process.env.BAMBU_BRIDGE || 'http://127.0.0.1:8080';
const origin = new URL(target).origin;

function bridgeProxy(ws = false) {
  return {
    target,
    changeOrigin: true,
    secure: false,
    ws,
    timeout: 0,
    proxyTimeout: 0,
    configure(proxy) {
      const rewrite = (req) => {
        req.removeHeader('origin');
        req.setHeader('origin', origin);
      };
      proxy.on('proxyReq', rewrite);
      proxy.on('proxyReqWs', rewrite);
    },
  };
}

const proxy = {
  '/api': bridgeProxy(true),
  '/camera.jpg': bridgeProxy(),
  '/camera.ts': bridgeProxy(),
  '/camera.html': bridgeProxy(),
  '/camera': bridgeProxy(),
};

export default defineConfig({
  server: { host: '127.0.0.1', port: 5173, proxy },
  preview: { host: '127.0.0.1', port: 4173, proxy },
});
