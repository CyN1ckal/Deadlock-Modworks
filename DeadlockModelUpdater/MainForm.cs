using System.Text.RegularExpressions;

namespace DeadlockModelUpdater;

sealed class MainForm : Form
{
    readonly TextBox _game = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right };
    readonly TextBox _mod = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right };
    readonly CheckBox _backup = new() { Text = "Back up the VPK before replacing it", Checked = true, AutoSize = true };
    readonly Button _update = new() { Text = "Update hero models", AutoSize = true };
    readonly ProgressBar _progress = new() { Dock = DockStyle.Fill, Minimum = 0, Maximum = 1 };
    readonly Label _status = new() { AutoSize = true, Text = "Choose the Deadlock citadel folder and a mod VPK.", MaximumSize = new Size(900, 0) };
    readonly ListView _results = new() { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, GridLines = true };
    readonly TextBox _log = new()
    {
        Dock = DockStyle.Fill,
        Multiline = true,
        ReadOnly = true,
        ScrollBars = ScrollBars.Vertical,
        Font = new Font(FontFamily.GenericMonospace, 9f),
    };

    bool _running;

    public MainForm()
    {
        Text = "Deadlock Model Updater";
        Width = 980;
        Height = 720;
        MinimumSize = new Size(760, 520);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9f);
        AllowDrop = true;

        _results.Columns.Add("Model", 360);
        _results.Columns.Add("Result", 90);
        _results.Columns.Add("Detail", -2);

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 4,
            Padding = new Padding(12),
        };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 58));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 42));
        root.Controls.Add(BuildPicker(), 0, 0);
        root.Controls.Add(BuildActions(), 0, 1);
        root.Controls.Add(_results, 0, 2);
        root.Controls.Add(_log, 0, 3);
        Controls.Add(root);

        _update.Click += async (_, _) => await RunUpdateAsync();
        DragEnter += (_, e) =>
        {
            if (e.Data?.GetDataPresent(DataFormats.FileDrop) == true)
                e.Effect = DragDropEffects.Copy;
        };
        DragDrop += (_, e) =>
        {
            if (_running || e.Data?.GetData(DataFormats.FileDrop) is not string[] files)
                return;
            var vpk = files.FirstOrDefault(file => file.EndsWith(".vpk", StringComparison.OrdinalIgnoreCase));
            if (vpk != null)
                _mod.Text = vpk;
        };
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        var game = GameFolder.Find();
        if (game == null)
            return;

        _game.Text = game;
        var addon = Path.Combine(game, "addons", "pak01_dir.vpk");
        if (File.Exists(addon))
            _mod.Text = addon;
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (_running)
        {
            e.Cancel = true;
            _status.Text = "Wait for the update to finish.";
        }

        base.OnFormClosing(e);
    }

    Control BuildPicker()
    {
        var grid = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3, RowCount = 2 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));

        grid.Controls.Add(new Label { Text = "Game folder", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0);
        grid.Controls.Add(_game, 1, 0);
        var gameBrowse = new Button { Text = "Browse...", Dock = DockStyle.Fill };
        gameBrowse.Click += (_, _) => BrowseFolder();
        grid.Controls.Add(gameBrowse, 2, 0);

        grid.Controls.Add(new Label { Text = "Mod VPK", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
        grid.Controls.Add(_mod, 1, 1);
        var modBrowse = new Button { Text = "Browse...", Dock = DockStyle.Fill };
        modBrowse.Click += (_, _) => BrowseMod();
        grid.Controls.Add(modBrowse, 2, 1);
        return grid;
    }

    Control BuildActions()
    {
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            WrapContents = true,
            Padding = new Padding(0, 8, 0, 8),
        };
        panel.Controls.Add(new Label
        {
            Text = "Updates hero model skeletons to the current game bone order. Other files in the VPK are copied unchanged.",
            AutoSize = true,
            MaximumSize = new Size(920, 0),
            Padding = new Padding(0, 0, 0, 4),
        });
        panel.Controls.Add(_backup);
        panel.Controls.Add(_update);
        var progressHost = new Panel { Width = 220, Height = 23, Margin = new Padding(12, 6, 0, 0) };
        progressHost.Controls.Add(_progress);
        panel.Controls.Add(progressHost);
        panel.Controls.Add(_status);
        return panel;
    }

    void BrowseFolder()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select the Deadlock citadel folder",
            UseDescriptionForTitle = true,
            SelectedPath = Directory.Exists(_game.Text) ? _game.Text : "",
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
            _game.Text = dialog.SelectedPath;
    }

    void BrowseMod()
    {
        var initial = Directory.Exists(_game.Text) ? Path.Combine(_game.Text, "addons") : "";
        using var dialog = new OpenFileDialog
        {
            Title = "Select a mod VPK",
            Filter = "VPK archives (*.vpk)|*.vpk|All files (*.*)|*.*",
            InitialDirectory = Directory.Exists(initial) ? initial : _game.Text,
            FileName = File.Exists(_mod.Text) ? Path.GetFileName(_mod.Text) : "",
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
            _mod.Text = dialog.FileName;
    }

    async Task RunUpdateAsync()
    {
        if (_running)
            return;
        if (!Directory.Exists(_game.Text))
        {
            MessageBox.Show(this, "Choose the game folder that contains pak01_dir.vpk.", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        if (!File.Exists(_mod.Text))
        {
            MessageBox.Show(this, "Choose a mod VPK.", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        _running = true;
        _update.Enabled = false;
        _results.Items.Clear();
        _log.Clear();
        _progress.Value = 0;
        var progress = new Progress<UpdateProgress>(ShowProgress);
        try
        {
            var report = await Task.Run(() => HeroModelUpdater.Update(
                _game.Text,
                _mod.Text,
                outputPath: null,
                replaceSource: true,
                _backup.Checked,
                progress));
            ShowReport(report);
        }
        catch (Exception ex)
        {
            Log(ex.Message);
            _status.Text = "Update failed.";
            MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            _running = false;
            _update.Enabled = true;
        }
    }

    void ShowProgress(UpdateProgress update)
    {
        _progress.Maximum = Math.Max(1, update.Total);
        _progress.Value = Math.Clamp(update.Completed, 0, _progress.Maximum);
        _status.Text = update.Message;
        Log(update.Message);
    }

    void ShowReport(UpdateReport report)
    {
        foreach (var result in report.Results)
        {
            var item = new ListViewItem(result.Model);
            item.SubItems.Add(result.Outcome.ToString());
            item.SubItems.Add(result.Detail);
            item.ForeColor = result.Outcome switch
            {
                ModelOutcome.Updated => Color.DarkGreen,
                ModelOutcome.Failed => Color.DarkRed,
                _ => SystemColors.WindowText,
            };
            _results.Items.Add(item);
        }
    }

    void Log(string message)
    {
        if (_log.TextLength > 0)
            _log.AppendText(Environment.NewLine);
        _log.AppendText(message);
    }
}

static class GameFolder
{
    public static string? Find()
    {
        var standard = @"C:\Program Files (x86)\Steam\steamapps\common\Deadlock\game\citadel";
        if (File.Exists(Path.Combine(standard, "pak01_dir.vpk")))
            return standard;

        var libraries = new List<string> { @"C:\Program Files (x86)\Steam" };
        var vdf = @"C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf";
        if (File.Exists(vdf))
        {
            var text = File.ReadAllText(vdf);
            foreach (Match match in Regex.Matches(text, "\"path\"\\s+\"(?<path>[^\"]+)\""))
                libraries.Add(match.Groups["path"].Value.Replace("\\\\", "\\"));
        }

        foreach (var library in libraries)
        {
            var candidate = Path.Combine(library, "steamapps", "common", "Deadlock", "game", "citadel");
            if (File.Exists(Path.Combine(candidate, "pak01_dir.vpk")))
                return candidate;
        }

        return null;
    }
}
