using Avalonia;
using Avalonia.Controls;

namespace VendettaSettings.Views;

public partial class PlayView : UserControl
{
    private const double PictureShape = 2400.0 / 1100.0; // background.jpg's width / height
    private const double PageMargin = 26 * 2;             // the page's left + right margin
    private const double OtherRows = 230;                 // the title, the gaps and the two sections under the picture

    public PlayView()
    {
        InitializeComponent();
        Page.SizeChanged += (_, e) => SizeBanner(e.NewSize);
    }

    // The whole picture, never cut: as wide as the page, or less when that would make it so tall that the rows under it
    // leave the window (a wide, short window), its height always width / 2.18.
    private void SizeBanner(Size page)
    {
        var width = Math.Max(0, page.Width - PageMargin);
        var height = width / PictureShape;
        var room = page.Height - OtherRows;
        if (room > 160 && height > room)
        {
            height = room;
            width = height * PictureShape;
        }
        Banner.Width = width;
        Banner.Height = height;
    }
}
