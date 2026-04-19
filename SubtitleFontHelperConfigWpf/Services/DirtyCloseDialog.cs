using System.Windows;

namespace SubtitleFontHelperConfigWpf.Services;

public sealed partial class DirtyCloseDialog : Window
{
    public DirtyCloseDialog()
    {
        InitializeComponent();
    }

    public DirtyCloseAction SelectedAction { get; private set; } = DirtyCloseAction.Cancel;

    private void SaveAndClose(object sender, RoutedEventArgs e)
    {
        SelectedAction = DirtyCloseAction.SaveAndClose;
        DialogResult = true;
    }

    private void CloseWithoutSaving(object sender, RoutedEventArgs e)
    {
        SelectedAction = DirtyCloseAction.CloseWithoutSaving;
        DialogResult = true;
    }

    private void CancelClose(object sender, RoutedEventArgs e)
    {
        SelectedAction = DirtyCloseAction.Cancel;
        DialogResult = false;
    }
}
