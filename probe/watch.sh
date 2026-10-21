#!/bin/sh
# Wait for a new probe session, then for the game to exit; print the session's status log and a size summary.
D=/e/sws_community/probe_logs
mkdir -p $D
before=$(ls $D/*.status.txt 2>/dev/null | wc -l)
until [ "$(ls $D/*.status.txt 2>/dev/null | wc -l)" -gt "$before" ]; do sleep 5; done
f=$(ls -t $D/*.status.txt | head -1)
echo "probe session started: $f"
sleep 10
while tasklist 2>/dev/null | grep -qi "starwarssquadrons.exe"; do sleep 5; done
echo "game exited at $(date)"
cat "$f"
ls -la "${f%.status.txt}.bin"
