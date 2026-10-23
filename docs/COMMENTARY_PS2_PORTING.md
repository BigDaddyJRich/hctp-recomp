# HCTP commentary on the original PS2 build

This is a research and porting guide for people who want to build their own HCTP commentary restoration from their own
legally obtained discs. It does not include game files, extracted audio, copyrighted commentary, or a ready-to-apply
PS2 ELF patch.

## Important status

The native PC recompilation has the commentary-selection logic and hooks implemented. The original PS2 version still
needs a MIPS code-cave patch in `SLUS_207.87` before the rebuilt packs will play there. Copying the PC `BGM` files into
a PS2 game by itself is not sufficient.

The research identified the playback side that HCTP already contains: `CADX_CTRL`/ADXT can open AFS partitions and play
ADX clips. The missing work is the PS2-side lookup/selection layer and registration of the added AFS partitions.

## Source material

Build from your own extracted copies. The recommended core set is:

1. HCTP — destination game, roster IDs, moves, and existing ADX playback.
2. Shut Your Mouth — Howard Finkel and supplemental name/intro material.
3. SmackDown vs. Raw retail — Cole/Tazz and JR/King commentary plus the PBP list.
4. SmackDown vs. Raw public beta — additional HCTP-era Cole/Tazz lines.

Optional sources include SmackDown vs. Raw 2006/2007/2008, WWE 2K14, and Legends of WrestleMania. They can add
coverage but are not required for the core set.

## Rebuild the source-owned commentary data

The repository tools are reference/build tools, not downloaders:

- `tools/decode_svr_pbplist.py` — decode the retail/beta PBP lookup tables.
- `tools/transcribe_afs.py`, `tools/transcribe_yaf.py`, and `tools/transcribe_xwb.py` — build searchable transcripts from
  source archives.
- `tools/build_finkel_pack.py` — construct the Finkel clip pack and line index from source clips.
- `tools/build_pbp_extra.py` — construct the supplemental wrestler-line pack.
- `tools/split_akpk.py` and the local `vgmstream`/FFmpeg tools — inspect and convert source audio as needed.

Keep each source game's identity separate while importing. Do not use another game's numeric wrestler slots as HCTP
identities: cut wrestlers and reused slots make that unsafe. Join lines by HCTP roster ID/name evidence, then assign
the resulting clip to the rebuilt table.

The PC research uses these derived pack names:

```text
PBPSVR.afs          retail commentary ADX clips
PBPSVR_LIST.pac     retail PBP lookup tables
PBPBETA.afs         public-beta commentary ADX clips
PBPBETA_LIST.pac    public-beta PBP lookup tables
PBPEXTRA.afs        supplemental wrestler/legend clips
PBPEXTRA_LIST.csv   supplemental identity/index data
FINKEL.afs         Howard Finkel clips
FINKEL_LINES.csv   Finkel line metadata
```

These are generated user files. They must not be committed or redistributed.

## What the PS2 patch must do

The following are the confirmed HCTP hook points and the behavior they need:

| Purpose | HCTP function | Address |
|---|---|---:|
| Match bell/start/end and participants | `CAcxPlayer::PlayByIndex` | `0x1B1090` |
| Entrance setup data | `CSd5EventSequence::SetEvent` | `0x2A83B0` |
| Actual entrance start | `CEventSystem::Play` | `0x166E80` |
| Tag exchange | `Koutai` | `0x1E4080` |
| Move/finisher sound context | `GameSE` | `0x1B0680` |
| Belt-holder lookup | `CTitleHistoryData::CheckTitleHold` / `GetTitleHolder` | `0x324110` / `0x323260` |
| Native ADX AFS playback | `ADXT` functions | `0x3BF668` and related calls |

Use the entrance hook on `CEventSystem::Play`, not only `SetEvent`: `SetEvent` runs during the loading screen, while
`Play` returns the transition that means the entrance is actually on screen. The PC implementation uses this distinction
to avoid announcing wrestlers too early.

The patch needs to:

1. Read the HCTP participant/entrant roster IDs from the match cast data.
2. Select the corresponding decoded PBP row or Finkel line.
3. Start the selected ADX clip through the game's own ADXT player.
4. Register the additional AFS partitions in the `CADX_CTRL` partition/name table.
5. Preserve the original HCTP sound partitions and fall back silently when a line is unavailable.
6. Keep match type, tag/team, Royal Rumble, title-holder, entrance, and finisher contexts separate.

The research also found that HCTP's original `SFX/CHEE` table is not the full SvR-style PBP database. SvR's
`pbplist.pac` is a separate structured lookup system, so replacing `CHEE` or renaming a file is not enough.

## Validation checklist

Before distributing a patch, validate on an untouched personal HCTP copy:

- Boot and play a normal singles match with retail commentary.
- Test a tag match and confirm the connector changes to partner/opponent wording.
- Test a triple/fatal-four-way or other multi-entrant match.
- Test an entrance and confirm Finkel triggers when it appears on screen.
- Test Royal Rumble entrant one/two paths separately from later entrants.
- Test a title match after changing the title holder in the save data.
- Test a finisher and verify the move-ID translation rather than assuming SvR move IDs match HCTP.
- Check the log for missing tables, rejected identity mappings, failed AFS opens, and clips that are out of range.

Do not overwrite the original disc image. Keep the generated AFS/PAC files and the patched ELF in a separate test copy,
and retain a manifest containing source hashes, converted clip IDs, HCTP identity mappings, and output hashes.

## What is not yet provided here

This repository does not currently provide a finished PS2 MIPS code cave, an automatic ISO patcher, or copyrighted audio.
Those are the remaining steps for a complete original-PS2 commentary release. The native PC hook implementation and the
format/identity research are provided to make that work reproducible for other modders.
