import { useEffect, useState } from "react"
import { backend } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { Switch } from "@/components/ui/switch"
import { LightbulbIcon } from "lucide-react"
import { toast } from "sonner"

function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

// ──────────────────────────────────────────────────────────────
// The frontend half of LedManager's worked example: `led get` on connect,
// `led set` on toggle. Delete it along with LedManager once the product has
// real features.
// ──────────────────────────────────────────────────────────────
export function LedCard() {
  const connection = useConnectionStatus()
  const [on, setOn] = useState<boolean | null>(null)
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    if (connection !== "connected") return
    let live = true
    backend
      .getLed()
      .then((s) => {
        if (live) setOn(s.on)
      })
      .catch(() => {})
    return () => {
      live = false
    }
  }, [connection])

  // Optimistic: the switch follows the finger and the device's own reply is the
  // correction, so a slow link doesn't make the control feel broken. A failure
  // rolls back rather than leaving the UI lying about the hardware.
  const toggle = (next: boolean) => {
    const previous = on
    setOn(next)
    setBusy(true)
    backend
      .setLed({ on: next })
      .then((s) => setOn(s.on))
      .catch((e) => {
        setOn(previous)
        toast.error("Failed to switch the LED", { description: errorMessage(e) })
      })
      .finally(() => setBusy(false))
  }

  const ready = on !== null && connection === "connected"

  return (
    <div className="rounded-xl border bg-card p-6 text-card-foreground shadow-sm">
      <div className="mb-4 flex items-center gap-2">
        <LightbulbIcon className="size-5 text-muted-foreground" />
        <h2 className="text-lg font-semibold">Onboard LED</h2>
      </div>

      <div className="flex items-center justify-between gap-4">
        <div>
          <p className="text-sm font-medium">{!ready ? "Unknown" : on ? "On" : "Off"}</p>
          <p className="text-sm text-muted-foreground">
            Toggles the board LED over the device's own command surface (<code>led set</code>).
          </p>
        </div>
        <Switch
          checked={on === true}
          onCheckedChange={toggle}
          disabled={!ready || busy}
          aria-label="Toggle onboard LED"
        />
      </div>
    </div>
  )
}
