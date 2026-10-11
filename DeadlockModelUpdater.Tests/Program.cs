using System.Numerics;
using DeadlockModelUpdater;

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
