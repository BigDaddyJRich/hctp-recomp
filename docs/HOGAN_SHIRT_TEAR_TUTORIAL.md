# Restoring Hulk Hogan's shirt tear in SmackDown! Here Comes the Pain (SLUS_207.87)

How the entrance shirt tear works inside HCTP, and how to give Modern Hulk Hogan (roster id 0x20) a working tear on
**his own** entrance (his own motion and camera, not Legend Hogan's). Verified on the PC port (Oct 26 2026). The PS2
steps use the same data and one byte patch, but have **not** been tested on a PS2 or PCSX2 yet.

## 1. How the game does the tear

| Piece | Where | What it does |
|---|---|---|
| Dress flag | `CSd5EventData::IsDress` 0x2af300 reads byte `0x41B5C0 + id*6 + attire` | 1 = this attire has an entrance "dress" (gear). Stock: only Legend Hogan (0x4E) has it. |
| Dress model | pack `XXX2` (Modern Hogan = `3202`, EMD2 folder) | The removable gear (shirt / bandana / boa) worn during the entrance. |
| Dress cast | `SetDressCast` 0x1672c0 | Appends the dress as an extra cast with slot code `0xDD00` + body index. |
| Visibility | `CEventSystem::StartScene` 0x167c10 | At each scene start a body is shown only if its SEVP slot for that scene has `on = 1`. A `0xDDnn` cast copies body `nn`'s visibility, so **the gear disappears when the body it follows is hidden**. |
| Torn shirt prop | SEVP object model `2105` | Comes on at the tear scene (Modern Hogan: scene 5). |

So the tear is not a special animation. At the tear scene, the body wearing the gear is switched off, and a second
copy of the same wrestler (without gear) is switched on, playing the post-tear motion.

Stock Modern Hogan's entrance (event 3032) has one body playing all 8 scenes and no dress flag, so the gear never
comes off. Legend Hogan's event 3078 already has the second body (SEVP object 6).

## 2. The fix for Modern Hogan (his own entrance)

1. **Dress flag:** set byte `0x41B5C0 + 0x20*6 + 0` = `0x41B680` to `1` (attire 0).
2. **Gear model:** provide pack `3202` (the entrance gear) next to his body `3200`.
3. **Entrance event data (`evtEnt.pac`, EVET 3032 / 3132):** keep the stock Modern bank. Nothing in the motion
   data changes.
4. **SEVP row 3032 (and 3132), the cast list (`ENTR\SEVP`):**
   - Object 0 (body 3200): set slots 5, 6, 7 to `on = 0` (hidden from the tear on; the gear goes with it).
   - Add object 6: a copy of object 0 (model 3200) with slots 0-4 `on = 0` and slots 5-7 `on = 1`. Keep its
     motions `x0 / y5..7`, which are Modern's own post-tear motions.
   - Leave the props (2105 torn shirt, 2019) parented as they are. Moving them to object 6 made the match hang at
     loading.

On PC all of this is done by `tools\build_modern_hogan_own_tear.py` (SEVP + EVET as loose MODS files) plus
`enableHoganDress` in the runtime (the byte in step 1).

## 3. Doing it on a real PS2 / PCSX2 (untested)

- Byte patch (pnach): `patch=1,EE,0041B680,byte,00000001`
- Put the edited `SEVP` and the stock EVET 3032 bank back into the game's archives with the PAC Editor
  (DPAC archives), and add the `3202` gear pack. SEVP row layout: 28-byte objects, slots of
  `u16 slot, u8 on, u8 pad, u32 tid, u32 tx, u32 ty, 5 floats, u16 parent` (see `hctp_entrance.parse_sevp`).
- The archives may need space for the extra SEVP object; test the loading screen first.

## 4. Lighting

Entrance lights are effect rows / GMEV gimmicks in the SEVP data, not the EVET header bytes. Modern Hogan's
lighting was handled separately (notes Oct 20-22) and is not covered by this tear fix.

## 5. Other Hogans / attires

The same recipe works for any attire: set its `IsDress` byte, ship its `XXX2` gear pack, and split the body cast at
the tear scene. Planned next: a torn-shirt prop that matches each Hogan attire (Modern, nWo, Mr. America, Legend).
