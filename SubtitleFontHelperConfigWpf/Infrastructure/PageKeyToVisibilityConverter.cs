using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace SubtitleFontHelperConfigWpf.Infrastructure;

public sealed class PageKeyToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        string currentKey = value as string ?? string.Empty;
        string expectedKey = parameter as string ?? string.Empty;
        return string.Equals(currentKey, expectedKey, StringComparison.Ordinal) ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        throw new NotSupportedException();
    }
}
