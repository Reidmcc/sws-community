#!/bin/sh
# The stand-in test: runs the server DLL's test build in test_server_target.exe, a stand-in for the game that plants the
# game's code bytes and data at the addresses the DLL hooks, through the real launcher. It needs no game. Build first
# (build_server.bat and build_app.bat), then run this from Git Bash (or `bash mod/run_standin.sh`; CI runs it too).
#
# For the test it copies the stand-in's server.ini, limits.ini, profile and a made-up unlock list into build\. Your own
# files there are saved first and put back when it ends, however it ends. Its logs are moved to build\standin_logs\.
# It passes when the stand-in prints ALL PASS and the settings app computes the same profile fingerprint as the DLL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B="$HERE/build"
T="$HERE/test_profiles"
OUT="$B/standin_logs"

if tasklist //FI "IMAGENAME eq starwarssquadrons.exe" 2>/dev/null | grep -qi starwarssquadrons; then
    echo "REFUSING: the game is running (the stand-in test swaps files in build\\ that the game reads)"; exit 1
fi
for f in sws_launch.exe sws_server_test.dll test_server_target.exe sws_settings_test.exe; do
    [ -f "$B/$f" ] || { echo "build\\$f is missing: run build_server.bat and build_app.bat first"; exit 1; }
done
mkdir -p "$OUT" "$B/profiles"
SAVE="$OUT/saved"; mkdir -p "$SAVE"
FILES="server.ini limits.ini defaults.ini unlock_ids.txt loadouts.txt"
for f in $FILES; do [ -f "$B/$f" ] && cp "$B/$f" "$SAVE/$f"; done
rm -f "$B/loadouts.txt"   # the test starts with no saved loadouts
restore() {
    for f in $FILES; do
        if [ -f "$SAVE/$f" ]; then mv -f "$SAVE/$f" "$B/$f"; else rm -f "$B/$f"; fi
    done
}
trap restore EXIT

cp "$T/server_standin.ini" "$B/server.ini"
cp "$T/limits_standin.ini" "$B/limits.ini"
cp "$T/unlock_ids_standin.txt" "$B/unlock_ids.txt"
cp "$T/StandIn.ini" "$B/profiles/StandIn.ini"
START="$OUT/.start"; touch "$START"; sleep 1

# The launcher starts the stand-in and returns; the stand-in inherits its output (redirected to a file here) and keeps
# writing until it exits, so wait for it.
RESULT="$OUT/standin_output.txt"
( cd "$B" && ./sws_launch.exe --target "$(cygpath -w "$B/test_server_target.exe")" --dll sws_server_test.dll --yes ) > "$RESULT" 2>&1
sleep 2
i=0
while tasklist //FI "IMAGENAME eq test_server_target.exe" 2>/dev/null | grep -qi test_server_target; do
    i=$((i + 1)); [ $i -gt 240 ] && { echo "the stand-in is still running after 2 minutes; stopping it"; taskkill //F //IM test_server_target.exe >/dev/null 2>&1; break; }
    sleep 0.5
done
cat "$RESULT"

# The DLL's log: the development PC's logs folder or the one beside the DLL. Only the stand-in's (dedicated mode) move.
LOG=""
for d in "/e/sws_community/probe_logs" "$B/logs"; do
    [ -d "$d" ] || continue
    for f in $(find "$d" -maxdepth 1 -type f -newer "$START" 2>/dev/null); do
        if [ "${f##*.}" = "txt" ] && grep -q "server: dedicated=1" "$f" 2>/dev/null; then LOG="$OUT/$(basename "$f")"; mv -f "$f" "$OUT/"
        elif [ "${f##*.}" = "bin" ]; then mv -f "$f" "$OUT/"; fi
    done
done

STATUS=0
grep -q "^ALL PASS" "$RESULT" || { echo "STAND-IN FAILED"; STATUS=1; }
# The settings app must compute the profile's fingerprint exactly as the DLL does (the network version depends on it).
DLLFP=$( [ -n "$LOG" ] && grep -oE "profile: 'StandIn'.*fingerprint [0-9a-f]{8}" "$LOG" | grep -oE "[0-9a-f]{8}$" | head -1 )
APPFP=$( cd "$B" && ./sws_settings_test.exe "$(cygpath -w "$B")" StandIn | grep -oE "fingerprint [0-9a-f]{8}" | grep -oE "[0-9a-f]{8}$" | head -1 )
if [ -n "$DLLFP" ] && [ "$DLLFP" = "$APPFP" ]; then echo "PASS  the settings app and the DLL agree on the StandIn fingerprint ($APPFP)"
else echo "FAIL  fingerprints differ: DLL '${DLLFP:-none}', settings app '${APPFP:-none}'"; STATUS=1; fi
# local_loadouts (on by default): the stand-in's save wrote loadouts.txt with the three good records and not the bad one
L="$B/loadouts.txt"
if [ -f "$L" ] && grep -q "^ship Rebel_Flex_Loadout 00050000 00050001" "$L" && grep -q "^pilot Rebel_pilot_loadout 00070000" "$L" \
    && grep -q "^profile PlayerProfile_1 00030000" "$L" && ! grep -q "Rebel_Siege_Loadout" "$L" \
    && [ -n "$LOG" ] && grep -q "loadouts: the save's send to EA dropped" "$LOG" \
    && grep -q "your saved loadouts laid over 3 of them" "$LOG"; then
    echo "PASS  local loadouts: loadouts.txt holds the saved records, and the log shows the send dropped and the records laid over EA's"
else echo "FAIL  local loadouts: loadouts.txt or its log lines missing or wrong"; STATUS=1; fi
# loadout_log=1 (server_standin.ini): the stand-in's loadout save, the setter it calls, and the two pilot loadouts
if [ -n "$LOG" ] && grep -q "loadouts: SAVE from .* (connection 2)" "$LOG" \
    && grep -q "loadouts: set by .*, state 1, status 0, connection 2: pilot 2, ship 0, profile 1" "$LOG" \
    && grep -q 'loadouts:   pilot\[0\] +20="Rebel" +28=00000000,00000000 +30=00001234' "$LOG" \
    && grep -q "loadouts:   pilot\[1\] .* +30=00005678" "$LOG" && grep -q "loadouts:   profile\[0\] .* +28=00009abc" "$LOG"; then
    echo "PASS  loadout_log logged the save, the presets it set, and each loadout"
else echo "FAIL  loadout_log lines missing or wrong in the log"; STATUS=1; fi
# spawn_log=1 (server_standin.ini): the stand-in's two starship spawns, one that made a ship and one that didn't
if [ -n "$LOG" ] && grep -q "spawn: entity .* -> ship 5a5a0001" "$LOG" && grep -q "spawn: entity .* -> NOTHING SPAWNED" "$LOG"; then
    echo "PASS  spawn_log logged both starship spawns and their outcome"
else echo "FAIL  spawn_log lines missing from the log"; STATUS=1; fi
# hit_log=1 (server_standin.ini): two hits on the stand-in's U-wing, logged in the ship's own frame
if [ -n "$LOG" ] && grep -q "hit: starshiphealthdamagedata_uwing: transform found in the physics component+0x80; the impact point is info+0x30" "$LOG"    && grep -q "hit: starshiphealthdamagedata_uwing part 2 kind 0 material 3 damage 27.0 at (1.00, 2.00, 3.00) velocity ?[[:space:]]*$" "$LOG"    && grep -q "hit: starshiphealthdamagedata_uwing part 2 kind 0 material 3 damage 27.0 at (-4.00, 0.50, 10.00) velocity (-[2-5][0-9]\.[0-9], 0\.0, 0\.0)" "$LOG"; then
    echo "PASS  hit_log found the ship's frame and logged both hits in it"
else echo "FAIL  hit_log lines missing or wrong"; STATUS=1; fi
# capital_damage_log: the light cruiser's hull (ignores AI damage) takes a blast and a hit from a starship without the player
# mark; the drop rule's verdict, the 5 s summary and the shield bubble's first hit are logged
if [ -n "$LOG" ] && grep -q "capdmg: first hit on CapitalShipHealthSettings_LightCruiser (hull, which ignores AI damage)" "$LOG" \
   && grep -q "capdmg: CapitalShipHealthSettings_LightCruiser hull: blast of 4000 from starships without the player mark (mark 0) -> 4000 taken (DROPPED: AI damage ignored); health 44000" "$LOG" \
   && grep -q "capdmg: CapitalShipHealthSettings_LightCruiser hull, 5\.[0-9] s; starships without the player mark 2 hits, 4050 asked, 4050 taken (some dropped); health 43950" "$LOG" \
   && grep -q "capdmg: first hit on ShieldHealthSettings_Raider (shield bubble)" "$LOG"; then
    echo "PASS  capital_damage_log named the ships, judged the drop rule, summed the hits by attacker"
else echo "FAIL  capital_damage_log lines missing or wrong"; STATUS=1; fi
# creep hull by ship type: the profile's Y-wing value wins over the creep default; the A-wing pack keeps the default
if [ -n "$LOG" ] && grep -q "creep: ywing creep: hull 900 (the profile's), shields 600 (the creep default)" "$LOG" && grep -q "creep: awing creep: hull 125 (the creep default)" "$LOG"; then
    echo "PASS  creep hull and shields: a profile value wins over the creep default, by ship type"
else echo "FAIL  creep durability lines missing or wrong"; STATUS=1; fi
[ -n "$LOG" ] && echo "log: $LOG"
exit $STATUS
