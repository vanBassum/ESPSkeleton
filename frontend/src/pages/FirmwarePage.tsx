import { useEffect, useRef, useState } from "react"
import { backend, type Partition } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { UploadIcon, DownloadIcon } from "lucide-react"
import { Button } from "@/components/ui/button"

function fmtSize(bytes: number): string {
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(0)} KB`
  return `${bytes} B`
}

function uploadTarget(p: Partition): "app" | "www" | null {
  if (!p.uploadable) return null
  if (p.type === "app") return "app"
  if (p.label === "www") return "www"
  return null
}

export default function FirmwarePage() {
  const connection = useConnectionStatus()
  const [partitions, setPartitions] = useState<Partition[] | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [restartPending, setRestartPending] = useState(false)

  function refresh() {
    backend
      .getPartitions()
      .then((r) => { setPartitions(r.partitions); setLoadError(null) })
      .catch((e) => setLoadError(e instanceof Error ? e.message : "Unknown error"))
  }

  useEffect(() => {
    if (connection !== "connected") return
    refresh()
  }, [connection])

  return (
    <div className="mx-auto max-w-2xl space-y-6">
      <h1 className="text-2xl font-bold">Firmware</h1>

      {restartPending && (
        <div className="rounded-xl border border-amber-500/30 bg-amber-500/5 p-4 text-sm text-amber-600 dark:text-amber-400">
          Upload complete — reboot the device to apply the update.
        </div>
      )}

      <div className="rounded-xl border bg-card text-card-foreground shadow-sm">
        <div className="border-b p-4">
          <h2 className="text-lg font-semibold">Partitions</h2>
          <p className="mt-0.5 text-xs text-muted-foreground">
            Upload targets non-running OTA slots and the www FAT image. Download is always available.
          </p>
        </div>

        {partitions ? (
          <ul className="divide-y">
            {partitions.map((p) => (
              <PartitionRow
                key={p.label}
                partition={p}
                onAfterUpload={() => {
                  setRestartPending(true)
                  refresh()
                }}
              />
            ))}
          </ul>
        ) : loadError ? (
          <p className="p-6 text-sm text-red-500">Failed to load partitions: {loadError}</p>
        ) : (
          <p className="p-6 text-sm text-muted-foreground">Loading...</p>
        )}
      </div>
    </div>
  )
}

// ── Badge ─────────────────────────────────────────────────────

function Badge({ children, tone }: { children: React.ReactNode; tone: "emerald" | "sky" }) {
  const cls =
    tone === "emerald"
      ? "border-emerald-500/30 bg-emerald-500/10 text-emerald-600 dark:text-emerald-400"
      : "border-sky-500/30 bg-sky-500/10 text-sky-600 dark:text-sky-400"
  return (
    <span
      className={`inline-flex items-center rounded-md border px-1.5 py-0.5 text-[10px] font-medium uppercase tracking-wide ${cls}`}
    >
      {children}
    </span>
  )
}

// ── Partition row ─────────────────────────────────────────────

function PartitionRow({
  partition,
  onAfterUpload,
}: {
  partition: Partition
  onAfterUpload: () => void
}) {
  const p = partition
  const fileRef = useRef<HTMLInputElement>(null)
  const [progress, setProgress] = useState<number | null>(null)
  const [error, setError] = useState<string | null>(null)

  const target = uploadTarget(p)
  const canUpload = !!target && !p.running
  const uploading = progress !== null

  async function onFileChosen(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0]
    e.target.value = ""
    if (!file || !target) return

    setError(null)
    setProgress(0)
    try {
      await backend.uploadPartition(target, file, setProgress)
      onAfterUpload()
    } catch (err) {
      setError(err instanceof Error ? err.message : "Upload failed")
    } finally {
      setProgress(null)
    }
  }

  return (
    <li className="p-4">
      <div className="flex items-center justify-between gap-4">
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <span className="font-mono font-medium">{p.label}</span>
            {p.running && <Badge tone="emerald">running</Badge>}
            {p.nextOta && <Badge tone="sky">next OTA</Badge>}
            {p.version && (
              <span className="font-mono text-xs text-muted-foreground">v{p.version}</span>
            )}
          </div>
          <div className="mt-0.5 font-mono text-xs text-muted-foreground">
            {p.type}/{p.subtype} · 0x{p.offset.toString(16)} · {fmtSize(p.size)}
          </div>
        </div>

        <div className="flex shrink-0 gap-2">
          <input
            ref={fileRef}
            type="file"
            accept=".bin"
            className="hidden"
            onChange={onFileChosen}
          />
          <Button
            variant="outline"
            size="sm"
            disabled={!canUpload || uploading}
            onClick={() => fileRef.current?.click()}
            title={
              !target
                ? "This partition can't be updated over HTTP"
                : p.running
                  ? "Cannot overwrite the running slot"
                  : "Upload a .bin file"
            }
          >
            <UploadIcon className="mr-1.5 size-3.5" />
            Upload
          </Button>
          <Button
            variant="outline"
            size="sm"
            onClick={() => backend.downloadPartitionFile(p.label).catch((e) => setError(e.message))}
          >
            <DownloadIcon className="mr-1.5 size-3.5" />
            Download
          </Button>
        </div>
      </div>

      {uploading && (
        <div className="mt-3">
          <div className="mb-1 flex justify-between text-xs text-muted-foreground">
            <span>Uploading…</span>
            <span>{progress}%</span>
          </div>
          <div className="h-1.5 overflow-hidden rounded-full bg-muted">
            <div
              className="h-full bg-primary transition-all"
              style={{ width: `${progress}%` }}
            />
          </div>
        </div>
      )}

      {error && <p className="mt-2 text-xs text-red-500">{error}</p>}
    </li>
  )
}
