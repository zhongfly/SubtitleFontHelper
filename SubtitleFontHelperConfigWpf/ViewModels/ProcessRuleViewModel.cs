using System;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Linq;
using SubtitleFontHelperConfigWpf.Infrastructure;
using SubtitleFontHelperConfigWpf.Models;

namespace SubtitleFontHelperConfigWpf.ViewModels;

public sealed class ProcessRuleViewModel : ObservableObject
{
    private bool m_ignoreCase;
    private EditableStringItemViewModel? m_selectedRegexItem;
    private EditableStringItemViewModel? m_selectedProcessItem;

    public ProcessRuleViewModel()
    {
        RegexItems.CollectionChanged += OnRegexCollectionChanged;
        ProcessItems.CollectionChanged += OnProcessCollectionChanged;

        AddRegexCommand = new RelayCommand(AddRegexItem);
        RemoveRegexCommand = new RelayCommand(RemoveSelectedRegexItem, () => SelectedRegexItem is not null);

        AddProcessCommand = new RelayCommand(AddProcessItem);
        RemoveProcessCommand = new RelayCommand(RemoveSelectedProcessItem, () => SelectedProcessItem is not null);
    }

    public ProcessRuleViewModel(ProcessRuleModel model)
        : this()
    {
        foreach (string regex in model.Regex)
        {
            RegexItems.Add(new EditableStringItemViewModel(regex));
        }

        foreach (string process in model.Processes)
        {
            ProcessItems.Add(new EditableStringItemViewModel(process));
        }

        IgnoreCase = model.Flags.Contains('i');
        SelectedRegexItem = null;
        SelectedProcessItem = null;
    }

    public ObservableCollection<EditableStringItemViewModel> RegexItems { get; } = [];

    public ObservableCollection<EditableStringItemViewModel> ProcessItems { get; } = [];

    public RelayCommand AddRegexCommand { get; }

    public RelayCommand RemoveRegexCommand { get; }

    public RelayCommand AddProcessCommand { get; }

    public RelayCommand RemoveProcessCommand { get; }

    public EditableStringItemViewModel? SelectedRegexItem
    {
        get => m_selectedRegexItem;
        set
        {
            if (SetProperty(ref m_selectedRegexItem, value))
            {
                RefreshCommands();
            }
        }
    }

    public EditableStringItemViewModel? SelectedProcessItem
    {
        get => m_selectedProcessItem;
        set
        {
            if (SetProperty(ref m_selectedProcessItem, value))
            {
                RefreshCommands();
            }
        }
    }

    public bool IgnoreCase
    {
        get => m_ignoreCase;
        set
        {
            if (SetProperty(ref m_ignoreCase, value))
            {
                OnPropertyChanged(nameof(Summary));
            }
        }
    }

    public string Summary
    {
        get
        {
            string regexText = RegexItems.Count == 0 ? "未设置正则" : $"{RegexItems.Count} 条正则";
            string processText = ProcessItems.Count == 0 ? "未设置进程" : $"{ProcessItems.Count} 个进程";
            string suffix = IgnoreCase ? "，忽略大小写" : string.Empty;
            return $"{regexText} / {processText}{suffix}";
        }
    }

    public ProcessRuleModel ToModel()
    {
        var model = new ProcessRuleModel
        {
            Flags = IgnoreCase ? "i" : string.Empty,
        };

        foreach (EditableStringItemViewModel item in RegexItems.Where(static item => !string.IsNullOrWhiteSpace(item.Value)))
        {
            model.Regex.Add(item.Value.Trim());
        }

        foreach (EditableStringItemViewModel item in ProcessItems.Where(static item => !string.IsNullOrWhiteSpace(item.Value)))
        {
            model.Processes.Add(item.Value.Trim());
        }

        return model;
    }

    public void NotifyChildrenChanged()
    {
        OnPropertyChanged(nameof(Summary));
    }

    private void AddRegexItem()
    {
        RegexItems.Add(new EditableStringItemViewModel());
        SelectedRegexItem = RegexItems.LastOrDefault();
    }

    private void RemoveSelectedRegexItem()
    {
        RemoveSelectedItem(RegexItems, SelectedRegexItem, item => SelectedRegexItem = item);
    }

    private void AddProcessItem()
    {
        ProcessItems.Add(new EditableStringItemViewModel());
        SelectedProcessItem = ProcessItems.LastOrDefault();
    }

    private void RemoveSelectedProcessItem()
    {
        RemoveSelectedItem(ProcessItems, SelectedProcessItem, item => SelectedProcessItem = item);
    }

    private void OnRegexCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        DetachItems(e.OldItems);
        AttachItems(e.NewItems);
        RefreshCommands();
        NotifyChildrenChanged();
    }

    private void OnProcessCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
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
        RemoveRegexCommand.RaiseCanExecuteChanged();
        RemoveProcessCommand.RaiseCanExecuteChanged();
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
