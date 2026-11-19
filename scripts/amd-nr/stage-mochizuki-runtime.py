"""Stage a user's local mochizuki0323/DLSSNR-AMD Windows runtime for Veyra.

This script does NOT download or bundle proprietary NVIDIA model weights.
Point --source at an extracted mochizuki0323/DLSSNR-AMD Windows release folder
(after running its own installer/weight extraction with your own nvngx_dlssnr.dll),
and --veyra-app at your extracted Veyra folder containing runtime_local/.

Example:
  py -3.11 scripts/amd-nr/stage-mochizuki-runtime.py --source "C:/Games/DLSSNR-AMD" --veyra-app "C:/Veyra"
  py -3.11 scripts/amd-nr/stage-mochizuki-runtime.py --source "C:/Games/DLSSNR-AMD" --veyra-app "C:/Veyra" --link-only
"""
import argparse
import ctypes
import shutil
import sys
from pathlib import Path

REQUIRED_EXPORTS = (
    b"NVSDK_NGX_D3D12_Init_Ext",
    b"NVSDK_NGX_D3D12_CreateFeature",
    b"NVSDK_NGX_D3D12_EvaluateFeature",
    b"NVSDK_NGX_D3D12_ReleaseFeature",
)

DEFAULT_INI = """[Network]
Model = CC_SMALL_FP8
ACO Mode = 1
OptimizedLayers = 3
"""


def find_core_dll(source: Path) -> Path | None:
    for name in ("dlssnr_core.dll", "_nvngx.dll", "nvngx.dll"):
        candidate = source / name
        if candidate.is_file():
            return candidate
    return None


def verify_exports(dll_path: Path) -> list[str]:
    missing = []
    if sys.platform != "win32":
        return missing
    kernel32 = ctypes.windll.kernel32
    kernel32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    kernel32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
    # DONT_RESOLVE_DLL_REFERENCES (0x1) lets us inspect export names safely without executing DllMain.
    handle = kernel32.LoadLibraryExW(str(dll_path.resolve()), None, 0x00000001)
    if not handle:
        return ["<LoadLibraryExW failed>"]
    try:
        for exp in REQUIRED_EXPORTS:
            if not kernel32.GetProcAddress(handle, exp):
                missing.append(exp.decode("ascii"))
    finally:
        kernel32.FreeLibrary(handle)
    return missing


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage mochizuki0323/DLSSNR-AMD for Veyra.")
    parser.add_argument("--source", type=Path, required=True, help="Folder containing dlssnr_core.dll and dlssnr-amd/ assets")
    parser.add_argument("--veyra-app", type=Path, required=True, help="Veyra application directory containing runtime_local/")
    parser.add_argument("--link-only", action="store_true", help="Write runtime-path.txt pointer instead of copying files")
    parser.add_argument("--model", default="CC_SMALL_FP8",
                        choices=("CC_SMALL_FP8", "CC_SMALL_FP8_EXPERIMENTAL", "CC_SILVER_AARDWOLD", "CC_SILVER_AARDWOLD_OPTIMIZED"),
                        help="Default [Network] Model to write if dlssnr-amd.ini is missing")
    args = parser.parse_args()

    source = args.source.resolve()
    dll = find_core_dll(source)
    if not dll:
        print(f"ERROR: Could not find dlssnr_core.dll / _nvngx.dll / nvngx.dll in {source}", file=sys.stderr)
        return 1

    missing = verify_exports(dll)
    if missing:
        print(f"ERROR: {dll.name} is missing required NGX Feature 18 exports: {', '.join(missing)}", file=sys.stderr)
        return 1

    target = (args.veyra_app.resolve() / "runtime_local" / "amd-nr-mochizuki")
    target.mkdir(parents=True, exist_ok=True)

    if args.link_only:
        (target / "runtime-path.txt").write_text(str(source) + "\n", encoding="utf-8")
        print(f"OK: Wrote pointer {target / 'runtime-path.txt'} -> {source}")
        return 0

    shutil.copy2(dll, target / dll.name)
    assets_dir = source / "dlssnr-amd"
    if assets_dir.is_dir():
        dest_assets = target / "dlssnr-amd"
        if dest_assets.exists():
            shutil.rmtree(dest_assets)
        shutil.copytree(assets_dir, dest_assets)
    else:
        print(f"WARNING: {assets_dir} not found; ensure extracted model weights & shaders are present next to {dll.name}.")

    src_ini = source / "dlssnr-amd.ini"
    dst_ini = target / "dlssnr-amd.ini"
    if src_ini.is_file():
        shutil.copy2(src_ini, dst_ini)
    elif not dst_ini.exists():
        dst_ini.write_text(DEFAULT_INI.replace("CC_SMALL_FP8", args.model), encoding="utf-8")

    print(f"OK: Staged Mochizuki DLSSNR-AMD ({dll.name}) into {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
