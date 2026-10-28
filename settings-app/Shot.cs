using Avalonia;
using Avalonia.Headless;
using Avalonia.Threading;
using HctpSettings.Services;
using HctpSettings.ViewModels;
using HctpSettings.Views;

namespace HctpSettings;

/// <summary>Dev only: draws the window on one page to a PNG (1120 × 700 unless asked), headless (no screen, no DPI). Settings are read
/// but never written.</summary>
internal static class Shot
{
    public static void Render(string page, string path, int width, int height)
    {
        AppBuilder.Configure<App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .WithInterFont()
            .SetupWithoutStarting();

        var vm = new MainViewModel(GameFolder.Find(), readOnly: true);
        vm.ShowPage(page);
        var window = new MainWindow { DataContext = vm, Width = width, Height = height };
        window.Show();
        for (int i = 0; i < 6; i++)
        {
            Dispatcher.UIThread.RunJobs();
            window.CaptureRenderedFrame();
        }
        window.CaptureRenderedFrame()?.Save(path);
        Console.WriteLine($"shot {page} -> {path}");
    }

    /// <summary>Dev only (--selftest, with HCTP_LAUNCHER_ROOT = a scratch copy of the folder): changes settings the way the
    /// pages do, prints settings.toml after each, and with "play" starts the game as PLAY does.</summary>
    public static void SelfTest(bool play)
    {
        // it changes and saves settings: only ever on a scratch copy, never the real settings.toml
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("HCTP_LAUNCHER_ROOT")))
        {
            Console.WriteLine("--selftest changes and saves settings: set HCTP_LAUNCHER_ROOT to a scratch copy of the folder first");
            return;
        }
        var folder = GameFolder.Find();
        Console.WriteLine($"folder {folder.Root}; game {folder.HasGame}; exe {folder.GameExe ?? "none"}");
        if (play)
        {
            Console.WriteLine("play: " + (folder.Play(out var game) ?? $"started (pid {game?.Id})"));
            return;
        }
        var vm = new MainViewModel(folder, readOnly: false);
        var s = vm.Settings;
        Console.WriteLine($"loaded: mode {s.ModeIndex}, screen {s.Screen?.Label}, refresh {s.Refresh?.Label}, scale {s.ScaleIndex}");
        Console.WriteLine("refresh choices in fullscreen: " + string.Join(" | ", s.RefreshChoices.Select(c => c.Label)));
        Console.WriteLine("resolution choices: " + string.Join(" | ", s.ResolutionChoices.Take(6).Select(c => c.Label)));
        s.ModeIndex = 2;
        s.Refresh = s.RefreshChoices.FirstOrDefault(c => c.Value == 60) ?? s.Refresh;
        s.ScaleIndex = 2;
        s.FilterIndex = 2;
        s.Volume = 55;
        s.Reverb = false;
        s.ShapeIndex = 1;
        s.Multitap = false;
        s.Vibration3 = false;
        s.UnlockAll = true;
        Console.WriteLine($"status: {s.Status}");
        Console.WriteLine($"summary: {s.SummaryWindow} | {s.SummaryPicture} | {s.SummaryPlayers}");
        Console.WriteLine("--- settings.toml now:");
        Console.Write(File.ReadAllText(folder.SettingsPath));
    }

    /// <summary>Dev only (--selftest-update, with HCTP_LAUNCHER_ROOT = a test install and HCTP_UPDATE_URL = a test
    /// server): an update's steps without the window: check, download and check it, put it in place, rebuild (the
    /// install's tools\setup_game.py), then finish, or undo it all when the rebuild fails.</summary>
    public static async Task<int> SelfTestUpdate()
    {
        var folder = GameFolder.Find();
        Console.WriteLine($"folder {folder.Root}; can update {Updater.CanUpdate(folder)}; this version {Updater.CurrentText}");
        try
        {
            var release = await Updater.CheckAsync();
            Console.WriteLine(release == null
                ? "no release"
                : $"latest {release.Version} ({release.ZipSize} bytes, sha256 {release.Sha256 ?? "none"})");
            if (release == null || release.Version <= Updater.Current)
                return 0;
            var zip = Updater.ZipPath(folder);
            await Updater.DownloadAsync(release, zip, (_, _) => { }, CancellationToken.None);
            Console.WriteLine("downloaded and checked");
            var restart = Updater.Apply(folder, zip, release, _ => { });
            var journal = Updater.LoadJournal(folder)!;
            foreach (var e in journal.Entries)
                Console.WriteLine($"  {e.Kind} {e.Path}");
            Console.WriteLine($"in place (journal: {journal.State}); this program replaced: {restart}");
            using var rebuild = System.Diagnostics.Process.Start(folder.SetupStart(null))!;
            var output = await rebuild.StandardOutput.ReadToEndAsync();
            await rebuild.WaitForExitAsync();
            Console.Write(output);
            if (rebuild.ExitCode == 0)
            {
                Updater.Finish(folder);
                Console.WriteLine("rebuilt: the update is finished");
            }
            else
            {
                var back = Updater.RollBack(folder, journal);
                Updater.DeleteJournal(folder);
                Console.WriteLine($"the rebuild failed: undone (this program put back: {back})");
            }
            return 0;
        }
        catch (UpdateException ex)
        {
            Console.WriteLine($"stopped ({ex.Kind}): {ex.Message}");
            return 1;
        }
    }
}
