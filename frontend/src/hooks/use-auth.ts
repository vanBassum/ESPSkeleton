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
