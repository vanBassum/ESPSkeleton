import { useCallback, useEffect, useState } from "react"
import { backend, type LedState } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { Switch } from "@/components/ui/switch"
import { LightbulbIcon } from "lucide-react"
import { toast } from "sonner"

function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

// The relay link moves on its own, so the card re-reads rather than trusting the
// state it fetched when the page opened. Slow enough to be free, quick enough that
// the board and the page never disagree for long.
const POLL_MS = 3000

// ──────────────────────────────────────────────────────────────
// The frontend half of LedManager's worked example: the board LED says whether the
// device is connected to its relay server, and this card says the same thing plus
// the switch that turns the indication off. Delete it along with LedManager once
// the product has real features.
// ──────────────────────────────────────────────────────────────
export function LedCard() {
  const connection = useConnectionStatus()
  const [state, setState] = useState<LedState | null>(null)
  const [busy, setBusy] = useState(false)

  const refresh = useCallback(() => {
    backend
      .getLed()
      .then(setState)
      .catch(() => {})
  }, [])

  useEffect(() => {
    if (connection !== "connected") return
    refresh()
    const id = setInterval(refresh, POLL_MS)
    return () => clearInterval(id)
  }, [connection, refresh])

  // Optimistic: the switch follows the finger and the device's own reply is the
  // correction, so a slow link doesn't make the control feel broken. A failure
  // rolls back rather than leaving the UI lying about the hardware.
  const toggle = (next: boolean) => {
    const previous = state
    setState((s) => (s ? { ...s, enabled: next, on: next && s.connected } : s))
    setBusy(true)
    backend
      .setLed({ enabled: next })
      .then(setState)
      .catch((e) => {
        setState(previous)
        toast.error("Failed to switch the LED", { description: errorMessage(e) })
      })
      .finally(() => setBusy(false))
  }

  const ready = state !== null && connection === "connected"

  const status = !ready
    ? "Unknown"
    : !state.enabled
      ? "Off — not showing the link"
      : state.connected
        ? "On — connected to the relay"
        : "Off — no relay connection"

  return (
    <div className="rounded-xl border bg-card p-6 text-card-foreground shadow-sm">
      <div className="mb-4 flex items-center gap-2">
        <LightbulbIcon className="size-5 text-muted-foreground" />
        <h2 className="text-lg font-semibold">Onboard LED</h2>
      </div>

      <div className="flex items-center justify-between gap-4">
        <div>
          <p className="text-sm font-medium">{status}</p>
          <p className="text-sm text-muted-foreground">
            The board LED is lit while the device is connected to its relay server. Turn
            the indication off with <code>led set</code>; the relay itself is configured
            in Settings.
          </p>
        </div>
        <Switch
          checked={state?.enabled === true}
          onCheckedChange={toggle}
          disabled={!ready || busy}
          aria-label="Show the relay link on the onboard LED"
        />
      </div>
    </div>
  )
}
