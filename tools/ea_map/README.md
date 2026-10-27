# The EA app build's address map

The mod names every game address as the Steam build's. The EA app's copy is the same game code linked differently
(and nine months later: PE time stamps December 2020 and September 2021), so almost every address moves. It is also
encrypted on disk until EA's activation wrapper decrypts it at start. So the map is made from a decrypted image, once
per EA build, and `tools/gen_build_ea.py` writes it into `mod/build_ea.h`, with the evidence for each address. At run
time the mod tells the builds apart by the exe's PE header and looks addresses up through `G()` (`mod/game_build.h`).

Nothing from the game goes into the repo apart from `build_ea.h`'s addresses and the few expected prologue bytes that
differ (the mod's source already carries the Steam ones).

## What it needs (all under `work/`, which git ignores; `SWS_WORK` names another folder)

- `work/starwarssquadrons.exe`: a copy of the Steam build's exe.
- `work/ea/ea_image.bin`: the EA build's decrypted image, captured by `mod/sws_dump.dll` (`mod/build_dump.bat`),
  started through `sws_launch.exe --dll sws_dump.dll` with the EA app's environment. It is written beside the DLL.
- `capstone` in the project's `.venv`.

## Steps (results go to `work/ea/results`)

1. The function lists, from each image's unwind table:
   `.venv/Scripts/python.exe -c "import sys; sys.path.insert(0, 'tools/ea_map'); from imgs import *; import os; os.makedirs(RESULTS, exist_ok=True); pickle.dump(funcs(Steam()), open(os.path.join(RESULTS, 'funcs_steam.pkl'), 'wb')); pickle.dump(funcs(Dump()), open(os.path.join(RESULTS, 'funcs_ea.pkl'), 'wb'))"`
2. `align.py`: pairs the two lists by function size (unique sizes anchor it), writing `fmap.pkl`.
3. `verify.py`: checks every pair instruction by instruction. Lengths and text must match, except RIP-relative and
   branch targets, which are collected as evidence (`verified.pkl`, `codeev.pkl`, `dataev.pkl`).
4. `map_sites.py`: extends the verified set through call targets and a second pass that allows switch tables
   (`verified_all.pkl`; about 174,000 functions for the 2021 EA build).
5. `tools/gen_build_ea.py`: every address the source names, with evidence, strongest first (its docstring lists the
   kinds). It stops at any address without evidence or with contradicting evidence. The functions the alignment
   missed or that EA rebuilt are checked by hand and listed in its `HAND` table. These tools did that:
   - `pairchk.py`: the strict check on one pair, with every implied target compared against the map.
   - `shapediff.py` / `eqtargets.py`: where two functions differ, and whether the shared parts' targets agree.
   - `callers.py`: the direct callers of a function in either build.
   - `datacheck.py`: compares the contents of data at two addresses, slot by slot.

`tools/check_build_ea.py` (run by CI) checks that `build_ea.h` covers every address in the source. An address added to
the source needs `gen_build_ea.py` run again on a machine that has the EA image.

## Things learned the hard way (2026-10-27)

- **Nearest-reference placement is not evidence.** Placing a data address by the shift of the nearest code reference
  put the socket and bind import slots on the wrong slots. EA orders its import slots and its vtables differently.
- **Range bounds aren't addresses.** A few constants in the mod (`0x143000000`, `0x146000000`, `0x1459b0000`) are bounds
  for pointer checks in its logging; they get layout values, not matches.
- **Small vtables run together.** Many component vtables are three entries long, so a vtable is matched by its run of
  entries from the start, and must be the only best match.
- **Import slots are found by value.** The wrappers fill the game's own import table at start, so the dump holds live
  function addresses. System DLLs load at the same address in every process until a reboot, so the generator looks
  them up live only if the machine hasn't rebooted since the dump, and keeps the result in `imports_ea.json`.
