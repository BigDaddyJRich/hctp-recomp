# Release notes

Each version's notes: what's new, what's better, what's fixed, and anything good to know before updating.
The newest version is at the top.

---

## 0.1.3

### Fixed
- **Fullscreen no longer drops you back to the desktop.** Some programs, like Microsoft Copilot, jump to the front when
  the screen changes its refresh rate, which sent the game to the background and made Alt-Tab bounce straight back
  out. The game now stays fullscreen when another program does that, and only minimises when you switch away
  yourself (Alt-Tab, the Windows key or a click elsewhere).
- Pressing **PLAY** in the settings program brings the game to the front reliably.

### Better
- **New installs use your screen's own refresh rate** in fullscreen, so the screen doesn't switch at all. You can still
  pick "Smoothest" or a fixed rate under Display. If you installed an earlier version, your setting stays as it is.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.2

### Better
- **Clearer progress while the game is rebuilt,** in updates and in the first setup. The bar keeps moving through the
  longest part, the line under it says what is happening in plain words (for example "Checking which files changed:
  13,000 of 25,467 files"), and a time left is only shown when it can be worked out. No more "1 min left" while it
  is still busy for several minutes.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.1

### Fixed
- **Widescreen:** crowd members at the sides of the picture no longer vanish and pop back in. The whole crowd is now
  drawn across the full wide picture.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.0: the first release

Def Jam Vendetta (PlayStation 2, USA, SLUS-20639) as a native Windows program, built on your PC from your own disc.
No game files come with this download.

### New
- **Built from your own disc.** Def Jam Vendetta Recompiled Settings reads your copy (an `.iso` or `.chd` file), installs Microsoft's
  build tools if your PC doesn't have them, and builds the game in 20 to 30 minutes. It only does this once.
- **Up to 4 players**
- **Widescreen** matches (menus and movies stay 4:3), and fullscreen, borderless or windowed play.
- **A sharp picture:** drawn on your graphics card at up to 16 times the PS2's detail, with the HUD, text and menus
  as sharp as the 3D (Sharp 2D).
- **High Poly in every match:** full-detail wrestlers and arenas in 3- and 4-fighter matches too.
- **Camera styles:** Def Jam Vendetta's own camera, WWF No Mercy style or Wrestle Kingdom 2 style (the ring seen from a
  corner, with the controls turned to match). Choose it under Gameplay.
- **Unlock everything:** every wrestler, attire, arena and gallery picture for every player, with or without a save.
  Your save is never changed, and what you unlock while playing is still saved. Off by default, under Gameplay.
- **Vulkan:** the PS2's graphics are redrawn on your graphics card with Vulkan, and the picture is shown with Vulkan too.
- **Def Jam Vendetta Recompiled Settings:** display, graphics, sound, controls and gameplay in one place, with PLAY.

### Good to know
- You need: Windows 10 or 11 (64-bit), a graphics card with Vulkan 1.3, about 8 GB of free space, and your own
  copy of Def Jam Vendetta for PlayStation 2 (USA).
- Unzip somewhere short, like your Desktop or `C:\Games`. The build tools can't handle very long folder paths.
- Windows may say "Windows protected your PC" the first time: click **More info**, then **Run anyway**.
- 16× (the default) needs a fairly strong graphics card. If the game runs slowly, set Scale to 8× under Graphics.
- The keyboard plays as nobody by default: choose a player under Controls.
- Mod support is actively being worked on and will come in a future update.
- When a new version comes out, the game asks when it starts: **Update** opens the settings program on what's new,
  **Play** starts the game and doesn't ask about that version again. Turn it off under About.
- After each update Windows may show "Windows protected your PC" again (the program isn't signed yet): **More info** →
  **Run anyway**.
