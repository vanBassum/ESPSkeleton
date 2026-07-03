# UI errors via shadcn toast

Errors should surface as shadcn/ui toasts (sonner), not as red inline
error text scattered through the layout (raised 2026-07-03 — e.g. the
FirmwarePage upload/download error lines).

Sweep candidates: FirmwarePage (upload/download errors), SettingsPage
(save errors), wifi scan failures, and any future command failures.
Inline text stays only where the error is tied to a specific input
field (validation).
