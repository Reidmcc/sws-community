# Test 3b: dedicated server that spawns the custom-match waiting room (2026-10-19)
Same launcher as 3a (`mod\build\sws_launch.exe`, type YES). `server.ini` is in 3b mode: dedicated, Local backend,
and `[spawn]` set to the game's own custom-match setup (waiting room + Yavin), observed in your solo custom match.
About 3 seconds after the game simulation starts, the DLL spawns the server itself. There's still no window. Let it run
about a minute, then end `starwarssquadrons.exe` in Task Manager. The log says whether a server object appeared and what it did.
If it crashes, that's fine: since 2026-10-20 the DLL logs the crash address, so just tell me it disappeared.

# Test 3a: start the game as a dedicated server (sws_launch.exe + sws_server.dll), 2026-10-19
Experimental. This doesn't attach to a running game; it **starts** the game, with `sws_server.dll` loaded before the game's
own code runs, and then:
- tells the main loop it is a dedicated server (`MainLoop+0x8`, the same flag Kyber sets on BF2);
- makes the online backend always Local (the Blaze/LAN server backends aren't in this build);
- logs what the engine does next: which simulation clock it picks, and any server spawn with its flags and level.

1. Make sure the game is **not** running and Steam is.
2. Double-click `mod\build\sws_launch.exe` and type **YES**.
3. Watch what happens and tell me: does a window appear, does it reach a menu, does it hang, crash, or close itself?
   If nothing visible happens for a minute, end `starwarssquadrons.exe` in Task Manager.

Nothing is expected to be playable yet. This run is to see which way the engine goes with the flag set.
Settings are in `mod\build\server.ini` (`dedicated`, `force_local_backend`).

# Flying with the "as intended" rules (sws_rules.dll), 2026-10-19
This one **changes behaviour**. It hooks the same five places as the probe and logs the same way, plus:
- **no_zero_throttle_pinball:** when you boost, the jump straight to boost speed happens only if you're already above
  your current cruise speed. That's the live cruise for your engine and engine power, plus 1.0. Otherwise the boost
  ramps the way a boost from settled full throttle does, whatever the throttle says.
- **no_multidrift:** a drift starts only while the afterburner is on. The game turns it off when a drift starts, so it's
  one drift per boost, the same as the combined boost/drift input.
- **v2 (21:15):** both rules now read only the current tick's state, so the client's prediction replays agree with the
  server. v1's remembered decision made the client disagree with the server and get corrected, which showed as speed
  jumps after boosts.

Switch either off in `mod\build\rules.ini` (0 = off). It's read when the DLL loads.

1. Start the game directly, the same as before.
2. Double-click `mod\build\inject_rules.bat` and type YES.
   (Only one of probe/rules can be loaded per game run. To switch, restart the game.)
3. Fly however you like. Worth checking:
   - **normal flying feels unchanged:** full-throttle boost, drift, boost-drift chains, re-boosting above cruise
     (which should still be instant);
   - **zero-throttle boost:** should ramp up over about a second, not jump;
   - **multidrift:** the re-press shouldn't start a new drift;
   - **combined input:** should behave exactly as before.

Mark anything that feels off with F8 / F9 / F10 (they beep), and tell me which mark was what.

# Running the movement probe (Test 2)

It only observes. It reads movement values and writes them to `E:\sws_community\probe_logs\`.
It doesn't change how the game behaves, and nothing is written to the game folder.

## Start the game WITHOUT EasyAntiCheat
1. Steam running; `steam_appid.txt` still in the game folder (from Test 0).
2. Start `E:\SteamLibrary\steamapps\common\STAR WARS Squadrons\starwarssquadrons.exe` **directly**.
   **Not** Steam's Play button: that goes through the EasyAntiCheat launcher, and injecting into a
   game EAC is watching is exactly what EAC exists to catch. The injector refuses if it sees EAC, but
   don't rely on that.
3. Go to Practice and pick the X-wing if you can (the reference values are the X-wing's).

## Load the probe
Run `mod\build\inject.exe` (double-click works). A direct launch still loads
EAC's in-game DLL with the EAC services stopped. The injector explains this and asks you to type
**YES** (your decision of 2026-10-19). It still refuses outright if any EAC service or process is running.
It should then print "Probe loaded into pid …". If the game crashes at that moment, Denuvo probably objected to the code
patch. Tell me; there's a fallback method that doesn't touch the code.

## Probe v2 script: throttle and cruise (2026-10-19, second try)
Same start and inject steps as above. The marker keys now **beep** (F8 low, F9 mid, F10 high). The
status log should list **5** "hooked" lines this time, including `bounds` and `integ`.
Drifting right after a boost is fine: the instant-or-ramp decision happens on the boost's first tick.
Press each marker key **just before** its part, and hold each state for a few seconds so it stands out in the log:
- **F8, throttle only, no boost:**
  - full throttle until speed settles;
  - throttle to **zero** and coast down a few seconds;
  - **half** throttle and let it settle;
  - back to **full** and let it settle.
- **F9, normal boost:** at full throttle with speed settled, boost (no drift). Do it 2–3 times, letting speed settle between.
- **F10, the exploit cases (boost only, no drift):**
  - throttle to zero, coast below cruise, then boost; 2–3 times;
  - half throttle, speed settled, then boost; once or twice;
  - throttle to zero while still fast but below cruise, then boost; once.
- **Second engine:** back to the hangar, fit a different engine (one with a different top speed), return to
  Practice. The probe stays loaded; don't re-inject. Press **F8**, fly full throttle until settled, then boost once.

Tell me which two engines you used, and the top speed the loadout screen shows for each if it shows one.
I'll check the probe's cruise number against it.

## Fly this script (v1), with separate boost and drift inputs
Press the marker key **just before** each part:
- **F8, normal:** boost, hold drift, release, three times, with a few seconds between.
- **F9, multidrift:** boost, hold drift, then release and immediately re-press, several times in one drift. Do two or three of those.
- **F10, pinball:** throttle to zero, boost, then drift. Two or three times.
- **Optional, the reference for "as intended":** switch the input setting to combined boost/drift,
  press **F8** again, then do boost → drift → boost → drift a few times.

Then quit the game normally. I'm watching for that and will analyse as soon as it closes.
