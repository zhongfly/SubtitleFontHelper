using System.Collections.ObjectModel;

namespace SubtitleFontHelperConfigWpf.Models;

public sealed class ConfigModel
{
    public int WmiPollInterval { get; set; } = 1000;

    public int LruSize { get; set; } = 100;

    public string DataPath { get; set; } = string.Empty;

    public ObservableCollection<string> MonitorProcesses { get; } = [];

    public NotificationSettings Notifications { get; } = new();

    public ObservableCollection<ProcessRuleModel> ProcessMissingFontIgnore { get; } = [];

    public ObservableCollection<IndexFileModel> IndexFiles { get; } = [];
}
