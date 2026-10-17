#!/usr/bin/env python3
"""Copy the flash images of the board builds into one folder and write manifest.json for the
web installer.

The manifest lists, for each board, every image with its flash offset, size and SHA-256.
NVS (0x9000) is not in the list, so an update keeps the device settings.

Usage: package_firmware.py <output dir> <version> <release url> <board>=<build dir> [...]
Example: package_firmware.py dist v1.1.0 "" v1=build v2=build_v2
"""
import hashlib
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

# Board data for the installer. The installer selects the board by the flash size it reads.
BOARDS = {
    "v1": {"name": "V1 (ESP32-S3FH4R2, 4 MB flash, 2 MB PSRAM)", "flash_size": "4MB", "psram": "2MB"},
    "v2": {"name": "V2 (ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM)", "flash_size": "8MB", "psram": "8MB"},
}

# Short, stable names for the release assets: <name>-<board>.bin
NAMES = {
    "bootloader": "bootloader",
    "partition-table": "partition-table",
    "app": "epaper-stock",
    "storage": "storage",
    "audio": "audio",
}


def package_board(board: str, build: Path, out: Path) -> dict:
    args = json.loads((build / "flasher_args.json").read_text())
    parts = []
    for key, name in NAMES.items():
        entry = args[key]
        dst = out / f"{name}-{board}.bin"
        shutil.copyfile(build / entry["file"], dst)
        data = dst.read_bytes()
        parts.append({
            "name": key,
            "file": dst.name,
            "offset": int(entry["offset"], 16),
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    parts.sort(key=lambda p: p["offset"])

    fs = args["flash_settings"]
    files = " ".join(f"0x{p['offset']:x} {p['file']}" for p in parts)
    command = ("esptool --chip esp32s3 --before default_reset --after hard_reset write_flash "
               f"--flash_mode {fs['flash_mode']} --flash_size {fs['flash_size']} "
               f"--flash_freq {fs['flash_freq']} {files}")

    for p in parts:
        print(f"{board}  0x{p['offset']:06x}  {p['size']:>8}  {p['file']}")
    return {
        "id": board,
        **BOARDS[board],
        "chip": args["extra_esptool_args"]["chip"],
        "flash_settings": fs,
        "parts": parts,
        "command": command,
    }


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    version = sys.argv[2]
    release_url = sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)

    boards = []
    for spec in sys.argv[4:]:
        board, build = spec.split("=", 1)
        if board not in BOARDS:
            print(f"unknown board: {board}")
            return 2
        boards.append(package_board(board, Path(build), out))

    manifest = {
        "name": "ePaper Stock Ticker",
        "version": version,
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "release_url": release_url,
        "boards": [{k: v for k, v in b.items() if k != "command"} for b in boards],
    }
    # A browser can keep the v1.0.x page in its cache for some minutes. That page reads
    # only the top-level "parts" and accepts only the V1 board.
    v1 = next((b for b in boards if b["id"] == "v1"), None)
    if v1:
        manifest["parts"] = v1["parts"]
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    # The same layout as esptool commands, for users who flash by hand.
    lines = []
    for b in boards:
        lines += [f"# {b['name']}", b["command"], ""]
    (out / "flash-command.txt").write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
