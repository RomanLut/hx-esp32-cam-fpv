import argparse
import fnmatch
import json
import os
import sys
import urllib.request
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("--repository", required=True)
parser.add_argument("--release-ref", required=True)
parser.add_argument("--zip-dir", required=True)
parser.add_argument("--asset-pattern", action="append", default=[])
parser.add_argument("--current-assets", required=True)
parser.add_argument("--asset-catalog", required=True)
args = parser.parse_args()
release_ref = args.release_ref.rstrip("/").rsplit("/", 1)[-1]

token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")

if not token:
    print("GH_TOKEN or GITHUB_TOKEN is required", file=sys.stderr)
    sys.exit(1)

api_base = f"https://api.github.com/repos/{args.repository}"
headers = {
    "Accept": "application/vnd.github+json",
    "Authorization": f"Bearer {token}",
    "User-Agent": "hx-esp32-cam-fpv-pages-deploy",
    "X-GitHub-Api-Version": "2022-11-28",
}


def api_json(url):
    request = urllib.request.Request(url, headers=headers)

    with urllib.request.urlopen(request) as response:
        return json.loads(response.read().decode("utf-8"))


def api_binary(url):
    request_headers = dict(headers)
    request_headers["Accept"] = "application/octet-stream"
    request = urllib.request.Request(url, headers=request_headers)

    with urllib.request.urlopen(request) as response:
        return response.read()


def get_releases():
    releases = []
    page = 1

    while True:
        batch = api_json(f"{api_base}/releases?per_page=100&page={page}")

        if not batch:
            return releases

        releases.extend(batch)
        page += 1


def matches_release(release, release_ref):
    values = [
        str(release.get("id", "")),
        release.get("tag_name", ""),
        release.get("name", ""),
        release.get("html_url", ""),
        release.get("html_url", "").rstrip("/").rsplit("/", 1)[-1],
    ]

    return release_ref in values


releases = get_releases()
release = next((item for item in releases if matches_release(item, release_ref)), None)

if release is None:
    print(f"Release not found: {args.release_ref}", file=sys.stderr)
    print("Available releases:", file=sys.stderr)
    for item in releases:
        print(f"  id={item.get('id')} tag={item.get('tag_name')} name={item.get('name')} url={item.get('html_url')}", file=sys.stderr)
    sys.exit(1)

print(f"Matched release: id={release.get('id')} tag={release.get('tag_name')} name={release.get('name')} url={release.get('html_url')}")

assets = api_json(release["assets_url"])
asset_records = [
    {
        "name": asset["name"],
        "browser_download_url": asset["browser_download_url"],
    }
    for asset in assets
]

Path(args.current_assets).write_text(json.dumps(asset_records, indent=2) + "\n", encoding="utf-8")

catalog = []

for item in releases:
    item_assets = api_json(item["assets_url"])
    catalog.append(
        {
            "tagName": item["tag_name"],
            "name": item.get("name") or "",
            "htmlRef": item.get("html_url", "").rstrip("/").rsplit("/", 1)[-1],
            "assets": [
                {
                    "name": asset["name"],
                    "browser_download_url": asset["browser_download_url"],
                }
                for asset in item_assets
            ],
        }
    )

Path(args.asset_catalog).write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")

zip_dir = Path(args.zip_dir)
zip_dir.mkdir(parents=True, exist_ok=True)
patterns = args.asset_pattern or ["*"]
downloaded = 0

for asset in assets:
    if not any(fnmatch.fnmatch(asset["name"], pattern) for pattern in patterns):
        continue

    output_path = zip_dir / asset["name"]
    output_path.write_bytes(api_binary(asset["url"]))
    print(output_path)
    downloaded += 1

if downloaded == 0:
    print(f"No assets matched: {', '.join(patterns)}", file=sys.stderr)
    sys.exit(1)
