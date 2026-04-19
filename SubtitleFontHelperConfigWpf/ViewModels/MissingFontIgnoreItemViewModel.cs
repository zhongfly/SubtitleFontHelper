using SubtitleFontHelperConfigWpf.Infrastructure;

namespace SubtitleFontHelperConfigWpf.ViewModels;

public sealed class MissingFontIgnoreItemViewModel : ObservableObject
{
    private const string IgnoreCasePrefix = "i:";

    private string m_pattern;
    private bool m_ignoreCase;

    public MissingFontIgnoreItemViewModel(string pattern = "", bool ignoreCase = false)
    {
        m_pattern = pattern;
        m_ignoreCase = ignoreCase;
    }

    public string Pattern
    {
        get => m_pattern;
        set => SetProperty(ref m_pattern, value);
    }

    public bool IgnoreCase
    {
        get => m_ignoreCase;
        set => SetProperty(ref m_ignoreCase, value);
    }

    public string ToConfigValue()
    {
        string pattern = Pattern.Trim();
        return IgnoreCase ? $"{IgnoreCasePrefix}{pattern}" : pattern;
    }

    public static MissingFontIgnoreItemViewModel FromConfigValue(string value)
    {
        string trimmed = value.Trim();
        bool ignoreCase = trimmed.StartsWith(IgnoreCasePrefix, System.StringComparison.OrdinalIgnoreCase);
        string pattern = ignoreCase ? trimmed.Substring(IgnoreCasePrefix.Length) : trimmed;
        return new MissingFontIgnoreItemViewModel(pattern, ignoreCase);
    }
}
