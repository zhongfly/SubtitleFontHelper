using System.Collections.ObjectModel;

namespace SubtitleFontHelperConfigWpf.Models;

public sealed class IndexFileModel
{
    public string Path { get; set; } = string.Empty;

    public ObservableCollection<string> SourceFolders { get; } = [];
}
