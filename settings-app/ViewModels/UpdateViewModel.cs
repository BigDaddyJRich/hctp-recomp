using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One line of a release's notes: a heading, a point, or plain text.</summary>
public sealed record NoteLine(string Text, bool IsHeading, bool IsBullet)
{
    public bool IsText => !IsHeading && !IsBullet;
}

/// <summary>Updates: the notice on the Play page, the Updates part of About, and the update itself (download, put in
/// place, rebuild) with its pages: what's new, updating, updated, and couldn't finish.</summary>
public partial class UpdateViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly Func<bool> _checkOnOpen;
    private readonly Action _quit;
    private readonly Stopwatch _clock = new();
    private Release? _release;
    private CancellationTokenSource? _cancel;
    private Process? _rebuild;
    private int _setupStep = -1;
    private double _setupProgress;

    // the setup's 6 steps (tools\setup_game.py) within "Rebuilding the game": the disc is already read, the tools are
    // there, so the recompiler, the recompile and above all the game's build take the time
    // weights from a real update (0.1.0 to 0.1.1, 2026-10-12): the steps before ~0.5 min, turning the code into C++ and
    // checking which files changed ~6.5 min, the build ~1 min (longer when more changed)
    private static readonly double[] SetupWeights = { 0.01, 0.02, 0.04, 0.60, 0.31, 0.02 };
    private static readonly string[] SetupParts =
    {
        "reading your disc", "the build tools", "the recompiler", "turning the game's code into C++ and checking what changed",
        "building the game", "finishing",
    };
    private readonly Stopwatch _setupClock = new(); // since the rebuild's current part began

    public UpdateViewModel(GameFolder folder, Func<bool> checkOnOpen, Action quit)
    {
        _folder = folder;
        _checkOnOpen = checkOnOpen;
        _quit = quit;
        Steps = new ObservableCollection<SetupStep>
        {
            new("Downloading the new version", 0.12, ""),
            new("Checking the download", 0.02, ""),
            new("Putting the new files in place", 0.04, "the old ones kept until it works"),
            new("Rebuilding the game", 0.80, "a few minutes"),
            new("Finishing", 0.02, ""),
        };
    }

    public ObservableCollection<SetupStep> Steps { get; }
    public ObservableCollection<NoteLine> NoteLines { get; } = new();

    public string CurrentVersion => Updater.CurrentText;
    public bool CanUpdate => _sample || Updater.CanUpdate(_folder);
    private bool _sample; // the pictures of the pages (--shot) show what an install would

    // --- the page shown: none (the normal pages), notes (what's new), working, done, failed ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Active), nameof(IsNotes), nameof(IsWorking), nameof(IsDone), nameof(IsFailed),
        nameof(ShowNotice), nameof(NavDownloadNow), nameof(NavDownloadDone), nameof(NavRebuildNow), nameof(NavRebuildDone),
        nameof(NavRebuildNext), nameof(NavDoneNow), nameof(NavDoneNext), nameof(Status))]
    private string _mode = "none";

    public bool Active => Mode != "none";
    public bool IsNotes => Mode == "notes";
    public bool IsWorking => Mode == "working";
    public bool IsDone => Mode == "done";
    public bool IsFailed => Mode == "failed";

    // --- the notice on Play ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowNotice), nameof(NewVersion), nameof(NoticeTitle), nameof(NoticePoints), nameof(NoticeMore),
        nameof(NoticeSize), nameof(NavTitle), nameof(NotesTitle))]
    private Release? _available;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowNotice))]
    private bool _later;

    public bool ShowNotice => Available != null && !Later && !Active && CanUpdate;
    public string NewVersion => Available?.Version.ToString(3) ?? _toVersion;
    public string NoticeTitle => $"Version {NewVersion} is available";
    public string NoticeSize =>
        Available is { ZipSize: > 0 } r
            ? $"About {Math.Round(r.ZipSize / 1e6).ToString(CultureInfo.InvariantCulture)} MB to download, then the game is rebuilt on your PC. Your saves and settings stay as they are."
            : "A download, then the game is rebuilt on your PC. Your saves and settings stay as they are.";
    /// <summary>The notes' first few points, on one line.</summary>
    /// <summary>The notice's summary: the first 3 points that are news (not "Good to know"), each as its first sentence,
    /// one per line; the whole notes are under "What's new".</summary>
    public IReadOnlyList<string> NoticePoints => NewsPoints().Take(3).Select(FirstSentence).ToList();

    public string NoticeMore
    {
        get
        {
            var more = NewsPoints().Count() - 3;
            return Available == null ? "" : more > 0 ? $"and {more} more under What's new" : NoticePoints.Count == 0 ? "See what's new." : "";
        }
    }

    private IEnumerable<string> NewsPoints()
    {
        var section = "";
        foreach (var line in ParseNotes(Available?.Notes ?? ""))
        {
            if (line.IsHeading)
                section = line.Text;
            else if (line.IsBullet && !section.Equals("Good to know", StringComparison.OrdinalIgnoreCase))
                yield return line.Text;
        }
    }

    private static string FirstSentence(string text)
    {
        var end = text.IndexOf(". ", StringComparison.Ordinal);
        return end > 0 ? text[..(end + 1)] : text;
    }

    // --- the update's own page list (in the page list's place) ---
    private string _toVersion = "";
    public string NavTitle => $"UPDATE TO {NewVersion}";
    public string NotesTitle => $"What's new in {NewVersion}";
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NavDownloadNow), nameof(NavDownloadDone), nameof(NavRebuildNow), nameof(NavRebuildDone),
        nameof(NavRebuildNext), nameof(NavDoneNow), nameof(NavDoneNext))]
    private int _stage; // 0 download (steps 1-3), 1 rebuild (steps 4-5), 2 done

    public bool NavDownloadNow => IsWorking && Stage == 0;
    public bool NavDownloadDone => Stage > 0;
    public bool NavRebuildNow => IsWorking && Stage == 1;
    public bool NavRebuildDone => Stage > 1;
    public bool NavRebuildNext => Stage < 1;
    public bool NavDoneNow => IsDone;
    public bool NavDoneNext => !IsDone;

    // --- progress ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PercentText))]
    private double _percent;

    [ObservableProperty] private string _timeLine = "";
    [ObservableProperty] private string _lastLine = "";
    [ObservableProperty] private string _workingTitle = "";
    public string PercentText => $"{Math.Round(Percent * 100)}%";

    // --- couldn't finish ---
    [ObservableProperty] private string _failTitle = "";
    [ObservableProperty] private string _failReason = "";
    [ObservableProperty] private string _failAdvice = "";
    [ObservableProperty] private string _keptVersion = "";

    // --- updated ---
    [ObservableProperty] private string _doneTitle = "";

    /// <summary>The header's status while an update page is open.</summary>
    public string Status => Mode switch
    {
        "working" => "Updating…",
        "done" => "Ready to play",
        "failed" => "The update didn't finish",
        _ => "",
    };

    // --- About: Updates ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CheckButton), nameof(CanCheck))]
    private bool _checking;

    [ObservableProperty] private string _checkText = "";
    public string CheckButton => Checking ? "Checking…" : "Check for updates";
    public bool CanCheck => !Checking && !Active && CanUpdate;

    // ============================================================================================================

    /// <summary>Started by the game's update popup (--update): check at once and open on what's new.</summary>
    public static bool OpenOnUpdate { get; set; }

    /// <summary>When the window opens: an update left halfway is undone (or its rebuild carried on); otherwise, if
    /// asked to, GitHub is asked for a newer version.</summary>
    public async Task StartAsync(bool inSetup)
    {
        Updater.CleanUp(_folder);
        CheckText = CanUpdate ? $"Version {CurrentVersion}" : "This copy is updated with git (a developer's copy).";
        var journal = Updater.LoadJournal(_folder);
        if (journal != null)
        {
            Resume(journal);
            return;
        }
        if (!inSetup && CanUpdate && OpenOnUpdate)
        {
            // the player pressed Update in the game's popup: straight to the new version's notes and its Update button
            await CheckAsync(quiet: false);
            if (Available != null && Available.Version > Updater.Current)
                ShowNotes();
            return;
        }
        if (!inSetup && CanUpdate && _checkOnOpen())
            await CheckAsync(quiet: true);
    }

    [RelayCommand]
    private Task CheckNow() => CheckAsync(quiet: false);

    private async Task CheckAsync(bool quiet)
    {
        if (Checking)
            return;
        Checking = true;
        CheckText = "Checking…";
        try
        {
            var release = await Updater.CheckAsync();
            if (release != null && release.Version > Updater.Current)
            {
                Available = release;
                Later = false;
                CheckText = $"Version {NewVersion} is available. See the Play page.";
            }
            else
                CheckText = $"Up to date (version {CurrentVersion}). Checked today at {DateTime.Now:HH:mm}.";
        }
        catch (Exception ex)
        {
            Updater.Log(_folder, "checking for an update failed: " + ex.Message);
            CheckText = quiet ? $"Version {CurrentVersion}" : "Couldn't check: no connection to GitHub. Try again later.";
        }
        finally
        {
            Checking = false;
        }
    }

    [RelayCommand]
    private void LaterOn() => Later = true;

    [RelayCommand]
    private void ShowNotes()
    {
        FillNotes(Available?.Notes ?? "");
        Mode = "notes";
    }

    [RelayCommand]
    private void Back() => Mode = "none";

    [RelayCommand]
    private void OpenLog() => GameFolder.OpenText(Updater.LogPath(_folder));

    [RelayCommand]
    private void OpenThisVersion() => GameFolder.Open($"{Updater.ReleasesUrl}/tag/v{CurrentVersion}");

    [RelayCommand]
    private void OpenAllVersions() => GameFolder.Open(Updater.ReleasesUrl);

    [RelayCommand]
    private void Finished()
    {
        Mode = "none";
        Stage = 0;
    }

    // --- the update ---

    [RelayCommand]
    private async Task UpdateNow()
    {
        if (Available is not { } release)
            return;
        _release = release;
        _toVersion = release.Version.ToString(3);
        foreach (var s in Steps)
            s.State = "next";
        Percent = 0;
        TimeLine = "";
        LastLine = "";
        Stage = 0;
        WorkingTitle = $"Updating to version {_toVersion}";
        Mode = "working";
        _clock.Restart();
        Updater.Log(_folder, $"update to {_toVersion} started ({release.ZipUrl})");
        try
        {
            if (GameFolder.IsGameRunning())
                throw new UpdateException("busy", "The game is open. Close it, then press Update again.");
            if (_folder.FreeSpaceGb is >= 0 and < 1.5)
                throw new UpdateException("other", "Not enough free space: the update needs about 1.5 GB while it runs.");

            SetStep(0);
            var zip = Updater.ZipPath(_folder);
            _cancel = new CancellationTokenSource();
            await Updater.DownloadAsync(release, zip, (done, total) => Dispatcher.UIThread.Post(() =>
            {
                StepProgress(total > 0 ? done / (double)total : 0);
                LastLine = total > 0 ? $"{done / 1e6:0.0} of {total / 1e6:0.0} MB" : $"{done / 1e6:0.0} MB";
            }), _cancel.Token);

            SetStep(1);
            LastLine = "Checked: the download is complete.";
            SetStep(2);
            var restart = await Task.Run(() => Updater.Apply(_folder, zip, release,
                p => Dispatcher.UIThread.Post(() => StepProgress(p))));
            Updater.Log(_folder, "the new files are in place" + (restart ? "; starting the new settings program" : ""));
            if (restart)
            {
                // the new program carries on (it finds the journal: the rebuild)
                LastLine = "Starting the new version of this program…";
                await Task.Delay(400);
                Updater.Restart(_folder);
                _quit();
                return;
            }
            StartRebuild();
        }
        catch (Exception ex)
        {
            Fail(ex as UpdateException ?? new UpdateException("other", ex.Message), journal: null);
        }
    }

    /// <summary>A journal from an earlier run: a rebuild to carry on, an update cut off halfway to undo, or one that
    /// was undone (by the new program, which then started this one) to report.</summary>
    private void Resume(UpdateJournal journal)
    {
        _toVersion = journal.To;
        FillNotes(journal.Notes);
        switch (journal.State)
        {
            case "rebuild":
                Updater.Log(_folder, $"carrying on: the rebuild for {journal.To}");
                foreach (var s in Steps.Take(3))
                    s.State = "done";
                WorkingTitle = $"Updating to version {journal.To}";
                Mode = "working";
                _clock.Restart();
                StartRebuild();
                break;
            case "applying":
                Updater.Log(_folder, "an update was cut off while its files were put in place: undoing it");
                var restart = Updater.RollBack(_folder, journal);
                journal.State = "failed";
                journal.FailKind = "interrupted";
                journal.FailReason = "The update was cut off before it finished (the program closed or the PC turned off).";
                if (restart)
                {
                    Updater.SaveJournal(_folder, journal);
                    Updater.Restart(_folder);
                    _quit();
                    return;
                }
                Updater.DeleteJournal(_folder);
                ShowFailed(journal.FailKind, journal.FailReason, journal.From);
                break;
            default:
                Updater.DeleteJournal(_folder);
                ShowFailed(journal.FailKind, journal.FailReason, journal.From);
                break;
        }
    }

    private void StartRebuild()
    {
        SetStep(3);
        Stage = 1;
        _setupStep = -1;
        _setupProgress = 0;
        Steps[3].Note = "a few minutes";
        LastLine = "Rebuilding the game with the new version…";
        if (GameFolder.IsGameRunning())
        {
            FailRebuild("The game is open. Close it, then open this program again to finish the update.");
            return;
        }
        try
        {
            _rebuild = Process.Start(_folder.SetupStart(null));
        }
        catch (Exception ex)
        {
            FailRebuild($"Couldn't start the rebuild: {ex.Message}");
            return;
        }
        if (_rebuild == null)
        {
            FailRebuild("Couldn't start the rebuild.");
            return;
        }
        var process = _rebuild;
        var log = new StreamWriter(Path.Combine(_folder.LogsPath, "update-build.log"), append: false) { AutoFlush = true };
        string? error = null;
        void Line(string line) => Dispatcher.UIThread.Post(() =>
        {
            log.WriteLine(line);
            if (line.StartsWith("[error] "))
                error = line[8..];
            else
                OnSetupLine(line);
        });
        _ = Task.Run(async () =>
        {
            string? line;
            while ((line = await process.StandardOutput.ReadLineAsync()) != null)
                Line(line);
        });
        _ = Task.Run(async () =>
        {
            string? line;
            while ((line = await process.StandardError.ReadLineAsync()) != null)
                Line(line);
        });
        _ = Task.Run(async () =>
        {
            await process.WaitForExitAsync();
            await Task.Delay(300); // the last lines first
            Dispatcher.UIThread.Post(() =>
            {
                log.Dispose();
                _rebuild = null;
                if (process.ExitCode != 0 || error != null)
                    FailRebuild(error ?? $"The rebuild stopped (exit code {process.ExitCode}).");
                else
                    FinishUpdate();
            });
        });
    }

    private void OnSetupLine(string line)
    {
        if (line.StartsWith("[step] "))
        {
            var parts = line.Split(' ', 4);
            if (int.TryParse(parts[1], out var n))
            {
                _setupStep = n - 1;
                _setupProgress = 0;
                _setupClock.Restart();
                LastLine = "";
                if (_setupStep >= 0 && _setupStep < SetupParts.Length)
                    Steps[3].Note = $"{n} of {SetupParts.Length}: {SetupParts[_setupStep]}";
                RebuildProgress();
            }
            return;
        }
        if (line.StartsWith("[progress] "))
        {
            if (double.TryParse(line[11..], NumberStyles.Float, CultureInfo.InvariantCulture, out var p))
            {
                _setupProgress = p;
                RebuildProgress();
            }
            return;
        }
        if (SetupOutput.Friendly(line) is { } shown)
            LastLine = shown;
    }

    private void RebuildProgress()
    {
        double within = 0;
        for (int i = 0; i < _setupStep && i < SetupWeights.Length; i++)
            within += SetupWeights[i];
        if (_setupStep >= 0 && _setupStep < SetupWeights.Length)
            within += SetupWeights[_setupStep] * _setupProgress;
        StepProgress(Math.Clamp(within, 0, 1));
    }

    private void FinishUpdate()
    {
        SetStep(4);
        Updater.Finish(_folder);
        Updater.Log(_folder, $"updated to {_toVersion}");
        foreach (var s in Steps)
            s.State = "done";
        Percent = 1;
        Stage = 2;
        Available = null;
        DoneTitle = $"Updated to version {_toVersion}";
        CheckText = $"Up to date (version {_toVersion}).";
        Mode = "done";
    }

    /// <summary>The rebuild failed: every file the update replaced goes back. When this program's own file was
    /// replaced, the old program is started again to say so (it reads the journal).</summary>
    private void FailRebuild(string why)
    {
        Updater.Log(_folder, "the rebuild failed: " + why);
        var journal = Updater.LoadJournal(_folder);
        if (journal == null)
        {
            Fail(new UpdateException("build", why), null);
            return;
        }
        var restart = Updater.RollBack(_folder, journal);
        journal.State = "failed";
        journal.FailKind = "build";
        journal.FailReason = why;
        if (restart)
        {
            Updater.SaveJournal(_folder, journal);
            Updater.Restart(_folder);
            _quit();
            return;
        }
        Updater.DeleteJournal(_folder);
        Fail(new UpdateException("build", why), journal);
    }

    private void Fail(UpdateException error, UpdateJournal? journal)
    {
        Updater.Log(_folder, $"the update stopped ({error.Kind}): {error.Message}");
        try { _cancel?.Cancel(); } catch { }
        ShowFailed(error.Kind, error.Message, journal?.From ?? CurrentVersion);
    }

    private void ShowFailed(string kind, string reason, string kept)
    {
        KeptVersion = $"You still have version {kept}, exactly as it was. Your game, saves and settings work as before.";
        FailTitle = kind == "busy" ? "The update can't start yet" : "The update couldn't finish";
        FailReason = reason;
        FailAdvice = kind switch
        {
            "download" => "Check your internet connection and try again.",
            "damaged" => "Try again: it downloads afresh.",
            "build" => "Try again. If it keeps failing, open the update's log and send it to @jcsix on X.",
            "busy" => "",
            "interrupted" => "Try again when you're ready.",
            _ => "Try again. If it keeps failing, open the update's log and send it to @jcsix on X.",
        };
        foreach (var s in Steps.Where(s => s.State == "now"))
            s.State = "next";
        Mode = "failed";
    }

    [RelayCommand]
    private async Task TryAgain()
    {
        if (Available == null)
        {
            // after a restart the release isn't known yet: ask again
            Mode = "none";
            await CheckAsync(quiet: false);
            if (Available != null)
                await UpdateNow();
            return;
        }
        await UpdateNow();
    }

    // --- helpers ---

    private void SetStep(int index)
    {
        for (int i = 0; i < Steps.Count; i++)
            Steps[i].State = i < index ? "done" : i == index ? "now" : "next";
        _current = index;
        StepProgress(0);
    }

    private int _current;

    private void StepProgress(double within)
    {
        double before = 0;
        for (int i = 0; i < _current && i < Steps.Count; i++)
            before += Steps[i].Weight;
        Percent = Math.Clamp(before + Steps[Math.Clamp(_current, 0, Steps.Count - 1)].Weight * Math.Clamp(within, 0, 1), 0, 1);
        // a time left only while the game itself is built (its own pace), else how long the update has been running
        if (Percent < 1)
            TimeLine = SetupTime.Line(_clock.Elapsed, _current == 3 && _setupStep == 4, _setupProgress, _setupClock.Elapsed);
    }

    private void FillNotes(string notes)
    {
        NoteLines.Clear();
        foreach (var line in ParseNotes(notes))
            NoteLines.Add(line);
        if (NoteLines.Count == 0)
            NoteLines.Add(new NoteLine("No notes came with this version.", false, false));
    }

    /// <summary>A release's notes (GitHub's Markdown) as headings, points and text: ## / ### headings, - or * points,
    /// **bold** and `code` marks dropped, the version's own heading left out.</summary>
    public static List<NoteLine> ParseNotes(string notes)
    {
        var lines = new List<NoteLine>();
        foreach (var raw in notes.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith("---") || line.StartsWith("<!--"))
                continue;
            if (line.StartsWith('#'))
            {
                var text = Clean(line.TrimStart('#'));
                if (raw.TrimStart().StartsWith("## ") && text.Length > 0 && char.IsDigit(text[0]))
                    continue; // "## 0.1.1 ...": the version, shown in the title already
                lines.Add(new NoteLine(text, true, false));
            }
            else if (line.StartsWith("- ") || line.StartsWith("* "))
                lines.Add(new NoteLine(Clean(line[2..]), false, true));
            else if (raw.StartsWith("  ") && lines.Count > 0 && lines[^1].IsBullet)
                lines[^1] = lines[^1] with { Text = lines[^1].Text + " " + Clean(line) }; // a point's next line
            else
                lines.Add(new NoteLine(Clean(line), false, false));
        }
        return lines;
    }

    private static string Clean(string text) => text.Replace("**", "").Replace("`", "").Trim();

    /// <summary>For pictures of the pages (--shot): a sample release and each page.</summary>
    /// <summary>Dev only (--selftest-progress): a rebuild's output fed through as if it ran, each change of what the
    /// page shows printed.</summary>
    public IEnumerable<string> ReplayRebuild(IEnumerable<string> lines)
    {
        _clock.Restart();
        Mode = "working";
        SetStep(3);
        Stage = 1;
        _setupStep = -1;
        Steps[3].Note = "a few minutes";
        var last = "";
        foreach (var line in lines)
        {
            OnSetupLine(line);
            var shown = $"{Math.Round(Percent * 100),3}% | {Steps[3].Note} | {LastLine} | {TimeLine}";
            if (shown != last)
                yield return last = shown;
        }
    }

    public void ShowSample(string page)
    {
        _sample = true;
        var sample = new Release(new Version(0, 1, 1), "v0.1.1",
            "## 0.1.1\n\n### Fixed\n- [Example] The fullscreen start goes straight to black.\n- [Example] The hit sounds play as on the PS2.\n\n" +
            "### Good to know\n- [Example] The update rebuilds the game on your PC in a few minutes.",
            Updater.ReleasesUrl, "", 88_300_000, null);
        // VENDETTA_SAMPLE_NOTES=<file>: a real release's notes in the pictures
        if (Environment.GetEnvironmentVariable("VENDETTA_SAMPLE_NOTES") is { Length: > 0 } notesFile && File.Exists(notesFile))
        {
            var notes = File.ReadAllText(notesFile);
            var heading = System.Text.RegularExpressions.Regex.Match(notes, @"^## (\d+\.\d+\.\d+)", System.Text.RegularExpressions.RegexOptions.Multiline);
            sample = sample with { Notes = notes, Version = heading.Success ? Version.Parse(heading.Groups[1].Value) : sample.Version };
        }
        Available = sample;
        _toVersion = sample.Version.ToString(3);
        FillNotes(sample.Notes);
        switch (page)
        {
            case "notes":
                Mode = "notes";
                break;
            case "working":
                WorkingTitle = "Updating to version 0.1.1";
                Mode = "working";
                Stage = 1;
                for (int i = 0; i < Steps.Count; i++)
                    Steps[i].State = i < 3 ? "done" : i == 3 ? "now" : "next";
                _current = 3;
                Percent = 0.55;
                TimeLine = "About 3 minutes left";
                LastLine = "[212/799] Building CXX object ps2xRuntime/…";
                break;
            case "done":
                DoneTitle = "Updated to version 0.1.1";
                Stage = 2;
                foreach (var s in Steps)
                    s.State = "done";
                Percent = 1;
                Mode = "done";
                break;
            case "failed":
                ShowFailed("download", "The download stopped: no connection to the internet.", CurrentVersion);
                break;
        }
    }
}
