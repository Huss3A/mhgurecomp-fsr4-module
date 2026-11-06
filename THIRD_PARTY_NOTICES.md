# Third-party notices

## In this repository

- `third_party/ffx-api/` : AMD FidelityFX ffx-api headers, SDK 2.4.0. Copyright (C) 2026 Advanced Micro Devices, Inc.; MIT
  licence, text in `third_party/ffx-api/LICENSE.txt` (and in each file's header). Unmodified.
- `third_party/ffx_vk_portable.h` : the FSR Vulkan portability layer's public header, as used by MHGURecomp. Copyright (c) 2026
  Q2RTX FSR Vulkan contributors; MIT licence, text in `third_party/LICENSE-ffx_vk_portable.txt`. Used only for the layout of the
  FSR 3.1 dispatch structure the module intercepts.
- `engine_module_api.h` : from MHGURecomp (https://github.com/morerokk/MHGURecomp, `runtime/engine_module_api.h`), MIT licence,
  Copyright (c) 2026 Rokk.

## Downloaded by `make` (not stored in this repository)

- `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` : AMD's signed FSR SDK 2.4.0 binaries, from
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag `v2.4.0`, `Kits/FidelityFX/signedbin`), unmodified. AMD's
  `docs/license.md` lists these files as subject to the MIT licence. `make` saves that file as `AMD-FSR-SDK.txt` beside the DLLs,
  and a release package must keep it there. The SHA-256 of each downloaded file is pinned in the Makefile.
