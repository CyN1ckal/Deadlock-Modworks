using System.Runtime.InteropServices;

namespace DeadlockModelUpdater;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        if (args.Length > 0)
        {
            AttachParentConsole();
            using var cancellation = new CancellationTokenSource();
            Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };
            Environment.Exit(CommandLine.Execute(args, Console.Out, cancellation.Token));
        }
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
    }

    static void AttachParentConsole()
    {
        if (!AttachConsole(-1)) return;
        Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true });
        Console.SetError(new StreamWriter(Console.OpenStandardError()) { AutoFlush = true });
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AttachConsole(int processId);
}
