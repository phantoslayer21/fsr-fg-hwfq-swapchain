# fsr-fg-hwfq-swapchain

> **Status: work in progress.** Early research build. It works in a test harness and is about to get its first
> real-game playtests. Expect rough edges (see [Known gaps](#known-gaps)). Not a release.

**FSR frame-generation swapchain, paced by the hardware flip queue.** The DLL it builds is `fgswap.dll` ("fgswap" below).

fgswap is a drop-in replacement for the **frame-generation swapchain** of AMD FSR (including FSR 4 / "Redstone" ML frame
generation) on Windows / D3D12. It keeps AMD's ML frame generation exactly as it is and replaces only the part that
presents and paces the frames. It can hand the frames to the GPU's **hardware flip queue** so that the display engine,
not a spinning CPU thread, spaces them.

It is a **one-file swap**: it takes the place of AMD's 26 KB loader `amd_fidelityfx_loader_dx12.dll` in games that ship
the FSR SDK 2.x loader. AMD's own DLLs (frame generation, upscaler, denoiser) stay untouched next to it.

## Why
AMD's frame-generation swapchain (FidelityFX SDK 2.3, swapchain 3.1.7) paces generated frames in software: a
high-priority thread busy-waits on the clock and calls DXGI `Present` at each frame's moment. Windows 11 (WDDM 3.0+)
also has a **hardware flip queue**: several future flips can be queued to the display controller with a target time.
On an RX 9070 XT (RDNA 4) the driver reports it as enabled, and every independent flip already goes through it, but
nothing uses it to *meter* frame generation. fgswap does.

What we measured (RX 9070 XT, driver 32.0.32015.2008, Windows 11 25H2, 360 Hz FreeSync panel):
- **FreeSync off:** queued flips are honoured to the exact vblank (frame-interval error p99 0.001-0.003 ms).
- **FreeSync on:** the driver ignores the target time and shows each queued flip at the next refresh the panel can
  make. Setting the display refresh to the frame-generation output rate (e.g. 120 Hz for 2x of 60 fps) makes that next
  refresh the right one.
- **AMD ML frame generation 4.0.1, full screen, FreeSync on, 120 Hz:** AMD swapchain p99 frame-interval error 1.03 ms,
  fgswap `queue` mode **0.089 ms**. Under CPU load, AMD's swapchain slowed the test game down (284 / 244 of 300
  frames), fgswap did not (300 / 296).
- Details and caveats: [docs/RESULTS.md](docs/RESULTS.md).

## How it works
- Exports the five FFX API entry points (`ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`,
  `ffxDispatch`). Named `amd_fidelityfx_loader_dx12.dll`, it acts as the loader: it loads AMD's effect DLLs next to it
  by name and routes every call to them. With any other file name it acts as a proxy for one AMD DLL whose original
  path is set in `fgswap.ini` (`original=`).
- Frame-generation **swapchain** contexts (`FOR_HWND`, `NEW`, `WRAP`) are created by fgswap. The game renders into
  replacement back buffers; each `Present` copies the real frame, records the generated frame and hands both to a
  presenter thread.
- AMD's frame-generation context is configured "interpolate only"
  (`FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY`). The generated frame is produced through the public
  `ffxDispatch(FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION)`, or through the game's own `frameGenerationCallback`.
  No private AMD interface is used.
- UI: the game's present callback is honoured; a registered UI resource is blended (straight or premultiplied alpha);
  with a HUD-less image the generated frame gets the real frame's UI added back.

### Pacing modes (`fgswap.ini`, `[fgswap] pacing=`)
| mode | what paces the frames | use when |
|---|---|---|
| `spin` (default) | high-priority thread, absolute schedule, timer then short busy-spin | any refresh rate / frame rate, VRR or tearing |
| `queue` | **hardware flip queue**: each generated + real pair is queued at once | fixed refresh where the output rate divides it, or FreeSync with the display at the output rate and a steady game frame rate |
| `early` | present 1-2 ms early, the display's refresh limit holds the frame | FreeSync with the display at the output rate (still being tuned) |
| `amd` | AMD's own swapchain (fgswap only routes) | A/B comparisons |

Other keys: `early_ms`, `spin_us`, `queue_interval`, `presenter_priority`, `max_queued_frames`, `allow_tearing`, `log`,
`present_log`, `original`. See [fgswap.ini.example](fgswap.ini.example).

## Install (one file)
1. Back up the game's `amd_fidelityfx_loader_dx12.dll` (or use your store's "verify files" later).
2. Put `fgswap.dll` in its place, named `amd_fidelityfx_loader_dx12.dll`.
3. Optional: `fgswap.ini` next to it. `fgswap.log` shows what happened.

Only for single-player games without anti-cheat. Uninstall: restore AMD's loader.

## Build
`build.cmd` (Visual Studio 2022+ with C++ tools, Windows 11 SDK). Output in `bin\`:
- `fgswap.dll`: the swapchain DLL.
- `fg_host.exe`: a minimal FFX "game" (moving bar, depth, motion vectors) to compare AMD's swapchain with fgswap.
- `fg_probe.exe`: off-screen check that frame generation works through the public dispatch path.

- `caps_probe.exe`, `vblank_probe.exe`, `modes_probe.exe`, `media_probe.exe`, `pace_test.exe`: the hardware flip queue
  research tools (sources in `hwfq/`, Python analysis in `tools/`). See [docs/HWFQ.md](docs/HWFQ.md).

Neither tool ships AMD code; they load AMD's DLLs at run time (get them from the
[FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) `Kits/FidelityFX/signedbin`).

## Known gaps
- 2x only (AMD's ML frame generation produces one frame per pair).
- Not yet tested in a real game: UI composition paths (HUD-less, UI resource, present callback), resize and
  fullscreen switches, HDR, async-compute frame generation, the `WRAP`/`NEW` creation paths.
- `early` mode still lets some frames land a refresh late.
- `queue` mode adds about one output frame of latency and needs a steady frame rate at refresh/2.
- AMD's driver ignores `TargetFlipTime` when FreeSync is on; only AMD can fix that properly.
- Games without the FSR SDK 2.x loader need proxy mode (two files).

## Licence
MIT, see [LICENSE](LICENSE). The FFX API declarations in `src/ffx_abi.h` are our own, written for binary compatibility
with AMD's published FidelityFX API (MIT); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). AMD, FidelityFX and
FSR are trademarks of Advanced Micro Devices, Inc. This project is not affiliated with AMD.
