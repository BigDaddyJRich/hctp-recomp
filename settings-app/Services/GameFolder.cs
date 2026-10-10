using System.Diagnostics;

namespace VendettaSettings.Services;

/// <summary>The folder everything lives in: the game's files from the disc (<c>game\SLUS_206.39</c>), its memory card
/// (<c>game\mc0</c>), <c>settings.toml</c> and the game program. Found by looking up from this program's folder; for a
/// developer build (this program deep in <c>settings-app\bin\…</c>) that reaches the repository's root.
/// VENDETTA_ROOT=&lt;folder&gt; picks it by hand.</summary>
public sealed class GameFolder
{
    public const string Serial = "SLUS-20639";

    public string Root { get; }
    public string SettingsPath => Path.Combine(Root, "settings.toml");
    public string ElfPath => Path.Combine(Root, "game", "SLUS_206.39");
    public string SavesPath => Path.Combine(Root, "game", "mc0");
    public string LogsPath => Path.Combine(Root, "logs");
    public bool HasGame => File.Exists(ElfPath);
    public bool HasSave => Directory.Exists(Path.Combine(SavesPath, "BASLUS-20639GAMEDATA"));

    /// <summary>The game program: the newest of the places a build puts it (a player's install has it beside this program).</summary>
    public string? GameExe =>
        new[]
        {
            Path.Combine(Root, GameExeName),
            Path.Combine(Root, "PS2Recomp", "out", "fast", "ps2xRuntime", "ps2EntryRunner.exe"),
            Path.Combine(Root, "PS2Recomp", "out", "runtime", "ps2xRuntime", "ps2EntryRunner.exe"),
        }
        .Where(File.Exists)
        .OrderByDescending(File.GetLastWriteTimeUtc)
        .FirstOrDefault();

    private GameFolder(string root) => Root = root;

    public const string GameExeName = "Def Jam Vendetta Recompiled.exe";

    /// <summary>Where the build kit is: a release's <c>source\</c> beside this program, else the folder itself (the repository).</summary>
    public string SourceRoot =>
        Directory.Exists(Path.Combine(Root, "source", "tools")) ? Path.Combine(Root, "source") : Root;

    /// <summary>A release's own Python (<c>python\python.exe</c>), else the one on PATH.</summary>
    public string PythonExe =>
        File.Exists(Path.Combine(Root, "python", "python.exe")) ? Path.Combine(Root, "python", "python.exe") : "python";

    public string SetupScript => Path.Combine(SourceRoot, "tools", "setup_game.py");

    /// <summary>First run: no game files from a disc yet, or no game program built from them.</summary>
    public bool NeedsSetup => !HasGame || GameExe == null;

    public double FreeSpaceGb
    {
        get
        {
            try { return new DriveInfo(Path.GetPathRoot(Root)!).AvailableFreeSpace / 1e9; }
            catch { return -1; }
        }
    }

    /// <summary>Is it the right disc (the USA Def Jam Vendetta, SLUS-20639)? Null = yes, else why not.</summary>
    public async Task<string?> CheckDiscAsync(string disc)
    {
        var start = PythonStart(Path.Combine(SourceRoot, "tools", "prepare_disc.py"), "--check", disc);
        try
        {
            using var process = Process.Start(start)!;
            var output = await process.StandardOutput.ReadToEndAsync();
            var errors = await process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();
            if (process.ExitCode == 0 && output.Contains("ok SLUS-20639"))
                return null;
            var why = (errors.Trim().Split('\n').LastOrDefault() ?? "").Trim();
            return why.Length > 0 ? why : "This file couldn't be read as a disc.";
        }
        catch (Exception ex)
        {
            return $"Couldn't check the disc: {ex.Message}";
        }
    }

    /// <summary>The setup (tools\setup_game.py): its output comes line by line on standard output.</summary>
    public ProcessStartInfo SetupStart(string? disc) =>
        disc == null ? PythonStart(SetupScript, "--data", Root) : PythonStart(SetupScript, "--data", Root, "--disc", disc);

    private ProcessStartInfo PythonStart(string script, params string[] args)
    {
        var start = new ProcessStartInfo(PythonExe)
        {
            WorkingDirectory = SourceRoot,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8,
        };
        start.ArgumentList.Add("-u");
        start.ArgumentList.Add(script);
        foreach (var arg in args)
            start.ArgumentList.Add(arg);
        start.Environment["PYTHONIOENCODING"] = "utf-8";
        return start;
    }

    /// <summary>Desktop shortcuts: the game, and this program. Both with the logo (this program's icon).</summary>
    public string? CreateShortcuts(string? folder = null)
    {
        try
        {
            var desktop = folder ?? Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            var settings = Environment.ProcessPath ?? Path.Combine(Root, "Vendetta Settings.exe");
            if (GameExe is { } exe)
                Shortcut(Path.Combine(desktop, "Def Jam Vendetta Recompiled.lnk"), exe, settings);
            Shortcut(Path.Combine(desktop, "Vendetta Settings.lnk"), settings, settings);
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't make the desktop shortcuts: {ex.Message}";
        }
    }

    private void Shortcut(string link, string target, string icon)
    {
        var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("no WScript.Shell");
        dynamic shell = Activator.CreateInstance(type)!;
        dynamic shortcut = shell.CreateShortcut(link);
        shortcut.TargetPath = target;
        shortcut.WorkingDirectory = Root;
        shortcut.IconLocation = icon + ",0";
        shortcut.Save();
    }

    /// <summary>The game's program names (a player's install, a developer build); one running = PLAY is off, so two
    /// copies never share the memory card.</summary>
    private static readonly string[] GameProcessNames = { "Def Jam Vendetta Recompiled", "ps2EntryRunner" };

    public const string AlreadyRunning = "The game is already running.";

    public static bool IsGameRunning() =>
        GameProcessNames.Any(name =>
        {
            var found = Process.GetProcessesByName(name);
            foreach (var p in found)
                p.Dispose();
            return found.Length > 0;
        });

    public static GameFolder Find()
    {
        var chosen = Environment.GetEnvironmentVariable("VENDETTA_ROOT");
        if (!string.IsNullOrEmpty(chosen) && Directory.Exists(chosen))
            return new GameFolder(Path.GetFullPath(chosen));
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "game", "SLUS_206.39")) ||
                File.Exists(Path.Combine(dir.FullName, "settings.toml")))
                return new GameFolder(dir.FullName);
        }
        return new GameFolder(AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar));
    }

    /// <summary>Starts the game; what it prints goes to <c>logs\game.log</c> / <c>game.log.err</c>. Returns why it couldn't,
    /// or null with <paramref name="game"/> = the process that ends when the game does.</summary>
    public string? Play(out Process? game)
    {
        game = null;
        if (GameExe is not { } exe)
            return "The game isn't built yet.";
        if (!HasGame)
            return $"The game's files from your disc aren't here ({ElfPath}).";
        if (IsGameRunning())
            return AlreadyRunning;
        Directory.CreateDirectory(LogsPath);
        var log = Path.Combine(LogsPath, "game.log");
        // through cmd, so the log keeps being written after this program closes
        var start = new ProcessStartInfo("cmd.exe",
            $"/c \"\"{exe}\" \"{ElfPath}\" > \"{log}\" 2> \"{log}.err\"\"")
        {
            WorkingDirectory = Root,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        game = Process.Start(start);
        return null;
    }

    public static void Open(string pathOrUrl)
    {
        if (!pathOrUrl.StartsWith("http") && !Directory.Exists(pathOrUrl))
            Directory.CreateDirectory(pathOrUrl);
        Process.Start(new ProcessStartInfo(pathOrUrl) { UseShellExecute = true });
    }
}
