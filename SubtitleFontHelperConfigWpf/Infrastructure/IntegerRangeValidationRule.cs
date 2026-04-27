using System.Globalization;
using System.Windows.Controls;

namespace SubtitleFontHelperConfigWpf.Infrastructure;

public sealed class IntegerRangeValidationRule : ValidationRule
{
    public int Minimum { get; set; }

    public int Maximum { get; set; }

    public override ValidationResult Validate(object value, CultureInfo cultureInfo)
    {
        string text = value as string ?? string.Empty;
        if (!int.TryParse(text, NumberStyles.Integer, cultureInfo, out int parsed))
        {
            return new ValidationResult(false, "请输入整数。");
        }

        if (parsed < Minimum || parsed > Maximum)
        {
            return new ValidationResult(false, $"请输入 {Minimum} 到 {Maximum} 之间的整数。");
        }

        return ValidationResult.ValidResult;
    }
}
