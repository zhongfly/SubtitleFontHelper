using System;
using System.Windows;
using Forms = System.Windows.Forms;
using Microsoft.Win32;

namespace SubtitleFontHelperConfigWpf.Services;

public enum DirtyCloseAction
{
    Cancel,
    CloseWithoutSaving,
    SaveAndClose,
}

public sealed class DialogService
{
    public bool ConfirmDiscard(Window owner, string action)
    {
        MessageBoxResult result = MessageBox.Show(
            owner,
            $"当前有未保存的修改，确认要{action}吗？未保存内容会丢失。",
            "SubtitleFontHelper Config",
            MessageBoxButton.OKCancel,
            MessageBoxImage.Question);

        return result == MessageBoxResult.OK;
    }

    public DirtyCloseAction ConfirmCloseWithUnsavedChanges(Window owner)
    {
        var dialog = new DirtyCloseDialog
        {
            Owner = owner,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
        };

        bool? result = dialog.ShowDialog();
        return result == true ? dialog.SelectedAction : DirtyCloseAction.Cancel;
    }

    public void ShowError(Window owner, string message)
    {
        MessageBox.Show(owner, message, "SubtitleFontHelper Config", MessageBoxButton.OK, MessageBoxImage.Error);
    }

    public void ShowWarning(Window owner, string message)
    {
        MessageBox.Show(owner, message, "SubtitleFontHelper Config", MessageBoxButton.OK, MessageBoxImage.Warning);
    }

    public void ShowInformation(Window owner, string message)
    {
        MessageBox.Show(owner, message, "SubtitleFontHelper Config", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    public string? PickFolder(Window owner, string? initialPath = null)
    {
        using var dialog = new Forms.FolderBrowserDialog
        {
            Description = "选择文件夹",
            ShowNewFolderButton = true,
        };

        if (!string.IsNullOrWhiteSpace(initialPath))
        {
            dialog.SelectedPath = initialPath;
        }

        IntPtr ownerHandle = new System.Windows.Interop.WindowInteropHelper(owner).Handle;
        Forms.DialogResult result = dialog.ShowDialog(new Win32WindowWrapper(ownerHandle));
        return result == Forms.DialogResult.OK ? dialog.SelectedPath : null;
    }

    public string? PickExecutable(Window owner)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "可执行文件 (*.exe)|*.exe|所有文件 (*.*)|*.*",
            Title = "选择要监视的可执行文件",
            CheckFileExists = true,
            CheckPathExists = true,
        };

        bool? result = dialog.ShowDialog(owner);
        return result == true ? dialog.FileName : null;
    }

    public string? PickIndexPath(Window owner, string? currentValue = null)
    {
        var dialog = new SaveFileDialog
        {
            Filter = "索引文件 (*.xml)|*.xml|所有文件 (*.*)|*.*",
            Title = "选择索引文件保存位置",
            DefaultExt = ".xml",
            AddExtension = true,
            CheckPathExists = true,
            OverwritePrompt = true,
            FileName = currentValue ?? string.Empty,
        };

        bool? result = dialog.ShowDialog(owner);
        return result == true ? dialog.FileName : null;
    }

    private sealed class Win32WindowWrapper : Forms.IWin32Window
    {
        public Win32WindowWrapper(IntPtr handle)
        {
            Handle = handle;
        }

        public IntPtr Handle { get; }
    }
}
