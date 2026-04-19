using System.Collections.ObjectModel;

namespace SubtitleFontHelperConfigWpf.Models;

public sealed class ProcessRuleModel
{
    public ObservableCollection<string> Regex { get; } = [];

    public ObservableCollection<string> Processes { get; } = [];

    public string Flags { get; set; } = string.Empty;
}
