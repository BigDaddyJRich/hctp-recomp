using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One line of the build's step list.</summary>
public partial class SetupStep : ObservableObject
{
    public SetupStep(string label, double weight, string note)
    {
        Label = label;
        Weight = weight;
        Note = note;
    }

    public string Label { get; }
    [ObservableProperty] private string _note;   // about how long it takes (or, while it runs, what it is doing)
    public double Weight { get; }  // its share of the whole
    [ObservableProperty] private string _state = "next"; // next | now | done
    public string Mark => State switch { "done" => "✓", "now" => "●", _ => "○" };
    public bool IsDone => State == "done";
    public bool IsNow => State == "now";
    public bool IsNext => State == "next";
    partial void OnStateChanged(string value)
    {
        OnPropertyChanged(nameof(Mark));
        OnPropertyChanged(nameof(IsDone));
        OnPropertyChanged(nameof(IsNow));
        OnPropertyChanged(nameof(IsNext));
    }
}

/// <summary>The setup's output (tools\setup_game.py), shown in plain words under the progress bar: null = keep the
/// line already shown.</summary>
public static class SetupOutput
{
    private static readonly System.Text.RegularExpressions.Regex Ninja = new(@"^\[(\d+)/(\d+)\]");
    private static readonly System.Text.RegularExpressions.Regex Sectors = new(@"^(\d+)/(\d+) sectors$");

    /// <summary>Only lines written for people: errors come as [error] lines and are shown on their own, so the build's
    /// other output (paths, CMake's checks and warnings, listings) stays in the log.</summary>
    public static string? Friendly(string line)
    {
        line = line.Trim();
        if (line.StartsWith("[retry] "))
            return "The build stopped. Trying again, more slowly";
        if (line.StartsWith("-- "))
            return "Getting the build ready";
        if (Ninja.Match(line) is { Success: true } m && m.Groups[1].Value != "0")
            return $"Building: part {m.Groups[1].Value} of {m.Groups[2].Value}";
        if (line.StartsWith("Checking which files changed:"))
            return line + " files";
        if (line == "Turning the game's code into C++" || line.StartsWith("Microsoft's C++ build tools are missing"))
            return line;
        if (line == "the disc is already prepared")
            return "Your disc was already read";
        if (Sectors.Match(line) is { Success: true } d && long.TryParse(d.Groups[2].Value, out var all) && all > 0)
            return $"Reading your disc: {100 * long.Parse(d.Groups[1].Value) / all}%";
        return null;
    }

    /// <summary>The error, then the tool's own error lines (the [detail] lines before it): what a player's screenshot
    /// of the error needs to show.</summary>
    public static string WithDetails(string error, IReadOnlyList<string> details) =>
        details.Count == 0 ? error : error + "\n\nWhat the build said:\n" + string.Join("\n", details);
}

/// <summary>The time line under the progress bar. A time left only while the game is built, from that step's own pace
/// (it is most of a first setup and has real progress); before that the steps' lengths vary too much to guess, so
/// how long it has been running.</summary>
public static class SetupTime
{
    public static string Line(TimeSpan elapsed, bool building, double buildProgress, TimeSpan buildElapsed)
    {
        if (building && buildProgress >= 0.25 && buildElapsed.TotalSeconds >= 30)
        {
            var left = buildElapsed.TotalSeconds * (1 - buildProgress) / buildProgress;
            var minutes = Math.Max(1, (int)Math.Round(left / 60));
            return $"About {minutes} minute{(minutes == 1 ? "" : "s")} left";
        }
        var so = (int)elapsed.TotalMinutes;
        return so < 1 ? "Under a minute so far" : $"{so} min so far";
    }
}

public partial class SetupViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly Action _finished;
    private Process? _process;
    private bool _cancelled;
    private readonly Stopwatch _clock = new();
    private readonly Stopwatch _stepClock = new(); // since the current step began (the build's pace)
    private StreamWriter? _log;

    public SetupViewModel(GameFolder folder, Action finished)
    {
        _folder = folder;
        _finished = finished;
        // the steps tools\setup_game.py goes through, with about how much of the time each takes
        Steps = new ObservableCollection<SetupStep>
        {
            new("Reading your disc", 0.06, "about 1 min"),
            new("Getting the build tools", 0.04, "only if they're missing"),
            new("Building the recompiler", 0.12, "about 1 min"),
            new("Turning the game's code into C++", 0.10, "under a minute"),
            new("Building the game", 0.66, "about 10 min"),
            new("Finishing", 0.02, ""),
        };
    }

    public ObservableCollection<SetupStep> Steps { get; }

    // --- step 1: the disc ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsDiscPage), nameof(IsBuildPage), nameof(BuildingNow))]
    private int _page = 1;

    public bool IsDiscPage => Page == 1;
    public bool IsBuildPage => Page == 2;
    public bool BuildingNow => IsBuildPage && Building;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasDisc), nameof(CanBuild), nameof(DiscAlreadyRead))]
    private string? _discPath;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanBuild), nameof(ShowDiscOk), nameof(ShowDiscBad))]
    private bool _discOk;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanBuild))]
    private bool _checking;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowDiscBad))]
    private string _discProblem = "";

    [ObservableProperty] private bool _makeShortcuts = true;

    public bool HasDisc => DiscPath != null;
    public bool ShowDiscOk => DiscOk && !Checking;
    public bool ShowDiscBad => DiscProblem.Length > 0 && !Checking;
    // a disc read in an earlier attempt (game\ is there) needs no new one
    public bool CanBuild => !Checking && !FolderTooLong && (DiscOk || (DiscPath == null && _folder.HasGame));

    // The build's deepest files lie ~140 characters below this folder, and the compiler can't write past Windows'
    // 260-character path limit ("cannot open program database"): a folder deep inside others has to move first.
    public const int MaxFolderLength = 100;
    public bool FolderTooLong => _folder.Root.Length > MaxFolderLength;
    public bool DiscAlreadyRead => DiscPath == null && _folder.HasGame;
    public string FolderPath => _folder.Root;
    public string SpaceText
    {
        get
        {
            var free = _folder.FreeSpaceGb;
            return "About 8 GB needed" + (free >= 0 ? $" · {free.ToString("0", CultureInfo.InvariantCulture)} GB free" : "");
        }
    }

    public string SpaceLine
    {
        get
        {
            var free = _folder.FreeSpaceGb;
            var freeText = free >= 0 ? $" ({free.ToString("0", CultureInfo.InvariantCulture)} GB free)" : "";
            return $"Needs about 8 GB of free space{freeText}. Want it somewhere else? Close this, move the folder, open it again.";
        }
    }

    partial void OnCheckingChanged(bool value)
    {
        OnPropertyChanged(nameof(ShowDiscOk));
        OnPropertyChanged(nameof(ShowDiscBad));
    }

    public async Task SetDiscAsync(string path)
    {
        DiscPath = path;
        DiscOk = false;
        DiscProblem = "";
        Checking = true;
        var problem = await _folder.CheckDiscAsync(path);
        Checking = false;
        DiscOk = problem == null;
        DiscProblem = problem ?? "";
    }

    // --- step 2: building ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PercentText), nameof(BarWidth))]
    private double _percent;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Building), nameof(Title), nameof(BuildingNow))]
    private bool _done;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Building), nameof(HasError), nameof(BuildingNow), nameof(Title))]
    private string _error = "";

    [ObservableProperty] private string _timeLine = "";
    [ObservableProperty] private string _lastLine = "";

    public bool Building => !Done && Error.Length == 0;
    public bool HasError => Error.Length > 0;

    /// <summary>A shortcut that couldn't be made (the game is built and plays all the same).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasShortcutProblem))]
    private string _shortcutProblem = "";

    public bool HasShortcutProblem => ShortcutProblem.Length > 0;
    public string PercentText => $"{Math.Round(Percent * 100)}%";
    public double BarWidth => Percent; // 0-1, the view scales it
    public string Title => Done ? "The game is built" : HasError ? "The setup stopped" : "Building the game from your disc";
    public string LogPath => Path.Combine(_folder.LogsPath, "setup.log");

    [RelayCommand]
    private void Build()
    {
        if (!CanBuild)
            return;
        Page = 2;
        Start();
    }

    private void Start()
    {
        foreach (var s in Steps)
            s.State = "next";
        Percent = 0;
        Done = false;
        Error = "";
        TimeLine = "Starting…";
        LastLine = "";
        _cancelled = false;
        Directory.CreateDirectory(_folder.LogsPath);
        _log = GameFolder.OpenRunLog(LogPath);
        _details.Clear();
        _clock.Restart();
        try
        {
            _process = Process.Start(_folder.SetupStart(DiscOk ? DiscPath : null));
        }
        catch (Exception ex)
        {
            Fail($"Couldn't start the setup: {ex.Message}");
            return;
        }
        if (_process == null)
        {
            Fail("Couldn't start the setup.");
            return;
        }
        var process = _process;
        _ = Task.Run(async () =>
        {
            string? line;
            while ((line = await process.StandardOutput.ReadLineAsync()) != null)
            {
                var text = line;
                Dispatcher.UIThread.Post(() => OnLine(text));
            }
        });
        _ = Task.Run(async () =>
        {
            string? line;
            while ((line = await process.StandardError.ReadLineAsync()) != null)
            {
                var text = line;
                Dispatcher.UIThread.Post(() => OnLine(text));
            }
        });
        _ = Task.Run(async () =>
        {
            await process.WaitForExitAsync();
            await Task.Delay(300); // the last lines first
            Dispatcher.UIThread.Post(() => OnExit(process.ExitCode));
        });
    }

    private int _step = -1;
    private double _stepProgress;

    private void OnLine(string line)
    {
        _log?.WriteLine(line);
        if (line.StartsWith("[step] "))
        {
            var parts = line.Split(' ', 4);
            if (int.TryParse(parts[1], out var n))
            {
                for (int i = 0; i < Steps.Count; i++)
                    Steps[i].State = i < n - 1 ? "done" : (i == n - 1 ? "now" : "next");
                _step = n - 1;
                _stepProgress = 0;
                _stepClock.Restart();
                LastLine = "";
                UpdatePercent();
            }
            return;
        }
        if (line.StartsWith("[progress] "))
        {
            if (double.TryParse(line[11..], NumberStyles.Float, CultureInfo.InvariantCulture, out var p))
            {
                _stepProgress = p;
                UpdatePercent();
            }
            return;
        }
        if (line.StartsWith("[detail] "))
        {
            _details.Add(line[9..]);
            return;
        }
        if (line.StartsWith("[error] "))
        {
            Fail(SetupOutput.WithDetails(line[8..], _details));
            return;
        }
        if (SetupOutput.Friendly(line) is { } shown)
            LastLine = shown;
    }

    private readonly List<string> _details = new(); // the tool's own error lines, shown under the error

    private void UpdatePercent()
    {
        double before = 0;
        for (int i = 0; i < _step && i < Steps.Count; i++)
            before += Steps[i].Weight;
        var total = before + (_step >= 0 && _step < Steps.Count ? Steps[_step].Weight * _stepProgress : 0);
        Percent = Math.Clamp(total, 0, 1);
        if (Percent < 1)
            TimeLine = SetupTime.Line(_clock.Elapsed, _step == 4, _stepProgress, _stepClock.Elapsed);
    }

    private void OnExit(int code)
    {
        _log?.Dispose();
        _log = null;
        _process = null;
        if (_cancelled)
            return;
        if (code != 0 || Error.Length > 0)
        {
            if (Error.Length == 0)
                Fail($"The setup stopped (exit code {code}).");
            return;
        }
        foreach (var s in Steps)
            s.State = "done";
        Percent = 1;
        // the game's shortcut in the install folder always, the desktop ones if asked for
        ShortcutProblem = _folder.CreateShortcuts(MakeShortcuts) ?? "";
        TimeLine = $"Done in {Math.Max(1, (int)Math.Round(_clock.Elapsed.TotalMinutes))} minutes";
        Done = true;
    }

    private void Fail(string why)
    {
        Error = why;
        TimeLine = "";
        foreach (var s in Steps.Where(s => s.State == "now"))
            s.State = "next";
    }

    [RelayCommand]
    private void Cancel()
    {
        _cancelled = true;
        try { _process?.Kill(entireProcessTree: true); } catch { }
        _process = null;
        _log?.Dispose();
        _log = null;
        Error = "";
        Page = 1;
    }

    [RelayCommand]
    private void TryAgain() => Start();

    [RelayCommand]
    private void Back()
    {
        Error = "";
        Page = 1;
    }

    /// <summary>Step 3: on to the settings.</summary>
    [RelayCommand]
    private void Continue() => _finished();

    [RelayCommand]
    private void OpenLog() => GameFolder.Open(LogPath);

    /// <summary>For pictures (--shot setup2): the build page part-way.</summary>
    public void ShowSample(double percent, bool done)
    {
        DiscPath = @"C:\Games\Discs\WWE SmackDown! Here Comes the Pain (USA).chd";
        DiscOk = true;
        Page = 2;
        _step = done ? Steps.Count : 4;
        for (int i = 0; i < Steps.Count; i++)
            Steps[i].State = done || i < 4 ? "done" : (i == 4 ? "now" : "next");
        Percent = percent;
        Done = done;
        TimeLine = done ? "Done in 17 minutes" : "About 7 minutes left";
        LastLine = done ? "" : "[512/799] Building CXX object ps2xRuntime/…";
    }

    /// <summary>For pictures (--shot setupfailed): a build that ran out of memory.</summary>
    public void ShowSampleError()
    {
        ShowSample(0.4, false);
        Fail(SetupOutput.WithDetails(SampleError, SampleDetails));
    }

    public const string SampleError = "Building the game: cmake.exe failed (exit code 1). Your PC ran out of memory while " +
                                      "building. Close other programs (browsers, games) and try again.";
    public static readonly string[] SampleDetails =
    {
        "FAILED: ps2xRuntime/CMakeFiles/ps2EntryRunner.dir/src/runner/out_00412.cpp.obj",
        @"C:\Games\HCTP Recompiled\files\source\PS2Recomp\ps2xRuntime\src\runner\out_00412.cpp(18211): fatal error C1060: compiler is out of heap space",
        "ninja: build stopped: subcommand failed.",
    };
}
