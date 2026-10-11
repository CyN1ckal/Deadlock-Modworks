using System.Runtime.InteropServices;

namespace DeadlockModelUpdater;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        if (args.Length > 0 && string.Equals(args[0], "--update", StringComparison.OrdinalIgnoreCase))
        {
            AttachParentConsole();
            Environment.Exit(UpdateCommand(args));
        }

        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
    }

    static int UpdateCommand(string[] args)
    {
        if (args.Length != 4)
        {
            Console.Error.WriteLine("Usage: DeadlockModelUpdater --update <citadel-folder> <mod.vpk> <output.vpk>");
            return 2;
        }

        try
        {
            var progress = new Progress<UpdateProgress>(update => Console.WriteLine(update.Message));
            var report = HeroModelUpdater.Update(
                args[1],
                args[2],
                args[3],
                replaceSource: false,
                makeBackup: false,
                progress);

            var updated = report.Results.Count(result => result.Outcome == ModelOutcome.Updated);
            var failed = report.Results.Count(result => result.Outcome == ModelOutcome.Failed);
            Console.WriteLine(report.WroteFile
                ? $"Updated {updated} model(s). Failed {failed}. Wrote {report.OutputPath}."
                : $"Updated {updated} model(s). Failed {failed}. No file written.");
            return failed == 0 ? 0 : 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 2;
        }
    }

    static void AttachParentConsole()
    {
        if (!AttachConsole(-1))
            return;

        Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true });
        Console.SetError(new StreamWriter(Console.OpenStandardError()) { AutoFlush = true });
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AttachConsole(int processId);
}
