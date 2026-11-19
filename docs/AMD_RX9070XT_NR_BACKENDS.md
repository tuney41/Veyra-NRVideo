# AMD RX 9070 XT (RDNA 4) Multi-Backend Neural Rendering Guide

This fork of **Veyra** expands AMD Neural Rendering (NR) beyond the single hardcoded `lmxxf` 0.44 path so **AMD Radeon RX 9070 XT (RDNA 4)** — as well as RX 7000 (RDNA 3) and RX 6000 (RDNA 2) where supported — can choose between **four independent Neural Rendering runtimes** directly inside Veyra's **List Mode**, **Node Editor**, **Fullscreen Quick Controls**, and **Windows 11 Screen Capture → Game Mode Overlay**.

## Supported AMD Neural Rendering Backends

| Runtime ID | UI Name | Upstream Project | License | Supported GPUs | Max Internal NR Res | Model Controls |
|---|---|---|---|---|---|---|
| `4` | **RX 9000 · lmxxf** | [`lmxxf/dlss5-on-amd-9070xt-porting`](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) | MIT | RX 9000 (RX 7000 routes to RedDuke) | ≤ 1080p budget | Intensity + Small/Original |
| `5` | **RX 9000 / 7000 · Mochizuki DLSSNR-AMD** | [`mochizuki0323/DLSSNR-AMD`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) | MIT | RX 9000 (FP8 WMMA), RX 7000 (FP16 exp.) | **Up to Native 4K** | **All 6 DLSSNR controls** + `CC_SMALL_FP8` / `CC_SILVER_AARDWOLD` + `dlssnr-amd.ini` live sync |
| `6` | **RX 9000 / 7000 · RedDuke** | [`RedDukeDev/dlss5-nr-amd-custom`](https://github.com/RedDukeDev/dlss5-nr-amd-custom) | MIT | **RX 9000 (unlocked)** & RX 7000 | ≤ 1080p budget | Intensity + Small/Original |
| `7` | **RX 9000 / 7000 / 6000 · DLSS-NR-on-AMD (NeuralHub)** | [`danielblnc/DLSS-NR-on-AMD`](https://github.com/danielblnc/DLSS-NR-on-AMD) | Freeware (user-installed only) | RX 9000, RX 7000, RX 6000 | **Up to Native 4K** | **All 6 DLSSNR controls** + CC/HT preset |

> **Legal / Licensing Boundary:** No proprietary NVIDIA DLLs (`nvngx_dlssnr.dll`), model weights, or closed-source `NeuralHub` binaries are bundled in this repository or its releases. Each runtime loads strictly from your own local installation.

---

## Quick Setup

### 1. Mochizuki DLSSNR-AMD (`runtime 5` — Recommended for RX 9070 XT)
Why it shines on Windows 11 + RX 9070 XT:
- Runs native RDNA 4 **FP8 WMMA** compute shaders via Mesa ACO machine code or native D3D12 ELF (`~2.9–3.3 ms` at 1080p, `~4.8–5.5 ms` at 1440p, `~11–13 ms` at 4K).
- Exposes a clean MSVC-ABI D3D12 NGX Feature 18 core (`dlssnr_core.dll`) that Veyra drives directly — **no DXGI swapchain hook conflicts**, so it works seamlessly inside **Veyra's Windows 11 Screen Capture → Game Mode Overlay**, capture cards, PS5/Xbox/PC streaming, local videos, and 4K exports.
- Unlocks **1440p / Native 4K** internal NR and **all 6 model sliders** (Local Tone, Local Structure, Skin, Style 0/1/2, Auto Mask).

Setup:
1. Download the Windows release of [`mochizuki0323/DLSSNR-AMD`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) and extract weights using your own legitimately obtained `nvngx_dlssnr.dll` (310.9.16+).
2. Run the staging helper (or copy `dlssnr_core.dll`, `dlssnr-amd/`, and `dlssnr-amd.ini` into `runtime_local/amd-nr-mochizuki/`):
   ```powershell
   py -3.11 scripts/amd-nr/stage-mochizuki-runtime.py --source "C:\path\to\DLSSNR-AMD" --veyra-app "C:\path\to\Veyra"
   ```
   Or use `--link-only` (or set `VEYRA_AMD_NR_MOCHIZUKI_DIR`) to point directly at your existing folder without copying files.
3. Optional power-user environment overrides:
   - `VEYRA_MOCHIZUKI_EXPERIMENTAL=1` — selects `CC_SMALL_FP8_EXPERIMENTAL` when Performance mode is ON.
   - `VEYRA_MOCHIZUKI_MODEL=CC_SILVER_AARDWOLD_OPTIMIZED` — selects any exact Mochizuki model name.

### 2. RedDukeDev (`runtime 6` — Now Unlocked for RX 9070 XT)
Previously locked to RX 7000 only; now selectable on both **RX 9000** and **RX 7000** using `runtime_local/amd-nr-rdna3/` (requires AMD HIP 7 driver, `amdhip64_7.dll`).

### 3. danielblnc / DLSS-NR-on-AMD (`runtime 7` — User-Installed Only)
1. Install `NeuralHub` from the official release page: https://github.com/danielblnc/DLSS-NR-on-AMD/releases
2. Point Veyra to your local installation without copying or redistributing any files:
   ```powershell
   py -3.11 scripts/amd-nr/stage-neuralhub-runtime.py --veyra-app "C:\path\to\Veyra"
   ```
   Or set `VEYRA_AMD_NR_NEURALHUB_DIR` to the folder containing your local NGX D3D12 provider DLL.
   > *Compatibility note:* `DLSS-NR-on-AMD` is designed primarily as a DirectX 12 game proxy loader; Veyra invokes standard public `NVSDK_NGX_D3D12_*` Feature 18 entry points (`dlssnr_core.dll` / `_nvngx.dll` / `nvngx.dll` / `nvngx_dlssnr.dll`). If a given NeuralHub release only injects via `dxgi.dll`/`version.dll` into games and does not expose direct NGX D3D12 Feature 18 exports, use **Mochizuki DLSSNR-AMD (`runtime 5`)** or **lmxxf (`runtime 4`)** for standalone video & overlay enhancement.

### 4. Using in the Windows 11 Game Mode Overlay
1. Open **Home → Screen Capture**, pick your borderless game window or monitor, and enable **Game Mode**.
2. Press **`Home`** to open Veyra's live quick controls overlay on Windows 11 (`Ctrl+Alt+Home` exits passthrough).
3. Choose **NR Version → Mochizuki DLSSNR-AMD** (or lmxxf / RedDuke / NeuralHub), adjust Performance mode, Resolution (up to 4K on Mochizuki/NeuralHub), and all 6 model sliders live.
