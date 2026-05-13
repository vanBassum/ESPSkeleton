// Singleton backend service — all communication over a single WebSocket.

const DEV_HOST = "strux.local"

// ── Types ────────────────────────────────────────────────────────

type BroadcastHandler = (msg: Record<string, unknown>) => void
type BinaryHandler = (data: ArrayBuffer) => void

interface PendingRequest {
  resolve: (data: unknown) => void
  reject: (err: Error) => void
  timer: ReturnType<typeof setTimeout>
}

export type ConnectionStatus = "connected" | "connecting" | "disconnected"
type StatusHandler = (status: ConnectionStatus) => void

// ── Service ──────────────────────────────────────────────────────

let nextId = 1

class BackendService {
  private ws: WebSocket | null = null
  private pending = new Map<number, PendingRequest>()
  private broadcastHandlers = new Set<BroadcastHandler>()
  private binaryHandlers = new Set<BinaryHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null
  private connecting: Promise<void> | null = null
  private _status: ConnectionStatus = "disconnected"

  get status(): ConnectionStatus {
    return this._status
  }

  private setStatus(s: ConnectionStatus) {
    if (s !== this._status) {
      this._status = s
      this.statusHandlers.forEach((fn) => fn(s))
    }
  }

  onStatusChange(fn: StatusHandler): () => void {
    this.statusHandlers.add(fn)
    return () => {
      this.statusHandlers.delete(fn)
    }
  }

  connect() {
    this.ensureConnected().catch(() => {})
  }

  private ensureConnected(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return Promise.resolve()
    if (this.connecting) return this.connecting
    return this.doConnect()
  }

  private doConnect(): Promise<void> {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }

    this.setStatus("connecting")
    this.connecting = new Promise<void>((resolve, reject) => {
      const host = import.meta.env.DEV ? DEV_HOST : location.host
      const proto = location.protocol === "https:" ? "wss:" : "ws:"
      const url = `${proto}//${host}/ws`
      console.log(`[BackendService] connecting to ${url} (DEV=${import.meta.env.DEV})`)
      const ws = new WebSocket(url)
      ws.binaryType = "arraybuffer"
      let opened = false

      ws.onopen = () => {
        opened = true
        this.ws = ws
        this.connecting = null
        this.setStatus("connected")
        this.startHeartbeat()
        resolve()
      }

      ws.onmessage = (ev) => {
        // Binary frames are dispatched to binary subscribers.
        if (ev.data instanceof ArrayBuffer) {
          this.binaryHandlers.forEach((fn) => fn(ev.data))
          return
        }
        // Defensive: some browsers may deliver the first frame as a Blob if
        // binaryType wasn't applied in time. Convert and dispatch.
        if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
          ev.data.arrayBuffer().then((buf) => {
            this.binaryHandlers.forEach((fn) => fn(buf))
          })
          return
        }
        try {
          const msg = JSON.parse(ev.data)
          if (typeof msg.id === "number") {
            const req = this.pending.get(msg.id)
            if (req) {
              this.pending.delete(msg.id)
              clearTimeout(req.timer)
              if (msg.error) {
                req.reject(new Error(msg.error))
              } else {
                req.resolve(msg)
              }
            }
          } else {
            this.broadcastHandlers.forEach((fn) => fn(msg))
          }
        } catch (e) {
          const sample = typeof ev.data === "string" ? ev.data.slice(-80) : "(non-string)"
          console.warn(
            `[BackendService] failed to parse WS frame (${typeof ev.data === "string" ? ev.data.length : "?"} bytes); tail: ${sample}`,
            e,
          )
        }
      }

      ws.onclose = () => {
        this.ws = null
        this.connecting = null
        this.stopHeartbeat()
        this.setStatus("disconnected")
        for (const [, req] of this.pending) {
          clearTimeout(req.timer)
          req.reject(new Error("WebSocket closed"))
        }
        this.pending.clear()
        if (!opened) reject(new Error("Connection failed"))
        this.reconnectTimer = setTimeout(() => {
          this.doConnect().catch(() => {})
        }, 2000)
      }

      ws.onerror = () => ws.close()
    })

    return this.connecting
  }

  private startHeartbeat() {
    this.stopHeartbeat()
    this.heartbeatTimer = setInterval(() => {
      if (this.ws?.readyState !== WebSocket.OPEN) return
      this.send("ping").catch(() => {
        this.setStatus("disconnected")
        this.ws?.close()
      })
    }, 15000)
  }

  private stopHeartbeat() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer)
      this.heartbeatTimer = null
    }
  }

  async send<T>(
    type: string,
    params: Record<string, unknown> = {},
  ): Promise<T> {
    await this.ensureConnected()
    const id = nextId++
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error("Request timeout"))
      }, 10000)
      this.pending.set(id, {
        resolve: resolve as (data: unknown) => void,
        reject,
        timer,
      })
      this.ws!.send(JSON.stringify({ id, type, ...params }))
    })
  }

  subscribe(fn: BroadcastHandler): () => void {
    this.broadcastHandlers.add(fn)
    this.ensureConnected()
    return () => {
      this.broadcastHandlers.delete(fn)
    }
  }

  subscribeBinary(fn: BinaryHandler): () => void {
    this.binaryHandlers.add(fn)
    this.ensureConnected()
    return () => {
      this.binaryHandlers.delete(fn)
    }
  }

  // ── API methods ──────────────────────────────────────────────

  async getInfo(): Promise<DeviceInfo> {
    return this.send<DeviceInfo>("info")
  }

  async getLogs(): Promise<LogsResponse> {
    return this.send<LogsResponse>("getLogs")
  }

  async getUpdateStatus(): Promise<UpdateStatus> {
    return this.send<UpdateStatus>("updateStatus")
  }

  async getSettings(): Promise<SettingsResponse> {
    return this.send<SettingsResponse>("getSettings")
  }

  async setSetting(key: string, value: string): Promise<{ ok: boolean }> {
    return this.send("setSetting", { key, value })
  }

  async saveSettings(): Promise<{ ok: boolean }> {
    return this.send("saveSettings")
  }

  async wifiScan(): Promise<WifiScanResponse> {
    return this.send<WifiScanResponse>("wifiScan")
  }

  async getPartitions(): Promise<PartitionsResponse> {
    return this.send<PartitionsResponse>("partitions")
  }

  async reboot(): Promise<{ ok: boolean }> {
    return this.send("reboot")
  }

  partitionDownloadUrl(label: string): string {
    const host = import.meta.env.DEV ? `http://${DEV_HOST}` : ""
    return `${host}/api/download?partition=${encodeURIComponent(label)}`
  }

  async uploadFirmware(
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    return this.upload("/api/upload/app", file, onProgress)
  }

  async uploadWww(
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    return this.upload("/api/upload/www", file, onProgress)
  }

  private upload(
    url: string,
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    const host = import.meta.env.DEV ? `http://${DEV_HOST}` : ""
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest()
      xhr.open("POST", `${host}${url}`)

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable && onProgress) {
          onProgress(Math.round((e.loaded / e.total) * 100))
        }
      }

      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(JSON.parse(xhr.responseText))
        } else {
          reject(new Error(xhr.responseText || `${xhr.status} ${xhr.statusText}`))
        }
      }

      xhr.onerror = () => reject(new Error("Upload failed"))
      xhr.ontimeout = () => reject(new Error("Upload timed out"))
      xhr.timeout = 120000

      xhr.send(file)
    })
  }
}

const instance = new BackendService()
instance.connect()
export const backend = instance

// ── Types ────────────────────────────────────────────────────────

export interface DeviceInfo {
  project: string
  firmware: string
  idf: string
  date: string
  time: string
  chip: string
  heapFree: number
  heapMin: number
  deviceTime: string
}

export interface UpdateStatus {
  firmware: string
  running: string
  nextSlot: string
}

export interface UploadResult {
  ok: boolean
  size: number
}

export interface SettingEntry {
  key: string
  label: string
  type: "string" | "int" | "bool"
  value: string | number | boolean
}

export interface SettingsResponse {
  settings: SettingEntry[]
}

export interface WifiNetwork {
  ssid: string
  rssi: number
  channel: number
  secure: boolean
}

export interface WifiScanResponse {
  ok: boolean
  networks: WifiNetwork[]
}

export interface LogsResponse {
  lines: string[]
}

export interface Partition {
  label: string
  type: string
  subtype: string
  offset: number
  size: number
  running: boolean
  nextOta: boolean
  uploadable: boolean
  version: string
}

export interface PartitionsResponse {
  partitions: Partition[]
}

