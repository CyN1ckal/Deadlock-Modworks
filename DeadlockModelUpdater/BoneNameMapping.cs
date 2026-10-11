namespace DeadlockModelUpdater;

public sealed record BoneNameMappingResult(IReadOnlyDictionary<string, string> Aliases,
    IReadOnlyList<string> Unmatched, IReadOnlyList<string> Ambiguous);

/// <summary>Maps only unique case-only changes. It never removes separators or guesses semantic renames.</summary>
public static class BoneNameMapping
{
    public static BoneNameMappingResult Resolve(IReadOnlyList<string> oldNames, IReadOnlyList<string> currentNames)
    {
        var exact = currentNames.ToHashSet(StringComparer.Ordinal);
        var oldGroups = oldNames.GroupBy(name => name, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.Count(), StringComparer.OrdinalIgnoreCase);
        var newGroups = currentNames.GroupBy(name => name, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.ToArray(), StringComparer.OrdinalIgnoreCase);
        var used = oldNames.Where(exact.Contains).ToHashSet(StringComparer.Ordinal);
        var aliases = new Dictionary<string, string>(StringComparer.Ordinal);
        var unmatched = new List<string>();
        var ambiguous = new List<string>();
        foreach (var name in oldNames.Where(name => !exact.Contains(name)))
        {
            if (!newGroups.TryGetValue(name, out var candidates))
            {
                unmatched.Add(name);
                continue;
            }
            if (candidates.Length != 1 || oldGroups[name] != 1 || !used.Add(candidates[0]))
            {
                unmatched.Add(name);
                ambiguous.Add(name);
                continue;
            }
            aliases.Add(name, candidates[0]);
        }
        return new(aliases, unmatched, ambiguous);
    }
}
