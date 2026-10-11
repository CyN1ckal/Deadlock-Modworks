using System.Text;
using ValveKeyValue;
using ValveResourceFormat;
using ValveResourceFormat.ResourceTypes;
using ValveResourceFormat.ResourceTypes.ModelAnimation;

namespace DeadlockModelUpdater;

public enum ModelOutcome
{
    Repairable,
    NeedsReview,
    Updated,
    Current,
    Skipped,
    Failed,
}

public sealed record ModelResult(string Model, ModelOutcome Outcome, string Detail);

public sealed record UpdateProgress(int Completed, int Total, string Message);

public sealed record UpdateReport(IReadOnlyList<ModelResult> Results, bool WroteFile, string? OutputPath, string? BackupPath)
{
    public bool HasFailures => Results.Any(result => result.Outcome == ModelOutcome.Failed);
    public bool HasBlockers => Results.Any(result => result.Outcome is ModelOutcome.Failed or ModelOutcome.NeedsReview or ModelOutcome.Skipped);
    public bool CanRepair => !HasBlockers && Results.Any(result => result.Outcome == ModelOutcome.Repairable);
}

/// <summary>
/// Rewrites every hero model in a mod VPK onto the current game skeleton.
/// Game bones stay in official order. Bones that exist only in the mod are appended.
/// </summary>
public static class HeroModelUpdater
{
    public static UpdateReport Analyze(string gameDirectory, string modVpkPath,
        IProgress<UpdateProgress>? progress = null, CancellationToken cancellationToken = default) =>
        Update(gameDirectory, modVpkPath, null, false, false, progress, cancellationToken, analyzeOnly: true);

    public static UpdateReport Update(
        string gameDirectory,
        string modVpkPath,
        string? outputPath,
        bool replaceSource,
        bool makeBackup,
        IProgress<UpdateProgress>? progress,
        CancellationToken cancellationToken = default,
        bool analyzeOnly = false)
    {
        if (string.IsNullOrWhiteSpace(gameDirectory))
            throw new ArgumentException("Game folder is required.", nameof(gameDirectory));
        if (string.IsNullOrWhiteSpace(modVpkPath))
            throw new ArgumentException("Mod VPK is required.", nameof(modVpkPath));
        if (!analyzeOnly && !replaceSource && string.IsNullOrWhiteSpace(outputPath))
            throw new ArgumentException("Output VPK is required.", nameof(outputPath));
        if (!analyzeOnly && replaceSource && !makeBackup)
            throw new InvalidOperationException("Replacing a mod requires a backup. Enable backups or write to a new file.");

        var gamePak = Path.GetFullPath(Path.Combine(gameDirectory, "pak01_dir.vpk"));
        var modFull = Path.GetFullPath(modVpkPath);
        if (!File.Exists(gamePak))
            throw new FileNotFoundException("Could not find pak01_dir.vpk in the game folder.", gamePak);
        if (!File.Exists(modFull))
            throw new FileNotFoundException("Could not find the mod VPK.", modFull);
        if (string.Equals(modFull, gamePak, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("That file is the game archive. Choose a mod VPK from the addons folder.");
        var gameFolder = Path.GetDirectoryName(gamePak)!;
        if (string.Equals(Path.GetDirectoryName(modFull), gameFolder, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Choose a mod outside the game's archive folder, such as a VPK in addons.");

        string? destination = null;
        if (!analyzeOnly && !replaceSource)
        {
            destination = Path.GetFullPath(outputPath!);
            if (string.Equals(Path.GetDirectoryName(destination), gameFolder, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Output must not be placed beside the official game archives.");
            if (string.Equals(destination, gamePak, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Refusing to overwrite the game archive.");
            if (string.Equals(destination, modFull, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The output VPK has to be a different file from the mod. The tool reads the mod while it writes.");
        }

        progress?.Report(new UpdateProgress(0, 0, "Reading the game archive..."));
        cancellationToken.ThrowIfCancellationRequested();
        var game = VpkArchive.Open(gamePak);
        using var gameLease = game.LockSources();
        var gameHeroes = new List<string>();
        foreach (var entry in game.Entries)
        {
            if (HeroPaths.IsHeroModel(entry.FullPath))
                gameHeroes.Add(entry.FullPath);
        }

        var exact = new Dictionary<string, string>(gameHeroes.Count, StringComparer.OrdinalIgnoreCase);
        foreach (var path in gameHeroes)
            exact.TryAdd(path, path);

        cancellationToken.ThrowIfCancellationRequested();
        var mod = VpkArchive.Open(modFull);
        using var modLease = mod.LockSources();
        progress?.Report(new UpdateProgress(0, 0, "Checking mod archive integrity..."));
        mod.Validate(cancellationToken);
        var modHeroes = mod.Entries.Where(entry => HeroPaths.IsHeroModel(entry.FullPath)).ToList();
        var availableResources = game.Entries.Concat(mod.Entries).Select(e => e.FullPath)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        progress?.Report(new UpdateProgress(0, modHeroes.Count, $"Game hero models: {gameHeroes.Count}. Mod hero models: {modHeroes.Count}."));

        var plans = new List<Plan>(modHeroes.Count);
        var seenSource = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in modHeroes)
        {
            if (!seenSource.Add(entry.FullPath))
            {
                plans.Add(new Plan(entry, null, "Duplicate path in the mod VPK."));
                continue;
            }

            var resolved = HeroPaths.Resolve(entry.FullPath, exact, gameHeroes, out var error);
            plans.Add(new Plan(entry, resolved, resolved == null ? error : null));
        }

        var edits = new MigrationEdit?[plans.Count];
        for (var i = 0; i < plans.Count; i++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var plan = plans[i];
            if (plan.Error != null || plan.Resolved == null)
                continue;

            progress?.Report(new UpdateProgress(i, plans.Count, $"Checking {plan.Entry.FullPath}..."));
            var gameEntry = FindEntry(game, plan.Resolved);
            try
            {
                edits[i] = ModelMigration.Migrate(mod.Read(plan.Entry), plan.Entry.FullPath, game.Read(gameEntry), plan.Resolved);
                if (edits[i]!.Outcome is ModelOutcome.Updated or ModelOutcome.Current)
                {
                    using var resource = ModelFile.Load(mod.Read(plan.Entry), plan.Entry.FullPath);
                    var missing = resource.Resource.ExternalReferences?.ResourceRefInfoList
                        .Select(reference => reference.Name)
                        .Where(IsRigReference)
                        .Where(path => !availableResources.Contains(VpkArchive.Normalize(path.EndsWith("_c", StringComparison.Ordinal) ? path : path + "_c")))
                        .Distinct().ToArray() ?? [];
                    if (missing.Length > 0)
                        edits[i] = new MigrationEdit(null, ModelOutcome.NeedsReview,
                            $"Rig dependencies are missing from the game and this mod: {string.Join(", ", missing.Take(6))}. The mod may depend on another addon or need author repair.");
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or ArgumentException)
            {
                edits[i] = new MigrationEdit(null, ModelOutcome.Failed, ex.Message);
            }
        }

        AcceptEdits(plans, edits, mod.Entries);

        var results = new List<ModelResult>(plans.Count);
        var changes = new Dictionary<string, byte[]?>(StringComparer.OrdinalIgnoreCase);
        var writtenTo = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < plans.Count; i++)
        {
            var plan = plans[i];
            var edit = edits[i];
            if (edit?.Bytes != null && plan.Error == null && plan.Resolved != null)
            {
                var pathMoved = !plan.Resolved.Equals(plan.Entry.FullPath, StringComparison.OrdinalIgnoreCase);
                var destinationPath = pathMoved ? plan.Resolved : plan.Entry.FullPath;
                writtenTo.Add(destinationPath);
            }
        }

        for (var i = 0; i < plans.Count; i++)
        {
            var plan = plans[i];
            var edit = edits[i];
            ModelResult result;
            if (plan.Error != null || plan.Resolved == null)
            {
                result = new ModelResult(plan.Entry.FullPath, ModelOutcome.NeedsReview, plan.Error ?? "No matching game model.");
            }
            else if (edit == null)
            {
                result = new ModelResult(plan.Entry.FullPath, ModelOutcome.Failed, "The model was not processed.");
            }
            else
            {
                result = new ModelResult(plan.Entry.FullPath,
                    edit.Outcome == ModelOutcome.Updated ? ModelOutcome.Repairable : edit.Outcome, edit.Detail);
                if (edit.Bytes != null)
                {
                    var pathMoved = !plan.Resolved.Equals(plan.Entry.FullPath, StringComparison.OrdinalIgnoreCase);
                    var destinationPath = pathMoved ? plan.Resolved : plan.Entry.FullPath;
                    if (pathMoved && !writtenTo.Contains(plan.Entry.FullPath))
                        changes[plan.Entry.FullPath] = null;
                    changes[destinationPath] = edit.Bytes;
                }
            }

            results.Add(result);
            progress?.Report(new UpdateProgress(i + 1, plans.Count, $"{result.Model}: {OutcomeText(result.Outcome)}. {result.Detail}"));
        }

        if (modHeroes.Count == 0)
            results.Add(new ModelResult(Path.GetFileName(modFull), ModelOutcome.Skipped,
                "No supported hero models. This MVP cannot assess this mod."));

        var analysis = new UpdateReport(results, false, null, null);
        if (analyzeOnly || analysis.HasBlockers)
        {
            progress?.Report(new UpdateProgress(plans.Count, plans.Count,
                analysis.HasBlockers ? "Repair blocked. Resolve the reported issues first; no files were written." : "Analysis complete. No files were written."));
            return analysis;
        }

        if (changes.Count == 0)
        {
            var idle = modHeroes.Count == 0
                ? "No hero models in this VPK. Nothing was written."
                : "No hero models needed an update. The VPK was not modified.";
            progress?.Report(new UpdateProgress(plans.Count, plans.Count, idle));
            return new UpdateReport(results, false, null, null);
        }

        cancellationToken.ThrowIfCancellationRequested();
        var finalPath = replaceSource ? modFull : destination!;
        progress?.Report(new UpdateProgress(plans.Count, plans.Count, "Writing and validating the repaired VPK..."));
        var backup = ArchiveTransaction.Write(mod, modFull, finalPath, changes, replaceSource, cancellationToken, modLease.Dispose);
        var completed = results.Select(result => result.Outcome == ModelOutcome.Repairable
            ? result with { Outcome = ModelOutcome.Updated } : result).ToArray();
        progress?.Report(new UpdateProgress(plans.Count, plans.Count,
            backup == null ? $"Wrote {finalPath}." : $"Repaired VPK. Original backup: {backup}"));
        return new UpdateReport(completed, true, finalPath, backup);
    }

    static bool IsRigReference(string path)
    {
        if (path.EndsWith("_c", StringComparison.Ordinal)) path = path[..^2];
        return Path.GetExtension(path).ToLowerInvariant() is ".vmdl" or ".vagrp" or ".vanim" or ".vanmgrph"
            or ".vnmskel" or ".vnmclip" or ".vnmgraph" or ".vphys";
    }

    static void AcceptEdits(List<Plan> plans, MigrationEdit?[] edits, List<VpkEntry> entries)
    {
        var owners = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (var pass = 0; pass < 2; pass++)
        {
            for (var i = 0; i < plans.Count; i++)
            {
                var plan = plans[i];
                if (edits[i]?.Bytes == null || plan.Resolved == null || plan.Error != null)
                    continue;
                var moved = !plan.Resolved.Equals(plan.Entry.FullPath, StringComparison.OrdinalIgnoreCase);
                if ((pass == 0 && moved) || (pass == 1 && !moved))
                    continue;
                if (!owners.TryAdd(plan.Resolved, i))
                    plan.Error = $"Another model in this VPK already uses {plan.Resolved}.";
            }
        }

        var movingAway = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < plans.Count; i++)
        {
            var plan = plans[i];
            if (edits[i]?.Bytes == null || plan.Resolved == null || plan.Error != null)
                continue;
            if (!plan.Resolved.Equals(plan.Entry.FullPath, StringComparison.OrdinalIgnoreCase))
                movingAway.Add(plan.Entry.FullPath);
        }

        for (var i = 0; i < plans.Count; i++)
        {
            var plan = plans[i];
            if (edits[i]?.Bytes == null || plan.Resolved == null || plan.Error != null)
                continue;
            if (plan.Resolved.Equals(plan.Entry.FullPath, StringComparison.OrdinalIgnoreCase))
                continue;
            foreach (var entry in entries)
            {
                if (!entry.FullPath.Equals(plan.Resolved, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (entry.FullPath.Equals(plan.Entry.FullPath, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (movingAway.Contains(entry.FullPath))
                    continue;
                plan.Error = $"{plan.Resolved} is already used by {entry.FullPath}.";
                movingAway.Remove(plan.Entry.FullPath);
                break;
            }
        }
    }

    static VpkEntry FindEntry(VpkArchive archive, string path)
    {
        foreach (var entry in archive.Entries)
        {
            if (entry.FullPath.Equals(path, StringComparison.OrdinalIgnoreCase))
                return entry;
        }

        throw new InvalidDataException($"Game archive is missing {path}.");
    }

    static string OutcomeText(ModelOutcome outcome) => outcome switch
    {
        ModelOutcome.Repairable => "Repairable",
        ModelOutcome.NeedsReview => "Needs review",
        ModelOutcome.Updated => "Updated",
        ModelOutcome.Current => "Current",
        ModelOutcome.Skipped => "Skipped",
        _ => "Failed",
    };

    sealed class Plan(VpkEntry entry, string? resolved, string? error)
    {
        public VpkEntry Entry { get; } = entry;
        public string? Resolved { get; } = resolved;
        public string? Error { get; set; } = error;
    }
}

static class HeroPaths
{
    public static bool IsHeroModel(string path)
    {
        if (!path.EndsWith(".vmdl_c", StringComparison.OrdinalIgnoreCase))
            return false;

        foreach (var part in path.Split('/'))
        {
            if (part.Equals("heroes", StringComparison.OrdinalIgnoreCase))
                return true;
            if (part.StartsWith("heroes_", StringComparison.OrdinalIgnoreCase))
                return true;
        }

        return false;
    }

    public static string? Resolve(string modPath, Dictionary<string, string> exact, IReadOnlyList<string> gamePaths, out string error)
    {
        error = "";
        var norm = VpkArchive.Normalize(modPath);
        if (exact.TryGetValue(norm, out var canonical))
            return canonical;

        var parts = norm.Split('/');
        for (var segments = parts.Length - 1; segments >= 1; segments--)
        {
            var suffix = string.Join('/', parts[^segments..]);
            string? match = null;
            var count = 0;
            foreach (var path in gamePaths)
            {
                if (path.Equals(suffix, StringComparison.OrdinalIgnoreCase)
                    || path.EndsWith("/" + suffix, StringComparison.OrdinalIgnoreCase))
                {
                    count++;
                    match = path;
                    if (count > 1)
                        break;
                }
            }

            if (count == 1)
                return match;
            if (count > 1)
            {
                error = $"Ambiguous: {count} game models end with {suffix}.";
                return null;
            }
        }

        error = "No matching model in the current game.";
        return null;
    }
}

static class ModelMigration
{
    public static MigrationEdit Migrate(byte[] modBytes, string modPath, byte[] gameBytes, string gamePath)
    {
        try
        {
            return MigrateCore(modBytes, modPath, gameBytes, gamePath);
        }
        catch (Exception ex)
        {
            var message = ex is InvalidDataException && !string.IsNullOrWhiteSpace(ex.Message)
                ? ex.Message
                : ex.ToString();
            return new MigrationEdit(null, ModelOutcome.Failed, message);
        }
    }

    static MigrationEdit MigrateCore(byte[] modBytes, string modPath, byte[] gameBytes, string gamePath)
    {
        using var mod = ModelFile.Load(modBytes, modPath);
        using var game = ModelFile.Load(gameBytes, gamePath);
        ValidateRawSkeleton(mod);
        ValidateRawSkeleton(game);
        var modBones = mod.Model.Skeleton.Bones;
        var gameBones = game.Model.Skeleton.Bones;
        if (modBones.Length == 0)
            return new MigrationEdit(null, ModelOutcome.Skipped, "No bones.");
        if (gameBones.Length == 0)
            throw new InvalidDataException("The game model has no bones.");

        var modNames = new string[modBones.Length];
        var modParents = new string?[modBones.Length];
        var modIndex = new Dictionary<string, int>(modBones.Length, StringComparer.Ordinal);
        for (var i = 0; i < modBones.Length; i++)
        {
            modNames[i] = modBones[i].Name;
            modParents[i] = modBones[i].Parent?.Name;
            if (!modIndex.TryAdd(modNames[i], i))
                throw new InvalidDataException($"Duplicate bone name {modNames[i]}.");
        }

        var gameNames = new string[gameBones.Length];
        var gameParents = new string?[gameBones.Length];
        var gameIndex = new Dictionary<string, int>(gameBones.Length, StringComparer.Ordinal);
        for (var i = 0; i < gameBones.Length; i++)
        {
            gameNames[i] = gameBones[i].Name;
            gameParents[i] = gameBones[i].Parent?.Name;
            if (!gameIndex.TryAdd(gameNames[i], i))
                throw new InvalidDataException($"The game model has a duplicate bone name {gameNames[i]}.");
        }

        var pathMoved = !modPath.Equals(gamePath, StringComparison.OrdinalIgnoreCase);
        if (pathMoved)
            return new MigrationEdit(null, ModelOutcome.NeedsReview,
                $"Possible model move to {gamePath}. Path migration requires checking dependent resources; this MVP will not guess.");

        var assessment = SkeletonCompatibility.Assess(ReadShapes(mod), ReadShapes(game));
        if (assessment.NeedsReview)
            return new MigrationEdit(null, ModelOutcome.NeedsReview, assessment.Detail);
        var flagChanges = gameBones.Where(b => modIndex.TryGetValue(b.Name, out var i) && b.Flags != modBones[i].Flags)
            .Select(b => b.Name).ToArray();
        if (flagChanges.Length > 0)
            return new MigrationEdit(null, ModelOutcome.NeedsReview,
                $"Bone usage or procedural flags differ ({flagChanges.Length}): {string.Join(", ", flagChanges.Take(6))}. These changes need author review.");
        var aligned = modBones.Length >= gameBones.Length;
        if (aligned)
        {
            for (var i = 0; i < gameBones.Length; i++)
            {
                if (!string.Equals(modNames[i], gameNames[i], StringComparison.Ordinal))
                {
                    aligned = false;
                    break;
                }
            }
        }

        if (aligned && !pathMoved)
            return new MigrationEdit(null, ModelOutcome.Current,
                "No supported skeleton differences found. In-game animation is not verified. " + assessment.Detail);

        if (aligned)
            return MoveWithoutSkeletonChange(mod, game, modBytes, modNames, modParents, gamePath);

        KVObject modRoot = ReadData(mod).Data;
        if (!modRoot.ContainsKey("m_remappingTable") || modRoot["m_remappingTable"].Count == 0)
            return new MigrationEdit(null, ModelOutcome.NeedsReview,
                "Skeleton differs, but no mesh bone remap table is available. Direct mesh indices require author review.");
        var supportedSkeletonKeys = new HashSet<string>(StringComparer.Ordinal)
        {
            "m_boneName", "m_nParent", "m_boneSphere", "m_nFlag", "m_bonePosParent", "m_boneRotParent", "m_boneScaleParent",
        };
        var unknownSkeletonKeys = modRoot["m_modelSkeleton"].Keys.Where(key => !supportedSkeletonKeys.Contains(key)).ToArray();
        if (unknownSkeletonKeys.Length > 0)
            return new MigrationEdit(null, ModelOutcome.NeedsReview,
                $"Skeleton contains unsupported metadata: {string.Join(", ", unknownSkeletonKeys)}. Its bone references need author review.");

        // These blocks may contain additional bone indices. Preserve them, but do not
        // change the skeleton underneath them until their remapping is supported.
        var unsupported = new[] { BlockType.PHYS, BlockType.ANIM, BlockType.ASEQ, BlockType.AGRP }
            .Where(type => mod.Resource.GetBlockByType(type) is { Size: > 0 }).ToArray();
        if (unsupported.Length > 0)
            return new MigrationEdit(null, ModelOutcome.NeedsReview,
                $"Skeleton differs and the model has embedded {string.Join(", ", unsupported)} data. Its bone references need author review.");

        var edit = RewriteSkeleton(mod, game, modBytes, modNames, modParents, modIndex, gameNames, gameParents, gameIndex, gamePath, pathMoved);
        return edit with { Detail = edit.Detail + " " + assessment.Detail + " Test animations and attachments in-game before keeping the repair." };
    }

    static BoneShape[] ReadShapes(ModelFile file)
    {
        var root = ReadData(file).Data;
        var bones = file.Model.Skeleton.Bones;
        var scales = RequireArray(root["m_modelSkeleton"], "m_boneScaleParent", bones.Length, "Model");
        return bones.Select((b, i) => new BoneShape(b.Name, b.Parent?.Name, b.Position, b.Angle,
            (float)scales[i].ToDouble())).ToArray();
    }

    static void ValidateRawSkeleton(ModelFile file)
    {
        KVObject root = ReadData(file).Data;
        if (!root.ContainsKey("m_modelSkeleton"))
            throw new InvalidDataException("Model has no supported embedded skeleton.");
        var skeleton = root["m_modelSkeleton"];
        if (!skeleton.ContainsKey("m_boneName") || !skeleton["m_boneName"].IsArray)
            throw new InvalidDataException("Skeleton has no bone-name array.");
        var names = skeleton["m_boneName"];
        var parents = RequireArray(skeleton, "m_nParent", names.Count, "Model");
        var unique = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < names.Count; i++)
        {
            var name = names[i].ToString();
            if (string.IsNullOrWhiteSpace(name) || !unique.Add(name))
                throw new InvalidDataException("Skeleton has an empty or duplicate bone name.");
            var visited = new HashSet<int> { i };
            var parent = IntOf(parents[i]);
            while (parent != -1)
            {
                if (parent < 0 || parent >= names.Count)
                    throw new InvalidDataException($"Bone {name} has an out-of-range parent index.");
                if (!visited.Add(parent))
                    throw new InvalidDataException($"Skeleton has a parent cycle at {name}.");
                parent = IntOf(parents[parent]);
            }
        }
        foreach (var key in new[] { "m_nFlag", "m_boneSphere", "m_bonePosParent", "m_boneRotParent", "m_boneScaleParent" })
            RequireArray(skeleton, key, names.Count, "Model");
        if (root.ContainsKey("m_remappingTable"))
        {
            var remap = root["m_remappingTable"];
            if (!remap.IsArray) throw new InvalidDataException("Mesh remapping table is not an array.");
            for (var i = 0; i < remap.Count; i++)
            {
                var value = IntOf(remap[i]);
                if (value < -1 || value >= names.Count)
                    throw new InvalidDataException($"Mesh remapping entry {i} points outside the skeleton.");
            }
        }
    }

    static MigrationEdit MoveWithoutSkeletonChange(
        ModelFile mod,
        ModelFile game,
        byte[] modBytes,
        string[] modNames,
        string?[] modParents,
        string gamePath)
    {
        var modKv = ReadData(mod);
        var gameKv = ReadData(game);
        var gameName = RequiredName(gameKv);
        var modName = RequiredName(modKv);
        if (string.Equals(modName, gameName, StringComparison.Ordinal))
        {
            return new MigrationEdit(
                modBytes,
                ModelOutcome.Updated,
                $"Already matches the current game skeleton. The file moves to {gamePath}.");
        }

        SetName(modKv, gameName);
        var patched = Splice(modBytes, modKv);
        VerifySameSkeleton(patched, mod.Resource.FileName ?? "model.vmdl_c", modNames, modParents);
        return new MigrationEdit(patched, ModelOutcome.Updated, $"Skeleton already matched. Model name updated. Saved as {gamePath}.");
    }

    static MigrationEdit RewriteSkeleton(
        ModelFile mod,
        ModelFile game,
        byte[] modBytes,
        string[] modNames,
        string?[] modParents,
        Dictionary<string, int> modIndex,
        string[] gameNames,
        string?[] gameParents,
        Dictionary<string, int> gameIndex,
        string gamePath,
        bool pathMoved)
    {
        var modKv = ReadData(mod);
        var gameKv = ReadData(game);
        KVObject modRoot = modKv.Data;
        KVObject gameRoot = gameKv.Data;
        if (!modRoot.ContainsKey("m_modelSkeleton") || !gameRoot.ContainsKey("m_modelSkeleton"))
            throw new InvalidDataException("Model is missing m_modelSkeleton.");

        var modSkel = modRoot["m_modelSkeleton"];
        var gameSkel = gameRoot["m_modelSkeleton"];
        var modBoneNames = RequireArray(modSkel, "m_boneName", modNames.Length, "Mod");
        var oldParents = RequireArray(modSkel, "m_nParent", modNames.Length, "Mod");
        var oldSphere = RequireArray(modSkel, "m_boneSphere", modNames.Length, "Mod");
        var oldFlag = RequireArray(modSkel, "m_nFlag", modNames.Length, "Mod");
        var oldPos = RequireArray(modSkel, "m_bonePosParent", modNames.Length, "Mod");
        var oldRot = RequireArray(modSkel, "m_boneRotParent", modNames.Length, "Mod");
        var oldScale = RequireArray(modSkel, "m_boneScaleParent", modNames.Length, "Mod");
        var gameSphere = RequireArray(gameSkel, "m_boneSphere", gameNames.Length, "Game");
        var gameFlag = RequireArray(gameSkel, "m_nFlag", gameNames.Length, "Game");
        var gamePos = RequireArray(gameSkel, "m_bonePosParent", gameNames.Length, "Game");
        var gameRot = RequireArray(gameSkel, "m_boneRotParent", gameNames.Length, "Game");
        var gameScale = RequireArray(gameSkel, "m_boneScaleParent", gameNames.Length, "Game");
        var gameParentValues = RequireArray(gameSkel, "m_nParent", gameNames.Length, "Game");
        _ = modBoneNames;

        // Official bones occupy 0..gameCount-1, so a game parent index is already the new index.
        // Mod-only helpers stay after them and do not shift the game's bone numbers.
        var newNames = new List<string>(gameNames.Length + 64);
        newNames.AddRange(gameNames);
        var modOnly = new List<int>();
        for (var i = 0; i < modNames.Length; i++)
        {
            if (gameIndex.ContainsKey(modNames[i]))
                continue;
            modOnly.Add(i);
            newNames.Add(modNames[i]);
        }

        var newIndexOfOld = new int[modNames.Length];
        for (var i = 0; i < newNames.Count; i++)
        {
            if (modIndex.TryGetValue(newNames[i], out var old))
                newIndexOfOld[old] = i;
        }

        for (var i = 0; i < modNames.Length; i++)
        {
            if (!string.Equals(newNames[newIndexOfOld[i]], modNames[i], StringComparison.Ordinal))
                throw new InvalidDataException($"Bone {modNames[i]} was not placed in the new skeleton.");
        }

        KVObject Take(KVObject modArray, KVObject gameArray, int newIndex)
        {
            var name = newNames[newIndex];
            if (modIndex.TryGetValue(name, out var old))
                return modArray[old];
            return gameArray[gameIndex[name]];
        }

        int NewParent(int newIndex)
        {
            var name = newNames[newIndex];
            if (modIndex.TryGetValue(name, out var old))
            {
                var parent = IntOf(oldParents[old]);
                if (parent < 0)
                    return -1;
                if ((uint)parent >= (uint)newIndexOfOld.Length)
                    throw new InvalidDataException($"Bone {name} has parent index {parent} outside the skeleton.");
                return newIndexOfOld[parent];
            }

            var gameParent = IntOf(gameParentValues[gameIndex[name]]);
            if (gameParent < 0)
                return -1;
            if ((uint)gameParent >= (uint)gameNames.Length)
                throw new InvalidDataException($"Game bone {name} has parent index {gameParent} outside the skeleton.");
            var parentName = gameNames[gameParent];
            if (!gameIndex.ContainsKey(parentName))
                throw new InvalidDataException($"Game bone {name} parent {parentName} is missing.");
            return gameIndex[parentName];
        }

        Replace(modSkel, "m_boneName", KVObject.Array(newNames.Select(name => new KVObject(name))));
        Replace(modSkel, "m_nParent", KVObject.Array(Enumerable.Range(0, newNames.Count).Select(index => new KVObject(NewParent(index)))));
        Replace(modSkel, "m_boneSphere", KVObject.Array(Enumerable.Range(0, newNames.Count).Select(index => Take(oldSphere, gameSphere, index))));
        Replace(modSkel, "m_nFlag", KVObject.Array(Enumerable.Range(0, newNames.Count).Select(index => Take(oldFlag, gameFlag, index))));
        Replace(modSkel, "m_bonePosParent", KVObject.Array(Enumerable.Range(0, newNames.Count).Select(index => Take(oldPos, gamePos, index))));
        Replace(modSkel, "m_boneRotParent", KVObject.Array(Enumerable.Range(0, newNames.Count).Select(index => Take(oldRot, gameRot, index))));
        Replace(modSkel, "m_boneScaleParent", KVObject.Array(Enumerable.Range(0, newNames.Count).Select(index => Take(oldScale, gameScale, index))));

        var hadRemap = modRoot.ContainsKey("m_remappingTable");
        var oldRemapCount = 0;
        if (hadRemap)
        {
            var oldRemap = modRoot["m_remappingTable"];
            if (!oldRemap.IsArray)
                throw new InvalidDataException("m_remappingTable is not an array.");
            oldRemapCount = oldRemap.Count;
            var newRemap = new KVObject[oldRemapCount];
            for (var i = 0; i < oldRemapCount; i++)
            {
                var value = IntOf(oldRemap[i]);
                if (value < 0)
                    newRemap[i] = new KVObject(value);
                else if ((uint)value >= (uint)newIndexOfOld.Length)
                    throw new InvalidDataException($"Remap entry {i} points at bone {value}, which is outside the skeleton.");
                else
                    newRemap[i] = new KVObject(newIndexOfOld[value]);
            }

            Replace(modRoot, "m_remappingTable", KVObject.Array(newRemap));
        }

        if (pathMoved)
            SetName(modKv, RequiredName(gameKv));

        var patched = Splice(modBytes, modKv);
        VerifyRewritten(patched, gamePath, newNames, modOnly, modNames, modParents, gameNames, gameParents, modIndex, newIndexOfOld, hadRemap, oldRemapCount);

        var remapNote = hadRemap
            ? "Mesh bone indices were remapped."
            : "This model has no mesh bone remap table.";
        var move = pathMoved ? $" Saved as {gamePath}." : "";
        var detail = $"Bones {modNames.Length} to {newNames.Count} ({gameNames.Length} from the game, {modOnly.Count} kept from the mod). {remapNote}{move}";
        return new MigrationEdit(patched, ModelOutcome.Updated, detail);
    }

    static void VerifyRewritten(
        byte[] patched,
        string path,
        List<string> newNames,
        List<int> modOnly,
        string[] modNames,
        string?[] modParents,
        string[] gameNames,
        string?[] gameParents,
        Dictionary<string, int> modIndex,
        int[] newIndexOfOld,
        bool hadRemap,
        int oldRemapCount)
    {
        using var check = ModelFile.Load(patched, path);
        var bones = check.Model.Skeleton.Bones;
        if (bones.Length != newNames.Count)
            throw new InvalidDataException($"Verified bone count is {bones.Length}, expected {newNames.Count}.");

        for (var i = 0; i < gameNames.Length; i++)
        {
            if (bones[i].Name != gameNames[i])
                throw new InvalidDataException($"Bone {i} is {bones[i].Name}, expected {gameNames[i]}.");
            if (!modIndex.ContainsKey(gameNames[i]) && bones[i].Parent?.Name != gameParents[i])
                throw new InvalidDataException($"Parent of game bone {gameNames[i]} changed.");
        }

        for (var i = 0; i < modOnly.Count; i++)
        {
            var expect = modNames[modOnly[i]];
            if (bones[gameNames.Length + i].Name != expect)
                throw new InvalidDataException($"Mod-only bone {expect} was not kept in order.");
        }

        for (var i = 0; i < modNames.Length; i++)
        {
            var parent = bones[newIndexOfOld[i]].Parent?.Name;
            if (!string.Equals(parent, modParents[i], StringComparison.Ordinal))
                throw new InvalidDataException($"Parent of {modNames[i]} changed from {modParents[i] ?? "(none)"} to {parent ?? "(none)"}.");
        }

        if (!hadRemap)
            return;

        KVObject root = ReadData(check).Data;
        if (!root.ContainsKey("m_remappingTable"))
            throw new InvalidDataException("Remap table disappeared.");
        var remap = root["m_remappingTable"];
        if (remap.Count != oldRemapCount)
            throw new InvalidDataException("Remap table length changed.");
        for (var i = 0; i < remap.Count; i++)
        {
            var value = IntOf(remap[i]);
            if (value >= bones.Length)
                throw new InvalidDataException($"Remap entry {i} points at bone {value}, past the skeleton.");
        }
    }

    static void VerifySameSkeleton(byte[] patched, string path, string[] names, string?[] parents)
    {
        using var check = ModelFile.Load(patched, path);
        var bones = check.Model.Skeleton.Bones;
        if (bones.Length != names.Length)
            throw new InvalidDataException("Bone count changed while updating the model name.");
        for (var i = 0; i < names.Length; i++)
        {
            if (bones[i].Name != names[i])
                throw new InvalidDataException($"Bone {i} changed while updating the model name.");
            if (bones[i].Parent?.Name != parents[i])
                throw new InvalidDataException($"Parent of {names[i]} changed while updating the model name.");
        }
    }

    static byte[] Splice(byte[] modBytes, BinaryKV3 kv)
    {
        using var serialized = new MemoryStream();
        kv.Serialize(serialized);
        return ResourceData.ReplaceDataBlock(modBytes, serialized.ToArray());
    }

    static BinaryKV3 ReadData(ModelFile file)
    {
        var block = file.Resource.GetBlockByType(BlockType.DATA) ?? throw new InvalidDataException("Model has no DATA block.");
        var offset = checked((int)block.Offset);
        var size = checked((int)block.Size);
        if (offset < 0 || size < 0 || offset > file.Bytes.Length || size > file.Bytes.Length - offset)
            throw new InvalidDataException("DATA block is outside the model file.");

        var kv = new BinaryKV3(BlockType.DATA) { Resource = file.Resource };
        using var reader = new BinaryReader(new MemoryStream(file.Bytes, offset, size, writable: false), Encoding.UTF8, leaveOpen: true);
        kv.Read(reader);
        return kv;
    }

    static string RequiredName(BinaryKV3 kv)
    {
        KVObject root = kv.Data;
        if (!root.ContainsKey("m_name"))
            throw new InvalidDataException("Model is missing m_name.");
        var name = root["m_name"].ToString();
        if (string.IsNullOrEmpty(name))
            throw new InvalidDataException("Model m_name is empty.");
        return name;
    }

    static void SetName(BinaryKV3 kv, string name)
    {
        KVObject root = kv.Data;
        var value = new KVObject(name);
        if (root.ContainsKey("m_name"))
            root.Remove("m_name");
        root.Add("m_name", value);
    }

    static KVObject RequireArray(KVObject skeleton, string key, int count, string who)
    {
        if (!skeleton.ContainsKey(key))
            throw new InvalidDataException($"{who} skeleton is missing {key}.");
        var array = skeleton[key];
        if (!array.IsArray || array.Count != count)
            throw new InvalidDataException($"{who} {key} has {array.Count} items, expected {count}.");
        return array;
    }

    static void Replace(KVObject parent, string key, KVObject value)
    {
        parent.Remove(key);
        parent.Add(key, value);
    }

    static int IntOf(KVObject value) => checked((int)value.ToInt64());
}

sealed record MigrationEdit(byte[]? Bytes, ModelOutcome Outcome, string Detail);

sealed class ModelFile : IDisposable
{
    public required Resource Resource { get; init; }
    public required Model Model { get; init; }
    public required byte[] Bytes { get; init; }
    public required MemoryStream Stream { get; init; }

    public static ModelFile Load(byte[] bytes, string path)
    {
        var stream = new MemoryStream(bytes, writable: false);
        var resource = new Resource { FileName = path };
        try
        {
            resource.Read(stream, verifyFileSize: false, leaveOpen: true);
        }
        catch
        {
            resource.Dispose();
            stream.Dispose();
            throw;
        }

        if (resource.DataBlock is not Model model)
        {
            resource.Dispose();
            stream.Dispose();
            throw new InvalidDataException($"{path} is not a compiled model.");
        }

        return new ModelFile
        {
            Resource = resource,
            Model = model,
            Bytes = bytes,
            Stream = stream,
        };
    }

    public void Dispose()
    {
        Resource.Dispose();
        Stream.Dispose();
    }
}

