using System;
using System.Windows.Input;

namespace SubtitleFontHelperConfigWpf.Infrastructure;

public sealed class RelayCommand : ICommand
{
    private readonly Action<object?> m_execute;
    private readonly Func<object?, bool>? m_canExecute;

    public RelayCommand(Action execute, Func<bool>? canExecute = null)
        : this(_ => execute(), canExecute is null ? null : _ => canExecute())
    {
    }

    public RelayCommand(Action<object?> execute, Func<object?, bool>? canExecute = null)
    {
        m_execute = execute;
        m_canExecute = canExecute;
    }

    public event EventHandler? CanExecuteChanged;

    public bool CanExecute(object? parameter)
    {
        return m_canExecute?.Invoke(parameter) ?? true;
    }

    public void Execute(object? parameter)
    {
        m_execute(parameter);
    }

    public void RaiseCanExecuteChanged()
    {
        CanExecuteChanged?.Invoke(this, EventArgs.Empty);
    }
}
