"""Configure Veyra to point at a user's existing local DLSS-NR-on-AMD (NeuralHub) install.

IMPORTANT LICENSE NOTICE:
danielblnc/DLSS-NR-on-AMD is proprietary freeware whose license forbids bundling,
copying/redistributing, or reverse engineering. This helper NEVER downloads,
patches, or copies NeuralHub binaries: it only writes a local path pointer file
(runtime_local/amd-nr-neuralhub/neuralhub-path.txt) pointing to the folder you
already installed from https://github.com/danielblnc/DLSS-NR-on-AMD/releases.

Example:
  py -3.11 scripts/amd-nr/stage-neuralhub-runtime.py --veyra-app "C:/Veyra"
  py -3.11 scripts/amd-nr/stage-neuralhub-runtime.py --veyra-app "C:/Veyra" --neuralhub-dir "C:/Users/you/AppData/Local/NeuralHub"
"""
import argparse
import os
import sys
from pathlib import Path

CANDIDATE_NAMES = ("dlssnr_core.dll", "_nvngx.dll", "nvngx.dll", "nvngx_dlssnr.dll")


def find_dll(folder: Path) -> Path | None:
    if folder.is_file():
        return folder
    if not folder.is_dir():
        return None
    for sub in (folder, folder / "runtime", folder / "bin", folder / "dlss"):
        if not sub.is_dir():
            continue
        for name in CANDIDATE_NAMES:
            p = sub / name
            if p.is_file():
                return p
    return None


def auto_detect() -> Path | None:
    local = os.environ.get("LOCALAPPDATA")
    if local:
        for name in ("NeuralHub", "Programs/NeuralHub"):
            candidate = Path(local) / name
            if find_dll(candidate):
                return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Point Veyra at a local DLSS-NR-on-AMD (NeuralHub) installation.")
    parser.add_argument("--veyra-app", type=Path, required=True, help="Veyra application directory containing runtime_local/")
    parser.add_argument("--neuralhub-dir", type=Path, help="Local NeuralHub or NGX D3D12 runtime folder (auto-detected if omitted)")
    args = parser.parse_args()

    source = args.neuralhub_dir.resolve() if args.neuralhub_dir else auto_detect()
    if not source:
        print(
            "ERROR: Could not locate a local NeuralHub NGX D3D12 runtime.\n"
            "Install NeuralHub from https://github.com/danielblnc/DLSS-NR-on-AMD/releases\n"
            "and pass --neuralhub-dir <path>.",
            file=sys.stderr,
        )
        return 1

    dll = find_dll(source)
    if not dll:
        print(f"ERROR: No NGX D3D12 DLL ({', '.join(CANDIDATE_NAMES)}) found under {source}", file=sys.stderr)
        return 1

    target_dir = args.veyra_app.resolve() / "runtime_local" / "amd-nr-neuralhub"
    target_dir.mkdir(parents=True, exist_ok=True)
    pointer = target_dir / "neuralhub-path.txt"
    pointer.write_text(
        "# Local pointer to user-installed DLSS-NR-on-AMD (NeuralHub) runtime.\n"
        f"{dll.parent.resolve()}\n",
        encoding="utf-8",
    )
    print(f"OK: Configured pointer {pointer} -> {dll.parent.resolve()} (detected {dll.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
