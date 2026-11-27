import { defineConfig } from 'vite';

export default defineConfig(() => {
  const target = (process.env.BAMBU_BRIDGE || 'http://127.0.0.1:8080').replace(/\/$/, '');
  const proxy = {
    target,
    changeOrigin: true,
    secure: false,
    ws: true,
    configure(server: { on: (event: string, handler: (req: { setHeader: (name: string, value: string) => void }) => void) => void }) {
      const stamp = (req: { setHeader: (name: string, value: string) => void }) => {
        req.setHeader('origin', target);
      };
      server.on('proxyReq', stamp);
      server.on('proxyReqWs', stamp);
    },
  };
  return {
    server: {
      host: '127.0.0.1',
      port: 5173,
      strictPort: true,
      proxy: {
        '/api': proxy,
        '/camera.jpg': proxy,
        '/camera.ts': proxy,
        '/camera.html': proxy,
        '/camera': proxy,
      },
    },
    preview: {
      host: '127.0.0.1',
      port: 4173,
    },
    build: {
      assetsDir: 'assets',
      rollupOptions: {
        output: {
          assetFileNames: 'assets/[name]-[hash][extname]',
          chunkFileNames: 'assets/[name]-[hash].js',
          entryFileNames: 'assets/[name]-[hash].js',
        },
      },
    },
  };
});
