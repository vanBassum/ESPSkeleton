import { useEffect } from "react"
import { HomeIcon, TerminalIcon, SettingsIcon, DownloadIcon } from "lucide-react"
import {
  Sidebar,
  SidebarContent,
  SidebarFooter,
  SidebarGroup,
  SidebarGroupContent,
  SidebarHeader,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
} from "@/components/ui/sidebar"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { useDeviceInfo } from "@/hooks/use-device-info"
import { useLatestRelease } from "@/hooks/use-latest-release"
import { isNewerVersion } from "@/lib/version"
import { PreReleaseBadge } from "@/components/PreReleaseBadge"

const navItems = [
  { title: "Home", icon: HomeIcon, page: "home" as const },
  { title: "Console", icon: TerminalIcon, page: "console" as const },
  { title: "Settings", icon: SettingsIcon, page: "settings" as const },
  { title: "Firmware", icon: DownloadIcon, page: "firmware" as const },
]

export type Page = (typeof navItems)[number]["page"]

interface AppSidebarProps {
  currentPage: Page
  onNavigate: (page: Page) => void
}

const statusColor = {
  connected: "bg-emerald-500",
  connecting: "bg-amber-500 animate-pulse",
  disconnected: "bg-red-500",
} as const

const statusLabel = {
  connected: "Online",
  connecting: "Connecting",
  disconnected: "Offline",
} as const

export function AppSidebar({ currentPage, onNavigate }: AppSidebarProps) {
  const connection = useConnectionStatus()
  const info = useDeviceInfo()
  const release = useLatestRelease()
  const updateAvailable = info && release && isNewerVersion(info.firmware, release.version)

  // Browser tab title follows the device name (login page covers pre-auth).
  useEffect(() => {
    if (info?.name) document.title = info.name
  }, [info?.name])

  return (
    <Sidebar>
      <SidebarHeader className="px-4 py-3">
        <div className="flex items-center gap-2">
          <span className="text-sm font-semibold">{info?.name ?? "…"}</span>
          <PreReleaseBadge version={info?.firmware} />
        </div>
      </SidebarHeader>
      <SidebarContent>
        <SidebarGroup>
          <SidebarGroupContent>
            <SidebarMenu>
              {navItems.map((item) => (
                <SidebarMenuItem key={item.page}>
                  <SidebarMenuButton
                    isActive={currentPage === item.page}
                    onClick={() => onNavigate(item.page)}
                  >
                    <item.icon />
                    <span>{item.title}</span>
                    {item.page === "firmware" && updateAvailable && (
                      <span className="ml-auto h-2 w-2 rounded-full bg-emerald-500" />
                    )}
                  </SidebarMenuButton>
                </SidebarMenuItem>
              ))}
            </SidebarMenu>
          </SidebarGroupContent>
        </SidebarGroup>
      </SidebarContent>
      <SidebarFooter className="p-3">
        <div className="rounded-lg border bg-card p-3 text-xs">
          {info && (
            <div className="mb-1.5 flex items-center justify-between">
              <span className="text-muted-foreground">Version</span>
              <div className="flex items-center gap-1.5">
                <PreReleaseBadge version={info.firmware} />
                <span className="font-mono">{info.firmware}</span>
              </div>
            </div>
          )}
          <div className="flex items-center justify-between">
            <span className="text-muted-foreground">Status</span>
            <div className="flex items-center gap-1.5">
              <span className={`h-2 w-2 rounded-full ${statusColor[connection]}`} />
              <span>{statusLabel[connection]}</span>
            </div>
          </div>
        </div>
      </SidebarFooter>
    </Sidebar>
  )
}
