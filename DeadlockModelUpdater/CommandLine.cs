namespace DeadlockModelUpdater;

public static class CommandLine
{
    public static int Execute(string[] args, TextWriter output, CancellationToken cancellationToken = default)
    {
        if (args.Length == 1 && args[0] is "--help" or "-h")
        {
            Usage(output);
            return 0;
        }
        var positional = args.ToList();
        string? reportPath = null;
        var reportIndex = positional.IndexOf("--report");
        if (reportIndex >= 0)
        {
            if (reportIndex != positional.Count - 2) { Usage(output); return 2; }
            reportPath = positional[^1];
            positional.RemoveRange(reportIndex, 2);
            if (!reportPath.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
            {
                output.WriteLine("Report output must be a .json file.");
                return 2;
            }
        }
        var action = positional.FirstOrDefault();
        if ((action is "--analyze" or "--repair") && positional.Count == 3
            || action == "--update" && positional.Count == 4)
        {
            UpdateReport report;
            try
            {
                var progress = new InlineProgress(update => output.WriteLine(update.Message));
                report = action == "--analyze"
                    ? HeroModelUpdater.Analyze(positional[1], positional[2], progress, cancellationToken)
                    : HeroModelUpdater.Update(positional[1], positional[2], action == "--update" ? positional[3] : null,
                        replaceSource: action == "--repair", makeBackup: true, progress, cancellationToken);
            }
            catch (OperationCanceledException)
            {
                output.WriteLine("Cancelled. No pending repair was installed.");
                return 2;
            }
            catch (Exception ex)
            {
                report = new UpdateReport([new ModelResult(positional[2], ModelOutcome.Failed, ex.Message)], false, null, null);
            }
            foreach (var result in report.Results)
                output.WriteLine($"{result.Outcome}: {result.Model}: {result.Detail}");
            output.WriteLine(report.WroteFile ? $"Wrote {report.OutputPath}." : "No VPK was written.");
            if (report.BackupPath != null) output.WriteLine("Original backup: " + report.BackupPath);
            if (reportPath != null)
            {
                try
                {
                    ReportExporter.Write(reportPath, positional[1], [new ArchiveReport(positional[2], report)]);
                    output.WriteLine("Saved report: " + Path.GetFullPath(reportPath));
                }
                catch (Exception ex) { output.WriteLine("Report could not be saved: " + ex.Message); return 2; }
            }
            return report.HasFailures ? 2 : report.HasBlockers ? 1 : 0;
        }
        Usage(output);
        return 2;
    }

    static void Usage(TextWriter output)
    {
        output.WriteLine("Deadlock Model Updater — hero model skeleton repairs");
        output.WriteLine("  --analyze <citadel-folder> <mod.vpk> [--report <report.json>]");
        output.WriteLine("  --update  <citadel-folder> <mod.vpk> <new-output.vpk> [--report <report.json>]");
        output.WriteLine("  --repair  <citadel-folder> <mod.vpk> [--report <report.json>]");
        output.WriteLine("Repair replaces the mod with a validated archive and keeps an original backup.");
        output.WriteLine("Exit codes: 0 = supported analysis/repair; 1 = needs review or unsupported; 2 = invalid input, failure, or cancellation.");
    }

    sealed class InlineProgress(Action<UpdateProgress> report) : IProgress<UpdateProgress>
    {
        public void Report(UpdateProgress value) => report(value);
    }
}
