using SubtitleFontHelperConfigWpf.Infrastructure;

namespace SubtitleFontHelperConfigWpf.ViewModels;

public sealed class EditableStringItemViewModel : ObservableObject
{
    private string m_value;

    public EditableStringItemViewModel(string value = "")
    {
        m_value = value;
    }

    public string Value
    {
        get => m_value;
        set => SetProperty(ref m_value, value);
    }

    public override string ToString()
    {
        return string.IsNullOrWhiteSpace(Value) ? "(空)" : Value;
    }
}
