using System.Reflection;
using System.Windows.Forms;

static class UiSmoke
{
    public static void Run(string gameDirectory, string modPath, string screenshotPath, bool expectRepair = false)
    {
        Exception? error = null;
        var thread = new Thread(() =>
        {
            try
            {
                Application.SetHighDpiMode(HighDpiMode.SystemAware);
                Application.EnableVisualStyles();
                var type = typeof(DeadlockModelUpdater.HeroModelUpdater).Assembly.GetType("DeadlockModelUpdater.MainForm")!;
                using var form = (Form)Activator.CreateInstance(type, nonPublic: true)!;
                form.ShowInTaskbar = false;
                form.StartPosition = FormStartPosition.Manual;
                form.Location = new System.Drawing.Point(-2000, -2000);
                form.Show();
                SynchronizationContext.SetSynchronizationContext(new WindowsFormsSynchronizationContext());
                var game = (TextBox)type.GetField("_game", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
                game.Text = gameDirectory;
                type.GetMethod("SelectMods", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form, [new[] { modPath }]);
                var task = (Task)type.GetMethod("RunAsync", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form, [false])!;
                Complete(task);
                var results = (ListView)type.GetField("_results", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
                var repair = (Button)type.GetField("_repair", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
                var export = (Button)type.GetField("_export", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
                if (results.Items.Count == 0 || repair.Enabled != expectRepair || !export.Enabled)
                    throw new Exception("Analysis controls do not match the expected repair eligibility.");
                if (expectRepair)
                {
                    var before = File.ReadAllBytes(modPath);
                    var repairTask = (Task)type.GetMethod("RunAsync", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form, [true])!;
                    Complete(repairTask);
                    var reports = (List<DeadlockModelUpdater.ArchiveReport>)type.GetField("_reports", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
                    var report = reports.Single().Report;
                    if (!report.WroteFile || report.BackupPath == null || !File.ReadAllBytes(report.BackupPath).SequenceEqual(before) || repair.Enabled)
                        throw new Exception("UI repair did not create the expected repair and original backup.");
                    var status = (Label)type.GetField("_status", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
                    if (!status.Text.StartsWith("1 repaired;"))
                        throw new Exception("A stale progress callback replaced the final repair summary.");
                }
                using var bitmap = new System.Drawing.Bitmap(form.Width, form.Height);
                form.DrawToBitmap(bitmap, new System.Drawing.Rectangle(0, 0, form.Width, form.Height));
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(screenshotPath))!);
                bitmap.Save(screenshotPath);
                form.Close();
            }
            catch (Exception ex) { error = ex; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (error != null) throw new Exception("UI smoke test failed", error);
        Console.WriteLine("PASS UI analysis flow and rendered screenshot: " + screenshotPath);
    }

    static void Complete(Task task)
    {
        var limit = DateTime.UtcNow.AddSeconds(60);
        while (!task.IsCompleted && DateTime.UtcNow < limit)
        {
            Application.DoEvents();
            Thread.Sleep(10);
        }
        if (!task.IsCompleted) throw new TimeoutException("UI operation did not complete.");
        task.GetAwaiter().GetResult();
        Application.DoEvents();
    }
}
