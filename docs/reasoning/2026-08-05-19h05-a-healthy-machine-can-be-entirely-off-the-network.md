---
id: 2026-08-05-19h05
date: 2026-08-05
time: "19:05"
title: Memory pressure took the network, not a process — so every health check said fine
builds-on: 2026-08-05-16h40
supersedes:
---

**Before:** "the server is unreachable" and "the server is unhealthy" were the same
thought. Debugging an outage meant asking what broke *on* the box — which process died,
what got OOM-killed, what filled up. The mental model had a machine either working or
not.

**What changed it:** `vanbassum.com` went completely dark for three hours — no ICMP, no
HTTPS, no SSH, from two separate networks — while the machine sat there in perfect
health. The console showed load 0.16, 46% memory, zero swap used, sshd up for four days,
every container running. `ping 8.8.8.8` from the box itself said `Network is unreachable`.

The journal from that boot:

```
15:19  systemd-journald: Under memory pressure, flushing caches.   (six times, three minutes)
15:22  systemd-networkd: ens6: Could not set route: Connection timed out
15:22  systemd-networkd: ens6: Failed
```

The host had **no swap** and was at 94% commit with 285 MB free. Its public address comes
from DHCP on a ten-minute lease, so `systemd-networkd` re-sets the default route
constantly. Under sustained pressure one of those route operations timed out, networkd
gave up on the interface, and the default route was gone. Nothing was OOM-killed. No
service crashed. The kernel just squeezed one daemon at the exact moment it was doing
the one thing that keeps the machine on the internet, and that daemon's failure was
invisible to every other part of the system.

**Now:** memory pressure is not only a threat to the biggest process. It is a threat to
whichever process next needs to allocate — and on a machine reached over the network,
the *networking daemon* is a uniquely bad one to lose, because losing it removes your
ability to see that anything happened. The failure hides its own evidence.

Two things follow, and only one of them is about memory.

**Swap is not for running out of memory.** It is slack that lets the kernel resolve
pressure by paging something cold instead of stalling whoever asks next. A box with 16 GB
and no swap has no graceful mode: it goes from fine to squeezing system daemons with
nothing in between. That is why the fix is 8 GB of swap and per-container `mem_limit`s,
and why the swap matters more — limits stop one container causing pressure, swap stops
pressure becoming a casualty.

**"Unreachable" and "unhealthy" are different questions, and the first one is not
answerable from outside.** I spent three hours reasoning from external symptoms and
produced four confident wrong diagnoses in sequence: the memory hypothesis (right
mechanism, wrong reasoning, discarded), a Docker subnet collision (checked, false), an
IP ban from my own SSH hammering (plausible, disproven the moment a phone on a different
network also failed), and a provider-side fault. Every one fit the external evidence,
because the external evidence was one bit wide: *no answer*. One bit cannot distinguish
between a dozen causes, and reasoning harder about it only produces better-argued
guesses.

The thing that actually solved it was the out-of-band console — the first look at state
rather than at symptoms. `ip route` and one grep of the previous boot's journal ended
three hours of theorising in under a minute. **When a machine stops answering, the next
move is not another probe from outside; it is any channel that shows internal state.**
A probe from outside can only ever re-measure the same bit.

**Related:** the same day's [pipe teardown note](2026-08-05-16h40-replacing-without-closing-makes-teardown-lie-about-order.md)
has the same shape one layer down — facts that were individually true adding up to a
false story, and the fix being better evidence rather than better inference.
