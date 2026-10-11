using System.Text.Json;
using System.Text.Json.Serialization;

namespace DeadlockModelUpdater;

public sealed record ArchiveReport(string ModPath, UpdateReport Report);

public static class ReportExporter
{
    public static void Write(string path, string gameDirectory, IReadOnlyList<ArchiveReport> archives)
    {
        if (!path.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Reports must be saved as .json files.", nameof(path));
        path = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, Serialize(gameDirectory, archives));
            File.Move(temp, path, overwrite: true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }

    static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };

    public static string Serialize(string gameDirectory, IReadOnlyList<ArchiveReport> archives) =>
        JsonSerializer.Serialize(new
        {
            SchemaVersion = 1,
            GeneratedUtc = DateTimeOffset.UtcNow,
            GameDirectory = gameDirectory,
            Scope = "Hero model skeleton compatibility. In-game appearance and animation are not verified.",
            Archives = archives,
        }, Options);
}
