#!/usr/bin/env python3
"""Copy the flash images of a build into one folder and write manifest.json for the web installer.

The manifest lists every image with its flash offset, size and SHA-256. NVS (0x9000) is not in
the list, so an update keeps the device settings.

Usage: package_firmware.py <build dir> <output dir> <version> [release url]
"""
import hashlib
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

# Short, stable names for the release assets.
NAMES = {
    "bootloader": "bootloader.bin",
    "partition-table": "partition-table.bin",
    "app": "epaper-stock.bin",
    "storage": "storage.bin",
    "audio": "audio.bin",
}


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    build = Path(sys.argv[1])
    out = Path(sys.argv[2])
    version = sys.argv[3]
    release_url = sys.argv[4] if len(sys.argv) > 4 else ""

    args = json.loads((build / "flasher_args.json").read_text())
    out.mkdir(parents=True, exist_ok=True)

    parts = []
    for key, name in NAMES.items():
        entry = args[key]
        src = build / entry["file"]
        dst = out / name
        shutil.copyfile(src, dst)
        data = dst.read_bytes()
        parts.append({
            "name": key,
            "file": name,
            "offset": int(entry["offset"], 16),
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    parts.sort(key=lambda p: p["offset"])

    manifest = {
        "name": "ePaper Stock Ticker",
        "version": version,
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "chip": args["extra_esptool_args"]["chip"],
        "flash_settings": args["flash_settings"],
        "release_url": release_url,
        "parts": parts,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    # The same layout as an esptool command, for users who flash by hand.
    files = " ".join(f"0x{p['offset']:x} {p['file']}" for p in parts)
    fs = args["flash_settings"]
    (out / "flash-command.txt").write_text(
        "esptool --chip esp32s3 --before default_reset --after hard_reset write_flash "
        f"--flash_mode {fs['flash_mode']} --flash_size {fs['flash_size']} --flash_freq {fs['flash_freq']} "
        f"{files}\n")

    for p in parts:
        print(f"0x{p['offset']:06x}  {p['size']:>8}  {p['file']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
