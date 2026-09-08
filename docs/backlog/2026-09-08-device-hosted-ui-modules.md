# Device-hosted UI modules

Idea (Bas, 2026-09-08), reviewed the same day. Not started. The goal is that neither
shell knows any device's features at build time: **firmware owns the device-specific UI,
and a shell provides navigation, transport and theme.**

One sentence: *a device-hosted plugin system where firmware ships its own UI extensions,
and interchangeable shells provide a versioned host API, navigation and transport.*

## The shape

There are two **shells** — the site the device serves itself, and the site the relay
serves — and they are interchangeable by design. Each device's frontend build emits its
own shell plus a set of **modules**: self-contained ESM bundles that contribute pages.
A shell reads the device's **manifest**, draws the nav from it, and dynamically imports
a module's bundle when one of its pages is opened.

The property the whole thing rests on is one Strux already has: the UI is served *from
the device*. Module and firmware ship in the same `www` build and the relay serves those
same bytes, so a module can never disagree with the firmware it talks to. That is what
removes the version-skew problem that usually sinks plugin architectures.

Vocabulary, so it stays consistent: **shell** (host/container), **module**, **manifest**,
**extension point** (the nav and the router are the first two; dashboard cards are the
obvious third), **host API** (what the shell hands a module — a *provider*, in this
codebase's existing vocabulary), **activation** (the module's entry function),
**shared singleton** (React, exactly one instance). This is manifest-discovered ESM
plugins, not Module Federation — "remote module" is borrowed terminology, not an
implementation claim, so avoid the word.

## What is already true

- **The relay already serves it.** `device_frontend` in
  [`relay-server/relay.py`](../../relay-server/relay.py) proxies any path in the device's
  `www` over the pipe, with the device's own content-type and gzip passed through. A
  module chunk is just another file, and `import('/devices/<id>/assets/mod-abc123.js')`
  is an HTTP GET the relay serves today. Correct MIME is not incidental — `import()`
  refuses a module served with the wrong one.
- **Cache invalidation is solved.** Vite content-hashes chunk names, and the relay's
  cache already splits immutable from mutable: `IMMUTABLE` matches only
  `/assets/<name>-<hash>.<ext>`, so a manifest at an unhashed path lands in the
  short-TTL bucket by itself. See
  [the frontend already carries its own cache key](../reasoning/2026-08-11-11h19-the-frontend-already-carries-its-own-cache-key.md).
  Getting this wrong is what would recreate the mismatch the design exists to prevent.
- **The device already describes itself.** `help list` gives categories, commands and
  their argument schemas, and the settings UI is already generated from registered
  definitions. Modules are for the view a shell *could not have described*; anything
  expressible as a declaration should stay a declaration.

## Steps, in order

### 1. The shell-to-module contract

First, because both shells and every module compile against it, and because it is the
only decision that is expensive to change later.

The host API is narrow and capability-oriented — *not* the raw `BackendService`. A module
receives roughly:

```ts
interface ShellProvider {
  hostApi: 1;
  device: { id: string; type: string };
  transport: {
    request(command: string, args?: unknown): Promise<unknown>;
    subscribe(channel: string, cb: (data: unknown) => void): Disposable;
  };
  routes: { register(route: RouteContribution): void };
  ui: { notify(message: string, kind?: "info" | "error"): void };
}
```

Two reasons it must be narrow, and the second is the forcing one. First, the raw service
is implementation detail — behind `transport.request` a shell may use the local
WebSocket, the relay pipe, or something that does not exist yet. Second,
[`frontend/src/lib/backend.ts`](../../frontend/src/lib/backend.ts) is a module-level
singleton whose socket URL derives from where the page was served. That is right on the
device (one page, one device) and wrong on the relay shell, which is inherently
multi-device. So a module cannot import `backend` at all — it must be *handed* a client
already bound to a device. Scoping which commands a module may call then comes for free.

The module's entry is an activation function, not a default-exported component:

```ts
export function activate(shell: ShellProvider) { /* register contributions */ }
```

A module contributing several pages cannot be one `React.lazy` default export.
`activate` registers; the page components are lazily imported behind those
registrations. Lifecycle: discover, load, validate contract, activate, contributions
live, deactivate.

### 2. Split the existing device frontend into shell plus modules

One job, not two — the current frontend is carved in two rather than rebuilt. React is
`external` in the module build, leaving a bare specifier that the shell resolves through
an **import map**, so there is exactly one React instance; two copies is the one thing
that genuinely breaks. This step needs no relay work at all, which is what makes it the
item that proves the idea.

### 3. The relay shell

Note what this changes on the relay: today the device's own `index.html` *is* the page at
`/devices/<id>/`. A relay shell serves its own HTML and pulls only the manifest and the
module chunks from the device, so `device_frontend` stops being "serve the device's site"
and becomes "serve the device's assets". Also extend `warm_cache`: it scrapes
`/assets/...` references out of `index.html`, and a lazily imported chunk is named inside
the entry chunk rather than the HTML, so module chunks are missed and the first open of a
module page costs a live pipe round trip.

## Settled while reviewing

- **The manifest carries the nav declarations.** Discovery and contribution are not
  cleanly separable here: if only `activate()` knows which pages a module contributes, a
  shell must fetch and run *every* module just to draw the sidebar, which throws away the
  lazy loading. So the manifest declares pages statically and `activate` supplies what is
  behind them.
- **Version with a plain integer range** (`hostApi: { min, max }`), not a list of
  required capability strings. A capability list earns its keep when shells and modules
  are released by different parties; while one party owns the firmware and both shells,
  the range plus a clear "this device needs a newer shell" message covers it. Add
  capabilities when something concretely needs partial support, not before.
- **Design tokens, not shared Tailwind.** A module must not assume a utility class the
  shell's build happened to emit. The shell publishes CSS variables and the module
  bundles its own CSS. A token contract is far more stable than a shared Tailwind
  compilation.

## Still open

- **Trust, but only for the relay shell.** In-process module code runs with the shell's
  origin and session. On the device shell that escalates nothing — device code on the
  device's own origin is the status quo. On the relay shell it means one device's code
  runs with the operator's session for *every other* device. So the choice (all firmware
  trusted and cryptographically controlled, versus isolating device UI in a sandboxed
  iframe behind a message API) belongs to step 3 and does not block steps 1 and 2.
- **Where the contract lives if the relay moves to its own repo.** See below.

## Interaction with splitting the relay out

Moving `relay-server/` to its own repository was always going to be a copy rather than a
refactor — nothing in it imports from the firmware, and the only outside references are
[`.github/workflows/relay-image.yml`](../../.github/workflows/relay-image.yml), this
repo's `CLAUDE.md`, and two backlog files.

This plan is the first thing that changes that calculus, and only at step 3. The relay
shell is a TypeScript app compiling against the same `ShellProvider` the modules compile
against, so the contract becomes a cross-repo dependency: either a small published
package that both repos consume, or a duplicated type file that will drift. That is an
argument for doing the split *before* the relay shell exists, while the relay is still
only Python and the boundary is genuinely clean.
