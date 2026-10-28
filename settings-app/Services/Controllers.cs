using System.Runtime.InteropServices;

namespace HctpSettings.Services;

/// <summary>Which Xbox-style (XInput) controllers are plugged in, in XInput's order — the order the game gives them
/// out (player 1 the first, …). Other controllers (DirectInput) aren't seen here.</summary>
public static class Controllers
{
    [StructLayout(LayoutKind.Sequential)]
    private struct XInputState
    {
        public uint PacketNumber;
        public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short ThumbLX, ThumbLY, ThumbRX, ThumbRY;
    }

    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    private static extern uint XInputGetState(uint user, out XInputState state);

    /// <summary>How many XInput controllers are plugged in (0-4).</summary>
    public static int XInputCount()
    {
        try
        {
            int count = 0;
            for (uint user = 0; user < 4; user++)
                if (XInputGetState(user, out _) == 0)
                    count++;
            return count;
        }
        catch (Exception)
        {
            return 0; // no XInput on this PC
        }
    }
}
