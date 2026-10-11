using DeadlockModelUpdater;

using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };
return CommandLine.Execute(args.Length == 0 ? ["--help"] : args, Console.Out, cancellation.Token);
