import { useState, useEffect } from "react"
import { CpuIcon, Loader2Icon } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { backend } from "@/lib/backend"

// shadcn login-03, stripped: brand slot + password + submit. No email,
// no social logins, no signup/forgot links, no terms footer (spec).
export default function LoginPage() {
  const [password, setPassword] = useState("")
  const [error, setError] = useState("")
  const [loading, setLoading] = useState(false)
  const [deviceName, setDeviceName] = useState("Strux")

  useEffect(() => {
    backend.getLoginInfo().then((res) => {
      setDeviceName(res.name)
      document.title = res.name
    }).catch(() => { /* open endpoint unreachable — keep fallback */ })
  }, [])

  async function handleSubmit(e: React.FormEvent) {
    e.preventDefault()
    setError("")
    setLoading(true)
    try {
      const ok = await backend.login(password)
      if (!ok) setError("Invalid password")
    } catch {
      setError("Connection failed")
    } finally {
      setLoading(false)
    }
  }

  return (
    <div className="bg-muted flex min-h-svh flex-col items-center justify-center gap-6 p-6 md:p-10">
      <div className="flex w-full max-w-sm flex-col gap-6">
        <div className="flex items-center gap-2 self-center font-medium">
          <div className="bg-primary text-primary-foreground flex size-6 items-center justify-center rounded-md">
            <CpuIcon className="size-4" />
          </div>
          {deviceName}
        </div>
        <Card>
          <CardHeader className="text-center">
            <CardTitle className="text-xl">Welcome back</CardTitle>
            <CardDescription>Sign in to access the device</CardDescription>
          </CardHeader>
          <CardContent>
            <form onSubmit={handleSubmit} className="grid gap-6">
              <div className="grid gap-2">
                <Label htmlFor="password">Password</Label>
                <Input
                  id="password"
                  type="password"
                  autoComplete="current-password"
                  autoFocus
                  required
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                />
              </div>
              {error && <p className="text-destructive text-center text-sm">{error}</p>}
              <Button type="submit" className="w-full" disabled={loading}>
                {loading
                  ? <><Loader2Icon className="mr-1.5 size-4 animate-spin" />Signing in...</>
                  : "Sign in"}
              </Button>
            </form>
          </CardContent>
        </Card>
      </div>
    </div>
  )
}
