// Def Jam Vendetta (USA, SLUS-20639) - the main menu's 8th item, Quit: a question box, then the game closes.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The main menu is one routine, FUN_00155eb8(menu), run once a frame: input, then drawing. Instruction patches
// (config/patches.toml) make it an 8-item menu and point it at tables in the store's fixed area, which this file fills:
//
//   0x02000000  the texts: 8 items x 3 pointers (heading, two lines), the game's 7 from 0x23AEC8 + Quit's
//   0x02000060  the icon ids: 8 x s32, the game's 7 from 0x23AF20 + Quit's (the icons of the end-of-match menu)
//   0x02000080  the title picture ids: 8 x s32, the game's 7 from 0x23AF40 + Quit's
//   0x020000C0  the help box: 3 lines of {s32 button, text pointer} (the game's from 0x27D4F8), +0x18 how many,
//   0x020000E0  and its title (the game's "Help" from 0x295850)
//   0x02000100  Quit's texts
//
// The menu's state: +0xC the selected item, +0x60 the menu's phase (2 = taking input), +0xC4 the buttons held (0x8000
// square), +0xC8 the buttons pressed (0x4000 cross, 0x1000 triangle), +0xCC the left / right repeats, +0x8B4 busy,
// +0xFD0 the help box shown. Holding square shows the help box: the routine plays its opening sound, takes no input
// and draws the box while square is held, and plays the closing sound when it is let go. Cross on Quit (item 7) goes
// to the switch's empty case in the game's code, so the routine itself does nothing for it: this file then opens the
// same box with the question (Yes = cross, No = triangle) by giving the routine "square held" with the question's
// lines in the box, and on Yes asks the runtime to stop, as closing the window does.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_store.h"

#include <cstring>
#include <iostream>

namespace vendetta_menu
{
    constexpr uint32_t kMainMenu = 0x155EB8u;     // FUN_00155eb8(menu)
    constexpr uint32_t kGameTexts = 0x23AEC8u;    // 7 x 3 text pointers
    constexpr uint32_t kGameIcons = 0x23AF20u;    // 7 x s32
    constexpr uint32_t kGameTitles = 0x23AF40u;   // 7 x s32
    constexpr uint32_t kGameHelp = 0x27D4F8u;     // 3 x {s32 button, text pointer}
    constexpr uint32_t kGameHelpTitle = 0x295850u; // "Help"
    constexpr uint32_t kGameItems = 7u;
    constexpr uint32_t kQuitItem = 7u;

    constexpr uint32_t kTexts = 0x000u;           // offsets in the fixed area
    constexpr uint32_t kIcons = 0x060u;
    constexpr uint32_t kTitles = 0x080u;
    constexpr uint32_t kHelp = 0x0C0u;
    constexpr uint32_t kHelpCount = 0x0D8u;
    constexpr uint32_t kHelpTitle = 0x0E0u;
    constexpr uint32_t kStrings = 0x100u;
    constexpr uint32_t kSize = 0x200u;

    constexpr uint32_t kSelected = 0xCu;
    constexpr uint32_t kPhase = 0x60u;
    constexpr uint32_t kHeld = 0xC4u;
    constexpr uint32_t kPressed = 0xC8u;
    constexpr uint32_t kRepeats = 0xCCu;
    constexpr uint32_t kBusy = 0x8B4u;
    constexpr uint32_t kHelpShown = 0xFD0u;
    constexpr uint32_t kSquare = 0x8000u;
    constexpr uint32_t kCross = 0x4000u;
    constexpr uint32_t kTriangle = 0x1000u;

    constexpr uint32_t kExitPicture = 10u;        // the menus' icon / title picture "EXIT"

    constexpr uint32_t kButtonCross = 2u;         // the help box's button pictures: 0 d-pad, 1 triangle, 2 cross
    constexpr uint32_t kButtonTriangle = 1u;

    PS2Runtime::RecompiledFunction g_original = nullptr;
    uint32_t g_area = 0;                          // the fixed area's guest address of offset 0
    bool g_filled = false;
    bool g_asking = false;
    uint32_t g_swallow = 0;                       // buttons that opened / closed the box: ignored until let go

    // the game's help box, and the question, as written into the box before a frame
    uint8_t g_gameHelp[kHelpTitle + 0x20u - kHelp]{};
    uint8_t g_question[kHelpTitle + 0x20u - kHelp]{};

    uint8_t *guest(uint32_t address) { return vendetta_store::host(address); }

    uint32_t g_stringTop = 0;
    uint32_t putString(const char *text)
    {
        const uint32_t length = static_cast<uint32_t>(std::strlen(text)) + 1u;
        if (g_stringTop + length > kSize)
            return g_area + kStrings - 1u; // the byte before is a 0: an empty string (never expected)
        const uint32_t where = g_area + g_stringTop;
        std::memcpy(guest(where), text, length);
        g_stringTop += length;
        return where;
    }

    void fill(uint8_t *rdram)
    {
        g_filled = true;
        std::memset(guest(g_area), 0, kSize);
        g_stringTop = kStrings;
        for (uint32_t i = 0; i < kGameItems * 3u; ++i)
            Ps2FastWrite32(rdram, g_area + kTexts + i * 4u, Ps2FastRead32(rdram, kGameTexts + i * 4u));
        Ps2FastWrite32(rdram, g_area + kTexts + kQuitItem * 12u + 0u, putString("QUIT"));
        Ps2FastWrite32(rdram, g_area + kTexts + kQuitItem * 12u + 4u, putString("Close the game and head"));
        Ps2FastWrite32(rdram, g_area + kTexts + kQuitItem * 12u + 8u, putString("back to the desktop"));
        for (uint32_t i = 0; i < kGameItems; ++i)
        {
            Ps2FastWrite32(rdram, g_area + kIcons + i * 4u, Ps2FastRead32(rdram, kGameIcons + i * 4u));
            Ps2FastWrite32(rdram, g_area + kTitles + i * 4u, Ps2FastRead32(rdram, kGameTitles + i * 4u));
        }
        // Quit's icon and title picture: 10, the end-of-match menu's EXIT (the pictures go 0-25: 6-9 the match types,
        // 11 DEVELOP, 12 CONTINUE, 13 TUTORIAL, 14-25 CHAPTER 1-12; a number past 25 stops the game)
        Ps2FastWrite32(rdram, g_area + kIcons + kQuitItem * 4u, kExitPicture);
        Ps2FastWrite32(rdram, g_area + kTitles + kQuitItem * 4u, kExitPicture);

        // the game's help box as it was, and the question
        uint8_t *help = guest(g_area + kHelp);
        std::memcpy(help, guest(kGameHelp), 24u);
        Ps2FastWrite32(rdram, g_area + kHelpCount, 3u);
        std::memcpy(guest(g_area + kHelpTitle), guest(kGameHelpTitle), 5u); // "Help"
        std::memcpy(g_gameHelp, help, sizeof(g_gameHelp));

        Ps2FastWrite32(rdram, g_area + kHelp + 0u, kButtonCross);
        Ps2FastWrite32(rdram, g_area + kHelp + 4u, putString("Yes, quit to the desktop"));
        Ps2FastWrite32(rdram, g_area + kHelp + 8u, kButtonTriangle);
        Ps2FastWrite32(rdram, g_area + kHelp + 12u, putString("No, back to the menu"));
        Ps2FastWrite32(rdram, g_area + kHelp + 16u, 0u);
        Ps2FastWrite32(rdram, g_area + kHelp + 20u, 0u);
        Ps2FastWrite32(rdram, g_area + kHelpCount, 2u);
        std::memset(guest(g_area + kHelpTitle), 0, 0x20u);
        std::memcpy(guest(g_area + kHelpTitle), "Quit the game?", 15u);
        std::memcpy(g_question, help, sizeof(g_question));

        std::memcpy(help, g_gameHelp, sizeof(g_gameHelp));
    }

    void endAsking()
    {
        g_asking = false;
        std::memcpy(guest(g_area + kHelp), g_gameHelp, sizeof(g_gameHelp));
    }

    void mainMenu(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t menu = getRegU32(ctx, 4);
        if (!g_filled)
            fill(rdram);

        const bool taking = Ps2FastRead32(rdram, menu + kPhase) == 2u && Ps2FastRead32(rdram, menu + kBusy) == 0u;
        if (!taking && g_asking)
            endAsking();
        const uint32_t held = Ps2FastRead32(rdram, menu + kHeld);
        uint32_t pressed = Ps2FastRead32(rdram, menu + kPressed);
        // a press can show on more than one of the menu's frames: the button that opened or closed the box counts
        // once, until a frame where it is neither pressed nor held
        g_swallow &= (pressed | held);
        if (g_swallow & pressed)
        {
            pressed &= ~g_swallow;
            Ps2FastWrite32(rdram, menu + kPressed, pressed);
        }

        bool open = false;
        if (taking && !g_asking && Ps2FastRead32(rdram, menu + kSelected) == kQuitItem && (pressed & kCross) &&
            !(held & kSquare) && Ps2FastRead32(rdram, menu + kHelpShown) == 0u)
        {
            g_asking = open = true;
            g_swallow = kCross;
            std::cerr << "[menu] Quit: asking" << std::endl;
        }
        if (!g_asking)
        {
            g_original(rdram, ctx, runtime);
            return;
        }

        bool close = false;
        if (!open && (pressed & kCross))
        {
            std::cerr << "[menu] Quit: the game closes" << std::endl;
            runtime->requestStop();
        }
        else if (!open && (pressed & kTriangle))
            close = true;

        // the box: the question in it; "square held" opens or keeps it, let go closes it (with the game's sounds).
        // What the routine is given stays for the rest of the frame, as when square is really held: the code that
        // runs the menu screens reads the buttons after it (a triangle given back there went to the title screen)
        std::memcpy(guest(g_area + kHelp), g_question, sizeof(g_question));
        if (open)
            Ps2FastWrite32(rdram, menu + kHelpShown, 0u);
        Ps2FastWrite32(rdram, menu + kHeld, close ? (held & ~kSquare) : (held | kSquare));
        Ps2FastWrite32(rdram, menu + kPressed, 0u);
        Ps2FastWrite32(rdram, menu + kRepeats, 0u);
        g_original(rdram, ctx, runtime);
        if (close)
        {
            endAsking();
            g_swallow = kTriangle;
            std::cerr << "[menu] Quit: no, back to the menu" << std::endl;
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_original = runtime.lookupFunction(kMainMenu);
        g_area = vendetta_store::fixed(0u, kSize, "main menu");
        if (!g_original || g_area == 0u)
        {
            std::cerr << "[menu] the main menu was not found: no Quit item" << std::endl;
            return;
        }
        runtime.replaceFunction(kMainMenu, mainMenu);
    }
}

void installVendettaMenu(PS2Runtime &runtime) { vendetta_menu::install(runtime); }
