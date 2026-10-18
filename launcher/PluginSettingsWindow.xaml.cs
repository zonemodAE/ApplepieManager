using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace Applepie.Launcher;

public partial class PluginSettingsWindow : Window
{
    private sealed class TextEdit(BasicField field, TextBox box, string value)
    {
        public BasicField Field = field;
        public TextBox Box = box;
        public string Queued = value;
    }
    private readonly BasicConfiguration configuration;
    private readonly List<TextEdit> textEdits = [];
    private readonly HashSet<string> errors = [];
    private (BasicField Field, Button Button, TextEdit? Text)? capture;
    private int pending;
    private TaskCompletionSource? drained;
    private bool allowClose, closing;
    public PluginSettingsWindow(PluginItem plugin, BasicConfiguration configuration)
    {
        InitializeComponent(); this.configuration = configuration;
        PluginTitle.Text = plugin.Name; Notice.Text = configuration.Notice; ConfigPath.Text = configuration.FilePath;
        foreach (var field in configuration.Fields) Add(field);
        PreviewKeyDown += CaptureKey; PreviewMouseDown += CaptureMouse; Closing += OnClosing;
        Deactivated += (_, _) => CancelCapture();
    }
    private void Add(BasicField field)
    {
        var grid = new Grid(); grid.ColumnDefinitions.Add(new()); grid.ColumnDefinitions.Add(new() { Width = new GridLength(250) });
        var label = new StackPanel { Margin = new Thickness(0, 0, 24, 0), VerticalAlignment = VerticalAlignment.Center };
        label.Children.Add(new TextBlock { Text = field.Label, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        if (field.Description.Length > 0)
        {
            var description = new TextBlock { Text = field.Description, TextWrapping = TextWrapping.Wrap, FontSize = 11, Margin = new Thickness(0, 7, 0, 0) };
            description.SetResourceReference(TextBlock.ForegroundProperty, "Muted"); label.Children.Add(description);
        }
        grid.Children.Add(label);
        var value = configuration.Value(field);
        FrameworkElement editor;
        if (field.Type == 1)
        {
            var check = new CheckBox { Content = "启用", IsChecked = value is "true" or "1", VerticalAlignment = VerticalAlignment.Center };
            check.Checked += async (_, _) => await Save(field, "true"); check.Unchecked += async (_, _) => await Save(field, "false"); editor = check;
        }
        else if (field.Type == 4)
        {
            var combo = new ComboBox { ItemsSource = field.Options, SelectedItem = value, VerticalAlignment = VerticalAlignment.Center };
            combo.SelectionChanged += async (_, _) => { if (combo.SelectedItem is string selection) await Save(field, selection); }; editor = combo;
        }
        else if (field.Type == 6) editor = KeyButtons(field, null);
        else
        {
            var box = new TextBox { Text = value, VerticalContentAlignment = VerticalAlignment.Center };
            var edit = new TextEdit(field, box, value); textEdits.Add(edit);
            box.LostKeyboardFocus += async (_, _) => await Commit(edit);
            box.KeyDown += async (_, e) => { if (e.Key == Key.Enter) { e.Handled = true; await Commit(edit); } };
            var stack = new StackPanel { VerticalAlignment = VerticalAlignment.Center }; stack.Children.Add(box);
            if (field.Type == 0) stack.Children.Add(KeyButtons(field, edit));
            editor = stack;
        }
        Grid.SetColumn(editor, 1); grid.Children.Add(editor);
        FieldsPanel.Children.Add(new Border { Style = (Style)FindResource("Card"), Padding = new Thickness(18), Margin = new Thickness(0, 0, 0, 10), Child = grid });
    }
    private FrameworkElement KeyButtons(BasicField field, TextEdit? text)
    {
        var grid = new Grid { Margin = text is null ? new Thickness(0) : new Thickness(0, 6, 0, 0), VerticalAlignment = VerticalAlignment.Center };
        grid.ColumnDefinitions.Add(new()); grid.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var button = new Button { Content = text is null ? KeyName(configuration.Value(field)) : "录入按键…", Padding = new Thickness(10, 8, 10, 8) };
        button.Click += (_, _) => { CancelCapture(); capture = (field, button, text); button.Content = "请按键 · Esc 取消"; button.Focus(); };
        grid.Children.Add(button);
        if (text is null)
        {
            var clear = new Button { Content = "清除", Padding = new Thickness(10, 8, 10, 8), Margin = new Thickness(6, 0, 0, 0) };
            clear.Click += async (_, _) => { CancelCapture(); if (await Save(field, "0")) button.Content = KeyName("0"); };
            Grid.SetColumn(clear, 1); grid.Children.Add(clear);
        }
        return grid;
    }
    public static string KeyName(string value)
    {
        if (!int.TryParse(value, out var vk) || vk is < 0 or > 255) return "重新录入按键";
        if (vk == 0) return "未绑定 · 点击录入";
        if (vk is >= 96 and <= 105) return "Numpad " + (vk - 96);
        if (vk is >= 48 and <= 57) return ((char)vk).ToString();
        return vk switch { 1 => "鼠标左键", 2 => "鼠标右键", 4 => "鼠标中键", 5 => "鼠标侧键 1", 6 => "鼠标侧键 2", _ => KeyInterop.KeyFromVirtualKey(vk).ToString() };
    }
    private void CancelCapture()
    {
        if (capture is not { } active) return;
        active.Button.Content = active.Text is null ? KeyName(configuration.Value(active.Field)) : "录入按键…"; capture = null;
    }
    private async void CaptureKey(object sender, KeyEventArgs e)
    {
        if (capture is not { } active) return;
        e.Handled = true;
        if (e.Key == Key.Escape) { CancelCapture(); return; }
        if (e.IsRepeat) return;
        var vk = KeyInterop.VirtualKeyFromKey(e.Key == Key.System ? e.SystemKey : e.Key);
        if (vk is <= 0 or > 255) return;
        await ApplyCapture(active, vk);
    }
    private async void CaptureMouse(object sender, MouseButtonEventArgs e)
    {
        if (capture is not { } active) return;
        var vk = e.ChangedButton switch { MouseButton.Left => 1, MouseButton.Right => 2, MouseButton.Middle => 4, MouseButton.XButton1 => 5, MouseButton.XButton2 => 6, _ => 0 };
        if (vk == 0) return;
        e.Handled = true; await ApplyCapture(active, vk);
    }
    private async Task ApplyCapture((BasicField Field, Button Button, TextEdit? Text) active, int vk)
    {
        capture = null; var value = vk.ToString(CultureInfo.InvariantCulture);
        if (active.Text is { } text) { text.Box.Text = value; await Commit(text); active.Button.Content = "录入按键…"; }
        else { await Save(active.Field, value); active.Button.Content = KeyName(configuration.Value(active.Field)); }
    }
    private async Task Commit(TextEdit edit)
    {
        if (edit.Box.Text == edit.Queued && !errors.Contains(edit.Field.Key)) return;
        var requested = edit.Box.Text; edit.Queued = requested;
        if (await Save(edit.Field, requested))
        {
            if (edit.Box.Text == requested) edit.Box.Text = edit.Queued = configuration.Value(edit.Field);
        }
        else edit.Queued = configuration.Value(edit.Field);
    }
    private async Task<bool> Save(BasicField field, string value)
    {
        if (++pending == 1) drained = new(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            Status.Text = "正在保存…";
            await configuration.Save(field, value);
            errors.Remove(field.Key);
            if (errors.Count == 0) { Status.Text = "已自动保存，下次启动游戏生效。"; Status.SetResourceReference(TextBlock.ForegroundProperty, "Muted"); DiscardButton.Visibility = Visibility.Collapsed; }
            else Status.Text = "其他设置仍有未保存的修改，请修正后重试。";
            return true;
        }
        catch (Exception error)
        {
            errors.Add(field.Key); Status.Text = "未保存：" + error.Message; Status.SetResourceReference(TextBlock.ForegroundProperty, "Danger"); DiscardButton.Visibility = Visibility.Visible;
            return false;
        }
        finally { if (--pending == 0) drained!.TrySetResult(); }
    }
    private async void OnClosing(object? sender, CancelEventArgs e)
    {
        if (allowClose) return;
        e.Cancel = true;
        if (closing) return;
        closing = true; CancelCapture(); FieldsPanel.IsEnabled = false;
        foreach (var edit in textEdits) await Commit(edit);
        if (drained is not null) await drained.Task;
        if (errors.Count == 0) _ = Dispatcher.BeginInvoke(new Action(() => { allowClose = true; Close(); }));
        else { closing = false; FieldsPanel.IsEnabled = true; }
    }
    private void CloseClick(object sender, RoutedEventArgs e) => Close();
    private void Discard(object sender, RoutedEventArgs e) { if (pending != 0) return; allowClose = true; Close(); }
}
