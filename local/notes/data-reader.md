# STAR WARS: Squadrons data format: what the reader found

Reader: `tools/swsdata.py` (CLI) and the `tools/frostbite/` package. The spec was FrostyToolsuite
(commit e400dcb, 2023-09-29), profile `StarWarsSquadrons` (20201001). Every finding below was checked
against the Steam install at `E:\SteamLibrary\steamapps\common\STAR WARS Squadrons` (game build of 2021-10-21).
The install is only ever opened with mode `rb`.

## Package layout

```
tools/swsdata.py              CLI: info | list | find | dump | types
tools/frostbite/dbobject.py   binary DbObject (TOC) reader          <- IO/DbReader.cs
tools/frostbite/filesystem.py layout.toc, catalogs, manifest, cas.cat <- FileSystem.cs, IO/CatReader.cs
tools/frostbite/binarysb.py   binary bundle header                  <- BaseProfile/BaseBinarySbReader.cs
tools/frostbite/compression.py CAS block decoding                   <- IO/CasReader.cs
tools/frostbite/ebx.py        EBX reader (partial-type, v4)         <- IO/EbxReader.cs (non-V2 class)
tools/frostbite/assets.py     asset index (ManifestAssetLoader path) <- Managers/AssetManager.cs
```

The only dependency outside the standard library is `zstandard` (pip, installed into the venv).

## 1. Install layout and layout.toc

```
Data/layout.toc        signed DbObject (0x01CED100 magic, 0x22C-byte signature header skipped)
Data/initfs_Win32      signed DbObject list of {$file:{name,payload}}: 80 files (cfg/lua/yaml)
Data/bom.fb2           a ZIP (PK..); not used by Frosty or by this reader
Data/Win32/game/installation/{initialexperience,misc,sp,mp}/cas.cat + cas_NN.cas
Patch/Win32/game/installation/{same four}/cas.cat + cas_NN.cas
```

- **No patch layout.toc.** `Patch/` and `Patch/Win32/` contain only `cas.cat`/`cas_NN.cas`. There is also no `Update/` folder. `FileSystem.ProcessLayouts()` therefore takes its "no patch layout" branch: superbundles, catalogs and the manifest all come from `Data/layout.toc`. The reader still supports a patch layout (merge superbundles, then patch overrides catalogs and manifest) in case a future build ships one.
- `Data/layout.toc` keys: `fs`, `head` (336352), `installManifest`, `manifest`, `superBundles` (81). There is **no `base` key**.
- `installManifest.installChunks` has 5 entries: `installation/default` (meta, requires the 4 others), `game/installation/initialexperience` (alwaysInstalled, 19 superbundles), `misc` (5), `sp` (27), `mp` (14). The key is spelled `superbundles` (lower-case b). Frosty's DbObject lookups are case-insensitive, so the reader's `DbDict` is too.
- **Catalog indexing:** the Frosty BFV/SWS hack applies. `win32/installation/default` is skipped and does **not** take a catalog index, so the catalogs are `[0] initialexperience, [1] misc, [2] sp, [3] mp`. No install chunk has `files`, `splitSuperBundles` or `splitTocs`.
- `initfs_Win32` has **no `__fsinternal__`** and **no `Dictionaries/ebx.dict`**. ZStd dictionary decompression is never needed (see section 4).

## 2. Manifest (ManifestAssetLoader)

`layout.toc` `manifest = {file: 0x1101, offset: 0x10931F0, size: 4608424, sha1, installChunk}`.

`ManifestFileRef 0x1101` decodes to catalog `(0x1101>>12)-1 = 0` (initialexperience), patch bit `0x100` = **Patch**, cas `(0x01)+1 = 2`. So the manifest lives in `Patch/Win32/game/installation/initialexperience/cas_02.cas`.

The binary layout is exactly as in `FileSystem.ProcessManifest` (all little-endian):

```
u32 fileCount, u32 bundleCount, u32 chunkCount
file[fileCount]   : i32 fileRef, u32 offset, i64 size                 (16 bytes)
bundle[bundleCount]: i32 nameHash, i32 firstFile, i32 fileCount, i32 0, i32 0  (20 bytes)
chunk[chunkCount] : guid (16, .NET little-endian layout), i32 fileIndex (20 bytes)
```

Counts: **163,487 file slices, 1,112 bundles, 97,398 loose chunks.**

- A bundle's `files[0]` is its binary bundle header. `files[1..]` are **merged contiguous CAS ranges** covering the bundle's payloads; there is not one slice per asset. For example, bundle 0 has 556 EBX but only 6 data slices. Frosty's mod writer (`ManifestBundle.cs`) splits them using the cat offsets. The reader instead does what Frosty's loader does and finds each payload by SHA1 in `cas.cat`.
- Bundle names are not stored, only `Fnv1.HashString(name)` (offset 5381, prime 33, `h = h*33 ^ byte`, **not** lower-cased by the hash). The reader names a bundle by hashing these candidates:
  - the profile's shared-bundle list
  - the superbundle names
  - `"win32/" + <every EBX name>`

  This names **1,080 of 1,112** bundles. The remaining 32 are left as 8-hex-digit hashes, as Frosty does.

## 3. Bundle header (binary SB)

`BaseBinarySbReader.ReadDbObject`, big-endian:

```
u32 dataOffset-4 | u32 magic^0x7065636E | u32 total | u32 ebx | u32 res | u32 chunks
u32 stringsOffset | u32 metaOffset | u32 metaSize
sha1[total], ebx[(nameOff u32, origSize u32)], res[(nameOff, origSize)],
resType u32[res], resMeta 16B[res], resRid u64[res], chunk[(guid BE, logicalOffset, logicalSize)],
chunkMeta DbObject @ 4+metaOffset, strings @ 4+stringsOffset
```

- Every Squadrons bundle has magic `0xED1CEDBB` (raw `0x9D798ED5`), which means "has SHA1s". It is neither the encrypted `0xC3E5D5C3` nor the no-SHA1 `0xC3889333`.
- `dataOffset == slice size`: the header slice carries **no inline data**, so Frosty's `ReadDataBlock` never runs. Entries get no `sb`, `cas` or `offset` keys, so `AddEbx` gives them `AssetDataLocation.Cas`, which is resolved by SHA1 through the ResourceManager (cas.cat).
- Note on the C# source: Frosty subtracts 0x24 from the string and meta offsets, but it also reads them relative to a buffer that starts at file offset 0x24. In file coordinates the name is therefore at `4 + stringsOffset + nameOffset`.
- Totals across all bundles: 97,444 unique EBX names, 68,730 res refs, 76,486 bundle chunk refs.

## 4. Catalogs (cas.cat) and compression

`cas.cat`: signed header (0x22C) + `"NyanNyanNyanNyan"` + `u32 resCount, u32 patchCount, u32 encCount, 12 unknown bytes`. Then:

- `resCount` entries of 36 bytes each: `sha1, u32 offset, u32 size, u32 logicalOffset, i32 casIndex & 0xFF`
- `patchCount` entries of 60 bytes each: `sha1, baseSha1, deltaSha1`

Frosty forces `EncryptedCount = 0` for SWS, and the reader does the same.

- 8 cats (4 Data + 4 Patch) give **213,952 resources**. **All Patch cats have patchCount = 0**: there are no delta patches, and patched files are whole replacements. Patch entries override Data entries with the same SHA1.
- Only **9 of 97,444 EBX** resolve to `Patch/`. Everything else, including all ship tuning, is served from `Data/`.
- **The bundle SHA1 is the SHA1 of the stored (compressed) bytes**, not of the decompressed EBX. This held for 3000/3000 sampled assets. It matters for writers: a modded payload needs a new SHA1 computed over its compressed form.

CAS block header (`CasReader.ReadBlock`): `u32 BE decompressedSize | u16 LE compressionType | u16 BE bufferSize`.

- Low 7 bits of the compression type give the codec. The low nibble of its high byte holds bits 16..19 of `bufferSize`. A non-zero top byte of `decompressedSize` would mean "use the ZStd dictionary".
- **Squadrons stores every EBX block as ZStd (codec 0x0F)**, type word `0x700F` (high byte 0x70), with **no dictionary flag**. Across all 97,444 EBX there are 98,164 ZStd blocks and 1 uncompressed (0x00) block. A sample of res and chunk payloads was also ZStd or uncompressed.
- **Oodle:** the profile sets `OodleCompressionLevel = 18` for SWS, but `LoadOodle` is false for SWS and `LoadZStd` is true. The install contains **no `oo2core_*_win64.dll`** (searched recursively). No Oodle-compressed (0x11/0x15) block was seen, so Oodle is not needed to read Squadrons data. If a 0x11/0x15 block ever turns up, `compression.py` raises `UnsupportedCodec`.

## 5. EBX

- The profile has `EbxVersion = 4`, so `(4 & 1) == 0` selects the **plain `EbxReader`**, not `EbxReaderV2` (which needs a shared type descriptor file). The file magic is **`0x0FB4D1CE` (EbxVersion.Version4)** on all 97,444 files.
- The files are **self-describing**:
  - the header carries every class and field descriptor the file uses (name hash, type, classRef, dataOffset, size and alignment)
  - it also carries a type-name string table, so names are recovered by hashing (`h = h*33 ^ byte` from 5381)
  - enum value names are included as well

  **The SWSSDK.dll type library is not needed to decode values.** Cross-check against `notes/sws_sdk_types.json`: StarshipMovementConfig's EBX fields equal the SDK's properties (its 88 own properties plus the inherited ones), apart from `DebugName`, which the EBX has and the SDK dump lacks, and Frosty's own `__InstanceGuid`/`__Id`. The DLL will matter only for a writer that adds types or fields a file doesn't already contain.
- Header layout (v4): 4 × u32, 6 × u16, 3 × u32, file GUID, `u32 boxedValueCount`, `u32 boxedValueOffset`. Then:
  - imports (fileGuid, classGuid)
  - type names
  - fields (16 bytes; type word `>> 1` for v4)
  - classes (16 bytes)
  - instances (u16 classRef, u16 count; the first `exportedCount` are exported)
  - pad 16, arrays (offset, count, classRef), pad 16, boxed values
  - instance data from `stringsOffset + stringsLen`
- **Reading rule (verified):** for each instance:
  - align to the class alignment
  - read the 16-byte instance GUID if the instance is exported
  - skip an 8-byte header **only when alignment != 4**
  - read the fields sequentially, with the alignment fix-ups from `EbxReader.ReadClass`

  This reproduced `field.DataOffset` for every field of 300 random files. The one systematic difference: for alignment-4 classes (`IncomAttribute*` and similar), `DataOffset` counts the absent 8-byte header, so the real position is `DataOffset - 8`. A future writer must apply the same rule.
- **Full-install test:** all 97,444 EBX decompressed to their `originalSize` and parsed with **0 failures** (68 s, one process).

## 6. Index output

`swsdata.py list` writes `out/ebx_index.tsv` with columns:

- `name`
- `sha1` (bundle SHA1 = SHA1 of the stored bytes)
- `casref` (`data|patch:<catalog>/cas_NN@0xOFFSET+storedSize`)
- `size` (decompressed EBX size)
- `bundle` (all bundles containing the asset, `;`-separated)

It has 97,444 rows.

## Where Squadrons differs from what Frosty's code implies

1. **No `Patch/layout.toc`**, even though the profile lists `Patch` as the first source. The Data layout.toc holds the manifest, and the manifest itself points into a Patch CAS.
2. **No inline bundle data:** all bundle headers have `dataOffset == size`, so the `sb`/`offset` path of `ReadDataBlock` is dead for SWS. Payloads come from cas.cat by SHA1.
3. Manifest bundle file slices are **merged runs**, not one per asset.
4. **Oodle is irrelevant for reading.** Despite `OodleCompressionLevel = 18`, the data is ZStd without a dictionary, and there is no Oodle DLL in the install.
5. **No ZStd dictionary** in initfs (`Dictionaries/ebx.dict` is absent), and no block sets the dictionary flag.
6. **Zero delta patches** (patchCount = 0 in every cat).
7. EBX v4 is self-contained; the alignment-4 `DataOffset` + 8 quirk is described in section 5.
8. `layout.toc` has no `base` value, and install-chunk keys use `superbundles` (lower-case).

## Open items / not done

- RES and chunk payloads are indexed but not decoded (textures and meshes are out of scope).
- Import GUIDs are mapped to asset names by scanning the asset's own folder and its parents (`dump`, about 5 s). A full GUID-to-name table would take roughly one 70 s pass over all EBX and could be cached in `out/`.
- Which copy of a tunable the runtime reads (the typed field / IncomAttribute object, or the `*IncomAttributesToRegister` list) is unknown. See `balancing-knobs-sample.md`.
