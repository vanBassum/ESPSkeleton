---
id: 2026-09-02-15h52
date: 2026-09-02
time: "15:52"
title: Tickless idle is not a step toward light sleep, it is light sleep
builds-on:
supersedes:
---

**Before:** a power-reduction plan for this template listed
`CONFIG_FREERTOS_USE_TICKLESS_IDLE` as its own item, one line of config, cheap, and a
*prerequisite* for automatic light sleep later. It read like the free half of the work:
land it now, stop the 100 Hz tick waking a CPU with nothing to do, and leave the
contentious half — light sleep, which fights the latency the template is tuned for — to
a battery-powered fork. Two items, landable independently, in that order.

**What changed it:** reading `esp_pm`'s `get_lowest_allowed_mode()` instead of its
Kconfig help text. The mode ladder bottoms out at `PM_MODE_APB_MIN` whenever
`s_light_sleep_en` is false, and `PM_MODE_LIGHT_SLEEP` is unreachable from there;
`should_skip_light_sleep()` then refuses every sleep attempt because `s_mode` is never
the sleep mode. `s_light_sleep_en` comes from one place — the `light_sleep_enable` field
of the struct passed to `esp_pm_configure()`. So tickless idle with light sleep disabled
installs the hook, runs the arithmetic in `vApplicationSleep()`, and sleeps never. It
costs RTOS tick accuracy and buys nothing at all.

**Now:** they are not two items in an order, they are one decision with a misleading
name. The Kconfig option reads as a scheduler optimization and behaves as a switch that
is inert until a *different* switch is thrown; "also known as automatic light sleep" in
its help text is the whole truth rather than an aside. So the cheap half of the power
work does not exist, and enabling tickless idle on its own is strictly worse than not —
which is why it is called out as deliberately *not* enabled in `sdkconfig.defaults`
rather than silently omitted. Someone reading that file will otherwise reach for it for
exactly the reason we did.

The general shape: a config flag that depends on a runtime field is not a config flag,
and its help text describes what it *enables* rather than what it *does*. The evidence
for "this option is a free prerequisite" has to come from the code that reads it.

**Follows:** the power work splits into DFS (landed in the template, no behavioural
cost) and light sleep (a fork's decision, and it now carries tickless idle with it
rather than finding it already done). Nothing in between.
