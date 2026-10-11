using System.Security.Cryptography;
using DeadlockModelUpdater;
using ValveKeyValue;
using ValveKeyValue.KeyValues3;
using ValveResourceFormat;
using ValveResourceFormat.ResourceTypes;

static class SampleRegression
{
    public static void Run(string gameDirectory, string inputDirectory, string outputDirectory)
    {
        Directory.CreateDirectory(outputDirectory);
        var reports = new List<ArchiveReport>();
        foreach (var filename in new[] { "pak01_dir.vpk", "pak06_dir.vpk", "pak09_dir.vpk", "pak11_dir.vpk", "pak13_dir.vpk" })
        {
            var source = Path.GetFullPath(Path.Combine(inputDirectory, filename));
            var before = HashFile(source);
            var analysis = HeroModelUpdater.Analyze(gameDirectory, source);
            reports.Add(new(source, analysis));
            if (filename is "pak06_dir.vpk" or "pak09_dir.vpk" or "pak11_dir.vpk")
            {
                var output = Path.Combine(outputDirectory, filename);
                var repair = HeroModelUpdater.Update(gameDirectory, source, output, false, false, null);
                Check(!analysis.HasBlockers && analysis.Results.All(result => result.Outcome == ModelOutcome.Current), filename + " should have no actionable defect.");
                Check(!repair.WroteFile && !File.Exists(output), filename + " must not be rewritten.");
                Console.WriteLine("PASS known-working mod unchanged: " + filename);
            }
            else if (filename == "pak01_dir.vpk")
            {
                Check(analysis.CanRepair, "Unicorn animation override should be repairable.");
                var output = Path.GetFullPath(Path.Combine(outputDirectory, filename));
                var repair = HeroModelUpdater.Update(gameDirectory, source, output, false, false, null);
                Check(repair.WroteFile, "Unicorn repair was not written.");
                var oldArchive = VpkArchive.Open(source);
                var newArchive = VpkArchive.Open(output);
                var skeletonEntry = oldArchive.Entries.Single(entry => entry.FullPath.EndsWith(".vnmskel_c"));
                foreach (var entry in oldArchive.Entries.Where(entry => entry.FullPath != skeletonEntry.FullPath))
                {
                    var written = newArchive.Entries.Single(value => value.FullPath == entry.FullPath);
                    Check(SHA256.HashData(oldArchive.Read(entry)).SequenceEqual(SHA256.HashData(newArchive.Read(written))), "Unrelated asset changed: " + entry.FullPath);
                }
                using var oldSkeleton = Read(oldArchive.Read(skeletonEntry), skeletonEntry.FullPath);
                using var newSkeleton = Read(newArchive.Read(newArchive.Entries.Single(entry => entry.FullPath == skeletonEntry.FullPath)), skeletonEntry.FullPath);
                var game = VpkArchive.Open(Path.Combine(gameDirectory, "pak01_dir.vpk"));
                using var official = Read(game.Read(game.Entries.Single(entry => entry.FullPath == skeletonEntry.FullPath)), skeletonEntry.FullPath);
                KVObject oldData = ((BinaryKV3)oldSkeleton.DataBlock!).Data;
                KVObject newData = ((BinaryKV3)newSkeleton.DataBlock!).Data;
                KVObject officialData = ((BinaryKV3)official.DataBlock!).Data;
                Check(newData["m_boneIDs"].Values.Select(value => value.ToString()).SequenceEqual(officialData["m_boneIDs"].Values.Select(value => value.ToString())), "Animation IDs do not match the current layout.");
                Check(newData["m_parentIndices"].Values.Select(value => value.ToInt64()).SequenceEqual(officialData["m_parentIndices"].Values.Select(value => value.ToInt64())), "Animation parent bindings do not match.");
                Check(Fingerprint(oldData["m_maskDefinitions"]).SequenceEqual(Fingerprint(newData["m_maskDefinitions"])), "Authored animation masks changed.");
                var newNames = newData["m_boneIDs"].Values.Select(value => value.ToString()).ToArray();
                var oldNames = oldData["m_boneIDs"].Values.Select(value => value.ToString()).ToArray();
                for (var i = 0; i < oldNames.Length; i++)
                {
                    var index = Array.IndexOf(newNames, oldNames[i]);
                    Check(oldData["m_parentSpaceReferencePose"][i].Values.Select(value => value.ToDouble())
                        .SequenceEqual(newData["m_parentSpaceReferencePose"][index].Values.Select(value => value.ToDouble())), "Authored reference pose changed: " + oldNames[i]);
                }
                var after = HeroModelUpdater.Analyze(gameDirectory, output);
                Check(!after.HasBlockers && !after.CanRepair, "Repaired Unicorn still reports an actionable issue.");
                using var independent = new SteamDatabase.ValvePak.Package();
                independent.Read(output);
                independent.VerifyHashes();
                independent.VerifyFileChecksums();
                reports.Add(new(output, repair));
                Console.WriteLine("PASS Unicorn repair: only animation skeleton changed; all other assets, poses and masks preserved. " + output);
            }
            else
            {
                Check(analysis.HasBlockers && analysis.Results.Any(result => result.Detail.Contains("geist_backup.vmdl")), "Geist should identify its missing declared animation include.");
                Console.WriteLine("PASS Geist alert identifies the missing animation include.");
            }
            Check(before.SequenceEqual(HashFile(source)), "Original VPK was changed: " + filename);
        }
        ReportExporter.Write(Path.Combine(inputDirectory, "deadlock-mod-report-fixed.json"), gameDirectory, reports);
    }

    static byte[] HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return SHA256.HashData(stream);
    }

    static Resource Read(byte[] bytes, string path)
    {
        var resource = new Resource { FileName = path };
        resource.Read(new MemoryStream(bytes, writable: false));
        return resource;
    }

    static byte[] Fingerprint(KVObject value)
    {
        var root = new KVObject();
        root.Add("value", value);
        using var resource = new Resource();
        var kv = new BinaryKV3(root, new KV3ID("generic", Guid.Parse("7412167c-06e9-4698-aff2-e63eb59037e7"))) { Resource = resource };
        using var stream = new MemoryStream();
        kv.Serialize(stream);
        return SHA256.HashData(stream.ToArray());
    }

    static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }
}
