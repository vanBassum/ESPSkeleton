# UI errors via sonner toasts — design

Closes backlog item `docs/backlog/ui-toast-errors.md`. Scope agreed 2026-07-06:
**full sweep** — not just the visible red-text errors, but every silently
swallowed error path in the frontend.

## Convention

Errors from user-initiated or page-load actions surface as
`toast.error("<what failed>", { description: e.message })` — a human headline
plus the raw device/network message as detail. Toast calls live at the call
sites in pages/components; `backend.ts` stays UI-free (centralizing there
would also toast reconnect churn, which the sidebar connection status already
covers).

Inline error text remains only where the error is tied to a specific input
(validation): the JSON editor's syntax error and the login form's two errors.

## Plumbing

- `pnpm add sonner` (the library shadcn's toast component wraps).
- `components/ui/sonner.tsx`: shadcn wrapper **without** the `next-themes`
  import the stock template assumes (this app has no theme provider);
  `theme="system"`, styled via the existing `--popover*`/`--border` CSS vars.
- `<Toaster richColors />` mounted once in `main.tsx` beside `<App />`, so it
  exists on both the login page and the app shell.

## Site-by-site

| Site | Before | After |
|---|---|---|
| FirmwarePage upload/download | inline red `<p>` per row | toast; inline state removed |
| FirmwarePage partitions load | inline red text | neutral empty-state + Retry button (persistent affordance); error detail in a toast |
| SettingsPage `handleChange` | silent catch; local state updated anyway (UI diverges from device) | toast; local state/dirty flag **not** updated on failure |
| SettingsPage `handleSave` / `handleReload` / reboot | silent catch | toast |
| SettingsPage wifi scan | failure looks like "No networks found" | toast on throw and on `ok: false` |
| ConsolePage history fetch | silent catch (empty console lies) | toast |

Deliberately unchanged: LoginPage (form-tied inline errors),
`use-device-info` 10 s poll (background poll must not toast during
disconnects), `getLoginInfo` fallback to "Strux".

Parked (noted in backlog): swap reboot `confirm()` for a shadcn AlertDialog;
success toasts.

## Verification

`pnpm typecheck`, `pnpm build`; runtime pass against a device via `pnpm dev`
where reachable.
