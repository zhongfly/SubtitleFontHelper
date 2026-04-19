using SubtitleFontHelperConfigWpf.Infrastructure;

namespace SubtitleFontHelperConfigWpf.ViewModels;

public sealed class NavigationItemViewModel : ObservableObject
{
    private bool m_isSelected;

    public NavigationItemViewModel(string key, string title, string description)
    {
        Key = key;
        Title = title;
        Description = description;
    }

    public string Key { get; }

    public string Title { get; }

    public string Description { get; }

    public bool IsSelected
    {
        get => m_isSelected;
        set => SetProperty(ref m_isSelected, value);
    }
}
