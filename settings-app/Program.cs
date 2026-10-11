using Avalonia;

namespace VendettaSettings;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        // Dev: a page as a picture, drawn without opening a window (nothing is saved):
        //   "Def Jam Vendetta Recompiled Settings" --shot <play|display|graphics|sound|controls|about> <out.png> [width height]
        if (args.Length >= 3 && args[0] == "--shot")
        {
            Shot.Render(args[1], args[2], args.Length >= 5 ? int.Parse(args[3]) : 1120, args.Length >= 5 ? int.Parse(args[4]) : 700);
            return;
        }
        // Dev: the desktop shortcuts, made in another folder: --selftest-shortcuts <folder>
        if (args.Length >= 2 && args[0] == "--selftest-shortcuts")
        {
            Console.WriteLine(Services.GameFolder.Find().CreateShortcuts(args[1]) ?? "made");
            return;
        }
        // Dev: the saving checked on a scratch folder (VENDETTA_ROOT): --selftest [play]
        if (args.Length >= 1 && args[0] == "--selftest")
        {
            Shot.SelfTest(args.Length >= 2 && args[1] == "play");
            return;
        }
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();
}
