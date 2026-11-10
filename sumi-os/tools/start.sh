#!/bin/bash
# Start Sumi OS headless for automated testing: QMP on a unix socket, serial log to a file.
#   tools/start.sh [name]      (name selects socket/log names; default "sumi")
# Environment: BUILD (build dir), SUMI_BOOT=direct (fast boot without firmware/GRUB)
cd "$(dirname "$0")/.."
N=${1:-sumi}; shift || true
rm -f /tmp/$N-qmp.sock /tmp/$N-serial.log
./run.sh -display none -qmp unix:/tmp/$N-qmp.sock,server,nowait -serial file:/tmp/$N-serial.log "$@" &
for i in $(seq 1 100); do [ -S /tmp/$N-qmp.sock ] && break; sleep 0.1; done
echo "started: SUMI_QMP=/tmp/$N-qmp.sock  serial=/tmp/$N-serial.log"
