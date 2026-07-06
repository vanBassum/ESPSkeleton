import { useEffect, useRef, useState } from "react"
import { backend, NUMERIC_SETTING_TYPES, type SettingEntry, type WifiNetwork } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { SaveIcon, Undo2Icon, PowerIcon, SearchIcon, LockIcon, BracesIcon } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogFooter } from "@/components/ui/dialog"
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from "@/components/ui/alert-dialog"
import { Switch } from "@/components/ui/switch"
import { toast } from "sonner"
import Editor from "react-simple-code-editor"
import Prism from "prismjs"
import "prismjs/components/prism-json"
import "prismjs/themes/prism-tomorrow.css"

function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : "Unknown error"
}

// Group settings by prefix (e.g. "wifi.ssid" → "wifi", "mqtt.broker" → "mqtt")
function groupSettings(settings: SettingEntry[]): { label: string; prefix: string; items: SettingEntry[] }[] {
  const groups = new Map<string, SettingEntry[]>()
  for (const s of settings) {
    const dot = s.key.indexOf(".")
    const prefix = dot > 0 ? s.key.slice(0, dot) : "general"
    if (!groups.has(prefix)) groups.set(prefix, [])
    groups.get(prefix)!.push(s)
  }

  const labels: Record<string, string> = {
    wifi: "WiFi",
    device: "Device",
    ntp: "Time & NTP",
  }

  return [...groups.entries()].map(([prefix, items]) => ({
    prefix,
    label: labels[prefix] ?? prefix.charAt(0).toUpperCase() + prefix.slice(1),
    items,
  }))
}

// ── Table of contents ────────────────────────────────────────

function SettingsToc({
  groups,
  activePrefix,
}: {
  groups: { label: string; prefix: string }[]
  activePrefix: string | null
}) {
  return (
    <div className="space-y-1">
      <p className="mb-3 text-xs font-semibold uppercase tracking-wider text-muted-foreground">
        On this page
      </p>
      {groups.map((g) => (
        <a
          key={g.prefix}
          href={`#settings-${g.prefix}`}
          onClick={(e) => {
            e.preventDefault()
            document.getElementById(`settings-${g.prefix}`)?.scrollIntoView({ behavior: "smooth" })
          }}
          className={`block rounded-md px-3 py-1.5 text-sm transition-colors hover:text-foreground ${
            activePrefix === g.prefix
              ? "bg-muted font-medium text-foreground"
              : "text-muted-foreground"
          }`}
        >
          {g.label}
        </a>
      ))}
    </div>
  )
}

export default function SettingsPage() {
  const connection = useConnectionStatus()
  const [settings, setSettings] = useState<SettingEntry[]>([])
  const [dirty, setDirty] = useState(false)
  const [saving, setSaving] = useState(false)
  const [activePrefix, setActivePrefix] = useState<string | null>(null)
  const [search, setSearch] = useState("")
  const [jsonOpen, setJsonOpen] = useState(false)
  const [jsonText, setJsonText] = useState("")
  const [jsonError, setJsonError] = useState("")
  const scrollRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (connection !== "connected") return
    backend.getSettings().then((r) => {
      setSettings(r.settings)
      setDirty(false)
    }).catch((e) => toast.error("Failed to load settings", { description: errorMessage(e) }))
  }, [connection])

  useEffect(() => {
    if (settings.length === 0 || !scrollRef.current) return
    const root = scrollRef.current
    const observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            const prefix = (entry.target as HTMLElement).dataset.prefix
            if (prefix) setActivePrefix(prefix)
          }
        }
      },
      { root, rootMargin: "-20% 0px -70% 0px" },
    )
    root.querySelectorAll<HTMLElement>("[data-prefix]").forEach((el) => observer.observe(el))
    return () => observer.disconnect()
  }, [settings])

  async function handleChange(key: string, value: string) {
    try {
      await backend.setSetting(key, value)
      // Only mirror the value locally once the device accepted it — a
      // failed write must not leave the UI claiming a change it never made.
      setSettings((prev) =>
        prev.map((s) =>
          s.key === key
            ? { ...s, value: NUMERIC_SETTING_TYPES.includes(s.type) ? Number(value) : s.type === "bool" ? value === "true" : value }
            : s,
        ),
      )
      setDirty(true)
    } catch (e) {
      toast.error(`Failed to update ${key}`, { description: errorMessage(e) })
    }
  }

  async function handleSave() {
    setSaving(true)
    try {
      await backend.saveSettings()
      setDirty(false)
      toast.success("Settings saved")
    } catch (e) {
      toast.error("Failed to save settings", { description: errorMessage(e) })
    }
    setSaving(false)
  }

  function handleReboot() {
    backend.reboot()
      .then(() => toast.info("Rebooting device…", { description: "The connection will drop for a few seconds." }))
      .catch((e) => toast.error("Reboot command failed", { description: errorMessage(e) }))
  }

  async function handleReload() {
    try {
      const r = await backend.getSettings()
      setSettings(r.settings)
      setDirty(false)
    } catch (e) {
      toast.error("Failed to reload settings", { description: errorMessage(e) })
    }
  }

  function openJsonEditor() {
    const obj: Record<string, unknown> = {}
    for (const s of settings) obj[s.key] = s.value
    setJsonText(JSON.stringify(obj, null, 2))
    setJsonError("")
    setJsonOpen(true)
  }

  async function applyJson() {
    try {
      const obj = JSON.parse(jsonText) as Record<string, unknown>
      for (const [key, value] of Object.entries(obj)) {
        await handleChange(key, String(value))
      }
      setJsonOpen(false)
    } catch {
      setJsonError("Invalid JSON — fix the syntax and try again.")
    }
  }

  const groups = groupSettings(settings)
  const filteredGroups = search.trim()
    ? groups
        .map((g) => ({
          ...g,
          items: g.items.filter(
            (s) =>
              s.label.toLowerCase().includes(search.toLowerCase()) ||
              s.key.toLowerCase().includes(search.toLowerCase()),
          ),
        }))
        .filter((g) => g.items.length > 0)
    : groups

  return (
    <div className="mx-auto flex h-full max-w-5xl flex-col">
      {/* Header */}
      <div className="shrink-0 pb-4">
        <div className="flex items-center gap-4">
          <h1 className="text-2xl font-bold">Settings</h1>
          {dirty && (
            <p className="flex-1 text-sm text-amber-500">Unsaved changes — press Save to write to flash.</p>
          )}
          <div className="ml-auto flex gap-2">
            <AlertDialog>
              <AlertDialogTrigger asChild>
                <Button
                  variant="outline"
                  size="sm"
                  className="text-destructive hover:text-destructive"
                >
                  <PowerIcon className="mr-1.5 size-3.5" />
                  Reboot
                </Button>
              </AlertDialogTrigger>
              <AlertDialogContent>
                <AlertDialogHeader>
                  <AlertDialogTitle>Reboot the device?</AlertDialogTitle>
                  <AlertDialogDescription>
                    The device restarts and drops this connection for a few seconds.
                    {dirty && " Unsaved settings changes will be lost."}
                  </AlertDialogDescription>
                </AlertDialogHeader>
                <AlertDialogFooter>
                  <AlertDialogCancel>Cancel</AlertDialogCancel>
                  <AlertDialogAction onClick={handleReboot}>Reboot</AlertDialogAction>
                </AlertDialogFooter>
              </AlertDialogContent>
            </AlertDialog>
            <Button variant="outline" size="sm" onClick={openJsonEditor}>
              <BracesIcon className="mr-1.5 size-3.5" />
              JSON
            </Button>
            <Button variant="outline" size="sm" onClick={handleReload}>
              <Undo2Icon className="mr-1.5 size-3.5" />
              Undo
            </Button>
            <Button size="sm" onClick={handleSave} disabled={!dirty || saving}>
              <SaveIcon className="mr-1.5 size-3.5" />
              {saving ? "Saving..." : "Save"}
            </Button>
          </div>
        </div>
      </div>

      <div className="flex min-h-0 flex-1 gap-12">
        {/* Main settings — scrollable */}
        <div ref={scrollRef} className="min-w-0 flex-1 overflow-y-auto">
          <div className="space-y-6 pb-6">
            {settings.length === 0 ? (
              <div className="rounded-xl border bg-card text-card-foreground shadow-sm">
                <p className="p-6 text-sm text-muted-foreground">Loading...</p>
              </div>
            ) : filteredGroups.length === 0 ? (
              <p className="text-sm text-muted-foreground">No settings match "{search}".</p>
            ) : (
              filteredGroups.map((group) => (
                <div
                  key={group.prefix}
                  id={`settings-${group.prefix}`}
                  data-prefix={group.prefix}
                  className="rounded-xl border bg-card text-card-foreground shadow-sm"
                >
                  <div className="border-b p-4">
                    <h2 className="text-lg font-semibold">{group.label}</h2>
                  </div>
                  <ul className="divide-y">
                    {group.items.map((setting) => (
                      <SettingRow
                        key={setting.key}
                        setting={setting}
                        onChange={(value) => handleChange(setting.key, value)}
                      />
                    ))}
                  </ul>
                </div>
              ))
            )}

          </div>
        </div>

        {/* Sidebar — only on wide screens */}
        {groups.length > 0 && (
          <aside className="hidden w-48 shrink-0 xl:flex xl:flex-col xl:gap-4">
            <div className="relative shrink-0">
              <SearchIcon className="absolute left-2.5 top-2.5 size-3.5 text-muted-foreground" />
              <Input
                className="pl-8 text-sm"
                placeholder="Search…"
                value={search}
                onChange={(e) => setSearch(e.target.value)}
              />
            </div>

            <div className="min-h-0 flex-1 overflow-y-auto">
              <SettingsToc groups={filteredGroups} activePrefix={activePrefix} />
            </div>

          </aside>
        )}
      </div>

      {/* JSON editor modal */}
      <Dialog open={jsonOpen} onOpenChange={setJsonOpen}>
        <DialogContent className="flex h-[80vh] flex-col sm:max-w-5xl">
          <DialogHeader>
            <DialogTitle>Edit Settings as JSON</DialogTitle>
          </DialogHeader>

          <div className="min-h-0 flex-1 overflow-auto rounded-lg border bg-neutral-950 font-mono text-sm">
            <Editor
              value={jsonText}
              onValueChange={(v) => { setJsonText(v); setJsonError("") }}
              highlight={(code) => Prism.highlight(code, Prism.languages.json, "json")}
              padding={16}
              style={{ minHeight: "100%" }}
            />
          </div>

          {jsonError && <p className="text-sm text-destructive">{jsonError}</p>}

          <DialogFooter>
            <Button variant="outline" onClick={() => setJsonOpen(false)}>Cancel</Button>
            <Button onClick={applyJson}>Apply</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  )
}


// ── Setting row ──────────────────────────────────────────────

const sensitiveKeys = ["password", "pass"]

function isSensitive(key: string): boolean {
  const field = key.split(".").pop() ?? ""
  return sensitiveKeys.includes(field)
}

function SettingRow({
  setting,
  onChange,
}: {
  setting: SettingEntry
  onChange: (value: string) => void
}) {
  const isWifiSsid = setting.key === "wifi.ssid"
  const isPassword = setting.type === "string" && isSensitive(setting.key)

  return (
    <li className="flex items-center justify-between gap-4 p-4">
      <div className="min-w-0">
        <div className="text-sm font-medium">{setting.label}</div>
        <div className="font-mono text-xs text-muted-foreground">{setting.key}</div>
      </div>

      {setting.type === "bool" ? (
        <Switch
          checked={Boolean(setting.value)}
          onCheckedChange={(checked) => onChange(checked ? "true" : "false")}
        />
      ) : isWifiSsid ? (
        <WifiSsidInput value={String(setting.value)} onChange={onChange} />
      ) : (
        <Input
          className="w-48"
          type={isPassword ? "password" : NUMERIC_SETTING_TYPES.includes(setting.type) ? "number" : "text"}
          defaultValue={String(setting.value)}
          onBlur={(e) => {
            if (e.target.value !== String(setting.value)) {
              onChange(e.target.value)
            }
          }}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              ;(e.target as HTMLInputElement).blur()
            }
          }}
        />
      )}
    </li>
  )
}

// ── WiFi SSID input with scan ────────────────────────────────

function rssiToStrength(rssi: number): number {
  if (rssi >= -50) return 4
  if (rssi >= -60) return 3
  if (rssi >= -70) return 2
  return 1
}

function WifiSsidInput({
  value,
  onChange,
}: {
  value: string
  onChange: (value: string) => void
}) {
  const inputRef = useRef<HTMLInputElement>(null)
  const [showScan, setShowScan] = useState(false)
  const [scanning, setScanning] = useState(false)
  const [networks, setNetworks] = useState<WifiNetwork[]>([])

  async function handleScan() {
    setShowScan(true)
    setScanning(true)
    setNetworks([])
    try {
      const result = await backend.wifiScan()
      if (result.ok) {
        // Deduplicate by SSID, keeping strongest signal
        const best = new Map<string, WifiNetwork>()
        for (const n of result.networks) {
          if (!n.ssid) continue
          const existing = best.get(n.ssid)
          if (!existing || n.rssi > existing.rssi) {
            best.set(n.ssid, n)
          }
        }
        setNetworks([...best.values()].sort((a, b) => b.rssi - a.rssi))
      } else {
        // A failed scan must not masquerade as "No networks found".
        setShowScan(false)
        toast.error("WiFi scan failed", { description: "The device reported a scan error." })
      }
    } catch (e) {
      setShowScan(false)
      toast.error("WiFi scan failed", { description: errorMessage(e) })
    }
    setScanning(false)
  }

  function selectNetwork(ssid: string) {
    setShowScan(false)
    onChange(ssid)
    if (inputRef.current) {
      inputRef.current.value = ssid
    }
  }

  return (
    <div className="relative">
      <div className="flex gap-1.5">
        <Button
          variant="outline"
          size="icon-sm"
          onClick={handleScan}
          disabled={scanning}
          title="Scan WiFi networks"
        >
          <SearchIcon className="size-3.5" />
        </Button>
        <Input
          ref={inputRef}
          className="w-48"
          defaultValue={value}
          onBlur={(e) => {
            if (e.target.value !== value) {
              onChange(e.target.value)
            }
          }}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              ;(e.target as HTMLInputElement).blur()
            }
          }}
        />
      </div>

      {showScan && (
        <>
          <div className="fixed inset-0 z-40" onClick={() => setShowScan(false)} />
          <div className="absolute right-0 top-full z-50 mt-1 w-72 rounded-lg border bg-card p-2 shadow-lg">
            <div className="mb-2 flex items-center justify-between px-2">
              <span className="text-xs font-medium text-muted-foreground">WiFi Networks</span>
              <Button variant="ghost" size="xs" onClick={handleScan} disabled={scanning}>
                {scanning ? "Scanning..." : "Rescan"}
              </Button>
            </div>

            {scanning && networks.length === 0 ? (
              <p className="px-2 py-4 text-center text-xs text-muted-foreground">Scanning...</p>
            ) : networks.length === 0 ? (
              <p className="px-2 py-4 text-center text-xs text-muted-foreground">No networks found</p>
            ) : (
              <div className="max-h-60 overflow-y-auto">
                {networks.map((n) => (
                  <button
                    key={`${n.ssid}-${n.channel}`}
                    className="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-sm hover:bg-muted"
                    onClick={() => selectNetwork(n.ssid)}
                  >
                    <SignalBars strength={rssiToStrength(n.rssi)} />
                    <span className="min-w-0 flex-1 truncate">{n.ssid}</span>
                    <span className="text-xs text-muted-foreground">ch{n.channel}</span>
                    {n.secure && <LockIcon className="size-3 text-muted-foreground" />}
                    <span className="w-10 text-right text-xs text-muted-foreground">{n.rssi}dB</span>
                  </button>
                ))}
              </div>
            )}
          </div>
        </>
      )}
    </div>
  )
}

function SignalBars({ strength }: { strength: number }) {
  return (
    <div className="flex items-end gap-px" title={`Signal: ${strength}/4`}>
      {[1, 2, 3, 4].map((i) => (
        <div
          key={i}
          className={`w-1 rounded-sm ${i <= strength ? "bg-foreground" : "bg-muted"}`}
          style={{ height: `${4 + i * 3}px` }}
        />
      ))}
    </div>
  )
}
