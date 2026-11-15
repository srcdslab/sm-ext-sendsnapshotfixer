# Signatures
## WriteTempEntity
- https://github.com/srcdslab/source-engine-2018-hl2_src/blob/0301dccccd5553587cba988daa0763081a1f409e/engine/baseserver.cpp#L2254
- https://github.com/perilouswithadollarsign/cstrike15_src/blob/f82112a2388b841d72cb62ca48ab1846dfcc11c8/engine/baseserver.cpp#L3656

## ReleaseReference
- https://github.com/srcdslab/source-engine-2018-hl2_src/blob/0301dccccd5553587cba988daa0763081a1f409e/engine/sv_framesnapshot.cpp#L486
- https://github.com/perilouswithadollarsign/cstrike15_src/blob/f82112a2388b841d72cb62ca48ab1846dfcc11c8/engine/sv_framesnapshot.cpp#L561

## CreateEmptySnapshot
- https://github.com/srcdslab/source-engine-2018-hl2_src/blob/0301dccccd5553587cba988daa0763081a1f409e/engine/sv_framesnapshot.cpp#L80
- https://github.com/perilouswithadollarsign/cstrike15_src/blob/f82112a2388b841d72cb62ca48ab1846dfcc11c8/engine/sv_framesnapshot.cpp#L89

## LevelChanged / RemoveEntityReference (packed entity leak fix)
- https://github.com/srcdslab/source-engine-2018-hl2_src/blob/0301dccccd5553587cba988daa0763081a1f409e/engine/sv_framesnapshot.cpp#L55
- https://github.com/srcdslab/source-engine-2018-hl2_src/blob/0301dccccd5553587cba988daa0763081a1f409e/engine/sv_framesnapshot.cpp#L197

`CFrameSnapshotManager::LevelChanged` clears `m_pPackedData[MAX_EDICTS]` with a `memset` without
releasing the reference each slot holds (taken in `CreatePackedEntity`), so every last sent
`PackedEntity` (and its data / change frame list) leaks on each map change. The extension releases
those references with `RemoveEntityReference` before calling the original function.

- `sv_ssf_fix_packedentity_leak` (default `1`): enable the fix.
- `sv_ssf_packedentity_stats`: show how many references were released on the last / all level changes.

Linux only for now (x86 + x64): without the Windows gamedata entries the fix is skipped and the
other fixes keep working.

# Tests

`tests/verify_gamedata.py <css-ds-root>` checks the linux signatures of `ssf.games.txt` against the
real `engine_srv.so` (x86 + x64) and recomputes the `m_pPackedData` offset from the disassembly of
`LevelChanged`. Re-run it after every CS:S engine update.
