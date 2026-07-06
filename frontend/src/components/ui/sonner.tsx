import { Toaster as Sonner, type ToasterProps } from "sonner"

// shadcn sonner wrapper, minus its next-themes dependency (no theme
// provider in this app — `theme="system"` follows the OS directly).
function Toaster(props: ToasterProps) {
  return (
    <Sonner
      theme="system"
      className="toaster group"
      style={
        {
          "--normal-bg": "var(--popover)",
          "--normal-text": "var(--popover-foreground)",
          "--normal-border": "var(--border)",
        } as React.CSSProperties
      }
      {...props}
    />
  )
}

export { Toaster }
