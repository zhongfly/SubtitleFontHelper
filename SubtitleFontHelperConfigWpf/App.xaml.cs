using System;
using System.IO;
using System.Windows;
using SubtitleFontHelperConfigWpf.Services;
using SubtitleFontHelperConfigWpf.ViewModels;

namespace SubtitleFontHelperConfigWpf;

public partial class App : Application
{
    private const string DefaultConfigFileName = "SubtitleFontHelper.toml";

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        string configPath = ResolveConfigPath(e.Args);
        var dialogService = new DialogService();
        var processPickerService = new ProcessPickerService();
        var configFileService = new ConfigFileService();
        var viewModel = new MainWindowViewModel(configPath, configFileService, dialogService, processPickerService);
        var window = new MainWindow(viewModel);

        MainWindow = window;
        window.Show();
    }

    private static string ResolveConfigPath(string[] args)
    {
        if (args.Length > 0 && !string.IsNullOrWhiteSpace(args[0]))
        {
            return Path.GetFullPath(args[0]);
        }

        string baseDirectory = AppDomain.CurrentDomain.BaseDirectory;
        return Path.GetFullPath(Path.Combine(baseDirectory, DefaultConfigFileName));
    }
}
