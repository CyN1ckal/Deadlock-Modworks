using System.Text.RegularExpressions;

namespace DeadlockModelUpdater;

sealed class MainForm : Form
{
    readonly TextBox _game = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right };
    readonly TextBox _mod = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right, ReadOnly = true };
    readonly Button _analyze = new() { Text = "1. Analyze mods", AutoSize = true };
    readonly Button _repair = new() { Text = "2. Repair supported mods", AutoSize = true, Enabled = false };
    readonly Button _export = new() { Text = "Save report", AutoSize = true, Enabled = false };
    readonly Button _cancel = new() { Text = "Cancel", AutoSize = true, Enabled = false };
    readonly ProgressBar _progress = new() { Dock = DockStyle.Fill, Minimum = 0, Maximum = 1 };
    readonly Label _status = new() { AutoSize = true, Text = "Choose or drop mod VPKs, then analyze them.", MaximumSize = new Size(920, 0) };
    readonly ListView _results = new() { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, GridLines = true, ShowItemToolTips = true };
    readonly TextBox _log = new()
    {
        Dock = DockStyle.Fill, Multiline = true, ReadOnly = true,
        ScrollBars = ScrollBars.Vertical, Font = new Font(FontFamily.GenericMonospace, 9f),
    };
    readonly List<string> _mods = [];
    readonly List<ArchiveReport> _reports = [];
    Control _picker = null!;
    CancellationTokenSource? _cancellation;
    string? _analyzedGame;
    int _progressGeneration;

    public MainForm()
    {
        Text = "Deadlock Model Updater";
        Width = 1100; Height = 780; MinimumSize = new Size(840, 580);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9f);
        AllowDrop = true;
        _results.Columns.Add("VPK", 140);
        _results.Columns.Add("Resource", 310);
        _results.Columns.Add("Result", 110);
        _results.Columns.Add("Detail", 460);
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(12) };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 62));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 38));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        _picker = BuildPicker();
        root.Controls.Add(_picker, 0, 0);
        root.Controls.Add(BuildActions(), 0, 1);
        root.Controls.Add(_results, 0, 2);
        root.Controls.Add(_log, 0, 3);
        var credit = new LinkLabel { Text = "Powered by Source 2 Viewer / ValveResourceFormat", AutoSize = true, Padding = new Padding(0, 6, 0, 0) };
        credit.LinkClicked += (_, _) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("https://s2v.app") { UseShellExecute = true });
        root.Controls.Add(credit, 0, 4);
        Controls.Add(root);
        _analyze.Click += async (_, _) => await RunAsync(repair: false);
        _repair.Click += async (_, _) => await RunAsync(repair: true);
        _export.Click += (_, _) => ExportReport();
        _cancel.Click += (_, _) => { _cancellation?.Cancel(); _status.Text = "Cancelling after the current operation..."; };
        _game.TextChanged += (_, _) => InvalidateAnalysis();
        _results.SelectedIndexChanged += (_, _) =>
        {
            if (_results.SelectedItems.Count > 0)
                _status.Text = _results.SelectedItems[0].ToolTipText;
        };
        DragEnter += (_, e) =>
        {
            if (_cancellation == null && e.Data?.GetDataPresent(DataFormats.FileDrop) == true)
                e.Effect = DragDropEffects.Copy;
        };
        DragDrop += (_, e) =>
        {
            if (_cancellation == null && e.Data?.GetData(DataFormats.FileDrop) is string[] files)
                SelectMods(files);
        };
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        try { _game.Text = GameFolder.Find() ?? ""; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            _status.Text = "Automatic game detection was unavailable. Choose the citadel folder manually.";
        }
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (_cancellation != null)
        {
            _cancellation.Cancel();
            e.Cancel = true;
            _status.Text = "Cancelling. Close the window after the operation stops.";
        }
        base.OnFormClosing(e);
    }

    Control BuildPicker()
    {
        var grid = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3, RowCount = 2 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));
        grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        grid.Controls.Add(new Label { Text = "Citadel folder", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0);
        grid.Controls.Add(_game, 1, 0);
        var gameBrowse = new Button { Text = "Browse...", Dock = DockStyle.Fill };
        gameBrowse.Click += (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { Description = "Select Deadlock's game/citadel folder", UseDescriptionForTitle = true };
            if (dialog.ShowDialog(this) == DialogResult.OK) _game.Text = dialog.SelectedPath;
        };
        grid.Controls.Add(gameBrowse, 2, 0);
        grid.Controls.Add(new Label { Text = "Mod VPKs", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
        grid.Controls.Add(_mod, 1, 1);
        var modBrowse = new Button { Text = "Choose VPKs...", Dock = DockStyle.Fill };
        modBrowse.Click += (_, _) =>
        {
            using var dialog = new OpenFileDialog
            {
                Title = "Choose one or more mod VPKs", Filter = "VPK archives (*.vpk)|*.vpk", Multiselect = true,
                InitialDirectory = Directory.Exists(Path.Combine(_game.Text, "addons")) ? Path.Combine(_game.Text, "addons") : "",
            };
            if (dialog.ShowDialog(this) == DialogResult.OK) SelectMods(dialog.FileNames);
        };
        grid.Controls.Add(modBrowse, 2, 1);
        return grid;
    }

    Control BuildActions()
    {
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, Padding = new Padding(0, 8, 0, 8) };
        panel.Controls.Add(new Label
        {
            Text = "Analyze first. Repairs replace each supported VPK and create an original backup. Mods needing review are left untouched.",
            AutoSize = true, MaximumSize = new Size(1020, 0), Padding = new Padding(0, 0, 0, 6),
        });
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true };
        buttons.Controls.AddRange([_analyze, _repair, _export, _cancel]);
        var progressHost = new Panel { Width = 180, Height = 23, Margin = new Padding(10, 6, 0, 0) };
        progressHost.Controls.Add(_progress);
        buttons.Controls.Add(progressHost);
        panel.Controls.Add(buttons);
        panel.Controls.Add(_status);
        return panel;
    }

    void SelectMods(IEnumerable<string> files)
    {
        var paths = files.Where(file => file.EndsWith(".vpk", StringComparison.OrdinalIgnoreCase))
            .Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if (paths.Length == 0) return;
        _mods.Clear(); _mods.AddRange(paths);
        _mod.Text = paths.Length == 1 ? paths[0] : $"{paths.Length} VPKs: {string.Join(", ", paths.Select(Path.GetFileName))}";
        InvalidateAnalysis();
        _status.Text = $"{paths.Length} VPK(s) selected. Click Analyze mods.";
    }

    void InvalidateAnalysis()
    {
        _reports.Clear(); _results.Items.Clear();
        _repair.Enabled = false; _export.Enabled = false; _analyzedGame = null;
    }

    async Task RunAsync(bool repair)
    {
        if (_cancellation != null) return;
        if (!File.Exists(Path.Combine(_game.Text, "pak01_dir.vpk")) || _mods.Count == 0)
        {
            _status.Text = "Choose the citadel folder containing pak01_dir.vpk and at least one mod VPK.";
            return;
        }
        var game = Path.GetFullPath(_game.Text);
        var targets = repair ? _reports.Where(r => r.Report.CanRepair).Select(r => r.ModPath).ToArray() : _mods.ToArray();
        if (repair && (_analyzedGame != game || targets.Length == 0)) return;
        if (!repair) { _reports.Clear(); _log.Clear(); }
        _results.Items.Clear();
        _cancellation = new CancellationTokenSource();
        var token = _cancellation.Token;
        _picker.Enabled = false; _analyze.Enabled = false; _repair.Enabled = false; _export.Enabled = false; _cancel.Enabled = true;
        try
        {
            foreach (var mod in targets)
            {
                token.ThrowIfCancellationRequested();
                Log($"{(repair ? "Repairing" : "Analyzing")} {mod}");
                var progress = new UiProgress(this, ++_progressGeneration);
                UpdateReport report;
                try
                {
                    report = await Task.Run(() => repair
                        ? HeroModelUpdater.Update(game, mod, null, true, true, progress, token)
                        : HeroModelUpdater.Analyze(game, mod, progress, token), token);
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex)
                {
                    report = new UpdateReport([new ModelResult(Path.GetFileName(mod), ModelOutcome.Failed, ex.Message)], false, null, null);
                    Log(ex.Message);
                }
                ++_progressGeneration;
                _reports.RemoveAll(r => r.ModPath.Equals(mod, StringComparison.OrdinalIgnoreCase));
                _reports.Add(new ArchiveReport(mod, report));
                foreach (var result in report.Results)
                {
                    Log($"{result.Outcome}: {result.Model}: {result.Detail}");
                    foreach (var warning in result.Warnings ?? []) Log("Note: " + warning);
                }
                if (report.BackupPath != null) Log("Original backup: " + report.BackupPath);
                RenderReports();
            }
            _analyzedGame = game;
            var ready = _reports.Count(r => r.Report.CanRepair);
            var blocked = _reports.Count(r => r.Report.HasBlockers);
            var updated = _reports.Count(r => r.Report.WroteFile);
            _status.Text = $"{updated} repaired; {ready} repairable; {blocked} need review or are unsupported. Select a result for details. Test repaired mods in-game.";
        }
        catch (OperationCanceledException)
        {
            _analyzedGame = null;
            _status.Text = "Cancelled. Completed repairs and their backup paths remain in the report. Analyze again to continue.";
        }
        finally
        {
            ++_progressGeneration;
            _cancellation.Dispose(); _cancellation = null;
            _picker.Enabled = true; _analyze.Enabled = true; _cancel.Enabled = false;
            _repair.Enabled = _analyzedGame == game && _reports.Any(r => r.Report.CanRepair);
            _export.Enabled = _reports.Count > 0;
            _progress.Style = ProgressBarStyle.Blocks;
            RenderReports();
        }
    }

    void RenderReports()
    {
        _results.BeginUpdate();
        _results.Items.Clear();
        foreach (var archive in _reports)
        foreach (var result in archive.Report.Results)
        {
            var detail = result.Detail + (result.Warnings is { Count: > 0 } ? " Notes: " + string.Join(" ", result.Warnings) : "");
            var item = new ListViewItem(Path.GetFileName(archive.ModPath)) { ToolTipText = detail };
            item.SubItems.Add(result.Model);
            item.SubItems.Add(result.Outcome == ModelOutcome.NeedsReview ? "Needs review" : result.Outcome.ToString());
            item.SubItems.Add(detail);
            item.ForeColor = result.Outcome switch
            {
                ModelOutcome.Updated or ModelOutcome.Repairable => Color.DarkGreen,
                ModelOutcome.NeedsReview or ModelOutcome.Skipped => Color.FromArgb(145, 83, 0),
                ModelOutcome.Failed => Color.DarkRed,
                _ => SystemColors.WindowText,
            };
            _results.Items.Add(item);
        }
        _results.EndUpdate();
    }

    void ShowProgress(UpdateProgress update)
    {
        _progress.Style = update.Total == 0 ? ProgressBarStyle.Marquee : ProgressBarStyle.Blocks;
        _progress.Maximum = Math.Max(1, update.Total);
        _progress.Value = Math.Clamp(update.Completed, 0, _progress.Maximum);
        _status.Text = update.Message;
        Log(update.Message);
    }

    void ExportReport()
    {
        using var dialog = new SaveFileDialog { Title = "Save analysis and repair report", Filter = "JSON report (*.json)|*.json", FileName = "deadlock-mod-report.json" };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        try
        {
            ReportExporter.Write(dialog.FileName, _game.Text, _reports);
            _status.Text = "Saved report: " + dialog.FileName;
        }
        catch (Exception ex) { _status.Text = "Could not save report: " + ex.Message; }
    }

    void Log(string message) => _log.AppendText(message + Environment.NewLine);

    sealed class UiProgress(MainForm form, int generation) : IProgress<UpdateProgress>
    {
        public void Report(UpdateProgress value)
        {
            if (form.IsDisposed || !form.IsHandleCreated) return;
            form.BeginInvoke((Action)(() =>
            {
                if (form._cancellation != null && form._progressGeneration == generation)
                    form.ShowProgress(value);
            }));
        }
    }
}

static class GameFolder
{
    public static string? Find()
    {
        var libraries = new List<string>();
        var steamPath = Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        if (!string.IsNullOrWhiteSpace(steamPath)) libraries.Add(steamPath);
        libraries.Add(@"C:\Program Files (x86)\Steam");
        foreach (var steam in libraries.ToArray())
        {
            var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
            if (!File.Exists(vdf)) continue;
            foreach (Match match in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"(?<path>[^\"]+)\""))
                libraries.Add(match.Groups["path"].Value.Replace("\\\\", "\\"));
        }
        return libraries.Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(library => Path.Combine(library, "steamapps", "common", "Deadlock", "game", "citadel"))
            .FirstOrDefault(path => File.Exists(Path.Combine(path, "pak01_dir.vpk")));
    }
}
