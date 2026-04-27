using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using SubtitleFontHelperConfigWpf.Models;

namespace SubtitleFontHelperConfigWpf.Services;

public sealed class ConfigFileService
{
    private const string LegacyMissingFontIgnoreKey = "missing_font_notification_ignore_queries";
    private const string CurrentMissingFontIgnoreKey = "missing_font_ignore";
    private const int MinimumWmiPollInterval = 50;
    private const int MaximumWmiPollInterval = 600000;
    private const int MinimumLruSize = 0;
    private const int MaximumLruSize = 1000000;
    private const string ArrayIndent = "\t";

    public ConfigModel Load(string path)
    {
        if (!File.Exists(path))
        {
            return new ConfigModel();
        }

        string content = File.ReadAllText(path, new UTF8Encoding(false, true));
        var parser = new SimpleTomlParser(content);
        TomlDocument document = parser.Parse();

        var model = new ConfigModel();
        ApplyRoot(document.Root, model);

        if (document.NotificationsTable is not null)
        {
            ApplyNotifications(document.NotificationsTable, model);
        }

        foreach (Dictionary<string, object?> ruleTable in document.ProcessRuleTables)
        {
            model.ProcessMissingFontIgnore.Add(ReadRule(ruleTable));
        }

        foreach (Dictionary<string, object?> indexTable in document.IndexFileTables)
        {
            model.IndexFiles.Add(ReadIndexFile(indexTable));
        }

        return model;
    }

    public void Save(string path, ConfigModel model)
    {
        Validate(model);

        string directory = Path.GetDirectoryName(path) ?? string.Empty;
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var builder = new StringBuilder();
        builder.AppendLine($"wmi_poll_interval = {model.WmiPollInterval.ToString(CultureInfo.InvariantCulture)}");
        builder.AppendLine($"lru_size = {model.LruSize.ToString(CultureInfo.InvariantCulture)}");
        if (!string.IsNullOrWhiteSpace(model.DataPath))
        {
            builder.AppendLine($"data_path = {FormatString(model.DataPath.Trim())}");
        }

        builder.AppendLine($"monitor_processes = {FormatArray(model.MonitorProcesses)}");
        builder.AppendLine();
        builder.AppendLine("[notifications]");
        builder.AppendLine($"managed_index_notifications = {FormatBool(model.Notifications.ManagedIndexNotifications)}");
        builder.AppendLine($"managed_index_failure_notifications = {FormatBool(model.Notifications.ManagedIndexFailureNotifications)}");
        builder.AppendLine($"missing_font_notifications = {FormatBool(model.Notifications.MissingFontNotifications)}");
        builder.AppendLine($"{CurrentMissingFontIgnoreKey} = {FormatArray(model.Notifications.MissingFontIgnore)}");

        foreach (ProcessRuleModel rule in model.ProcessMissingFontIgnore)
        {
            builder.AppendLine();
            builder.AppendLine("[[notifications.process_missing_font_ignore]]");
            builder.AppendLine($"regex = {FormatStringOrArray(rule.Regex)}");
            builder.AppendLine($"processes = {FormatArray(rule.Processes)}");
            if (!string.IsNullOrWhiteSpace(rule.Flags))
            {
                builder.AppendLine($"flags = {FormatString(rule.Flags)}");
            }
        }

        foreach (IndexFileModel indexFile in model.IndexFiles)
        {
            builder.AppendLine();
            builder.AppendLine("[[index_files]]");
            builder.AppendLine($"path = {FormatString(indexFile.Path)}");
            builder.AppendLine($"source_folders = {FormatArray(indexFile.SourceFolders)}");
        }

        File.WriteAllText(path, builder.ToString(), new UTF8Encoding(false));
    }

    public void Validate(ConfigModel model)
    {
        if (model.WmiPollInterval < MinimumWmiPollInterval || model.WmiPollInterval > MaximumWmiPollInterval)
        {
            throw new InvalidOperationException($"wmi_poll_interval 必须在 {MinimumWmiPollInterval} 到 {MaximumWmiPollInterval} 之间。");
        }

        if (model.LruSize < MinimumLruSize || model.LruSize > MaximumLruSize)
        {
            throw new InvalidOperationException($"lru_size 必须在 {MinimumLruSize} 到 {MaximumLruSize} 之间。");
        }

        foreach (string process in model.MonitorProcesses)
        {
            if (string.IsNullOrWhiteSpace(process))
            {
                throw new InvalidOperationException("monitor_processes 存在空项。");
            }

            EnsureMonitorProcessAllowed(process);
        }

        foreach (string query in model.Notifications.MissingFontIgnore)
        {
            if (string.IsNullOrWhiteSpace(query))
            {
                throw new InvalidOperationException("missing_font_ignore 存在空项。");
            }
        }

        for (int index = 0; index < model.ProcessMissingFontIgnore.Count; index++)
        {
            ProcessRuleModel rule = model.ProcessMissingFontIgnore[index];
            if (rule.Regex.Count == 0)
            {
                throw new InvalidOperationException($"notifications.process_missing_font_ignore[{index}].regex 不能为空。");
            }

            if (rule.Processes.Count == 0)
            {
                throw new InvalidOperationException($"notifications.process_missing_font_ignore[{index}].processes 不能为空。");
            }

            foreach (string regex in rule.Regex)
            {
                if (string.IsNullOrWhiteSpace(regex))
                {
                    throw new InvalidOperationException($"notifications.process_missing_font_ignore[{index}].regex 存在空项。");
                }
            }

            foreach (string process in rule.Processes)
            {
                if (string.IsNullOrWhiteSpace(process))
                {
                    throw new InvalidOperationException($"notifications.process_missing_font_ignore[{index}].processes 存在空项。");
                }

                EnsureMonitorProcessAllowed(process);
            }

            foreach (char flag in rule.Flags)
            {
                if (flag != 'i')
                {
                    throw new InvalidOperationException($"notifications.process_missing_font_ignore[{index}].flags 只支持 'i'。");
                }
            }
        }

        for (int index = 0; index < model.IndexFiles.Count; index++)
        {
            IndexFileModel item = model.IndexFiles[index];
            if (string.IsNullOrWhiteSpace(item.Path))
            {
                throw new InvalidOperationException($"index_files[{index}].path 不能为空。");
            }

            foreach (string sourceFolder in item.SourceFolders)
            {
                if (string.IsNullOrWhiteSpace(sourceFolder))
                {
                    throw new InvalidOperationException($"index_files[{index}].source_folders 存在空项。");
                }
            }
        }
    }

    public static string NormalizeExecutableName(string value)
    {
        string trimmed = value.Trim().Trim('"');
        if (string.IsNullOrWhiteSpace(trimmed))
        {
            return string.Empty;
        }

        return Path.GetFileName(trimmed);
    }

    public static void EnsureMonitorProcessAllowed(string value)
    {
        string normalized = NormalizeExecutableName(value);
        if (string.Equals(normalized, "rundll32.exe", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("禁止添加 rundll32.exe。");
        }
    }

    private static void ApplyRoot(Dictionary<string, object?> table, ConfigModel model)
    {
        if (TryReadInteger(table, "wmi_poll_interval", out int wmiPollInterval))
        {
            model.WmiPollInterval = wmiPollInterval;
        }

        if (TryReadInteger(table, "lru_size", out int lruSize))
        {
            model.LruSize = lruSize;
        }

        if (TryReadString(table, "data_path", out string? dataPath))
        {
            model.DataPath = dataPath ?? string.Empty;
        }

        if (table.TryGetValue("log_path", out _))
        {
            throw new InvalidOperationException("log_path 已被 data_path 取代。");
        }

        foreach (string process in ReadStringArray(table, "monitor_processes"))
        {
            model.MonitorProcesses.Add(process);
        }
    }

    private static void ApplyNotifications(Dictionary<string, object?> table, ConfigModel model)
    {
        if (TryReadBoolean(table, "managed_index_notifications", out bool managedIndexNotifications))
        {
            model.Notifications.ManagedIndexNotifications = managedIndexNotifications;
        }

        if (TryReadBoolean(table, "managed_index_failure_notifications", out bool managedIndexFailureNotifications))
        {
            model.Notifications.ManagedIndexFailureNotifications = managedIndexFailureNotifications;
        }

        if (TryReadBoolean(table, "missing_font_notifications", out bool missingFontNotifications))
        {
            model.Notifications.MissingFontNotifications = missingFontNotifications;
        }

        foreach (string item in ReadStringArray(table, CurrentMissingFontIgnoreKey))
        {
            model.Notifications.MissingFontIgnore.Add(item);
        }

        foreach (string item in ReadStringArray(table, LegacyMissingFontIgnoreKey))
        {
            model.Notifications.MissingFontIgnore.Add(item);
        }
    }

    private static ProcessRuleModel ReadRule(Dictionary<string, object?> table)
    {
        var model = new ProcessRuleModel();

        foreach (string regex in ReadStringOrStringArray(table, "regex").Concat(ReadStringOrStringArray(table, "query_regex")))
        {
            model.Regex.Add(regex);
        }

        foreach (string process in ReadStringArray(table, "processes"))
        {
            model.Processes.Add(process);
        }

        if (TryReadString(table, "flags", out string? flags))
        {
            model.Flags = flags ?? string.Empty;
        }

        return model;
    }

    private static IndexFileModel ReadIndexFile(Dictionary<string, object?> table)
    {
        var model = new IndexFileModel();

        if (TryReadString(table, "path", out string? path))
        {
            model.Path = path ?? string.Empty;
        }

        foreach (string sourceFolder in ReadStringArray(table, "source_folders"))
        {
            model.SourceFolders.Add(sourceFolder);
        }

        return model;
    }

    private static string FormatBool(bool value)
    {
        return value ? "true" : "false";
    }

    private static string FormatString(string value)
    {
        return $"\"{EscapeBasicString(value)}\"";
    }

    private static string FormatArray(IEnumerable<string> values)
    {
        IReadOnlyList<string> items = values.ToList();
        if (items.Count == 0)
        {
            return "[]";
        }

        var builder = new StringBuilder();
        builder.AppendLine("[");
        foreach (string item in items)
        {
            builder.Append(ArrayIndent);
            builder.Append(FormatString(item));
            builder.AppendLine(",");
        }

        builder.Append("]");
        return builder.ToString();
    }

    private static string FormatStringOrArray(IReadOnlyCollection<string> values)
    {
        if (values.Count == 1)
        {
            return FormatString(values.First());
        }

        return FormatArray(values);
    }

    private static string EscapeBasicString(string value)
    {
        var builder = new StringBuilder(value.Length + 8);
        foreach (char ch in value)
        {
            builder.Append(ch switch
            {
                '\\' => "\\\\",
                '"' => "\\\"",
                '\t' => "\\t",
                '\r' => "\\r",
                '\n' => "\\n",
                '\f' => "\\f",
                _ => ch.ToString(),
            });
        }

        return builder.ToString();
    }

    private static bool TryReadInteger(Dictionary<string, object?> table, string key, out int value)
    {
        value = 0;
        return table.TryGetValue(key, out object? rawValue) && rawValue is int intValue && (value = intValue) >= 0;
    }

    private static bool TryReadBoolean(Dictionary<string, object?> table, string key, out bool value)
    {
        value = false;
        return table.TryGetValue(key, out object? rawValue) && rawValue is bool boolValue && (value = boolValue) == boolValue;
    }

    private static bool TryReadString(Dictionary<string, object?> table, string key, out string? value)
    {
        value = null;
        return table.TryGetValue(key, out object? rawValue) && rawValue is string stringValue && (value = stringValue) == stringValue;
    }

    private static IReadOnlyList<string> ReadStringArray(Dictionary<string, object?> table, string key)
    {
        if (!table.TryGetValue(key, out object? rawValue) || rawValue is not List<object?> list)
        {
            return [];
        }

        return list.Select(static item => item as string ?? string.Empty).ToList();
    }

    private static IReadOnlyList<string> ReadStringOrStringArray(Dictionary<string, object?> table, string key)
    {
        if (!table.TryGetValue(key, out object? rawValue))
        {
            return [];
        }

        if (rawValue is string stringValue)
        {
            return [stringValue];
        }

        if (rawValue is List<object?> list)
        {
            return list.Select(static item => item as string ?? string.Empty).ToList();
        }

        return [];
    }

    private sealed class TomlDocument
    {
        public Dictionary<string, object?> Root { get; } = [];

        public Dictionary<string, object?>? NotificationsTable { get; set; }

        public List<Dictionary<string, object?>> ProcessRuleTables { get; } = [];

        public List<Dictionary<string, object?>> IndexFileTables { get; } = [];
    }

    private sealed class SimpleTomlParser(string text)
    {
        private readonly string m_text = text;

        public TomlDocument Parse()
        {
            var document = new TomlDocument();
            Dictionary<string, object?> currentTable = document.Root;

            using var reader = new StringReader(m_text);
            string? line;
            int lineNumber = 0;
            while ((line = reader.ReadLine()) is not null)
            {
                lineNumber++;
                string trimmed = StripComment(line).Trim();
                if (string.IsNullOrWhiteSpace(trimmed))
                {
                    continue;
                }

                if (trimmed.StartsWith("[[", StringComparison.Ordinal) && trimmed.EndsWith("]]", StringComparison.Ordinal))
                {
                    string tableName = trimmed.Substring(2, trimmed.Length - 4).Trim();
                    currentTable = tableName switch
                    {
                        "notifications.process_missing_font_ignore" => CreateArrayItem(document.ProcessRuleTables),
                        "index_files" => CreateArrayItem(document.IndexFileTables),
                        _ => throw CreateError(lineNumber, $"不支持的表 行: {tableName}"),
                    };
                    continue;
                }

                if (trimmed.StartsWith("[", StringComparison.Ordinal) && trimmed.EndsWith("]", StringComparison.Ordinal))
                {
                    string tableName = trimmed.Substring(1, trimmed.Length - 2).Trim();
                    currentTable = tableName switch
                    {
                        "notifications" => document.NotificationsTable ??= [],
                        _ => throw CreateError(lineNumber, $"不支持的表 行: {tableName}"),
                    };
                    continue;
                }

                int separatorIndex = trimmed.IndexOf('=');
                if (separatorIndex <= 0)
                {
                    throw CreateError(lineNumber, "键值对缺少 '='。");
                }

                string key = trimmed.Substring(0, separatorIndex).Trim();
                string valueText = trimmed.Substring(separatorIndex + 1).Trim();
                if (RequiresMoreLines(valueText))
                {
                    valueText = ReadMultilineValue(valueText, reader, ref lineNumber);
                }

                currentTable[key] = ParseValue(valueText, lineNumber);
            }

            return document;
        }

        private static bool RequiresMoreLines(string valueText)
        {
            return valueText.StartsWith("[", StringComparison.Ordinal) && !HasClosedArray(valueText);
        }

        private static string ReadMultilineValue(string firstLine, StringReader reader, ref int lineNumber)
        {
            var builder = new StringBuilder(firstLine);
            while (!HasClosedArray(builder.ToString()))
            {
                string? nextLine = reader.ReadLine();
                if (nextLine is null)
                {
                    throw CreateError(lineNumber, "数组未正确结束。");
                }

                lineNumber++;
                string stripped = StripComment(nextLine).Trim();
                if (string.IsNullOrWhiteSpace(stripped))
                {
                    continue;
                }

                builder.Append(' ');
                builder.Append(stripped);
            }

            return builder.ToString();
        }

        private static bool HasClosedArray(string text)
        {
            int depth = 0;
            bool inString = false;
            char quote = '\0';

            for (int index = 0; index < text.Length; index++)
            {
                char ch = text[index];
                if (inString)
                {
                    if (ch == '\\')
                    {
                        index++;
                        continue;
                    }

                    if (ch == quote)
                    {
                        inString = false;
                        quote = '\0';
                    }

                    continue;
                }

                if (ch is '\'' or '"')
                {
                    inString = true;
                    quote = ch;
                    continue;
                }

                if (ch == '[')
                {
                    depth++;
                    continue;
                }

                if (ch == ']')
                {
                    depth--;
                    if (depth == 0)
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        private static Dictionary<string, object?> CreateArrayItem(List<Dictionary<string, object?>> collection)
        {
            var item = new Dictionary<string, object?>(StringComparer.Ordinal);
            collection.Add(item);
            return item;
        }

        private static object ParseValue(string valueText, int lineNumber)
        {
            if (valueText.StartsWith("'") || valueText.StartsWith("\""))
            {
                return ParseString(valueText, lineNumber);
            }

            if (valueText.StartsWith("[", StringComparison.Ordinal))
            {
                return ParseArray(valueText, lineNumber);
            }

            if (bool.TryParse(valueText, out bool boolValue))
            {
                return boolValue;
            }

            if (int.TryParse(valueText, NumberStyles.None, CultureInfo.InvariantCulture, out int intValue))
            {
                return intValue;
            }

            throw CreateError(lineNumber, $"不支持的值 行: {valueText}");
        }

        private static List<object?> ParseArray(string valueText, int lineNumber)
        {
            if (!valueText.EndsWith("]", StringComparison.Ordinal))
            {
                throw CreateError(lineNumber, "数组未正确结束。");
            }

            string content = valueText.Substring(1, valueText.Length - 2).Trim();
            var result = new List<object?>();
            if (string.IsNullOrWhiteSpace(content))
            {
                return result;
            }

            int index = 0;
            while (index < content.Length)
            {
                SkipWhitespace(content, ref index);
                if (index >= content.Length)
                {
                    break;
                }

                if (content[index] is '\'' or '"')
                {
                    string parsed = ParseInlineString(content, ref index, lineNumber);
                    result.Add(parsed);
                }
                else
                {
                        throw CreateError(lineNumber, "当前只支持字符串数组。");
                }

                SkipWhitespace(content, ref index);
                if (index < content.Length)
                {
                    if (content[index] != ',')
                    {
                        throw CreateError(lineNumber, "数组元素之间缺少逗号。");
                    }

                    index++;
                    SkipWhitespace(content, ref index);
                    if (index >= content.Length)
                    {
                        break;
                    }
                }
            }

            return result;
        }

        private static string ParseString(string valueText, int lineNumber)
        {
            int index = 0;
            string result = ParseInlineString(valueText, ref index, lineNumber);
            SkipWhitespace(valueText, ref index);
            if (index != valueText.Length)
            {
                throw CreateError(lineNumber, "字符串值后存在无效内容。");
            }

            return result;
        }

        private static string ParseInlineString(string text, ref int index, int lineNumber)
        {
            if (index >= text.Length)
            {
                throw CreateError(lineNumber, "字符串格式无效。");
            }

            char quote = text[index];
            if (quote is not ('\'' or '"'))
            {
                throw CreateError(lineNumber, "字符串必须以引号开始。");
            }

            index++;
            return quote == '\''
                ? ParseLiteralString(text, ref index, lineNumber)
                : ParseBasicString(text, ref index, lineNumber);
        }

        private static string ParseLiteralString(string text, ref int index, int lineNumber)
        {
            var builder = new StringBuilder();
            while (index < text.Length)
            {
                char ch = text[index++];
                if (ch == '\'')
                {
                    return builder.ToString();
                }

                builder.Append(ch);
            }

            throw CreateError(lineNumber, "字符串未正确结束。");
        }

        private static string ParseBasicString(string text, ref int index, int lineNumber)
        {
            var builder = new StringBuilder();
            while (index < text.Length)
            {
                char ch = text[index++];
                if (ch == '"')
                {
                    return builder.ToString();
                }

                if (ch == '\\')
                {
                    if (index >= text.Length)
                    {
                        throw CreateError(lineNumber, "字符串转义不完整。");
                    }

                    char escaped = text[index++];
                    builder.Append(escaped switch
                    {
                        '\\' => '\\',
                        '"' => '"',
                        'n' => '\n',
                        'r' => '\r',
                        't' => '\t',
                        'f' => '\f',
                        _ => throw CreateError(lineNumber, $"不支持的转义序列: \\{escaped}"),
                    });
                    continue;
                }

                builder.Append(ch);
            }

            throw CreateError(lineNumber, "字符串未正确结束。");
        }

        private static string StripComment(string line)
        {
            bool inString = false;
            char quote = '\0';
            for (int index = 0; index < line.Length; index++)
            {
                char ch = line[index];
                if (!inString && ch == '#')
                {
                    return line.Substring(0, index);
                }

                if (ch is '\'' or '"')
                {
                    if (!inString)
                    {
                        inString = true;
                        quote = ch;
                    }
                    else if (quote == ch && index > 0 && line[index - 1] != '\\')
                    {
                        inString = false;
                        quote = '\0';
                    }
                }
            }

            return line;
        }

        private static void SkipWhitespace(string text, ref int index)
        {
            while (index < text.Length && char.IsWhiteSpace(text[index]))
            {
                index++;
            }
        }

        private static InvalidOperationException CreateError(int lineNumber, string message)
        {
            return new InvalidOperationException($"TOML 解析失败，第 {lineNumber} 行 行: {message}");
        }
    }
}
