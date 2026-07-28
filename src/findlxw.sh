#!/bin/bash
OD="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump"
"$OD" -d /mnt/e/WorkBuddy/GDKmini/GDK/src/epubreader | awk '/^[0-9a-f]+ </{fn=$2} /lxw|lxh[^u]|lxbu?/{print fn, $0}' | head -20
