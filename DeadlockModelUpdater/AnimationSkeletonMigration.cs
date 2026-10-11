using System.Numerics;
using ValveKeyValue;
using ValveResourceFormat;
using ValveResourceFormat.ResourceTypes;

namespace DeadlockModelUpdater;

/// <summary>Updates AG2 binding layout without replacing a mod's meshes, poses or masks.</summary>
static class AnimationSkeletonMigration
{
    public static MigrationEdit Migrate(byte[] modBytes, string modPath, byte[] gameBytes, string gamePath)
    {
        try
        {
            if (!modPath.Equals(gamePath, StringComparison.OrdinalIgnoreCase))
                return new(null, ModelOutcome.NeedsReview, "Animation skeleton path migration is not supported.");
            using var modStream = new MemoryStream(modBytes, writable: false);
            using var gameStream = new MemoryStream(gameBytes, writable: false);
            using var mod = new Resource { FileName = modPath };
            using var game = new Resource { FileName = gamePath };
            mod.Read(modStream, leaveOpen: true);
            game.Read(gameStream, leaveOpen: true);
            if (mod.DataBlock is not BinaryKV3 modKv || game.DataBlock is not BinaryKV3 gameKv)
                return new(null, ModelOutcome.NeedsReview, "Animation skeleton does not use a supported KV3 DATA block.");
            KVObject modRoot = modKv.Data;
            KVObject gameRoot = gameKv.Data;
            var old = Validate(modRoot);
            var current = Validate(gameRoot);
            if (modRoot["m_ID"].ToString() != gameRoot["m_ID"].ToString())
                return new(null, ModelOutcome.NeedsReview, "Animation skeleton identities differ; this may be a custom skeleton.");
            var oldIndex = old.Names.Select((name, index) => (name, index)).ToDictionary(x => x.name, x => x.index, StringComparer.Ordinal);
            var currentNames = current.Names.ToHashSet(StringComparer.Ordinal);
            var removed = old.Names.Where(name => !currentNames.Contains(name)).ToArray();
            if (removed.Length > 0)
                return new(null, ModelOutcome.NeedsReview,
                    $"Animation override has custom or removed bones: {string.Join(", ", removed.Take(6))}. An explicit mapping is needed.");
            string? Parent(SkeletonData skeleton, int index) => skeleton.Parents[index] < 0 ? null : skeleton.Names[skeleton.Parents[index]];
            var changedParents = current.Names.Select((name, index) => (name, index))
                .Where(x => oldIndex.TryGetValue(x.name, out var oldBone) && Parent(old, oldBone) != Parent(current, x.index))
                .Select(x => x.name).ToArray();
            if (changedParents.Length > 0)
                return new(null, ModelOutcome.NeedsReview,
                    $"Animation override parents changed: {string.Join(", ", changedParents.Take(6))}. Retargeting needs author review.");
            var oldLod = checked((int)modRoot["m_numBonesToSampleAtLowLOD"].ToInt64());
            var newLod = checked((int)gameRoot["m_numBonesToSampleAtLowLOD"].ToInt64());
            if (old.Names.SequenceEqual(current.Names) && old.Parents.SequenceEqual(current.Parents) && oldLod == newLod)
                return new(null, ModelOutcome.Current, "Animation bone order, parent bindings and low-LOD sampling match the installed game. Authored poses and masks are preserved.");
            var knownKeys = new HashSet<string>(StringComparer.Ordinal)
            {
                "m_ID", "m_boneIDs", "m_parentIndices", "m_parentSpaceReferencePose", "m_modelSpaceReferencePose",
                "m_numBonesToSampleAtLowLOD", "m_maskDefinitions", "m_secondarySkeletons", "m_bIsPropSkeleton",
            };
            var unknown = modRoot.Keys.Where(key => !knownKeys.Contains(key)).ToArray();
            if (unknown.Length > 0 || modRoot.ContainsKey("m_secondarySkeletons") && modRoot["m_secondarySkeletons"].Count > 0)
                return new(null, ModelOutcome.NeedsReview, "Animation override contains unsupported metadata or secondary skeletons.");
            var localPoses = current.Names.Select((name, index) => oldIndex.TryGetValue(name, out var oldBone)
                ? modRoot["m_parentSpaceReferencePose"][oldBone] : gameRoot["m_parentSpaceReferencePose"][index]).ToArray();
            modRoot["m_boneIDs"] = gameRoot["m_boneIDs"];
            modRoot["m_parentIndices"] = gameRoot["m_parentIndices"];
            modRoot["m_parentSpaceReferencePose"] = KVObject.Array(localPoses);
            modRoot["m_modelSpaceReferencePose"] = KVObject.Array(WorldPoses(current.Parents, localPoses));
            modRoot["m_numBonesToSampleAtLowLOD"] = gameRoot["m_numBonesToSampleAtLowLOD"];
            using var serialized = new MemoryStream();
            modKv.Serialize(serialized);
            var patched = ResourceData.ReplaceDataBlock(modBytes, serialized.ToArray());
            using var checkStream = new MemoryStream(patched, writable: false);
            using var check = new Resource { FileName = modPath };
            check.Read(checkStream, leaveOpen: true);
            KVObject checkedRoot = ((BinaryKV3)check.DataBlock!).Data;
            var checkedData = Validate(checkedRoot);
            if (!checkedData.Names.SequenceEqual(current.Names) || !checkedData.Parents.SequenceEqual(current.Parents))
                throw new InvalidDataException("Animation skeleton binding verification failed.");
            for (var i = 0; i < localPoses.Length; i++)
                if (!Numbers(localPoses[i]).SequenceEqual(Numbers(checkedRoot["m_parentSpaceReferencePose"][i])))
                    throw new InvalidDataException("An authored animation reference pose changed during serialization.");
            return new(patched, ModelOutcome.Updated,
                $"Animation override binding layout updated: {old.Names.Length} to {current.Names.Length} bones; low-LOD sampling {oldLod} to {newLod}. Authored parent-space reference poses and masks are preserved, world poses are rebuilt, and model geometry is untouched.",
                ["Test this animation-skeleton repair in-game; structural verification cannot confirm animation playback."]);
        }
        catch (Exception ex)
        {
            return new(null, ModelOutcome.Failed, ex.Message);
        }
    }

    sealed record SkeletonData(string[] Names, int[] Parents);

    static SkeletonData Validate(KVObject root)
    {
        if (!root.ContainsKey("m_ID") || !root.ContainsKey("m_boneIDs") || !root["m_boneIDs"].IsArray)
            throw new InvalidDataException("Animation skeleton has no identity or bone-ID array.");
        var names = root["m_boneIDs"].Values.Select(value => value.ToString()).ToArray();
        if (names.Length == 0 || names.Any(string.IsNullOrWhiteSpace) || names.Distinct(StringComparer.Ordinal).Count() != names.Length)
            throw new InvalidDataException("Animation skeleton has empty or duplicate bone IDs.");
        foreach (var key in new[] { "m_parentIndices", "m_parentSpaceReferencePose", "m_modelSpaceReferencePose" })
            if (!root.ContainsKey(key) || !root[key].IsArray || root[key].Count != names.Length)
                throw new InvalidDataException($"Animation skeleton {key} does not match its bone count.");
        var parents = root["m_parentIndices"].Values.Select(value => checked((int)value.ToInt64())).ToArray();
        var shapes = new BoneShape[names.Length];
        for (var i = 0; i < names.Length; i++)
        {
            if (parents[i] < -1 || parents[i] >= names.Length) throw new InvalidDataException("Animation parent index is outside the skeleton.");
            var local = Numbers(root["m_parentSpaceReferencePose"][i]);
            Numbers(root["m_modelSpaceReferencePose"][i]);
            shapes[i] = new(names[i], parents[i] < 0 ? null : names[parents[i]], new(local[0], local[1], local[2]),
                new(local[4], local[5], local[6], local[7]), local[3]);
        }
        SkeletonCompatibility.Assess(shapes, shapes);
        if (!root.ContainsKey("m_numBonesToSampleAtLowLOD")) throw new InvalidDataException("Animation skeleton has no LOD sampling count.");
        var lod = root["m_numBonesToSampleAtLowLOD"].ToInt64();
        if (lod < 0 || lod > names.Length) throw new InvalidDataException("Animation LOD sampling count is outside the skeleton.");
        return new(names, parents);
    }

    static float[] Numbers(KVObject pose)
    {
        if (!pose.IsArray || pose.Count != 8) throw new InvalidDataException("Animation reference pose is not an eight-component transform.");
        var values = pose.Values.Select(value => (float)value.ToDouble()).ToArray();
        if (values.Any(value => !float.IsFinite(value))) throw new InvalidDataException("Animation reference pose has non-finite values.");
        var rotation = new Quaternion(values[4], values[5], values[6], values[7]);
        if (values[3] <= 0 || !float.IsFinite(rotation.LengthSquared()) || rotation.LengthSquared() < 0.000001f)
            throw new InvalidDataException("Animation reference pose has an invalid scale or rotation.");
        return values;
    }

    static KVObject[] WorldPoses(int[] parents, KVObject[] local)
    {
        var world = new Matrix4x4[local.Length];
        var done = new bool[local.Length];
        Matrix4x4 Resolve(int index)
        {
            if (done[index]) return world[index];
            var pose = Numbers(local[index]);
            var matrix = Matrix4x4.CreateScale(pose[3]) * Matrix4x4.CreateFromQuaternion(new(pose[4], pose[5], pose[6], pose[7]))
                * Matrix4x4.CreateTranslation(pose[0], pose[1], pose[2]);
            world[index] = parents[index] < 0 ? matrix : matrix * Resolve(parents[index]);
            done[index] = true;
            return world[index];
        }
        return Enumerable.Range(0, local.Length).Select(index =>
        {
            if (!Matrix4x4.Decompose(Resolve(index), out var scale, out var rotation, out var position))
                throw new InvalidDataException("Animation world reference pose could not be rebuilt.");
            return KVObject.Array(new[] { position.X, position.Y, position.Z, scale.X, rotation.X, rotation.Y, rotation.Z, rotation.W }
                .Select(value => new KVObject(value)));
        }).ToArray();
    }
}
