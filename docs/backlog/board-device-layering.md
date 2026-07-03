# DeviceManager ↔ board layering

The board defines what devices exist, yet DeviceManager (Application
layer) hard-codes them. Different boards have different devices but
should expose the same capabilities. Maybe the DeviceManager definition
belongs in the board folders (`hardware/boards/<name>/`) — undecided,
brainstorm first. (Raised 2026-07-03.)
