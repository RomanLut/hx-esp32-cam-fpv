#!/bin/bash

echo "Filling empty space with zeroes..."
sudo dd if=/dev/zero of=zerofill.tmp bs=1M status=progress || true
sync
sudo rm -f zerofill.tmp
sync
echo "Done."