# Measurements (work in progress)

Machine: AMD Radeon RX 9070 XT (RDNA 4), Adrenalin 32.0.32015.2008, WDDM 3.2, Windows 11 Pro 25H2 (26200),
Ryzen 7 9700X, 2560x1440 360 Hz FreeSync Premium Pro panel. Frame generation: AMD ML frame generation 4.0.1 (FidelityFX
SDK 2.3.0 signed DLL). Test program: `fg_host` (borderless full screen, 2x, 5 s per run). Timing from Microsoft's
DxgKrnl ETW events (flip DDI calls, hardware flip queue log completions), not from our code.

## The hardware flip queue on RDNA 4
- `D3DKMT_WDDM_3_0_CAPS`: `HwFlipQueueSupportState = STABLE`, `HwFlipQueueEnabled = 1`.
- Every independent flip goes through `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3` with a `TargetFlipTime`
  and completes through the hardware flip queue log (`VSyncHwFlipQueueLogUpdate`).
- FreeSync **off**: flips queued ahead complete exactly on their intended vblank (frame-interval error p99
  0.001-0.003 ms at 120 / 180 fps output on 360 Hz).
- FreeSync **on**: the driver shows queued flips at the next refresh the panel can make, ignoring `TargetFlipTime`
  (median about 3 ms early). Windows hands the driver identical flips either way, so this is driver behaviour.

## Frame-interval error, full screen, FreeSync on (ms)
| run | swapchain | p50 | p95 | p99 | >1 ms | real frames / 300 |
|---|---|---|---|---|---|---|
| 120 Hz, base 59.4 fps | AMD 3.1.7 | 0.115 | 0.409 | 1.028 | 1.2 % | 297 |
| 120 Hz, base 59.4 fps | fgswap `queue` | 0.084 | 0.084 | 0.089 | 0 % | 297 |
| 120 Hz + 14 busy threads | AMD 3.1.7 | 0.094 | 0.322 | 0.457 | 0 % | 244 |
| 360 Hz + 14 busy threads | AMD 3.1.7 | 0.113 | 2.78 | 83.4 | 21.9 % | 284 |
| 360 Hz + 14 busy threads | fgswap `spin` | 0.125 | 2.78 | 16.6 | 7.3 % | 300 |

Not yet conclusive: `spin` vs AMD at 360 Hz idle. Single runs of *both* showed random 30-120 ms stalls below the
presenter (Windows/GPU held finished flips), so this needs alternating repeats. `early` mode still has refresh-late
frames.

## Windowed (DWM-composed), idle, `Present()` call timing (ms)
| swapchain | SD | p95 | p99 | max |
|---|---|---|---|---|
| AMD 3.1.7 | 0.176 | 0.385 | 0.574 | 0.710 |
| fgswap `spin` | 0.117 | 0.273 | 0.393 | 0.479 |

## Notes
- AMD's swapchain only generates frames when the game sets `frameGenerationCallback` (the SDK sample pattern);
  fgswap generates either way.
- FreeSync + `queue` latency: each flip waits about one output frame in the driver before scan-out.
