using System;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Linq;
using System.Windows;
using SubtitleFontHelperConfigWpf.Infrastructure;
using SubtitleFontHelperConfigWpf.Models;
using SubtitleFontHelperConfigWpf.Services;

namespace SubtitleFontHelperConfigWpf.ViewModels;

public sealed class MainWindowViewModel : ObservableObject
{
    private readonly string m_configPath;
    private readonly ConfigFileService m_configFileService;
    private readonly DialogService m_dialogService;
    private readonly ProcessPickerService m_processPickerService;

    private Window? m_window;
    private bool m_allowImmediateClose;
    private bool m_isDirty;
    private string m_statusMessage = "准备就绪";
    private string m_dataPath = string.Empty;
    private string m_newMonitorProcessValue = string.Empty;
    private int m_wmiPollInterval = 1000;
    private int m_lruSize = 100;
    private bool m_managedIndexNotifications;
    private bool m_managedIndexFailureNotifications = true;
    private bool m_missingFontNotifications;
    private NavigationItemViewModel? m_selectedNavigationItem;
    private EditableStringItemViewModel? m_selectedMonitorProcess;
    private MissingFontIgnoreItemViewModel? m_selectedMissingFontIgnore;

    public MainWindowViewModel(
        string configPath,
        ConfigFileService configFileService,
        DialogService dialogService,
        ProcessPickerService processPickerService)
    {
        m_configPath = configPath;
        m_configFileService = configFileService;
        m_dialogService = dialogService;
        m_processPickerService = processPickerService;

        SaveCommand = new RelayCommand(Save);
        ReloadCommand = new RelayCommand(Reload);
        CloseCommand = new RelayCommand(CloseWindow);
        BrowseDataPathCommand = new RelayCommand(BrowseDataPath);

        CommitMonitorProcessDraftCommand = new RelayCommand(CommitMonitorProcessDraft, CanCommitMonitorProcessDraft);
        RemoveMonitorProcessCommand = new RelayCommand(RemoveSelectedMonitorProcess, () => SelectedMonitorProcess is not null);
        BrowseMonitorProcessCommand = new RelayCommand(BrowseMonitorProcess);
        PickMonitorProcessCommand = new RelayCommand(PickMonitorProcess);

        AddMissingFontIgnoreCommand = new RelayCommand(AddMissingFontIgnore);
        RemoveMissingFontIgnoreCommand = new RelayCommand(RemoveSelectedMissingFontIgnore, () => SelectedMissingFontIgnore is not null);

        AddProcessRuleCommand = new RelayCommand(AddProcessRule);
        RemoveProcessRuleCardCommand = new RelayCommand(RemoveProcessRuleCard, CanRemoveProcessRuleCard);
        BrowseRuleProcessForCardCommand = new RelayCommand(BrowseRuleProcessForCard);
        PickRuleProcessForCardCommand = new RelayCommand(PickRuleProcessForCard);

        AddIndexFileCommand = new RelayCommand(AddIndexFile);
        RemoveIndexFileCardCommand = new RelayCommand(RemoveIndexFileCard, CanRemoveIndexFileCard);
        BrowseIndexPathForCardCommand = new RelayCommand(BrowseIndexPathForCard);
        BrowseSourceFolderForCardCommand = new RelayCommand(BrowseSourceFolderForCard);

        NavigationItems.Add(new NavigationItemViewModel("general", "常规", "轮询、缓存与运行数据保存目录"));
        NavigationItems.Add(new NavigationItemViewModel("monitor", "监视进程", "管理需要监视的播放器和目标进程"));
        NavigationItems.Add(new NavigationItemViewModel("notifications", "通知", "系统通知与忽略规则"));
        NavigationItems.Add(new NavigationItemViewModel("rules", "按进程规则", "按进程设置字体缺失忽略通知"));
        NavigationItems.Add(new NavigationItemViewModel("index", "索引文件", "管理字体索引文件与来源目录"));
        SelectedNavigationItem = NavigationItems[0];

        HookCollections();
        ReloadFromDisk(showSuccessMessage: false);
    }

    public event EventHandler? RequestClose;

    public ObservableCollection<NavigationItemViewModel> NavigationItems { get; } = [];

    public ObservableCollection<EditableStringItemViewModel> MonitorProcesses { get; } = [];

    public ObservableCollection<MissingFontIgnoreItemViewModel> MissingFontIgnoreItems { get; } = [];

    public ObservableCollection<ProcessRuleViewModel> ProcessRules { get; } = [];

    public ObservableCollection<IndexFileViewModel> IndexFiles { get; } = [];

    public RelayCommand SaveCommand { get; }
    public RelayCommand ReloadCommand { get; }
    public RelayCommand CloseCommand { get; }
    public RelayCommand BrowseDataPathCommand { get; }
    public RelayCommand CommitMonitorProcessDraftCommand { get; }
    public RelayCommand RemoveMonitorProcessCommand { get; }
    public RelayCommand BrowseMonitorProcessCommand { get; }
    public RelayCommand PickMonitorProcessCommand { get; }
    public RelayCommand AddMissingFontIgnoreCommand { get; }
    public RelayCommand RemoveMissingFontIgnoreCommand { get; }
    public RelayCommand AddProcessRuleCommand { get; }
    public RelayCommand RemoveProcessRuleCardCommand { get; }
    public RelayCommand BrowseRuleProcessForCardCommand { get; }
    public RelayCommand PickRuleProcessForCardCommand { get; }
    public RelayCommand AddIndexFileCommand { get; }
    public RelayCommand RemoveIndexFileCardCommand { get; }
    public RelayCommand BrowseIndexPathForCardCommand { get; }
    public RelayCommand BrowseSourceFolderForCardCommand { get; }

    public string ConfigPath => m_configPath;

    public string WindowTitle => $"SubtitleFontHelper Config Editor{(IsDirty ? " *" : string.Empty)}";

    public bool IsDirty
    {
        get => m_isDirty;
        private set
        {
            if (SetProperty(ref m_isDirty, value))
            {
                OnPropertyChanged(nameof(WindowTitle));
            }
        }
    }

    public string StatusMessage
    {
        get => m_statusMessage;
        private set => SetProperty(ref m_statusMessage, value);
    }

    public int WmiPollInterval
    {
        get => m_wmiPollInterval;
        set
        {
            if (SetProperty(ref m_wmiPollInterval, value))
            {
                MarkDirty("已修改 WMI 轮询间隔");
            }
        }
    }

    public int LruSize
    {
        get => m_lruSize;
        set
        {
            if (SetProperty(ref m_lruSize, value))
            {
                MarkDirty("已修改 LRU 容量");
            }
        }
    }

    public string DataPath
    {
        get => m_dataPath;
        set
        {
            if (SetProperty(ref m_dataPath, value))
            {
                MarkDirty("已修改运行数据保存目录");
            }
        }
    }

    public string NewMonitorProcessValue
    {
        get => m_newMonitorProcessValue;
        set
        {
            if (SetProperty(ref m_newMonitorProcessValue, value))
            {
                CommitMonitorProcessDraftCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public bool ManagedIndexNotifications
    {
        get => m_managedIndexNotifications;
        set
        {
            if (SetProperty(ref m_managedIndexNotifications, value))
            {
                MarkDirty("已修改索引通知开关");
            }
        }
    }

    public bool ManagedIndexFailureNotifications
    {
        get => m_managedIndexFailureNotifications;
        set
        {
            if (SetProperty(ref m_managedIndexFailureNotifications, value))
            {
                MarkDirty("已修改索引失败通知开关");
            }
        }
    }

    public bool MissingFontNotifications
    {
        get => m_missingFontNotifications;
        set
        {
            if (SetProperty(ref m_missingFontNotifications, value))
            {
                MarkDirty("已修改字体缺失通知开关");
            }
        }
    }

    public NavigationItemViewModel? SelectedNavigationItem
    {
        get => m_selectedNavigationItem;
        set
        {
            if (m_selectedNavigationItem == value)
            {
                return;
            }

            if (m_selectedNavigationItem is not null)
            {
                m_selectedNavigationItem.IsSelected = false;
            }

            if (SetProperty(ref m_selectedNavigationItem, value) && value is not null)
            {
                value.IsSelected = true;
                OnPropertyChanged(nameof(CurrentPageKey));
            }
        }
    }

    public string CurrentPageKey => SelectedNavigationItem?.Key ?? "general";

    public EditableStringItemViewModel? SelectedMonitorProcess
    {
        get => m_selectedMonitorProcess;
        set
        {
            if (SetProperty(ref m_selectedMonitorProcess, value))
            {
                RefreshCommands();
            }
        }
    }

    public MissingFontIgnoreItemViewModel? SelectedMissingFontIgnore
    {
        get => m_selectedMissingFontIgnore;
        set
        {
            if (SetProperty(ref m_selectedMissingFontIgnore, value))
            {
                RefreshCommands();
            }
        }
    }

    public void AttachWindow(Window window)
    {
        m_window = window;
    }

    public void ClearEditableSelections()
    {
        SelectedMonitorProcess = null;
        SelectedMissingFontIgnore = null;

        foreach (ProcessRuleViewModel rule in ProcessRules)
        {
            rule.SelectedRegexItem = null;
            rule.SelectedProcessItem = null;
        }

        foreach (IndexFileViewModel indexFile in IndexFiles)
        {
            indexFile.SelectedSourceFolder = null;
        }
    }

    public bool ConfirmClose()
    {
        if (m_allowImmediateClose)
        {
            m_allowImmediateClose = false;
            return true;
        }

        if (!IsDirty)
        {
            return true;
        }

        if (m_window is null)
        {
            return false;
        }

        DirtyCloseAction action = m_dialogService.ConfirmCloseWithUnsavedChanges(m_window);
        return action switch
        {
            DirtyCloseAction.SaveAndClose => SaveAndConfirmClose(),
            DirtyCloseAction.CloseWithoutSaving => true,
            _ => false,
        };
    }

    public ConfigModel BuildModel()
    {
        var model = new ConfigModel
        {
            WmiPollInterval = WmiPollInterval,
            LruSize = LruSize,
            DataPath = DataPath.Trim(),
        };

        model.Notifications.ManagedIndexNotifications = ManagedIndexNotifications;
        model.Notifications.ManagedIndexFailureNotifications = ManagedIndexFailureNotifications;
        model.Notifications.MissingFontNotifications = MissingFontNotifications;

        foreach (EditableStringItemViewModel item in MonitorProcesses.Where(static item => !string.IsNullOrWhiteSpace(item.Value)))
        {
            model.MonitorProcesses.Add(item.Value.Trim());
        }

        foreach (MissingFontIgnoreItemViewModel item in MissingFontIgnoreItems.Where(static item => !string.IsNullOrWhiteSpace(item.Pattern)))
        {
            model.Notifications.MissingFontIgnore.Add(item.ToConfigValue());
        }

        foreach (ProcessRuleViewModel rule in ProcessRules)
        {
            model.ProcessMissingFontIgnore.Add(rule.ToModel());
        }

        foreach (IndexFileViewModel indexFile in IndexFiles)
        {
            model.IndexFiles.Add(indexFile.ToModel());
        }

        return model;
    }

    private void HookCollections()
    {
        MonitorProcesses.CollectionChanged += OnFlatCollectionChanged;
        MissingFontIgnoreItems.CollectionChanged += OnMissingFontIgnoreChanged;
        ProcessRules.CollectionChanged += OnProcessRulesChanged;
        IndexFiles.CollectionChanged += OnIndexFilesChanged;
    }

    private void OnFlatCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        DetachFlatItems(e.OldItems);
        AttachFlatItems(e.NewItems);
        MarkDirty("已更新列表");
        RefreshCommands();
    }

    private void OnMissingFontIgnoreChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        DetachMissingFontIgnoreItems(e.OldItems);
        AttachMissingFontIgnoreItems(e.NewItems);
        MarkDirty("已更新忽略规则");
        RefreshCommands();
    }

    private void OnProcessRulesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null)
        {
            foreach (ProcessRuleViewModel item in e.OldItems.OfType<ProcessRuleViewModel>())
            {
                item.PropertyChanged -= OnRulePropertyChanged;
            }
        }

        if (e.NewItems is not null)
        {
            foreach (ProcessRuleViewModel item in e.NewItems.OfType<ProcessRuleViewModel>())
            {
                item.PropertyChanged += OnRulePropertyChanged;
            }
        }

        MarkDirty("已更新按进程规则");
        RefreshCommands();
    }

    private void OnIndexFilesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null)
        {
            foreach (IndexFileViewModel item in e.OldItems.OfType<IndexFileViewModel>())
            {
                item.PropertyChanged -= OnIndexFilePropertyChanged;
            }
        }

        if (e.NewItems is not null)
        {
            foreach (IndexFileViewModel item in e.NewItems.OfType<IndexFileViewModel>())
            {
                item.PropertyChanged += OnIndexFilePropertyChanged;
            }
        }

        MarkDirty("已更新索引文件");
        RefreshCommands();
    }

    private void AttachFlatItems(System.Collections.IList? items)
    {
        if (items is null)
        {
            return;
        }

        foreach (EditableStringItemViewModel item in items.OfType<EditableStringItemViewModel>())
        {
            item.PropertyChanged += OnEditableStringPropertyChanged;
        }
    }

    private void DetachFlatItems(System.Collections.IList? items)
    {
        if (items is null)
        {
            return;
        }

        foreach (EditableStringItemViewModel item in items.OfType<EditableStringItemViewModel>())
        {
            item.PropertyChanged -= OnEditableStringPropertyChanged;
        }
    }

    private void OnEditableStringPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        MarkDirty("已修改文本项");
        RefreshCommands();
    }

    private void AttachMissingFontIgnoreItems(System.Collections.IList? items)
    {
        if (items is null)
        {
            return;
        }

        foreach (MissingFontIgnoreItemViewModel item in items.OfType<MissingFontIgnoreItemViewModel>())
        {
            item.PropertyChanged += OnMissingFontIgnoreItemPropertyChanged;
        }
    }

    private void DetachMissingFontIgnoreItems(System.Collections.IList? items)
    {
        if (items is null)
        {
            return;
        }

        foreach (MissingFontIgnoreItemViewModel item in items.OfType<MissingFontIgnoreItemViewModel>())
        {
            item.PropertyChanged -= OnMissingFontIgnoreItemPropertyChanged;
        }
    }

    private void OnMissingFontIgnoreItemPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        MarkDirty("已修改忽略规则");
        RefreshCommands();
    }

    private void OnRulePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        MarkDirty("已修改按进程规则");
        RefreshCommands();
    }

    private void OnIndexFilePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        MarkDirty("已修改索引文件");
        RefreshCommands();
    }

    private void MarkDirty(string message)
    {
        IsDirty = true;
        StatusMessage = message;
    }

    private void RefreshCommands()
    {
        RemoveMonitorProcessCommand.RaiseCanExecuteChanged();
        CommitMonitorProcessDraftCommand.RaiseCanExecuteChanged();
        RemoveMissingFontIgnoreCommand.RaiseCanExecuteChanged();
        RemoveProcessRuleCardCommand.RaiseCanExecuteChanged();
        BrowseRuleProcessForCardCommand.RaiseCanExecuteChanged();
        PickRuleProcessForCardCommand.RaiseCanExecuteChanged();
        RemoveIndexFileCardCommand.RaiseCanExecuteChanged();
        BrowseIndexPathForCardCommand.RaiseCanExecuteChanged();
        BrowseSourceFolderForCardCommand.RaiseCanExecuteChanged();
    }

    private void Reload()
    {
        if (IsDirty && m_window is not null && !m_dialogService.ConfirmDiscard(m_window, "重新加载配置"))
        {
            return;
        }

        ReloadFromDisk(showSuccessMessage: true);
    }

    private void ReloadFromDisk(bool showSuccessMessage)
    {
        try
        {
            ConfigModel model = m_configFileService.Load(m_configPath);
            ApplyModel(model);
            IsDirty = false;
            StatusMessage = showSuccessMessage ? "已从磁盘重新加载配置" : "配置已加载";
        }
        catch (Exception ex)
        {
            if (m_window is not null)
            {
                m_dialogService.ShowWarning(m_window, $"无法读取配置文件：\n{m_configPath}\n\n{ex.Message}\n\n将以空配置打开。");
            }

            ApplyModel(new ConfigModel());
            IsDirty = false;
            StatusMessage = "读取失败，已切换为空配置";
        }
    }

    private void ApplyModel(ConfigModel model)
    {
        WmiPollInterval = model.WmiPollInterval;
        LruSize = model.LruSize;
        DataPath = model.DataPath;
        ManagedIndexNotifications = model.Notifications.ManagedIndexNotifications;
        ManagedIndexFailureNotifications = model.Notifications.ManagedIndexFailureNotifications;
        MissingFontNotifications = model.Notifications.MissingFontNotifications;

        ReplaceFlatItems(MonitorProcesses, model.MonitorProcesses);
        ReplaceMissingFontIgnoreItems(model.Notifications.MissingFontIgnore);

        ProcessRules.Clear();
        foreach (ProcessRuleModel rule in model.ProcessMissingFontIgnore)
        {
            ProcessRules.Add(new ProcessRuleViewModel(rule));
        }

        IndexFiles.Clear();
        foreach (IndexFileModel indexFile in model.IndexFiles)
        {
            IndexFiles.Add(new IndexFileViewModel(indexFile));
        }

        NewMonitorProcessValue = string.Empty;
        SelectedMonitorProcess = null;
        SelectedMissingFontIgnore = MissingFontIgnoreItems.FirstOrDefault();
        IsDirty = false;
    }

    private static void ReplaceFlatItems(ObservableCollection<EditableStringItemViewModel> target, ObservableCollection<string> source)
    {
        target.Clear();
        foreach (string value in source)
        {
            target.Add(new EditableStringItemViewModel(value));
        }
    }

    private void ReplaceMissingFontIgnoreItems(ObservableCollection<string> source)
    {
        MissingFontIgnoreItems.Clear();
        foreach (string value in source)
        {
            MissingFontIgnoreItems.Add(MissingFontIgnoreItemViewModel.FromConfigValue(value));
        }
    }

    private void Save()
    {
        try
        {
            SaveCore();
        }
        catch (Exception ex)
        {
            if (m_window is not null)
            {
                m_dialogService.ShowError(m_window, ex.Message);
            }
        }
    }

    private bool SaveAndConfirmClose()
    {
        try
        {
            SaveCore();
            return true;
        }
        catch (Exception ex)
        {
            if (m_window is not null)
            {
                m_dialogService.ShowError(m_window, ex.Message);
            }

            return false;
        }
    }

    private void SaveCore()
    {
        ConfigModel model = BuildModel();
        m_configFileService.Save(m_configPath, model);
        IsDirty = false;
        StatusMessage = "配置已保存";
    }

    private void CloseWindow()
    {
        if (ConfirmClose())
        {
            m_allowImmediateClose = true;
            RequestClose?.Invoke(this, EventArgs.Empty);
        }
    }

    private void BrowseDataPath()
    {
        if (m_window is null)
        {
            return;
        }

        string? folder = m_dialogService.PickFolder(m_window, DataPath);
        if (!string.IsNullOrWhiteSpace(folder))
        {
            DataPath = folder;
        }
    }

    private void CommitMonitorProcessDraft()
    {
        try
        {
            string value = ConfigFileService.NormalizeExecutableName(NewMonitorProcessValue);
            ConfigFileService.EnsureMonitorProcessAllowed(value);
            MonitorProcesses.Add(new EditableStringItemViewModel(value));
            NewMonitorProcessValue = string.Empty;
        }
        catch (Exception ex)
        {
            if (m_window is not null)
            {
                m_dialogService.ShowWarning(m_window, ex.Message);
            }
        }
    }

    private bool CanCommitMonitorProcessDraft()
    {
        return !string.IsNullOrWhiteSpace(NewMonitorProcessValue);
    }

    private void RemoveSelectedMonitorProcess()
    {
        RemoveSelectedItem(MonitorProcesses, SelectedMonitorProcess, item => SelectedMonitorProcess = item);
    }

    private void BrowseMonitorProcess()
    {
        if (m_window is null)
        {
            return;
        }

        string? fileName = m_dialogService.PickExecutable(m_window);
        if (string.IsNullOrWhiteSpace(fileName))
        {
            return;
        }

        TryAssignProcessToItem(fileName, true);
    }

    private void PickMonitorProcess()
    {
        if (m_window is null)
        {
            return;
        }

        string? processName = TryPickProcessName();
        if (string.IsNullOrWhiteSpace(processName))
        {
            return;
        }

        TryAssignProcessToItem(processName, true);
    }

    private void AddMissingFontIgnore()
    {
        MissingFontIgnoreItems.Add(new MissingFontIgnoreItemViewModel());
        SelectedMissingFontIgnore = MissingFontIgnoreItems.Last();
    }

    private void RemoveSelectedMissingFontIgnore()
    {
        RemoveSelectedItem(MissingFontIgnoreItems, SelectedMissingFontIgnore, item => SelectedMissingFontIgnore = item);
    }

    private void AddProcessRule()
    {
        var rule = new ProcessRuleViewModel();
        ProcessRules.Add(rule);
    }

    private void RemoveProcessRuleCard(object? parameter)
    {
        if (parameter is not ProcessRuleViewModel rule)
        {
            return;
        }

        RemoveItem(ProcessRules, rule);
    }

    private bool CanRemoveProcessRuleCard(object? parameter)
    {
        return parameter is ProcessRuleViewModel rule && ProcessRules.Contains(rule);
    }

    private void BrowseRuleProcessForCard(object? parameter)
    {
        if (parameter is not ProcessRuleViewModel rule || m_window is null)
        {
            return;
        }

        string? fileName = m_dialogService.PickExecutable(m_window);
        if (!string.IsNullOrWhiteSpace(fileName))
        {
            TryAssignProcessToRule(rule, fileName);
        }
    }

    private void PickRuleProcessForCard(object? parameter)
    {
        if (parameter is not ProcessRuleViewModel rule || m_window is null)
        {
            return;
        }

        string? processName = TryPickProcessName();
        if (!string.IsNullOrWhiteSpace(processName))
        {
            TryAssignProcessToRule(rule, processName);
        }
    }

    private void AddIndexFile()
    {
        var item = new IndexFileViewModel();
        IndexFiles.Add(item);
    }

    private void RemoveIndexFileCard(object? parameter)
    {
        if (parameter is not IndexFileViewModel indexFile)
        {
            return;
        }

        RemoveItem(IndexFiles, indexFile);
    }

    private bool CanRemoveIndexFileCard(object? parameter)
    {
        return parameter is IndexFileViewModel indexFile && IndexFiles.Contains(indexFile);
    }

    private void BrowseIndexPathForCard(object? parameter)
    {
        if (parameter is not IndexFileViewModel indexFile || m_window is null)
        {
            return;
        }

        string? path = m_dialogService.PickIndexPath(m_window, indexFile.Path);
        if (!string.IsNullOrWhiteSpace(path))
        {
            indexFile.Path = path;
        }
    }

    private void BrowseSourceFolderForCard(object? parameter)
    {
        if (parameter is not IndexFileViewModel indexFile || m_window is null)
        {
            return;
        }

        string initialPath = indexFile.SelectedSourceFolder?.Value ?? string.Empty;
        string? folder = m_dialogService.PickFolder(m_window, initialPath);
        if (string.IsNullOrWhiteSpace(folder))
        {
            return;
        }

        if (indexFile.SelectedSourceFolder is null)
        {
            indexFile.SourceFolders.Add(new EditableStringItemViewModel(folder));
        }
        else
        {
            indexFile.SelectedSourceFolder.Value = folder;
        }
    }

    private void TryAssignProcessToItem(string candidate, bool createWhenMissing)
    {
        try
        {
            string normalized = ConfigFileService.NormalizeExecutableName(candidate);
            ConfigFileService.EnsureMonitorProcessAllowed(normalized);

            if (SelectedMonitorProcess is null && createWhenMissing)
            {
                MonitorProcesses.Add(new EditableStringItemViewModel(normalized));
            }
            else if (SelectedMonitorProcess is not null)
            {
                SelectedMonitorProcess.Value = normalized;
            }
        }
        catch (Exception ex)
        {
            if (m_window is not null)
            {
                m_dialogService.ShowWarning(m_window, ex.Message);
            }
        }
    }

    private void TryAssignProcessToRule(ProcessRuleViewModel rule, string candidate)
    {
        try
        {
            string normalized = ConfigFileService.NormalizeExecutableName(candidate);
            ConfigFileService.EnsureMonitorProcessAllowed(normalized);

            if (rule.SelectedProcessItem is null)
            {
                rule.ProcessItems.Add(new EditableStringItemViewModel(normalized));
            }
            else
            {
                rule.SelectedProcessItem.Value = normalized;
            }
        }
        catch (Exception ex)
        {
            if (m_window is not null)
            {
                m_dialogService.ShowWarning(m_window, ex.Message);
            }
        }
    }

    private string? TryPickProcessName()
    {
        if (m_window is null)
        {
            return null;
        }

        try
        {
            return m_processPickerService.PickProcessExecutableName(m_window);
        }
        catch (Exception ex)
        {
            m_dialogService.ShowWarning(m_window, $"从窗口拾取失败：{ex.Message}");
            return null;
        }
    }

    private void RemoveSelectedItem<T>(ObservableCollection<T> collection, T? selectedItem, Action<T?> setSelection) where T : class
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
        T? nextItem = collection.ElementAtOrDefault(Math.Min(index, collection.Count - 1));
        setSelection(nextItem);
        MarkDirty("已删除条目");
        RefreshCommands();
    }

    private void RemoveItem<T>(ObservableCollection<T> collection, T item) where T : class
    {
        int index = collection.IndexOf(item);
        if (index < 0)
        {
            return;
        }

        collection.RemoveAt(index);
        MarkDirty("已删除条目");
        RefreshCommands();
    }
}
