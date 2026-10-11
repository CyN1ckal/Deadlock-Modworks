namespace DeadlockModelUpdater;

/// <summary>Writes and validates a complete archive before publishing it.</summary>
public static class ArchiveTransaction
{
    public static string? Write(VpkArchive source, string sourcePath, string destination,
        IReadOnlyDictionary<string, byte[]?> changes, bool replaceSource,
        CancellationToken cancellationToken = default, Action? beforeCommit = null)
    {
        sourcePath = Path.GetFullPath(sourcePath);
        destination = Path.GetFullPath(destination);
        if (replaceSource != string.Equals(sourcePath, destination, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Replacement destination does not match the source mode.");
        if (!replaceSource && File.Exists(destination))
            throw new IOException("Output already exists. Choose a new filename; existing files will not be overwritten.");
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        using var sourceLease = source.LockSources();
        var temp = destination + "." + Guid.NewGuid().ToString("N") + ".new";
        string? backup = null;
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            source.Write(temp, changes, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            var check = VpkArchive.Open(temp);
            check.Validate(cancellationToken);
            var expected = source.Entries.Select(e => e.FullPath).ToHashSet(StringComparer.OrdinalIgnoreCase);
            foreach (var (path, bytes) in changes)
            {
                if (bytes == null) expected.Remove(path);
                else expected.Add(path);
            }
            if (!expected.SetEquals(check.Entries.Select(e => e.FullPath)))
                throw new InvalidDataException("The output archive does not contain the expected resources.");
            foreach (var (path, bytes) in changes)
            {
                if (bytes == null) continue;
                var entry = check.Entries.Single(e => e.FullPath.Equals(path, StringComparison.OrdinalIgnoreCase));
                if (!check.Read(entry).AsSpan().SequenceEqual(bytes))
                    throw new InvalidDataException($"Output verification failed for {path}.");
            }
            cancellationToken.ThrowIfCancellationRequested();
            sourceLease.Dispose();
            beforeCommit?.Invoke();
            if (replaceSource)
            {
                backup = sourcePath + ".bak-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8];
                // File.Replace creates the backup as part of the same filesystem operation.
                File.Replace(temp, destination, backup);
            }
            else
            {
                File.Move(temp, destination, overwrite: false);
            }
            return backup;
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
        }
    }
}
