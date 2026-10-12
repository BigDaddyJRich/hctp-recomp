using Avalonia;

namespace VendettaSettings;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        // Dev: a page as a picture, drawn without opening a window (nothing is saved):
        //   "Def Jam Vendetta Recompiled Settings" --shot <play|display|graphics|sound|controls|gameplay|about> <out.png> [width height]
        if (args.Length >= 3 && args[0] == "--shot")
        {
            Shot.Render(args[1], args[2], args.Length >= 5 ? int.Parse(args[3]) : 1120, args.Length >= 5 ? int.Parse(args[4]) : 700);
            return;
        }
        // Dev: the shortcuts, the desktop ones made in another folder: --selftest-shortcuts <folder> [nodesktop]
        if (args.Length >= 2 && args[0] == "--selftest-shortcuts")
        {
            var desktop = !(args.Length >= 3 && args[2] == "nodesktop");
            Console.WriteLine(Services.GameFolder.Find().CreateShortcuts(desktop, args[1]) ?? "made");
            return;
        }
        // Dev: an update without the window (VENDETTA_ROOT = a test install, VENDETTA_UPDATE_URL = a test server)
        if (args.Length >= 1 && args[0] == "--selftest-update")
        {
            Environment.ExitCode = Shot.SelfTestUpdate().GetAwaiter().GetResult();
            return;
        }
        // Dev: the saving checked on a scratch folder (VENDETTA_ROOT): --selftest [play]
        if (args.Length >= 1 && args[0] == "--selftest")
        {
            Shot.SelfTest(args.Length >= 2 && args[1] == "play");
            return;
        }
        // --update: started by the game's update popup (the new version's notes open at once)
        ViewModels.UpdateViewModel.OpenOnUpdate = args.Contains("--update");
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();
}
