import path from "path"
import tailwindcss from "@tailwindcss/vite"
import react from "@vitejs/plugin-react"
import { defineConfig } from "vite"

export default defineConfig({
  // Relative asset URLs so the same build works served from the device root and
  // from the relay's /devices/<id>/ subpath — no build-time prefix, no per-device
  // build. Safe because the UI is a single page: if client-side routing with real
  // paths is ever added, a relative base breaks on deep routes and this needs
  // revisiting (see the remote-access relay design).
  base: "./",
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
  },
  build: {
    outDir: "../www",
    emptyOutDir: true,
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
  },
})
