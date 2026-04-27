using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using SubtitleFontHelperConfigWpf.ViewModels;

namespace SubtitleFontHelperConfigWpf;

public partial class MainWindow : Window
{
    private readonly MainWindowViewModel m_viewModel;

    public MainWindow(MainWindowViewModel viewModel)
    {
        InitializeComponent();
        m_viewModel = viewModel;
        DataContext = viewModel;
        viewModel.AttachWindow(this);
        viewModel.RequestClose += OnRequestClose;
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!m_viewModel.ConfirmClose())
        {
            e.Cancel = true;
        }

        base.OnClosing(e);
    }

    protected override void OnPreviewMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        if (e.OriginalSource is DependencyObject source && ShouldClearEditableSelections(source))
        {
            m_viewModel.ClearEditableSelections();
        }

        base.OnPreviewMouseLeftButtonDown(e);
    }

    private void OnRequestClose(object? sender, System.EventArgs e)
    {
        Close();
    }

    private static bool ShouldClearEditableSelections(DependencyObject source)
    {
        return FindAncestor<TextBoxBase>(source) is null
            && FindAncestor<ButtonBase>(source) is null
            && FindAncestor<ListBoxItem>(source) is null
            && FindAncestor<ScrollBar>(source) is null
            && FindAncestor<Thumb>(source) is null
            && FindAncestor<RepeatButton>(source) is null;
    }

    private static T? FindAncestor<T>(DependencyObject? current) where T : DependencyObject
    {
        while (current is not null)
        {
            if (current is T match)
            {
                return match;
            }

            current = VisualTreeHelper.GetParent(current);
        }

        return null;
    }
}
