using System.Globalization;
using System.Text;

namespace VendettaSettings.Services;

/// <summary>The game's settings, as in <c>settings.toml</c> (the game reads it at start; each line is explained in the
/// file). Saving changes only the values, in place: comments and lines added by hand stay.</summary>
public sealed class GameSettings
{
    public const int RefreshSmoothest = -1; // "smoothest": the highest multiple of 60 Hz the screen has
    public const int RefreshScreens = 0;    // the screen's own rate

    // [display]
    public string Mode = "fullscreen";      // fullscreen (the default) | window | borderless
    public int Screen = 1;
    public bool Maximised = true;
    public int FullscreenWidth;             // 0 = the screen's own resolution
    public int FullscreenHeight;
    public int RefreshRate = RefreshSmoothest;
    public bool Vsync = true;
    public string Shape = "widescreen";     // widescreen (the default) | 4:3 | stretch
    // [graphics]
    public string Renderer = "gpu";         // gpu | cpu
    public int Scale = 16;                  // 1 2 4 8 16 (16 the default)
    public string Filter = "fsr";           // fsr | smooth | blocky
    public int Sharpening = 75;             // 0-100
    public bool HighPoly = true;            // High Poly wrestlers and arenas in every match
    public bool Sharp2D = true;             // the HUD, text and menus in the 3-D's detail
    public string Camera = "vendetta";      // the fight camera: vendetta (the game's own) | nomercy | wk2
    // [sound]
    public int Volume = 100;                // 0-100
    public bool Reverb = true;
    // [controls]
    public bool Multitap = true;            // up to 4 players (a multitap in port 2)
    public bool[] Vibration = { true, true, true, true }; // per player
    public int Keyboard = 0;                // the player the keyboard plays as: 1-4, 0 = nobody (the default)
    // [updates] (this program's own: the game skips the section)
    public bool CheckUpdates = true;        // ask GitHub for a newer version when this program opens

    /// <summary>The same file the game writes when there is none.</summary>
    public const string DefaultFile =
        "# The game's settings. A settings program writes this file; you can also edit it by hand.\n" +
        "# A VENDETTA_* switch set in the environment wins over a line here.\n" +
        "\n" +
        "[display]\n" +
        "mode = \"fullscreen\"      # \"window\", \"borderless\" (a frameless window the size of the screen) or \"fullscreen\"\n" +
        "screen = 1               # which screen: 1 = the main one\n" +
        "maximised = true         # start maximised (window only)\n" +
        "fullscreen_width = 0     # fullscreen resolution; 0 = the screen's own\n" +
        "fullscreen_height = 0\n" +
        "refresh_rate = \"smoothest\" # fullscreen only: \"smoothest\" (the highest multiple of 60 Hz the screen has),\n" +
        "                         # a number such as 60 or 120, or 0 = the screen's own\n" +
        "vsync = true             # wait for the screen before showing each picture (no tearing)\n" +
        "shape = \"widescreen\"     # \"widescreen\" (matches at 16:9 with more of the arena; menus and movies 4:3),\n" +
        "                         # \"4:3\" (as on the PS2, bars at the sides) or \"stretch\" (fill the window)\n" +
        "\n" +
        "[graphics]\n" +
        "renderer = \"gpu\"         # \"gpu\" (the graphics card) or \"cpu\"\n" +
        "scale = 16               # how much sharper than the PS2: 1, 2, 4, 8 or 16 (16: four times its detail)\n" +
        "filter = \"fsr\"           # scaling to the window: \"fsr\", \"smooth\" or \"blocky\"\n" +
        "sharpening = 75          # 0-100, with \"fsr\"\n" +
        "sharp_2d = true          # the HUD, text and menus in the same detail as the 3-D (as PCSX2 upscales them); false = as on the PS2\n" +
        "high_poly = true         # every wrestler and arena at full detail in every match; false = as on the PS2\n" +
        "                         # (lower-detail wrestlers and arenas in 3- and 4-fighter matches)\n" +
        "camera = \"vendetta\"      # the camera in matches: \"vendetta\" (the game's own), \"nomercy\" (WWF No Mercy's\n" +
        "                         # style) or \"wk2\" (Wrestle Kingdom 2's style)\n" +
        "\n" +
        "[sound]\n" +
        "volume = 100             # 0-100\n" +
        "reverb = true            # the echo the PS2's sound chip adds in matches\n" +
        "\n" +
        "[controls]\n" +
        "multitap = true          # up to 4 players (a multitap in port 2, as on the PS2); false = 2 controllers\n" +
        "vibration_1 = true       # each player's controller vibrates (Xbox-style / XInput controllers)\n" +
        "vibration_2 = true\n" +
        "vibration_3 = true\n" +
        "vibration_4 = true\n" +
        "keyboard = 0             # which player the keyboard plays as (with their controller): 1-4, 0 = nobody\n";

    public static GameSettings Load(string path)
    {
        var s = new GameSettings();
        if (!File.Exists(path))
            return s;
        foreach (var (key, value) in Parse(File.ReadAllLines(path)))
        {
            switch (key)
            {
                case "display.mode": s.Mode = OneOf(value, s.Mode, "window", "borderless", "fullscreen"); break;
                case "display.screen": s.Screen = Int(value, s.Screen, 1, 16); break;
                case "display.maximised": s.Maximised = Bool(value, s.Maximised); break;
                case "display.fullscreen_width": s.FullscreenWidth = Int(value, s.FullscreenWidth, 0, 16384); break;
                case "display.fullscreen_height": s.FullscreenHeight = Int(value, s.FullscreenHeight, 0, 16384); break;
                case "display.refresh_rate":
                    s.RefreshRate = value.Equals("smoothest", StringComparison.OrdinalIgnoreCase)
                        ? RefreshSmoothest : Int(value, s.RefreshRate, 0, 1000);
                    break;
                case "display.vsync": s.Vsync = Bool(value, s.Vsync); break;
                case "display.shape": s.Shape = OneOf(value, s.Shape, "4:3", "stretch", "widescreen"); break;
                case "graphics.renderer": s.Renderer = OneOf(value, s.Renderer, "gpu", "cpu"); break;
                case "graphics.scale":
                    var scale = Int(value, s.Scale, 1, 16);
                    s.Scale = scale is 1 or 2 or 4 or 8 or 16 ? scale : s.Scale;
                    break;
                case "graphics.filter": s.Filter = OneOf(value, s.Filter, "fsr", "smooth", "blocky"); break;
                case "graphics.sharpening": s.Sharpening = Int(value, s.Sharpening, 0, 100); break;
                case "graphics.high_poly": s.HighPoly = Bool(value, s.HighPoly); break;
                case "graphics.sharp_2d": s.Sharp2D = Bool(value, s.Sharp2D); break;
                case "graphics.camera": s.Camera = OneOf(value, s.Camera, "vendetta", "nomercy", "wk2"); break;
                case "sound.volume": s.Volume = Int(value, s.Volume, 0, 100); break;
                case "sound.reverb": s.Reverb = Bool(value, s.Reverb); break;
                case "controls.multitap": s.Multitap = Bool(value, s.Multitap); break;
                case "controls.vibration_1": s.Vibration[0] = Bool(value, s.Vibration[0]); break;
                case "controls.vibration_2": s.Vibration[1] = Bool(value, s.Vibration[1]); break;
                case "controls.vibration_3": s.Vibration[2] = Bool(value, s.Vibration[2]); break;
                case "controls.vibration_4": s.Vibration[3] = Bool(value, s.Vibration[3]); break;
                case "controls.keyboard": s.Keyboard = Int(value, s.Keyboard, 0, 4); break;
                case "updates.check_on_open": s.CheckUpdates = Bool(value, s.CheckUpdates); break;
            }
        }
        return s;
    }

    /// <summary>Writes the values into the file (the default file first when there is none), each in its own line.</summary>
    public void Save(string path)
    {
        var lines = (File.Exists(path) ? File.ReadAllText(path) : DefaultFile)
            .Replace("\r\n", "\n").Split('\n').ToList();
        if (lines.Count > 0 && lines[^1] == "")
            lines.RemoveAt(lines.Count - 1);
        var values = new (string Section, string Key, string Value)[]
        {
            ("display", "mode", Quote(Mode)),
            ("display", "screen", Screen.ToString(CultureInfo.InvariantCulture)),
            ("display", "maximised", Maximised ? "true" : "false"),
            ("display", "fullscreen_width", FullscreenWidth.ToString(CultureInfo.InvariantCulture)),
            ("display", "fullscreen_height", FullscreenHeight.ToString(CultureInfo.InvariantCulture)),
            ("display", "refresh_rate", RefreshRate == RefreshSmoothest ? Quote("smoothest") : RefreshRate.ToString(CultureInfo.InvariantCulture)),
            ("display", "vsync", Vsync ? "true" : "false"),
            ("display", "shape", Quote(Shape)),
            ("graphics", "renderer", Quote(Renderer)),
            ("graphics", "scale", Scale.ToString(CultureInfo.InvariantCulture)),
            ("graphics", "filter", Quote(Filter)),
            ("graphics", "sharpening", Sharpening.ToString(CultureInfo.InvariantCulture)),
            ("graphics", "sharp_2d", Sharp2D ? "true" : "false"),
            ("graphics", "high_poly", HighPoly ? "true" : "false"),
            ("graphics", "camera", Quote(Camera)),
            ("sound", "volume", Volume.ToString(CultureInfo.InvariantCulture)),
            ("sound", "reverb", Reverb ? "true" : "false"),
            ("controls", "multitap", Multitap ? "true" : "false"),
            ("controls", "vibration_1", Vibration[0] ? "true" : "false"),
            ("controls", "vibration_2", Vibration[1] ? "true" : "false"),
            ("controls", "vibration_3", Vibration[2] ? "true" : "false"),
            ("controls", "vibration_4", Vibration[3] ? "true" : "false"),
            ("controls", "keyboard", Keyboard.ToString(CultureInfo.InvariantCulture)),
            ("updates", "check_on_open", CheckUpdates ? "true" : "false"),
        };
        foreach (var (section, key, value) in values)
            SetLine(lines, section, key, value);
        var text = string.Join("\n", lines) + "\n";
        var temp = path + ".new";
        File.WriteAllText(temp, text, new UTF8Encoding(false));
        File.Move(temp, path, overwrite: true);
    }

    // --- the file's TOML: [section], key = "string" | number | true / false, # comments ---

    private static IEnumerable<(string Key, string Value)> Parse(IEnumerable<string> lines)
    {
        var section = "";
        foreach (var raw in lines)
        {
            var line = StripComment(raw).Trim();
            if (line.Length == 0)
                continue;
            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                section = line[1..^1].Trim().ToLowerInvariant();
                continue;
            }
            var eq = line.IndexOf('=');
            if (eq < 0)
                continue;
            var key = line[..eq].Trim().ToLowerInvariant();
            var value = line[(eq + 1)..].Trim();
            if (value.Length >= 2 && value[0] == '"' && value[^1] == '"')
                value = value[1..^1];
            yield return (section.Length == 0 ? key : section + "." + key, value);
        }
    }

    private static string StripComment(string line)
    {
        var inString = false;
        for (int i = 0; i < line.Length; i++)
        {
            if (line[i] == '"')
                inString = !inString;
            else if (line[i] == '#' && !inString)
                return line[..i];
        }
        return line;
    }

    /// <summary>Replaces the value of <c>key</c> in <c>[section]</c>, keeping the line's spacing and comment; adds the line
    /// (and the section) when missing.</summary>
    private static void SetLine(List<string> lines, string section, string key, string value)
    {
        var current = "";
        int sectionEnd = -1;
        for (int i = 0; i < lines.Count; i++)
        {
            var trimmed = StripComment(lines[i]).Trim();
            if (trimmed.StartsWith('[') && trimmed.EndsWith(']'))
            {
                current = trimmed[1..^1].Trim().ToLowerInvariant();
                continue;
            }
            if (current != section)
                continue;
            if (trimmed.Length > 0)
                sectionEnd = i;
            var eq = trimmed.IndexOf('=');
            if (eq < 0 || !trimmed[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                continue;
            var line = lines[i];
            var valueStart = line.IndexOf('=') + 1;
            while (valueStart < line.Length && line[valueStart] == ' ')
                valueStart++;
            var body = StripComment(line);
            var oldValue = body[valueStart..].TrimEnd();
            var comment = line[(valueStart + oldValue.Length)..];
            // keep the comment's column where the old value was padded to it
            if (comment.StartsWith(' ') && value.Length != oldValue.Length)
            {
                var spaces = comment.Length - comment.TrimStart(' ').Length;
                comment = new string(' ', Math.Max(1, spaces + oldValue.Length - value.Length)) + comment.TrimStart(' ');
            }
            lines[i] = line[..valueStart] + value + comment;
            return;
        }
        if (sectionEnd < 0)
        {
            if (lines.Count > 0 && lines[^1].Trim().Length > 0)
                lines.Add("");
            lines.Add($"[{section}]");
            lines.Add($"{key} = {value}");
        }
        else
            lines.Insert(sectionEnd + 1, $"{key} = {value}");
    }

    private static string Quote(string text) => "\"" + text + "\"";

    private static string OneOf(string value, string fallback, params string[] words) =>
        words.FirstOrDefault(w => w.Equals(value, StringComparison.OrdinalIgnoreCase)) ?? fallback;

    private static int Int(string value, int fallback, int min, int max) =>
        int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n >= min && n <= max ? n : fallback;

    private static bool Bool(string value, bool fallback) =>
        value.Equals("true", StringComparison.OrdinalIgnoreCase) ? true
        : value.Equals("false", StringComparison.OrdinalIgnoreCase) ? false : fallback;
}
