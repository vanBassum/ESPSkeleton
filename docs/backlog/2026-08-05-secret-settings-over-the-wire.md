# `settings list` hands out passwords in plaintext

Raised 2026-08-03, still open, and it is the one item on the list that is a
straightforward security hole rather than a design question.

## What happens

`settings list` returns every registered setting's value, including
`wifi.password` and `web.password`, as plaintext JSON. Who can ask:

- anyone who reaches the WebSocket **while no web password is set** — which is the
  default, deliberately (the template is open out of the box), so a fresh device on a
  shared network hands out the WiFi PSK to anyone who connects;
- once a web password *is* set, only authenticated callers — but they get the
  password they authenticated with, plus the WiFi PSK, which is more than they need;
- the relay widens the blast radius past the LAN, and there is no device→server
  credential yet either (see [remote-access](2026-07-03-remote-access.md)).

The settings UI needs *names, types and current values* to render. It does not need
secret values — a password field that shows nothing and only sends on change is the
normal shape.

## The decision to make first

Three options, and they are not equivalent:

1. **Write-only over the wire.** A setting is marked secret; `settings list` returns
   its presence (`"set": true`) but never its value; `settings set` still works.
   Cleanest, and matches how the UI wants to behave anyway. Costs a flag on the
   setting definition and one branch in the serializer.
2. **Masked.** Return `"••••"` or the length. Slightly friendlier UI, but it invents
   a value that is not the value, and length alone is a real leak for short secrets.
3. **Gated.** Return secrets only to an authenticated caller. Does nothing about the
   default-open case, which is the case that matters.

Option 1 was the recommendation, and a fourth option has since displaced it as the one to
start from: **4. Bind without publishing.** `Register()` fuses two jobs — binding the leaf
to NVS storage and publishing it into the chain `settings list` walks — so a credential
that is bound but not published cannot leak, with no flag and nothing new on the wire.
What that costs is the generated UI for those settings, which has to be hand-rolled. The
reasoning is in
[registration fuses binding with publishing](../reasoning/2026-08-11-17h21-registration-fuses-binding-a-setting-with-publishing-it.md);
discussed 2026-08-11 and left undecided.

Whichever wins, it is worth doing once, properly, because every downstream fork inherits
it. Also settled: `relay.token` leaks today too, and the frontend's `sensitiveKeys`
heuristic does not even mask it.

## While it is open

Setting a `web.password` closes the default-open hole for everything except the
authenticated caller. That is the mitigation, and it is worth saying out loud in any
deployment that is not a private LAN.
