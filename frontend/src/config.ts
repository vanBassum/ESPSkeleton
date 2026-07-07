// Fork configuration — the only frontend file with project-specific
// constants. Everything user-visible (device name, project name) comes
// from the device at runtime; these are the things the frontend needs
// before it can talk to a device.

/** Device hostname the dev server proxies to (`pnpm dev`). */
export const DEV_HOST = "strux.local"

/** GitHub repo checked for new releases (update dot in the sidebar). */
export const GITHUB_REPO = "vanBassum/Strux"
