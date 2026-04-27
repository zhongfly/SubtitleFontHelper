using System.Collections.ObjectModel;

namespace SubtitleFontHelperConfigWpf.Models;

public sealed class NotificationSettings
{
    public bool ManagedIndexNotifications { get; set; }

    public bool ManagedIndexFailureNotifications { get; set; } = true;

    public bool MissingFontNotifications { get; set; }

    public ObservableCollection<string> MissingFontIgnore { get; } = [];
}
