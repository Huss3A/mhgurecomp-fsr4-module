# FSR 4 engine module for MHGU Recomp

Adds AMD **FSR 4** upscaling to [MHGURecomp](https://github.com/morerokk/MHGURecomp)'s FSR option, as an *engine module*: a
native library the port loads from its `engine_modules/` folder. It is not a mod and not a Forge plugin, and it does not touch
either. The port itself has no FSR 4 code and no hooks for it; this module is separate software and is maintained here, not by
the port's maintainers. Please report problems with it here.

## Requirements

- Windows x64.
- A build of MHGURecomp that has engine modules (upstream master after the "Engine plugins are now engine modules" commit).
  Developed and tested against the 0.15.0 development build at upstream commit `81d5a93`.
- `[Upscaler] FSR = On` in `mhgurecomp.ini`. With FSR Off the module does nothing.
- A GPU AMD's FSR 4 supports. Tested only on a Radeon RX 9070 XT (RDNA 4, the FP8 model). RDNA 2/3 use AMD's INT8 models and the
  module enables the features they need, but that is **untested** here. AMD's own loader decides whether your GPU gets FSR 4;
  if it doesn't, the port keeps using FSR 3.1.

## Install

Either build it (below) or take a release zip. Then:

1. Copy the `fsr4` folder to `engine_modules\fsr4\` beside `mhgurecomp.exe`.
2. Add a line `fsr4` to `engine_modules\enabled.txt` (create the file if it doesn't exist).
3. Start the game. `log.txt` should say `[engine modules] fsr4: loaded` and, once a quest renders,
   `[fsr4] first FSR 4.1.2 dispatch recorded`.

To uninstall, remove the line from `enabled.txt` (or delete the folder).

Engine modules are native code that runs with the game's rights. Only enable ones you trust; this one is open source here and
downloads nothing at run time.

## Build

MSYS2 UCRT64 with `clang`, `make` and `curl`:

    make
    make install MODULES=/path/to/mhgurecomp/engine_modules     # copies it and adds `fsr4` to enabled.txt
    make package                                                # optional: build/fsr4-module-<version>.zip (needs zip)

`make` downloads AMD's FSR SDK 2.4.0 loader and upscaler DLLs from AMD's tagged release and checks their SHA-256. They are
not stored in this repository.

## How it works

The port exposes no FSR 4 hooks, so the module applies its changes in memory when the port loads it:

1. **Vulkan:** it intercepts `vkCreateInstance` and `vkCreateDevice` in the exe's import table. The instance asks for Vulkan 1.3
   (FSR 4's shaders use SPIR-V 1.6), and the device is given the extensions and features AMD's models use (cooperative matrix,
   float8, integer dot product, compute shader derivatives...), wherever the GPU has them. If the driver refuses, the module
   retries with exactly what the port asked for.
2. **Upscale:** the exe exports its FSR 3.1 dispatch (`ffxVkPortableUpscaleContextRecordDispatch`), which already receives
   everything FSR 4 needs: colour, depth, motion vectors, reactive mask, jitter, camera and sizes. The module takes that call and
   records AMD's FSR 4 into the same command buffer.
3. **Failure:** if anything is missing or fails, the module puts the original code back for good and FSR 3.1 carries on from a
   fresh history, so the game never loses upscaling. Its entry point also undoes every change when it refuses to start.
4. **Exit:** AMD's context is released when the game closes normally.

Because it depends on the port's internals (the exported FSR 3.1 dispatch function and its struct, and the way the port imports
Vulkan), a future port release may break it. It checks what it finds and declines to load rather than guess; the log says why.

## Settings and debugging

No settings of its own: it follows `[Upscaler] FSR`, `FSRQuality` and `Sharpness`. Environment variables, for testing:

| Variable | Effect |
|---|---|
| `MHGU_FSR_DEBUG=1` | FFX debug checking of the inputs (messages in the log) |
| `MHGU_FSR4_LOADER=<path>` | use another loader DLL (its upscaler DLL is looked up beside it) |
| `MHGU_FSR4_LINEAR=1` | A/B test: the colour is not flagged as gamma-encoded |
| `MHGU_FSR4_FAILTEST=<n>` | report the n-th upscale as failed, to see the FSR 3.1 fallback |

Log lines all start with `[fsr4]`.

## Licences

- This module: MIT (`LICENSE`).
- Vendored headers and the AMD DLLs it fetches: see `THIRD_PARTY_NOTICES.md`.
- Not affiliated with or endorsed by AMD or the MHGURecomp maintainers. "FSR" and "FidelityFX" are AMD's names.

## Keeping in step with the port

`engine_module_api.h` is the port's `runtime/engine_module_api.h`; copy it again when the port changes its API.
