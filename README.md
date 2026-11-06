# FSR 4 engine module for MHGU Recomp

An engine module (not a mod, not a Forge plugin) that adds AMD FSR 4 to [MHGURecomp](https://github.com/morerokk/MHGURecomp)'s FSR upscaler.
The port has no FSR 4 code and no hooks for it: this module applies its changes to the running program itself, in memory.

* Instance and device: asks for Vulkan 1.3 and the extensions/features AMD's FSR 4 models use (where the GPU has them).
* Upscale: takes over the port's FSR 3.1 dispatch, records AMD's FSR 4 with the same inputs. If anything fails it puts the
  original code back and FSR 3.1 carries on.
* Needs `[Upscaler] FSR = On` in mhgurecomp.ini. Windows x64 only. Needs a GPU the AMD DLLs support (Radeon RX 9000 series for the
  FP8 model; RDNA 2/3 use AMD's INT8 models; untested here).

## Build and install

MSYS2 UCRT64 with `clang`, `make`, `curl`:

    make
    make install MODULES=/path/to/mhgurecomp/engine_modules

`make` downloads AMD's FSR SDK 2.4.0 loader and upscaler DLLs from AMD's tagged release and checks their SHA-256. They are not
in this repository. `install` copies everything to `engine_modules/fsr4/` and adds `fsr4` to `engine_modules/enabled.txt`.

`engine_module_api.h` is the port's (runtime/engine_module_api.h); keep it in step with the port.

## Licences

This module: MIT (LICENSE). `third_party/ffx-api`: AMD's ffx-api headers, MIT (their notice stays). AMD's DLLs: listed under the
MIT licence in AMD's license.md (AMD-FSR-SDK.txt is shipped beside them).
