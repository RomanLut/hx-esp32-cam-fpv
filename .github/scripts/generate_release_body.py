import argparse
import json
from pathlib import Path
from urllib.parse import quote, urlencode


parser = argparse.ArgumentParser()
parser.add_argument("--payload-dir", required=True)
parser.add_argument("--repository", required=True)
parser.add_argument("--owner", required=True)
parser.add_argument("--tag", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()

payload_dir = Path(args.payload_dir)
repo_name = args.repository.split("/", 1)[1]
release_base_url = f"https://github.com/{args.repository}/releases/download/{quote(args.tag)}"
pages_install_url = f"https://{args.owner}.github.io/{repo_name}/install.html"

rows = []
firmware_versions = []

for manifest_path in sorted(payload_dir.glob("*_manifest.json")):
    firmware = manifest_path.name.removesuffix("_manifest.json")
    ota_path = payload_dir / f"{firmware}_ota.bin"

    if not ota_path.is_file():
        continue

    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    version = str(manifest.get("version", args.tag))
    firmware_versions.append(version)
    manifest_url = f"{release_base_url}/{quote(manifest_path.name)}"
    ota_url = f"{release_base_url}/{quote(ota_path.name)}"
    install_query = urlencode(
        {
            "firmware": firmware,
            "version": version,
            "manifest": manifest_url,
            "ota": ota_url,
        }
    )
    install_url = f"{pages_install_url}?{install_query}"

    rows.append((firmware, install_url, ota_url))

version = firmware_versions[0] if firmware_versions else args.tag
android_apks = sorted(payload_dir.glob("hx-esp32-cam-fpv-*.apk"))
android_apk = next((path for path in android_apks if "-oculus-" not in path.name), None)
oculus_apk = next((path for path in android_apks if "-oculus-" in path.name), None)

gs_rows = [
    ("Radxa Zero 3", f"{release_base_url}/{quote(f'espvrx_dualboot_radxa3w_{version}.zip')}"),
    ("Raspberry Pi 2/3/4", f"{release_base_url}/{quote(f'espvrx_rpi_{version}.img.gz')}"),
]

if android_apk is not None:
    gs_rows.append(("Android", f"{release_base_url}/{quote(android_apk.name)}"))

if oculus_apk is not None:
    gs_rows.append(("Oculus Quest", f"{release_base_url}/{quote(oculus_apk.name)}"))

lines = [
    "## GS software",
    "",
    "| Platform | Download link |",
    "|---|---|",
]

for platform, download_url in gs_rows:
    lines.append(f"| {platform} | [Download]({download_url}) |")

lines.extend(
    [
        "",
        "## Firmware",
        "",
    ]
)

lines.extend([
    "| Firmware | Install | Download OTA image |",
    "|---|---|---|",
])

for firmware, install_url, ota_url in rows:
    lines.append(f"| {firmware} | [Install]({install_url}) | [Download]({ota_url}) |")

Path(args.output).write_text("\n".join(lines) + "\n", encoding="utf-8")
