import argparse
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("--project", required=True)
parser.add_argument("--suffix", default="")
parser.add_argument("--version", required=True)
parser.add_argument("--output-dir", required=True)
args = parser.parse_args()

project_dir = Path(args.project)
build_root = project_dir / ".pio" / "build"
base_name = f"{args.project}{args.suffix}"
output_dir = Path(args.output_dir)
output_dir.mkdir(parents=True, exist_ok=True)

if not build_root.is_dir():
    print(f"Missing PlatformIO build directory: {build_root}", file=sys.stderr)
    sys.exit(1)

build_dirs = [
    path
    for path in build_root.iterdir()
    if path.is_dir() and (path / "firmware.bin").is_file() and (path / "flasher_args.json").is_file()
]

if len(build_dirs) != 1:
    print(f"Expected one build output with firmware.bin and flasher_args.json under {build_root}, found {len(build_dirs)}", file=sys.stderr)
    for path in build_dirs:
        print(f"  {path}", file=sys.stderr)
    sys.exit(1)

build_dir = build_dirs[0]
firmware_bin = build_dir / "firmware.bin"
flasher_args = build_dir / "flasher_args.json"
merged_bin = output_dir / f"{base_name}_merged.bin"
manifest_json = output_dir / f"{base_name}_manifest.json"
ota_bin = output_dir / f"{base_name}_ota.bin"
firmware_zip = output_dir / f"{base_name}.zip"

if not flasher_args.is_file():
    print(f"Missing PlatformIO flasher metadata: {flasher_args}", file=sys.stderr)
    sys.exit(1)

flasher = json.loads(flasher_args.read_text(encoding="utf-8"))
merge_args = list(flasher["write_flash_args"])
flash_files = flasher["flash_files"]

for offset, file_name in sorted(flash_files.items(), key=lambda item: int(item[0], 16)):
    image_path = build_dir / file_name

    if not image_path.is_file():
        image_path = build_dir / image_path.name

    if not image_path.is_file():
        fallback_names = {
            "air_firmware.bin": "firmware.bin",
            "partition-table.bin": "partitions.bin",
        }
        fallback_name = fallback_names.get(image_path.name)

        if fallback_name is not None:
            image_path = build_dir / fallback_name

    if not image_path.is_file():
        print(f"Flash image from flasher_args.json does not exist: {build_dir / file_name}", file=sys.stderr)
        sys.exit(1)

    merge_args.extend([offset, str(image_path)])

chip = flasher.get("extra_esptool_args", {}).get("chip")
command = [sys.executable, "-m", "esptool"]

if chip:
    command.extend(["--chip", chip])

command.extend(["merge_bin", "-o", str(merged_bin), *merge_args])
subprocess.run(command, check=True)

shutil.copy2(firmware_bin, ota_bin)

env_name = build_dir.name.lower()
chip_family_by_env = {
    "esp32cam": "ESP32",
    "esp32s3sense": "ESP32-S3",
    "esp32c5": "ESP32-C5",
}
chip_family = chip_family_by_env.get(env_name)

if chip_family is None:
    print(f"Unknown PlatformIO env for ESP Web Tools chipFamily: {build_dir.name}", file=sys.stderr)
    sys.exit(1)

manifest = {
    "name": base_name,
    "version": args.version,
    "new_install_prompt_erase": True,
    "builds": [
        {
            "chipFamily": chip_family,
            "parts": [
                {
                    "path": merged_bin.name,
                    "offset": 0,
                }
            ],
        }
    ],
}

manifest_json.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

with zipfile.ZipFile(firmware_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for path in (merged_bin, manifest_json, ota_bin):
        archive.write(path, arcname=path.name)

print(f"Packaged {base_name}:")
print(f"  {merged_bin}")
print(f"  {manifest_json}")
print(f"  {ota_bin}")
print(f"  {firmware_zip}")
