# UI polish (small parked items)

Parked from the toast-errors sweep (2026-07-06, spec:
`docs/superpowers/specs/2026-07-06-ui-toast-errors-design.md`):

- SettingsPage reboot still uses browser `confirm()` — the only non-shadcn
  dialog left. Swap for a shadcn AlertDialog.
- Success toasts (e.g. "Settings saved") — errors-only was a deliberate
  scope cut; decide whether quiet success is actually better before adding.
- FirmwarePage hand-rolls its `Badge`; shadcn has one.
