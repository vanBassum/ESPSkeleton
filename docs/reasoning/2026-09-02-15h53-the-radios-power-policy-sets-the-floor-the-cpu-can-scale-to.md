---
id: 2026-09-02-15h53
date: 2026-09-02
time: "15:53"
title: The radio's power policy sets the floor the CPU can scale to
builds-on:
supersedes:
---

**Before:** the two power knobs looked independent, and were treated as separate items
on separate sides of a line. WiFi power save (`esp_wifi_set_ps`) was a *radio* decision,
already made deliberately and against saving power: `WIFI_PS_NONE` is set in
`WiFiInterface::Init()` because under the IDF default every TCP round trip waits out a
beacon interval and serving a 477-byte `index.html` measured 13.5 s. Dynamic frequency
scaling was a *CPU* decision, uncontentious, nobody's latency at stake, so it could land
in the template while the radio question stayed a fork's. Two subsystems, two budgets.

**What changed it:** `esp_wifi` creates its PM lock as `ESP_PM_APB_FREQ_MAX`
(`wifi_init.c`), and the only things that release it are `wifi_apb80m_request` /
`wifi_apb80m_release` — the modem-sleep path. `WIFI_PS_NONE` is precisely the setting
that never sleeps the modem, so the lock is held for as long as the radio is up. On
ESP32 and C3, APB at 80 MHz requires the PLL, and the CPU cannot run from the XTAL while
the PLL is required. So the DFS floor with WiFi associated is 80 MHz, not the XTAL's 40,
and it is *the radio's* policy that put it there.

**Now:** the CPU's scaling range is downstream of the radio's power policy, so the
latency-over-milliamps decision made in `WiFiInterface.cpp` also silently bounds what
DFS can deliver. This does not undo the DFS change — 160 → 80 MHz is most of the CPU
saving, and the XTAL floor still applies in the windows where the radio is down between
station rounds — but it does mean the two knobs cannot be reasoned about or estimated
separately. A fork that revisits `WIFI_PS_NONE` gets a deeper CPU floor thrown in, for
free and without touching a PM setting.

The general shape: a PM lock is a coupling between subsystems that own no code in
common. Two settings in different components are independent only until one of them
takes a lock the other's range depends on, and nothing in either setting's own
vocabulary says so.

**Follows:** the DFS bounds reported by `system info` and logged at boot are the
*configured* minimum, not the effective runtime floor, and `DescribeCpu` says so where
it is read. Estimating the saving from either knob alone overstates the total.
