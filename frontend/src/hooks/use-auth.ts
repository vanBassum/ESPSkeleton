import { useState, useEffect } from "react"
import { backend } from "@/lib/backend"

export function useAuth() {
  const [authenticated, setAuthenticated] = useState(backend.authenticated)
  // A stored token might still be valid — hold the login page back until
  // the auto-connect's validation resolves (or 3 s pass, e.g. device off).
  const [checking, setChecking] = useState(backend.hasToken && !backend.authenticated)

  useEffect(() => {
    const unsub = backend.onAuthChange((auth) => {
      setAuthenticated(auth)
      setChecking(false)
    })

    // Resync once after subscribing — covers the race where auth resolved
    // between first render and effect-mount (e.g. fast LAN), which would
    // otherwise leave `checking` stuck true forever (blank page).
    setAuthenticated(backend.authenticated)
    if (backend.authenticated || !backend.hasToken) setChecking(false)

    if (backend.hasToken && !backend.authenticated) {
      const timer = setTimeout(() => setChecking(false), 3000)
      return () => {
        unsub()
        clearTimeout(timer)
      }
    }

    return unsub
  }, [])

  return { authenticated, checking }
}
