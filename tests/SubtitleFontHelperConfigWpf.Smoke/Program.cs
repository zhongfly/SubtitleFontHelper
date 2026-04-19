using System;
using System.IO;
using System.Linq;
using System.Text;
using SubtitleFontHelperConfigWpf.Models;
using SubtitleFontHelperConfigWpf.Services;

const int ExpectedMonitorCount = 2;
const int ExpectedIndexCount = 1;

var service = new ConfigFileService();
string repositoryRoot = Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "..", "..", "..", "..", ".."));
string tempRoot = Path.Combine(repositoryRoot, "tests", "SubtitleFontHelperConfigWpf.Smoke", "artifacts");
Directory.CreateDirectory(tempRoot);

Run("load example config", () =>
{
    string examplePath = Path.Combine(repositoryRoot, "SubtitleFontHelper.example.toml");
    ConfigModel model = service.Load(examplePath);
    Expect(model.WmiPollInterval == 1000, "wmi_poll_interval should be 1000");
    Expect(model.LruSize == 100, "lru_size should be 100");
    Expect(
        model.MonitorProcesses.Count == ExpectedMonitorCount,
        $"monitor_processes count mismatch: {model.MonitorProcesses.Count}; values={string.Join("|", model.MonitorProcesses)}");
    Expect(model.IndexFiles.Count == ExpectedIndexCount, "index_files count mismatch");
});

Run("round-trip save and reload", () =>
{
    string path = Path.Combine(tempRoot, "roundtrip.toml");
    var model = new ConfigModel
    {
        WmiPollInterval = 1200,
        LruSize = 321,
        DataPath = "%LOCALAPPDATA%/SubtitleFontHelper",
    };

    model.MonitorProcesses.Add("mpc-hc64_nvo.exe");
    model.Notifications.ManagedIndexNotifications = true;
    model.Notifications.ManagedIndexFailureNotifications = false;
    model.Notifications.MissingFontNotifications = true;
    model.Notifications.MissingFontIgnore.Add("i:.*arial.*");

    var rule = new ProcessRuleModel
    {
        Flags = "i",
    };
    rule.Regex.Add("Some Missing Font");
    rule.Regex.Add("[A-Z0-9]{8}");
    rule.Processes.Add("mpc-hc64_nvo.exe");
    rule.Processes.Add("mpc-hc_nvo.exe");
    model.ProcessMissingFontIgnore.Add(rule);

    var indexFile = new IndexFileModel
    {
        Path = "indexes/FontIndex.xml",
    };
    indexFile.SourceFolders.Add("fonts");
    indexFile.SourceFolders.Add("%USERPROFILE%/Downloads/fonts");
    model.IndexFiles.Add(indexFile);

    service.Save(path, model);
    ConfigModel reloaded = service.Load(path);

    Expect(reloaded.WmiPollInterval == 1200, "round-trip wmi_poll_interval mismatch");
    Expect(reloaded.LruSize == 321, "round-trip lru_size mismatch");
    Expect(reloaded.DataPath == "%LOCALAPPDATA%/SubtitleFontHelper", "round-trip data_path mismatch");
    Expect(reloaded.Notifications.ManagedIndexNotifications, "managed_index_notifications should be true");
    Expect(!reloaded.Notifications.ManagedIndexFailureNotifications, "managed_index_failure_notifications should be false");
    Expect(reloaded.Notifications.MissingFontNotifications, "missing_font_notifications should be true");
    Expect(reloaded.Notifications.MissingFontIgnore.Single() == "i:.*arial.*", "missing_font_ignore mismatch");
    Expect(reloaded.ProcessMissingFontIgnore.Count == 1, "process_missing_font_ignore count mismatch");
    Expect(reloaded.ProcessMissingFontIgnore[0].Regex.Count == 2, "regex count mismatch");
    Expect(reloaded.IndexFiles[0].SourceFolders.Count == 2, "source_folders count mismatch");
});

Run("legacy key compatibility", () =>
{
    string path = Path.Combine(tempRoot, "legacy.toml");
    File.WriteAllText(path, """
wmi_poll_interval = 500
lru_size = 50
monitor_processes = ['mpc-hc64_nvo.exe']

[notifications]
missing_font_notification_ignore_queries = ['Legacy Font']

[[notifications.process_missing_font_ignore]]
query_regex = 'Legacy.*'
processes = ['mpc-hc64_nvo.exe']
""", new UTF8Encoding(false));

    ConfigModel model = service.Load(path);
    Expect(model.Notifications.MissingFontIgnore.Single() == "Legacy Font", "legacy missing_font_ignore key should load");
    Expect(model.ProcessMissingFontIgnore.Single().Regex.Single() == "Legacy.*", "query_regex should load as regex");
});

Run("literal string keeps backslashes", () =>
{
    string path = Path.Combine(tempRoot, "literal-string.toml");
    File.WriteAllText(path, """
wmi_poll_interval = 500
lru_size = 100
data_path = 'C:\Temp\SubtitleFontHelper'
monitor_processes = ['C:\Players\mpc-hc64_nvo.exe']

[[index_files]]
path = 'D:\Fonts\FontIndex.xml'
source_folders = [
    'D:\Fonts\Library',
]
""", new UTF8Encoding(false));

    ConfigModel model = service.Load(path);
    Expect(model.DataPath == @"C:\Temp\SubtitleFontHelper", "literal data_path should keep backslashes");
    Expect(model.MonitorProcesses.Single() == @"C:\Players\mpc-hc64_nvo.exe", "literal monitor_processes should keep backslashes");
    Expect(model.IndexFiles.Single().Path == @"D:\Fonts\FontIndex.xml", "literal index path should keep backslashes");
    Expect(model.IndexFiles.Single().SourceFolders.Single() == @"D:\Fonts\Library", "literal source_folders should keep backslashes");
});

Run("rundll32 validation", () =>
{
    var model = new ConfigModel
    {
        WmiPollInterval = 500,
        LruSize = 100,
    };

    model.MonitorProcesses.Add("rundll32.exe");
    model.IndexFiles.Add(new IndexFileModel { Path = "indexes/FontIndex.xml" });

    try
    {
        service.Validate(model);
        throw new Exception("expected validation failure");
    }
    catch (InvalidOperationException ex)
    {
        Expect(ex.Message.IndexOf("rundll32.exe", StringComparison.OrdinalIgnoreCase) >= 0, "validation message should mention rundll32.exe");
    }
});

Console.WriteLine("All smoke tests passed.");

static void Run(string name, Action action)
{
    try
    {
        action();
        Console.WriteLine($"[PASS] {name}");
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"[FAIL] {name}: {ex.Message}");
        Environment.Exit(1);
    }
}

static void Expect(bool condition, string message)
{
    if (!condition)
    {
        throw new Exception(message);
    }
}
