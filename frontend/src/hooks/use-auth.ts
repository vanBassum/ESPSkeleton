import { useState, useEffect } from "react"
import { backend } from "@/lib/backend"

export function useAuth() {
  const [authenticated, setAuthenticated] = useState(backend.authenticated)
  // Hold the login page back until the first handshake resolves — covers the
  // empty-password case (we become authed with no stored token) so there's no
  // login-page flash. Bounded so a dead device still falls through to login.
  const [checking, setChecking] = useState(!backend.authenticated)

  useEffect(() => {
    const unsub = backend.onAuthChange((auth) => {
      setAuthenticated(auth)
      setChecking(false)
    })

    setAuthenticated(backend.authenticated)
    if (backend.authenticated) setChecking(false)

    const timer = setTimeout(() => setChecking(false), 3000)
    return () => {
      unsub()
      clearTimeout(timer)
    }
  }, [])

  return { authenticated, checking }
}
