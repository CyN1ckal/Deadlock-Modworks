using System.Numerics;
using DeadlockModelUpdater;

if (args.Length == 4 && args[0] == "--ui-smoke")
{
    UiSmoke.Run(args[1], args[2], args[3]);
    return;
}
if (args.Length == 2 && args[0] == "--ui-repair-smoke")
{
    var smokeDirectory = Path.Combine(AppContext.BaseDirectory, "ui-fixtures-" + Guid.NewGuid().ToString("N"));
    var gameDirectory = Path.Combine(smokeDirectory, "game");
    var modPath = Path.Combine(smokeDirectory, "mod.vpk");
    try
    {
        Fixtures.Archive(Path.Combine(gameDirectory, "pak01_dir.vpk"), new()
            { [Fixtures.ModelPath] = Fixtures.Model(["root", "arm"], [-1, 0], [0, 1]) });
        Fixtures.Archive(modPath, new()
            { [Fixtures.ModelPath] = Fixtures.Model(["arm", "root"], [1, -1], [0, 1]) });
        UiSmoke.Run(gameDirectory, modPath, args[1], expectRepair: true);
    }
    finally { Directory.Delete(smokeDirectory, recursive: true); }
    return;
}

var passed = 0;
void Test(string name, Action check)
{
    check();
    Console.WriteLine("PASS " + name);
    passed++;
}
void Assert(bool condition)
{
    if (!condition) throw new Exception("Assertion failed");
}
void Invalid(Action action)
{
    try { action(); }
    catch (InvalidDataException) { return; }
    throw new Exception("Expected InvalidDataException");
}
BoneShape Bone(string name, string? parent = null) => new(name, parent, Vector3.Zero, Quaternion.Identity, 1);
var root = Bone("root");
var arm = Bone("arm", "root");
Test("Reordering is repairable", () => Assert(!SkeletonCompatibility.Assess([arm, root], [root, arm]).NeedsReview));
Test("Additions without ambiguous old bones are supported", () => Assert(!SkeletonCompatibility.Assess([root], [root, arm]).NeedsReview));
Test("Custom helper is preserved", () => Assert(!SkeletonCompatibility.Assess([root, arm], [root]).NeedsReview));
Test("Potential rename needs review", () => Assert(SkeletonCompatibility.Assess([root, arm], [root, Bone("new_arm", "root")]).NeedsReview));
Test("Parent change needs review even when names match", () => Assert(SkeletonCompatibility.Assess([root, arm], [root, arm with { Parent = null }]).NeedsReview));
Test("Rest-pose change needs review", () => Assert(SkeletonCompatibility.Assess([root, arm], [root, arm with { Position = Vector3.UnitX }]).NeedsReview));
Test("Equivalent quaternion signs match", () => Assert(!SkeletonCompatibility.Assess([root], [root with { Rotation = new Quaternion(0, 0, 0, -1) }]).NeedsReview));
Test("Scale change needs review", () => Assert(SkeletonCompatibility.Assess([root], [root with { Scale = 2 }]).NeedsReview));
Test("Duplicate bones fail", () => Invalid(() => SkeletonCompatibility.Assess([root, root], [root])));
Test("Missing parent fails", () => Invalid(() => SkeletonCompatibility.Assess([arm], [root])));
Test("Parent cycle fails", () => Invalid(() => SkeletonCompatibility.Assess([root with { Parent = "arm" }, arm], [root])));
Test("Non-finite transform fails", () => Invalid(() => SkeletonCompatibility.Assess([root with { Scale = float.NaN }], [root])));
Test("Review blocks an otherwise repairable package", () => Assert(!new UpdateReport([
    new("a", ModelOutcome.Repairable, ""), new("b", ModelOutcome.NeedsReview, "")], false, null, null).CanRepair));
Test("Unsupported mods are not called compatible", () => Assert(new UpdateReport([
    new("a", ModelOutcome.Skipped, "")], false, null, null).HasBlockers));

var fixtureDirectory = Path.Combine(AppContext.BaseDirectory, "fixtures-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(fixtureDirectory);
try
{
    var gameDirectory = Path.Combine(fixtureDirectory, "game");
    var gamePath = Path.Combine(gameDirectory, "pak01_dir.vpk");
    var modPath = Path.Combine(fixtureDirectory, "mod.vpk");
    var outputPath = Path.Combine(fixtureDirectory, "repaired.vpk");
    var gameModel = Fixtures.Model(["root", "arm"], [-1, 0], [0, 1]);
    var oldModel = Fixtures.Model(["arm", "root"], [1, -1], [0, 1]);
    Fixtures.Archive(gamePath, new() { [Fixtures.ModelPath] = gameModel });
    Fixtures.Archive(modPath, new() { [Fixtures.ModelPath] = oldModel, ["materials/custom.txt"] = [9, 8, 7] });
    var original = File.ReadAllBytes(modPath);
    Test("Analysis detects an actual model reorder without writing", () =>
    {
        var report = HeroModelUpdater.Analyze(gameDirectory, modPath);
        Assert(report.CanRepair && !report.WroteFile && File.ReadAllBytes(modPath).SequenceEqual(original));
    });
    Test("Repair writes a compatible model and preserves other assets", () =>
    {
        var report = HeroModelUpdater.Update(gameDirectory, modPath, outputPath, false, false, null);
        Assert(report.WroteFile && report.Results.Single().Outcome == ModelOutcome.Updated);
        var output = VpkArchive.Open(outputPath);
        output.Validate();
        Assert(output.Read(output.Entries.Single(e => e.FullPath == "materials/custom.txt")).SequenceEqual(new byte[] { 9, 8, 7 }));
        Assert(File.ReadAllBytes(modPath).SequenceEqual(original));
        Assert(HeroModelUpdater.Analyze(gameDirectory, outputPath).Results.Single().Outcome == ModelOutcome.Current);
    });
    Test("Repair remaps skinning to the same named bone", () =>
    {
        var archive = VpkArchive.Open(outputPath);
        using var resource = new ValveResourceFormat.Resource { FileName = Fixtures.ModelPath };
        resource.Read(new MemoryStream(archive.Read(archive.Entries.Single(e => e.FullPath == Fixtures.ModelPath))));
        ValveKeyValue.KVObject data = ((ValveResourceFormat.ResourceTypes.Model)resource.DataBlock!).Data;
        Assert(data["m_remappingTable"][0].ToInt64() == 1 && data["m_remappingTable"][1].ToInt64() == 0);
    });
    Test("Independent ValvePak reader verifies the repaired archive", () =>
    {
        using var package = new SteamDatabase.ValvePak.Package();
        package.Read(outputPath);
        package.VerifyHashes();
        package.VerifyFileChecksums();
        var material = package.FindEntry("materials/custom.txt");
        Assert(material != null);
        package.ReadEntry(material!, out byte[] contents);
        Assert(contents.SequenceEqual(new byte[] { 9, 8, 7 }));
    });
    Test("Replacement creates an exact original backup", () =>
    {
        var report = HeroModelUpdater.Update(gameDirectory, modPath, null, true, true, null);
        Assert(report.WroteFile && report.BackupPath != null && File.ReadAllBytes(report.BackupPath).SequenceEqual(original));
    });
    Test("One invalid model blocks the whole package", () =>
    {
        Fixtures.Archive(gamePath, new() { [Fixtures.ModelPath] = gameModel,
            ["models/heroes/test/broken.vmdl_c"] = gameModel });
        var mixedPath = Path.Combine(fixtureDirectory, "mixed.vpk");
        Fixtures.Archive(mixedPath, new() { [Fixtures.ModelPath] = oldModel, ["models/heroes/test/broken.vmdl_c"] = [1, 2, 3] });
        var before = File.ReadAllBytes(mixedPath);
        var report = HeroModelUpdater.Update(gameDirectory, mixedPath, null, true, true, null);
        Assert(report.HasFailures && !report.WroteFile && File.ReadAllBytes(mixedPath).SequenceEqual(before));
    });
    Test("An existing output is never overwritten", () =>
    {
        var before = File.ReadAllBytes(outputPath);
        var archive = VpkArchive.Open(modPath);
        try { ArchiveTransaction.Write(archive, modPath, outputPath, new Dictionary<string, byte[]?>(), false); }
        catch (IOException) { Assert(File.ReadAllBytes(outputPath).SequenceEqual(before)); return; }
        throw new Exception("Expected existing-output rejection");
    });
    Test("CRC corruption is rejected", () =>
    {
        var corruptPath = Path.Combine(fixtureDirectory, "corrupt.vpk");
        Fixtures.Archive(corruptPath, new() { ["a.txt"] = [1, 2, 3] });
        var bytes = File.ReadAllBytes(corruptPath);
        var dataStart = 28 + BitConverter.ToInt32(bytes, 8);
        bytes[dataStart] ^= 0xFF;
        File.WriteAllBytes(corruptPath, bytes);
        Invalid(() => VpkArchive.Open(corruptPath).Validate());
    });
    Test("Preload bytes survive repacking", () =>
    {
        var preloadPath = Path.Combine(fixtureDirectory, "preload.vpk");
        Fixtures.PreloadArchive(preloadPath);
        var preload = VpkArchive.Open(preloadPath);
        Assert(preload.Read(preload.Entries.Single()).SequenceEqual(new byte[] { 1, 2, 3, 4 }));
        var repackedPath = Path.Combine(fixtureDirectory, "preload-repacked.vpk");
        ArchiveTransaction.Write(preload, preloadPath, repackedPath, new Dictionary<string, byte[]?>(), false);
        var repacked = VpkArchive.Open(repackedPath);
        Assert(repacked.Read(repacked.Entries.Single()).SequenceEqual(new byte[] { 1, 2, 3, 4 }));
    });
    Test("Cancellation leaves the source untouched", () =>
    {
        var before = File.ReadAllBytes(modPath);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        try { ArchiveTransaction.Write(VpkArchive.Open(modPath), modPath, modPath, new Dictionary<string, byte[]?>(), true, cancellation.Token); }
        catch (OperationCanceledException) { Assert(File.ReadAllBytes(modPath).SequenceEqual(before)); return; }
        throw new Exception("Expected cancellation");
    });
    Test("Actual cyclic skeletons fail before building a hierarchy", () =>
    {
        var cyclePath = Path.Combine(fixtureDirectory, "cycle.vpk");
        Fixtures.Archive(cyclePath, new() { [Fixtures.ModelPath] = Fixtures.Model(["root", "arm"], [1, 0], [0, 1]) });
        var report = HeroModelUpdater.Analyze(gameDirectory, cyclePath);
        Assert(report.HasFailures && report.Results.Single().Detail.Contains("cycle"));
    });
    Test("Out-of-range mesh indices fail even when bone order matches", () =>
    {
        var badRemapPath = Path.Combine(fixtureDirectory, "bad-remap.vpk");
        Fixtures.Archive(badRemapPath, new() { [Fixtures.ModelPath] = Fixtures.Model(["root", "arm"], [-1, 0], [2]) });
        Assert(HeroModelUpdater.Analyze(gameDirectory, badRemapPath).HasFailures);
    });
    Test("Missing remap blocks an actual reorder", () =>
    {
        var directPath = Path.Combine(fixtureDirectory, "direct.vpk");
        Fixtures.Archive(directPath, new() { [Fixtures.ModelPath] = Fixtures.Model(["arm", "root"], [1, -1]) });
        var report = HeroModelUpdater.Analyze(gameDirectory, directPath);
        Assert(report.HasBlockers && report.Results.Single().Outcome == ModelOutcome.NeedsReview);
    });
    Test("Additive skeleton repair preserves existing skinning", () =>
    {
        var additivePath = Path.Combine(fixtureDirectory, "additive.vpk");
        Fixtures.Archive(additivePath, new() { [Fixtures.ModelPath] = Fixtures.Model(["root"], [-1], [0]) });
        var report = HeroModelUpdater.Update(gameDirectory, additivePath, null, true, true, null);
        Assert(report.WroteFile && HeroModelUpdater.Analyze(gameDirectory, additivePath).Results.Single().Outcome == ModelOutcome.Current);
    });
    Test("Failed copy never installs a partial archive", () =>
    {
        var corruptPath = Path.Combine(fixtureDirectory, "bad-copy.vpk");
        Fixtures.Archive(corruptPath, new() { ["a.txt"] = [1, 2, 3] });
        var bytes = File.ReadAllBytes(corruptPath);
        bytes[28 + BitConverter.ToInt32(bytes, 8)] ^= 0xFF;
        File.WriteAllBytes(corruptPath, bytes);
        Invalid(() => ArchiveTransaction.Write(VpkArchive.Open(corruptPath), corruptPath, corruptPath,
            new Dictionary<string, byte[]?>(), true));
        Assert(File.ReadAllBytes(corruptPath).SequenceEqual(bytes));
        Assert(Directory.GetFiles(fixtureDirectory, "*.new").Length == 0);
    });
    Test("CLI analysis and JSON report agree", () =>
    {
        var reportPath = Path.Combine(fixtureDirectory, "report.json");
        using var log = new StringWriter();
        Assert(CommandLine.Execute(["--analyze", gameDirectory, modPath, "--report", reportPath], log) == 0);
        using var json = System.Text.Json.JsonDocument.Parse(File.ReadAllText(reportPath));
        var archive = json.RootElement.GetProperty("Archives")[0];
        Assert(archive.GetProperty("Report").GetProperty("WroteFile").GetBoolean() == false);
        Assert(archive.GetProperty("Report").GetProperty("Results")[0].GetProperty("Outcome").GetString() == "Current");
    });
    Test("CLI unsupported mod exits with review status", () =>
    {
        var nonModelPath = Path.Combine(fixtureDirectory, "non-model.vpk");
        Fixtures.Archive(nonModelPath, new() { ["a.txt"] = [1] });
        Assert(CommandLine.Execute(["--analyze", gameDirectory, nonModelPath], new StringWriter()) == 1);
    });
    Test("CLI errors can be exported for troubleshooting", () =>
    {
        var reportPath = Path.Combine(fixtureDirectory, "error-report.json");
        Assert(CommandLine.Execute(["--analyze", gameDirectory, Path.Combine(fixtureDirectory, "missing.vpk"), "--report", reportPath], new StringWriter()) == 2);
        Assert(File.ReadAllText(reportPath).Contains("Failed"));
    });
    Test("Report filename cannot overwrite a VPK", () =>
    {
        Assert(CommandLine.Execute(["--analyze", gameDirectory, modPath, "--report", modPath], new StringWriter()) == 2);
    });
    Test("Truncated directories are rejected", () =>
    {
        var truncatedPath = Path.Combine(fixtureDirectory, "truncated.vpk");
        File.WriteAllBytes(truncatedPath, original[..28]);
        Invalid(() => VpkArchive.Open(truncatedPath));
    });
    Test("Resource table overflow is rejected", () =>
    {
        var broken = gameModel.ToArray();
        BitConverter.TryWriteBytes(broken.AsSpan(12), int.MaxValue);
        Invalid(() => ResourceData.ReplaceDataBlock(broken, [0x00, 0x33, 0x56, 0x4B]));
    });
}
finally { Directory.Delete(fixtureDirectory, recursive: true); }

if (args.Length == 2)
{
    foreach (var mod in Directory.GetFiles(args[1], "*.vpk"))
    {
        var report = HeroModelUpdater.Analyze(args[0], mod);
        foreach (var result in report.Results)
            Console.WriteLine($"SAMPLE {Path.GetFileName(mod)} {result.Model}: {result.Outcome}: {result.Detail}");
        Assert(!report.WroteFile);
    }
}
Console.WriteLine($"Passed {passed} regression checks.");
