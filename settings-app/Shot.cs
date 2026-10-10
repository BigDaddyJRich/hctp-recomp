using Avalonia;
using Avalonia.Headless;
using Avalonia.Threading;
using VendettaSettings.Services;
using VendettaSettings.ViewModels;
using VendettaSettings.Views;

namespace VendettaSettings;

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

    /// <summary>Dev only (--selftest, with VENDETTA_ROOT = a scratch copy of the folder): changes settings the way the
    /// pages do, prints settings.toml after each, and with "play" starts the game as PLAY does.</summary>
    public static void SelfTest(bool play)
    {
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
        Console.WriteLine($"status: {s.Status}");
        Console.WriteLine($"summary: {vm.PlaySummary}");
        Console.WriteLine("--- settings.toml now:");
        Console.Write(File.ReadAllText(folder.SettingsPath));
    }
}
