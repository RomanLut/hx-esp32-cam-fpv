import argparse
import html
import json
from pathlib import Path
from urllib.parse import quote, urlencode


parser = argparse.ArgumentParser()
parser.add_argument("--firmware-dir", required=True)
parser.add_argument("--repository", required=True)
parser.add_argument("--owner", required=True)
parser.add_argument("--tag", required=True)
parser.add_argument("--asset-list", required=True)
parser.add_argument("--release-asset-catalog", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()

firmware_dir = Path(args.firmware_dir)
asset_names = [
    line.strip()
    for line in Path(args.asset_list).read_text(encoding="utf-8").splitlines()
    if line.strip()
]
release_asset_catalog = json.loads(Path(args.release_asset_catalog).read_text(encoding="utf-8"))
repo_name = args.repository.split("/", 1)[1]
release_base_url = f"https://github.com/{args.repository}/releases/download/{quote(args.tag)}"
pages_base_url = f"https://{args.owner}.github.io/{repo_name}"
firmware_base_url = f"{pages_base_url}/firmware/{quote(args.tag, safe='')}"


def release_asset_url(name):
    return f"{release_base_url}/{quote(name)}"


def release_asset_url_for_tag(tag, name):
    return f"https://github.com/{args.repository}/releases/download/{quote(tag)}/{quote(name)}"


def first_asset(prefix, suffix):
    for name in asset_names:
        if name.startswith(prefix) and name.endswith(suffix):
            return name

    return None


def find_current_or_previous_asset(current_name, prefix, suffix):
    if current_name in asset_names:
        return args.tag, current_name

    for release in release_asset_catalog:
        tag = release.get("tagName")
        assets = release.get("assets") or []

        if tag == args.tag:
            continue

        for asset in assets:
            name = asset.get("name", "")

            if name.startswith(prefix) and name.endswith(suffix):
                return tag, name

    return args.tag, current_name


firmware_rows = []
versions = []

for manifest_path in sorted(firmware_dir.glob("*_manifest.json")):
    firmware = manifest_path.name.removesuffix("_manifest.json")
    ota_path = firmware_dir / f"{firmware}_ota.bin"

    if not ota_path.is_file():
        continue

    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    version = str(manifest.get("version", args.tag))
    versions.append(version)
    manifest_url = f"{firmware_base_url}/{quote(manifest_path.name)}"
    ota_url = f"{firmware_base_url}/{quote(ota_path.name)}"
    install_query = urlencode(
        {
            "firmware": firmware,
            "version": version,
            "manifest": manifest_url,
            "ota": ota_url,
        }
    )
    install_url = f"{pages_base_url}/install.html?{install_query}"
    firmware_rows.append((firmware, install_url, ota_url))

version = versions[0] if versions else args.tag
radxa_tag, radxa_asset = find_current_or_previous_asset(
    f"espvrx_dualboot_radxa3w_{version}.zip",
    "espvrx_dualboot_radxa3w_",
    ".zip",
)
rpi_tag, rpi_asset = find_current_or_previous_asset(
    f"espvrx_rpi_{version}.img.gz",
    "espvrx_rpi_",
    ".img.gz",
)
android_apk = next(
    (
        name
        for name in asset_names
        if name.startswith("hx-esp32-cam-fpv-")
        and "-oculus-" not in name
        and name.endswith(".apk")
    ),
    None,
)
oculus_apk = next(
    (
        name
        for name in asset_names
        if name.startswith("hx-esp32-cam-fpv-oculus-") and name.endswith(".apk")
    ),
    None,
)

gs_rows = [
    ("Radxa Zero 3", release_asset_url_for_tag(radxa_tag, radxa_asset)),
    ("Raspberry Pi 2/3/4", release_asset_url_for_tag(rpi_tag, rpi_asset)),
]

if android_apk:
    gs_rows.append(("Android", release_asset_url(android_apk)))

if oculus_apk:
    gs_rows.append(("Oculus Quest", release_asset_url(oculus_apk)))

html_lines = [
    "<!doctype html>",
    '<html lang="en">',
    "<head>",
    '  <meta charset="utf-8">',
    '  <meta name="viewport" content="width=device-width, initial-scale=1">',
    f"  <title>HX ESP32-CAM FPV downloads {html.escape(args.tag)}</title>",
    "  <style>",
    "    body { margin: 0; font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f6f7fb; color: #172033; line-height: 1.5; }",
    "    main { width: min(980px, calc(100% - 32px)); margin: 0 auto; padding: 48px 0; }",
    "    h1 { margin: 0 0 8px; font-size: 34px; letter-spacing: 0; }",
    "    h2 { margin-top: 34px; font-size: 22px; letter-spacing: 0; }",
    "    p { color: #59657a; }",
    "    table { width: 100%; border-collapse: collapse; background: #fff; border: 1px solid #d9deea; }",
    "    th, td { padding: 12px 14px; border-bottom: 1px solid #d9deea; text-align: left; vertical-align: top; }",
    "    th { background: #eef2f8; }",
    "    a { color: #1168d9; }",
    "  </style>",
    "</head>",
    "<body>",
    "  <main>",
    "    <h1>HX ESP32-CAM FPV Downloads</h1>",
    f"    <p>Release {html.escape(args.tag)}</p>",
    "    <h2>GS software</h2>",
    "    <table>",
    "      <thead><tr><th>Platform</th><th>Download link</th></tr></thead>",
    "      <tbody>",
]

for platform, url in gs_rows:
    html_lines.append(f'        <tr><td>{html.escape(platform)}</td><td><a href="{html.escape(url)}">Download</a></td></tr>')

html_lines.extend(
    [
        "      </tbody>",
        "    </table>",
        "    <h2>Firmware</h2>",
        "    <table>",
        "      <thead><tr><th>Firmware</th><th>Install</th><th>Download OTA image</th></tr></thead>",
        "      <tbody>",
    ]
)

for firmware, install_url, ota_url in firmware_rows:
    html_lines.append(
        f'        <tr><td>{html.escape(firmware)}</td><td><a href="{html.escape(install_url)}">Install</a></td><td><a href="{html.escape(ota_url)}">Download</a></td></tr>'
    )

html_lines.extend(
    [
        "      </tbody>",
        "    </table>",
        "  </main>",
        "</body>",
        "</html>",
    ]
)

Path(args.output).write_text("\n".join(html_lines) + "\n", encoding="utf-8")
