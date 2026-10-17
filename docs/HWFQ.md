# The hardware flip queue and frame generation (work in progress)

## What it is
Windows 11 (WDDM 3.0) added the **hardware flip queue**: the OS can queue several future flips per display plane to
the display controller, each with a `TargetFlipTime` in QPC units, and the hardware shows each one once its time has
come. Completion times come back through a flip queue log, so the CPU does not have to wake up per frame
([Microsoft docs](https://learn.microsoft.com/windows-hardware/drivers/display/hardware-flip-queue)). NVIDIA markets
the same idea on Blackwell as "flip metering"; Microsoft does not use that term.

- **The call:** the kernel-mode DDI `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3` with
  `DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3.TargetFlipTime`. The GPU driver implements it; only DxgKrnl
  calls it. There is no user-mode entry point.
- **Enabled when:** the driver negotiates `DXGK_FEATURE_HWFLIPQUEUE` and reports `MaxHwQueuedFlips > 1`, with GPU
  hardware scheduling on. User mode can see the result in `D3DKMT_WDDM_3_0_CAPS.HwFlipQueueSupportState` /
  `HwFlipQueueEnabled` (`hwfq/caps_probe.cpp`).
- **Observable:** DxgKrnl ETW events 386 `MMIOFlipMultiPlaneOverlay3` (the DDI call, with `TargetFlipTimeQpc`), 505
  `VSyncHwFlipQueueLogUpdate` (completion from the hardware log), 530 `SetInterruptTargetPresentId`
  (`PlaneTotalPendingFlip`), 518 `SelectPresentAtFlip`. `tools/etw_flips.py` joins them.

## How an application reaches it
With the queue enabled, every flip carries a target time:
- **DXGI / D3D12 swapchains:** DxgKrnl derives it from the sync interval
  (`previous vsync + interval x period - period/2`). Queue several presents with sync interval N and the display engine
  spaces them N refreshes apart.
- **Composition Swapchain API** (`Presentation.h`): `IPresentationManager::SetTargetTime()` + `Present()`; the time
  arrives unchanged as `PresentAtQpc` (on our system it is read as QPC). Needs D3D11 *displayable* buffers for independent
  flip; D3D12 can render into them through shared handles.
- `IDXGISwapChainMedia::SetPresentDuration` exists only on D3D11 swapchains; on our system the approved duration never
  reached the driver (`Duration = 0` on every flip).

## What we measured on an RX 9070 XT (driver 32.0.32015.2008)
- The queue is enabled (`STABLE`, enabled) and every independent flip goes through it.
- **FreeSync off:** targets are honoured. Queued flips complete exactly on their intended vblank (p99 error
  0.001-0.003 ms).
- **FreeSync on:** Windows hands the driver identical flips, but the driver shows each queued flip at the next refresh the
  panel can make (about 3 ms before the target at 360 Hz), for every lever above. Under VRR there is no regular vblank,
  so holding a flip needs a timer; the driver does not do that.
- **Workaround with FreeSync on:** set the display refresh to the frame-generation output rate (120 Hz for 2x of 60).
  "Next refresh the panel can make" is then exactly the next frame slot: queued pairs come out 8.33 ms apart (p99
  0.089 ms with AMD ML frame generation). Cost: about one output frame of queue latency. Capping the game slightly below
  refresh/2 and presenting each frame 1-2 ms early (`early` mode) is being tuned to cut that latency.

## Tools in this repo
- `hwfq/caps_probe.cpp`: D3DKMT caps (WDDM version, HAGS, hardware flip queue, independent flip, MPO), DXGI tearing,
  Composition Swapchain support with D3D11 and D3D12.
- `hwfq/vblank_probe.cpp`, `hwfq/modes_probe.cpp`, `hwfq/media_probe.cpp`: vblank cadence, available refresh rates,
  present-duration support.
- `hwfq/pace_test.cpp`: presents a frame-generation pattern through every lever: DXGI software pacing (`--api dxgi|dxgi11
  --mode sw|queue`, sync intervals, tearing, `--max-latency`, `--early-ms`, `--duration-ms`), the Composition Swapchain
  with `SetTargetTime` (`--api comp --mode meter --comp-dev 11 --target-qpc 1`), a temporary display mode
  (`--mode-hz`), CPU load (`--load`). Writes a CSV per run.
- `tools/etw_flips.py`: per-flip timing from a DxgKrnl capture decoded with `tracerpt -of XML`.
  `tools/analyze.py`: pacing summary of a `pace_test` CSV (+ capture). `tools/export_events.py`: flip events to CSV.
  `tools/manifest.py`: the DxgKrnl event table from `wevtutil gp Microsoft-Windows-DxgKrnl /ge:true /gm:true /f:xml`.

Capture (no admin needed for members of Performance Log Users):
`logman start x -p Microsoft-Windows-DxgKrnl 0x8000001 0x4 -ct perf -o x.etl -ets` ... `logman stop x -ets`.
