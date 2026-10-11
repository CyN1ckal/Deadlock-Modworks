using System.Numerics;

namespace DeadlockModelUpdater;

public sealed record BoneShape(string Name, string? Parent, Vector3 Position, Quaternion Rotation, float Scale);

public sealed record SkeletonAssessment(bool NeedsReview, string Detail, IReadOnlyList<string> Warnings);

/// <summary>Conservative two-way comparison. It never guesses bone renames or author intent.</summary>
public static class SkeletonCompatibility
{
    public static SkeletonAssessment Assess(IReadOnlyList<BoneShape> mod, IReadOnlyList<BoneShape> game,
        bool usesExternalAnimationSkeleton = false)
    {
        Validate(mod, "Mod");
        Validate(game, "Game");
        var old = mod.ToDictionary(b => b.Name, StringComparer.Ordinal);
        var current = game.ToDictionary(b => b.Name, StringComparer.Ordinal);
        var added = game.Where(b => !old.ContainsKey(b.Name)).Select(b => b.Name).ToArray();
        var extra = mod.Where(b => !current.ContainsKey(b.Name)).Select(b => b.Name).ToArray();
        var shared = game.Where(b => old.ContainsKey(b.Name)).ToArray();
        var parents = shared.Where(b => b.Parent != old[b.Name].Parent).Select(b => b.Name).ToArray();
        var poses = shared.Where(b => !SamePose(b, old[b.Name])).Select(b => b.Name).ToArray();
        var reasons = new List<string>();
        var warnings = new List<string>();
        if (shared.Length == 0)
            reasons.Add("No shared bones; a different rig or renamed skeleton needs author review.");
        if (parents.Length > 0)
        {
            var note = $"Authored hierarchy differs ({parents.Length}): {Examples(parents)}. Mod parents are preserved.";
            if (usesExternalAnimationSkeleton) warnings.Add(note);
            else reasons.Add(note);
        }
        if (poses.Length > 0)
            warnings.Add($"Custom rest poses ({poses.Length}): {Examples(poses)}. Mod transforms are preserved; this is not evidence of a broken mod.");
        if (added.Length > 0 && extra.Length > 0)
        {
            if (usesExternalAnimationSkeleton)
                warnings.Add("Stock-only bones and custom helpers differ. The render skeleton does not need to copy the stock mesh's bone list.");
            else
                reasons.Add("An embedded/legacy skeleton has both missing and extra bones; a rename mapping cannot be inferred safely.");
        }
        var summary = $"Game-only bones: {added.Length}; mod-only bones: {extra.Length}.";
        if (added.Length > 0) summary += $" Game-only examples: {Examples(added)}.";
        if (extra.Length > 0) summary += $" Mod-only examples: {Examples(extra)}.";
        return new SkeletonAssessment(reasons.Count > 0, string.Join(" ", reasons.Append(summary)), warnings);
    }

    static string Examples(IEnumerable<string> names) => string.Join(", ", names.Take(6));

    static bool SamePose(BoneShape a, BoneShape b) =>
        Vector3.Distance(a.Position, b.Position) <= 0.001f &&
        Math.Abs(Quaternion.Dot(Quaternion.Normalize(a.Rotation), Quaternion.Normalize(b.Rotation))) >= 0.99999f &&
        Math.Abs(a.Scale - b.Scale) <= 0.0001f;

    static void Validate(IReadOnlyList<BoneShape> bones, string label)
    {
        var byName = new Dictionary<string, BoneShape>(StringComparer.Ordinal);
        foreach (var bone in bones)
        {
            if (string.IsNullOrWhiteSpace(bone.Name) || !byName.TryAdd(bone.Name, bone))
                throw new InvalidDataException($"{label} has an empty or duplicate bone name.");
            if (!float.IsFinite(bone.Position.X) || !float.IsFinite(bone.Position.Y) || !float.IsFinite(bone.Position.Z)
                || !float.IsFinite(bone.Rotation.LengthSquared()) || bone.Rotation.LengthSquared() < 0.000001f
                || !float.IsFinite(bone.Scale) || bone.Scale <= 0)
                throw new InvalidDataException($"{label} bone {bone.Name} has an invalid transform.");
        }
        foreach (var bone in bones)
        {
            var seen = new HashSet<string>(StringComparer.Ordinal) { bone.Name };
            var parent = bone.Parent;
            while (parent != null)
            {
                if (!byName.TryGetValue(parent, out var ancestor))
                    throw new InvalidDataException($"{label} bone {bone.Name} has a missing parent {parent}.");
                if (!seen.Add(parent))
                    throw new InvalidDataException($"{label} skeleton contains a parent cycle at {parent}.");
                parent = ancestor.Parent;
            }
        }
    }
}
