#!/bin/bash
OD="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump"
BIN="/mnt/e/WorkBuddy/GDKmini/GDK/src/epubreader"
"$OD" -d "$BIN" | grep -B 30 "4011dc:" | tail -35
