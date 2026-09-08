# Device-hosted UI modules

Idea (Bas, 2026-09-08). Not planned yet — this file names the pieces so the idea can be
discussed and shared without re-deriving the vocabulary each time.

## The idea in one paragraph

There are two **shells**: the site the device serves itself, and the site the relay
serves. Neither shell knows any device's features. Each device's frontend build emits,
alongside its own shell, a set of **remote modules** — self-contained UI bundles that
register pages and menu items. Both shells discover a device's modules from a
**manifest**, fetch the bundles from the device (over `getWebFile`, directly or through
the relay), and mount them at their own **extension points** — the nav sidebar and the
router. Because both shells pull the module from the same place, the module can never
disagree with the firmware it talks to.

## The proper names

This is a **micro-frontend** architecture — one UI composed at runtime from
independently delivered pieces. The established vocabulary:

- **Shell** (also **host**, **container**, **app shell**) — the outer site: chrome,
  nav, routing, theme. Here there are two, and they are interchangeable by design.
- **Remote module** (also **remote**, **plugin**, **extension**) — a UI bundle loaded at
  runtime from a URL the shell did not know at build time. "Remote" is the Module
  Federation term and is the closest fit: the distinguishing property is *loaded at
  runtime from elsewhere*, not *written by a third party*.
- **Manifest** — the discovery document listing a device's modules: name, icon, route,
  chunk path. Module Federation calls its equivalent `remoteEntry.js`; the plugin world
  calls it a plugin manifest. Ours is a file in `www/`, fetched like any other.
- **Extension point** (also **contribution point**, VS Code's term, or **slot** /
  **outlet** for the render location) — a declared place a module may contribute to. The
  nav sidebar and the route table are the first two.
- **Host API** (also **plugin API**, and in this codebase's own vocabulary a
  **provider**) — what the shell hands a module: the React instance, the
  `BackendService`, and the registration calls. VS Code injects a `vscode` module; the
  same role here would be a `ShellProvider`, which is the pattern
  `BoardProvider`/`StruxProvider`/`AppProvider` already use one layer down.
- **Shared singleton dependency** — React must be exactly one instance across shell and
  modules; two copies break hooks and context. The module therefore treats React as a
  **peer/externalized dependency** (`external` in a Vite library build, leaving a **bare
  specifier** in the output), and the shell supplies it through an **import map**, the
  browser standard for resolving bare specifiers to URLs.
- **Dynamic import / lazy loading / code splitting** — `React.lazy(() => import(url))`
  is the mount mechanism; a module's bytes are only fetched when its menu item is opened.
- **Device-hosted frontend** — the property that makes the whole thing work: the UI is
  served from the device, so it cannot mismatch the firmware. Already true today.

The alternative worth naming because it is the road not taken: **server-driven UI**
(also **declarative** or **manifest-driven UI**), where the device ships a *description*
and the shell renders it with its own components. That is what `help list` and the
generated settings UI already do. It costs the device almost nothing and always looks
native, but a module can only ever be as expressive as the shell's vocabulary. Shipping
code is the escape hatch for the view the shell could not have described.

## What still needs deciding

The **shell ↔ module contract** is the only place a mismatch can still appear. The
module is welded to its firmware, but the two shells version independently, so both must
offer the same host API. Open: what is in it, how it is versioned, and what a shell does
with a module that wants a newer one.

Secondary: styling (a module cannot rely on Tailwind classes the shell's build never
emitted — inline its CSS or fix a shared token set), and the trust boundary (module code
runs inside the relay shell's origin, with the operator's session for every *other*
device — a concern that does not exist for the device's own shell).
