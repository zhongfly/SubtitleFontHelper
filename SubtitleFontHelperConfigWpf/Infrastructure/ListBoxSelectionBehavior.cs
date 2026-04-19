using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace SubtitleFontHelperConfigWpf.Infrastructure;

public static class ListBoxSelectionBehavior
{
    public static readonly DependencyProperty SyncSelectionWithTextInputFocusProperty =
        DependencyProperty.RegisterAttached(
            "SyncSelectionWithTextInputFocus",
            typeof(bool),
            typeof(ListBoxSelectionBehavior),
            new PropertyMetadata(false, OnSyncSelectionWithTextInputFocusChanged));

    public static readonly DependencyProperty AllowRepeatClickToClearSelectionProperty =
        DependencyProperty.RegisterAttached(
            "AllowRepeatClickToClearSelection",
            typeof(bool),
            typeof(ListBoxSelectionBehavior),
            new PropertyMetadata(false, OnAllowRepeatClickToClearSelectionChanged));

    public static bool GetSyncSelectionWithTextInputFocus(DependencyObject obj)
    {
        return (bool)obj.GetValue(SyncSelectionWithTextInputFocusProperty);
    }

    public static void SetSyncSelectionWithTextInputFocus(DependencyObject obj, bool value)
    {
        obj.SetValue(SyncSelectionWithTextInputFocusProperty, value);
    }

    public static bool GetAllowRepeatClickToClearSelection(DependencyObject obj)
    {
        return (bool)obj.GetValue(AllowRepeatClickToClearSelectionProperty);
    }

    public static void SetAllowRepeatClickToClearSelection(DependencyObject obj, bool value)
    {
        obj.SetValue(AllowRepeatClickToClearSelectionProperty, value);
    }

    private static void OnSyncSelectionWithTextInputFocusChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ListBox listBox)
        {
            return;
        }

        if ((bool)e.NewValue)
        {
            listBox.GotKeyboardFocus += OnGotKeyboardFocus;
        }
        else
        {
            listBox.GotKeyboardFocus -= OnGotKeyboardFocus;
        }
    }

    private static void OnAllowRepeatClickToClearSelectionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ListBox listBox)
        {
            return;
        }

        if ((bool)e.NewValue)
        {
            listBox.PreviewMouseLeftButtonDown += OnPreviewMouseLeftButtonDown;
        }
        else
        {
            listBox.PreviewMouseLeftButtonDown -= OnPreviewMouseLeftButtonDown;
        }
    }

    private static void OnPreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not ListBox listBox || e.OriginalSource is not DependencyObject source)
        {
            return;
        }

        if (IsSelectionManagedByInnerControl(source))
        {
            return;
        }

        ListBoxItem? item = FindAncestor<ListBoxItem>(source);
        if (item is null)
        {
            ClearSelectionLater(listBox, listBox.SelectedItem);
            return;
        }

        if (!item.IsSelected)
        {
            return;
        }

        ClearSelectionLater(listBox, item.DataContext);
    }

    private static void OnGotKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e)
    {
        if (sender is not ListBox listBox || e.OriginalSource is not DependencyObject source)
        {
            return;
        }

        if (FindAncestor<TextBoxBase>(source) is null)
        {
            return;
        }

        ListBoxItem? item = FindAncestor<ListBoxItem>(source);
        if (item?.DataContext is null)
        {
            return;
        }

        if (!ReferenceEquals(listBox.SelectedItem, item.DataContext))
        {
            listBox.SelectedItem = item.DataContext;
        }
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

    private static bool IsSelectionManagedByInnerControl(DependencyObject source)
    {
        return FindAncestor<TextBoxBase>(source) is not null
            || FindAncestor<ButtonBase>(source) is not null
            || FindAncestor<ScrollBar>(source) is not null
            || FindAncestor<Thumb>(source) is not null
            || FindAncestor<RepeatButton>(source) is not null;
    }

    private static void ClearSelectionLater(ListBox listBox, object? expectedSelection)
    {
        if (expectedSelection is null)
        {
            return;
        }

        listBox.Dispatcher.BeginInvoke(() =>
        {
            if (ReferenceEquals(listBox.SelectedItem, expectedSelection))
            {
                listBox.SelectedItem = null;
            }
        }, DispatcherPriority.Input);
    }
}
