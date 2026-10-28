using System.Runtime.InteropServices;

namespace HctpSettings.Services;

/// <summary>The PC's screens and the modes each has, from Windows (the same list the game's fullscreen chooses from).
/// Ordered as the game numbers them: the main screen first.</summary>
public static class Displays
{
    public sealed record Mode(int Width, int Height, int Rate);

    public sealed record Screen(int Number, string DeviceName, bool Primary, Mode Current, IReadOnlyList<Mode> Modes)
    {
        /// <summary>The refresh rates the screen has at a size, lowest first.</summary>
        public IReadOnlyList<int> RatesAt(int width, int height) =>
            Modes.Where(m => m.Width == width && m.Height == height).Select(m => m.Rate).Distinct().OrderBy(r => r).ToList();

        /// <summary>Does a rate show each of the game's 60 frames for the same time (60, 120, 240, or 59 / 119)?</summary>
        public static bool FitsSixtyFrames(int hz) => hz > 0 && (hz % 60 == 0 || (hz + 1) % 60 == 0);

        /// <summary>What the game's "automatic" picks (the same rule as the game): the screen's rate when it fits 60
        /// frames and the size has it, else the smoothest it has, else the screen's rate.</summary>
        public int AutomaticAt(int width, int height)
        {
            var rates = RatesAt(width, height);
            if (FitsSixtyFrames(Current.Rate) && (rates.Count == 0 || rates.Contains(Current.Rate)))
                return Current.Rate;
            var even = rates.Where(FitsSixtyFrames).ToList();
            return even.Count > 0 ? even.Max() : Current.Rate;
        }

        /// <summary>What the game's "smoothest" picks: the highest multiple of 60 Hz (59 / 60, 119 / 120 …), else the highest.</summary>
        public int SmoothestAt(int width, int height)
        {
            var rates = RatesAt(width, height);
            if (rates.Count == 0)
                return Current.Rate;
            var even = rates.Where(r => r % 60 == 0 || (r + 1) % 60 == 0).ToList();
            return even.Count > 0 ? even[^1] : rates[^1];
        }
    }

    public static IReadOnlyList<Screen> Find()
    {
        var screens = new List<Screen>();
        try
        {
            for (uint i = 0; ; i++)
            {
                var device = new DisplayDevice { cb = Marshal.SizeOf<DisplayDevice>() };
                if (!EnumDisplayDevices(null, i, ref device, 0))
                    break;
                if ((device.StateFlags & AttachedToDesktop) == 0)
                    continue;
                var mode = NewDevMode();
                if (!EnumDisplaySettings(device.DeviceName, CurrentSettings, ref mode))
                    continue;
                var current = new Mode(mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
                var modes = new List<Mode>();
                for (int m = 0; ; m++)
                {
                    var each = NewDevMode();
                    if (!EnumDisplaySettings(device.DeviceName, m, ref each))
                        break;
                    if (each.dmBitsPerPel == 32)
                        modes.Add(new Mode(each.dmPelsWidth, each.dmPelsHeight, each.dmDisplayFrequency));
                }
                screens.Add(new Screen(0, device.DeviceName, (device.StateFlags & PrimaryDevice) != 0, current,
                    modes.Distinct().ToList()));
            }
        }
        catch (Exception)
        {
            // not Windows, or no access: no list (the settings still work, by number)
        }
        return screens.OrderByDescending(s => s.Primary).Select((s, n) => s with { Number = n + 1 }).ToList();
    }

    private const int AttachedToDesktop = 0x1;
    private const int PrimaryDevice = 0x4;
    private const int CurrentSettings = -1;

    private static DevMode NewDevMode() => new() { dmSize = (short)Marshal.SizeOf<DevMode>() };

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DisplayDevice
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DevMode
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion;
        public short dmDriverVersion;
        public short dmSize;
        public short dmDriverExtra;
        public int dmFields;
        public int dmPositionX;
        public int dmPositionY;
        public int dmDisplayOrientation;
        public int dmDisplayFixedOutput;
        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel;
        public int dmPelsWidth;
        public int dmPelsHeight;
        public int dmDisplayFlags;
        public int dmDisplayFrequency;
        public int dmICMMethod;
        public int dmICMIntent;
        public int dmMediaType;
        public int dmDitherType;
        public int dmReserved1;
        public int dmReserved2;
        public int dmPanningWidth;
        public int dmPanningHeight;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplayDevices(string? lpDevice, uint iDevNum, ref DisplayDevice lpDisplayDevice, uint dwFlags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DevMode devMode);
}
