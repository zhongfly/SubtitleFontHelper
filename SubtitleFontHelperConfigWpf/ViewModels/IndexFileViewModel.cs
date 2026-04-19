using System;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Linq;
using SubtitleFontHelperConfigWpf.Infrastructure;
using SubtitleFontHelperConfigWpf.Models;

namespace SubtitleFontHelperConfigWpf.ViewModels;

public sealed class IndexFileViewModel : ObservableObject
{
    private string m_path = string.Empty;
    private EditableStringItemViewModel? m_selectedSourceFolder;

    public IndexFileViewModel()
    {
        SourceFolders.CollectionChanged += OnSourceFoldersChanged;

        AddSourceFolderCommand = new RelayCommand(AddSourceFolder);
        RemoveSourceFolderCommand = new RelayCommand(RemoveSelectedSourceFolder, () => SelectedSourceFolder is not null);
    }

    public IndexFileViewModel(IndexFileModel model)
        : this()
    {
        m_path = model.Path;
        foreach (string sourceFolder in model.SourceFolders)
        {
            SourceFolders.Add(new EditableStringItemViewModel(sourceFolder));
        }

        SelectedSourceFolder = null;
    }

    public RelayCommand AddSourceFolderCommand { get; }

    public RelayCommand RemoveSourceFolderCommand { get; }

    public string Path
    {
        get => m_path;
        set
        {
            if (SetProperty(ref m_path, value))
            {
                OnPropertyChanged(nameof(Title));
                OnPropertyChanged(nameof(Subtitle));
            }
        }
    }

    public ObservableCollection<EditableStringItemViewModel> SourceFolders { get; } = [];

    public EditableStringItemViewModel? SelectedSourceFolder
    {
        get => m_selectedSourceFolder;
        set
        {
            if (SetProperty(ref m_selectedSourceFolder, value))
            {
                RefreshCommands();
            }
        }
    }

    public string Title => string.IsNullOrWhiteSpace(Path) ? "未设置索引文件路径" : System.IO.Path.GetFileName(Path);

    public string Subtitle
    {
        get
        {
            int folderCount = SourceFolders.Count(static item => !string.IsNullOrWhiteSpace(item.Value));
            return folderCount switch
            {
                0 => "未设置字体来源目录",
                1 => "1 个字体来源目录",
                _ => $"{folderCount} 个字体来源目录",
            };
        }
    }

    public IndexFileModel ToModel()
    {
        var model = new IndexFileModel
        {
            Path = Path.Trim(),
        };

        foreach (EditableStringItemViewModel item in SourceFolders.Where(static item => !string.IsNullOrWhiteSpace(item.Value)))
        {
            model.SourceFolders.Add(item.Value.Trim());
        }

        return model;
    }

    public void NotifyChildrenChanged()
    {
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(Subtitle));
    }

    private void AddSourceFolder()
    {
        SourceFolders.Add(new EditableStringItemViewModel());
        SelectedSourceFolder = SourceFolders.LastOrDefault();
    }

    private void RemoveSelectedSourceFolder()
    {
        RemoveSelectedItem(SourceFolders, SelectedSourceFolder, item => SelectedSourceFolder = item);
    }

    private void OnSourceFoldersChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        DetachItems(e.OldItems);
        AttachItems(e.NewItems);
        RefreshCommands();
        NotifyChildrenChanged();
    }

    private void AttachItems(System.Collections.IList? items)
    {
        if (items is null)
        {
            return;
        }

        foreach (EditableStringItemViewModel item in items.OfType<EditableStringItemViewModel>())
        {
            item.PropertyChanged += OnChildPropertyChanged;
        }
    }

    private void DetachItems(System.Collections.IList? items)
    {
        if (items is null)
        {
            return;
        }

        foreach (EditableStringItemViewModel item in items.OfType<EditableStringItemViewModel>())
        {
            item.PropertyChanged -= OnChildPropertyChanged;
        }
    }

    private void OnChildPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        NotifyChildrenChanged();
    }

    private void RefreshCommands()
    {
        RemoveSourceFolderCommand.RaiseCanExecuteChanged();
    }

    private static void RemoveSelectedItem(
        ObservableCollection<EditableStringItemViewModel> collection,
        EditableStringItemViewModel? selectedItem,
        Action<EditableStringItemViewModel?> setSelection)
    {
        if (selectedItem is null)
        {
            return;
        }

        int index = collection.IndexOf(selectedItem);
        if (index < 0)
        {
            return;
        }

        collection.RemoveAt(index);
        EditableStringItemViewModel? nextItem = collection.ElementAtOrDefault(Math.Min(index, collection.Count - 1));
        setSelection(nextItem);
    }
}
