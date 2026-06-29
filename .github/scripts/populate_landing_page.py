"""Populate the static landing page (web/index.html) with real release links.

Replaces the previous download.html generation. The landing page ships with
`data-*` markers on the download/flash anchors and the release pill; this script
rewrites those anchors for the resolved release:

  * firmware "Download" anchors  -> release .zip asset
  * firmware "Flash online" anchors -> install.html?... (ESP Web Tools installer)
  * ground-station / app cards   -> matching release assets
  * release pill + "All assets"  -> release tag page, version label updated
"""

import argparse
import html
import json
import re
from pathlib import Path
from urllib.parse import quote, urlencode


parser = argparse.ArgumentParser()
parser.add_argument("--index", required=True, help="Path to the landing page index.html to rewrite in place")
parser.add_argument("--firmware-dir", required=True)
parser.add_argument("--repository", required=True)
parser.add_argument("--owner", required=True)
parser.add_argument("--tag", required=True)
parser.add_argument("--asset-list", required=True)
parser.add_argument("--release-asset-catalog", required=True)
args = parser.parse_args()

firmware_dir = Path(args.firmware_dir)
asset_records = json.loads(Path(args.asset_list).read_text(encoding="utf-8"))
asset_by_name = {asset["name"]: asset for asset in asset_records}
asset_names = list(asset_by_name.keys())
release_asset_catalog = json.loads(Path(args.release_asset_catalog).read_text(encoding="utf-8"))

repo_name = args.repository.split("/", 1)[1]
pages_base_url = f"https://{args.owner}.github.io/{repo_name}"
firmware_base_url = f"{pages_base_url}/firmware/{quote(args.tag, safe='')}"
release_tag_url = f"https://github.com/{args.repository}/releases/tag/{quote(args.tag)}"


def release_asset_url(name):
    asset = asset_by_name.get(name)
    if asset is not None:
        return asset["browser_download_url"]
    return f"https://github.com/{args.repository}/releases/download/{quote(args.tag)}/{quote(name)}"


def release_asset_url_for_tag(tag, name):
    for release in release_asset_catalog:
        tag_values = {
            release.get("tagName", ""),
            release.get("name", ""),
            release.get("htmlRef", ""),
        }
        if tag not in tag_values:
            continue
        for asset in release.get("assets") or []:
            if asset.get("name") == name:
                return asset["browser_download_url"]
    return f"https://github.com/{args.repository}/releases/download/{quote(tag)}/{quote(name)}"


def find_asset_current_or_previous(prefix, suffix):
    """Prefer an asset in the deploy release; otherwise fall back to the most
    recent previous release that still ships one (e.g. SD-card images that are
    not rebuilt for every firmware release). Returns (tag, name) or (None, None).
    """
    # Deploy release first, matched by prefix so any version suffix is accepted.
    for name in asset_names:
        if name.startswith(prefix) and name.endswith(suffix):
            return args.tag, name
    # Catalog is newest-first (gh release list), so the first hit is the most
    # recent previous release carrying the asset.
    for release in release_asset_catalog:
        tag = release.get("tagName")
        if args.tag in {tag, release.get("name", ""), release.get("htmlRef", "")}:
            continue
        for asset in release.get("assets") or []:
            name = asset.get("name", "")
            if name.startswith(prefix) and name.endswith(suffix):
                return tag, name
    return None, None


# ---- firmware: download (.zip) + flash online (install.html) per base name ----
firmware_links = {}

for manifest_path in sorted(firmware_dir.glob("*_manifest.json")):
    base_name = manifest_path.name.removesuffix("_manifest.json")
    ota_path = firmware_dir / f"{base_name}_ota.bin"
    if not ota_path.is_file():
        continue

    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    version = str(manifest.get("version", args.tag))

    manifest_url = f"{firmware_base_url}/{quote(manifest_path.name)}"
    ota_url = f"{firmware_base_url}/{quote(ota_path.name)}"
    install_query = urlencode(
        {
            "firmware": base_name,
            "version": version,
            "manifest": manifest_url,
            "ota": ota_url,
        }
    )
    firmware_links[base_name] = {
        "download": release_asset_url(f"{base_name}.zip"),
        "flash": f"{pages_base_url}/install.html?{install_query}",
    }

# ---- ground-station images & companion apps ----
# SD-card images may not be rebuilt every release, so fall back to a previous one.
radxa_tag, radxa_asset = find_asset_current_or_previous("espvrx_dualboot_radxa3w_", ".zip")
rpi_tag, rpi_asset = find_asset_current_or_previous("espvrx_rpi_", ".img.gz")
android_apk = next(
    (
        name
        for name in asset_names
        if name.startswith("hx-esp32-cam-fpv-") and "-oculus-" not in name and name.endswith(".apk")
    ),
    None,
)
oculus_apk = next(
    (name for name in asset_names if name.startswith("hx-esp32-cam-fpv-oculus-") and name.endswith(".apk")),
    None,
)

dl_links = {
    "radxa": release_asset_url_for_tag(radxa_tag, radxa_asset) if radxa_asset else release_tag_url,
    "rpi": release_asset_url_for_tag(rpi_tag, rpi_asset) if rpi_asset else release_tag_url,
    "android": release_asset_url(android_apk) if android_apk else release_tag_url,
    "oculus": release_asset_url(oculus_apk) if oculus_apk else release_tag_url,
}

# ---- rewrite the HTML ----
index_path = Path(args.index)
doc = index_path.read_text(encoding="utf-8")

ANCHOR_RE = re.compile(r"<a\b[^>]*>", re.IGNORECASE)


def set_anchor_href(markup, predicate, url):
    """Set the href of every <a> opening tag matching predicate(tag)."""
    safe_url = html.escape(url, quote=True)
    count = 0

    def repl(match):
        nonlocal count
        tag = match.group(0)
        if not predicate(tag):
            return tag
        count += 1
        if 'href="' in tag:
            return re.sub(r'href="[^"]*"', f'href="{safe_url}"', tag, count=1)
        return tag[:2] + f' href="{safe_url}"' + tag[2:]

    return ANCHOR_RE.sub(repl, markup), count


def has(attr):
    return lambda tag: attr in tag


# firmware anchors
for base_name, links in firmware_links.items():
    for kind in ("download", "flash"):
        marker = f'data-fw="{base_name}"'
        kind_marker = f'data-fw-kind="{kind}"'
        doc, n = set_anchor_href(
            doc,
            lambda tag, m=marker, k=kind_marker: m in tag and k in tag,
            links[kind],
        )
        if n == 0:
            print(f"WARNING: no anchor matched {marker} {kind_marker}")

# ground-station / app cards
for key, url in dl_links.items():
    doc, n = set_anchor_href(doc, has(f'data-dl="{key}"'), url)
    if n == 0:
        print(f"WARNING: no anchor matched data-dl=\"{key}\"")

# release tag links (pill + "All assets")
doc, _ = set_anchor_href(doc, has("data-release-link"), release_tag_url)

# version label inside the release pill
doc = re.sub(
    r"(<b[^>]*data-release-version[^>]*>).*?(</b>)",
    lambda m: m.group(1) + html.escape(args.tag) + m.group(2),
    doc,
    count=1,
    flags=re.DOTALL,
)

index_path.write_text(doc, encoding="utf-8")
print(f"Populated {index_path} for release {args.tag}")
print(f"  firmware builds: {', '.join(sorted(firmware_links)) or '(none)'}")
print(f"  radxa: {dl_links['radxa']}")
print(f"  rpi:   {dl_links['rpi']}")
print(f"  android: {dl_links['android']}")
print(f"  oculus:  {dl_links['oculus']}")
