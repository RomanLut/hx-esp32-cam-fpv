import argparse
import sys
import zipfile
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("--zip-dir", required=True)
parser.add_argument("--output-dir", required=True)
args = parser.parse_args()

zip_dir = Path(args.zip_dir)
output_dir = Path(args.output_dir)
output_dir.mkdir(parents=True, exist_ok=True)

archives = sorted(zip_dir.glob("air_firmware_*.zip"))

if not archives:
    print(f"No firmware zip files found in {zip_dir}", file=sys.stderr)
    sys.exit(1)

for archive_path in archives:
    with zipfile.ZipFile(archive_path) as archive:
        names = set(archive.namelist())
        base_name = archive_path.stem
        expected = {
            f"{base_name}_merged.bin",
            f"{base_name}_manifest.json",
            f"{base_name}_ota.bin",
        }
        missing = expected - names

        if missing:
            print(f"{archive_path} is missing: {', '.join(sorted(missing))}", file=sys.stderr)
            sys.exit(1)

        for name in sorted(expected):
            archive.extract(name, output_dir)
            print(output_dir / name)
