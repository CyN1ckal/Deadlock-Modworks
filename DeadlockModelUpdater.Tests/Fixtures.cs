using Encoding = System.Text.Encoding;
using ValveKeyValue;
using ValveKeyValue.KeyValues3;
using ValveResourceFormat.ResourceTypes;

static class Fixtures
{
    public const string ModelPath = "models/heroes/test/test.vmdl_c";

    public static void Archive(string path, Dictionary<string, byte[]?> entries)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var seed = path + ".seed";
        var bytes = new byte[29];
        BitConverter.TryWriteBytes(bytes.AsSpan(), 0x55AA1234u);
        BitConverter.TryWriteBytes(bytes.AsSpan(4), 2u);
        BitConverter.TryWriteBytes(bytes.AsSpan(8), 1u);
        File.WriteAllBytes(seed, bytes);
        try { DeadlockModelUpdater.VpkArchive.Open(seed).Write(path, entries); }
        finally { File.Delete(seed); }
    }

    public static byte[] Model(string[] names, int[] parents, int[]? remap = null, string? extraBlock = null)
    {
        KVObject Vector(params float[] components) => KVObject.Array(components.Select(x => new KVObject(x)));
        var skeleton = new KVObject();
        skeleton.Add("m_boneName", KVObject.Array(names.Select(x => new KVObject(x))));
        skeleton.Add("m_nParent", KVObject.Array(parents.Select(x => new KVObject(x))));
        skeleton.Add("m_boneSphere", KVObject.Array(names.Select(_ => new KVObject(1f))));
        skeleton.Add("m_nFlag", KVObject.Array(names.Select(_ => new KVObject(0))));
        skeleton.Add("m_bonePosParent", KVObject.Array(names.Select(_ => Vector(0, 0, 0))));
        skeleton.Add("m_boneRotParent", KVObject.Array(names.Select(_ => Vector(0, 0, 0, 1))));
        skeleton.Add("m_boneScaleParent", KVObject.Array(names.Select(_ => new KVObject(1f))));
        var root = new KVObject();
        root.Add("m_name", new KVObject("models/heroes/test/test.vmdl"));
        root.Add("m_modelSkeleton", skeleton);
        if (remap != null) root.Add("m_remappingTable", KVObject.Array(remap.Select(x => new KVObject(x))));
        using var resource = new ValveResourceFormat.Resource { FileName = ModelPath };
        var kv = new BinaryKV3(root, new KV3ID("generic", Guid.Parse("7412167c-06e9-4698-aff2-e63eb59037e7"))) { Resource = resource };
        using var serialized = new MemoryStream();
        kv.Serialize(serialized);
        var data = serialized.ToArray();
        var count = extraBlock == null ? 1 : 2;
        var start = 16 + count * 12;
        var file = new byte[start + data.Length + (extraBlock == null ? 0 : 4)];
        BitConverter.TryWriteBytes(file.AsSpan(), (uint)file.Length);
        BitConverter.TryWriteBytes(file.AsSpan(4), (ushort)12);
        BitConverter.TryWriteBytes(file.AsSpan(6), (ushort)1);
        BitConverter.TryWriteBytes(file.AsSpan(8), 8u);
        BitConverter.TryWriteBytes(file.AsSpan(12), count);
        Encoding.ASCII.GetBytes("DATA").CopyTo(file, 16);
        BitConverter.TryWriteBytes(file.AsSpan(20), (uint)(start - 20));
        BitConverter.TryWriteBytes(file.AsSpan(24), data.Length);
        data.CopyTo(file, start);
        if (extraBlock != null)
        {
            Encoding.ASCII.GetBytes(extraBlock).CopyTo(file, 28);
            BitConverter.TryWriteBytes(file.AsSpan(32), (uint)(start + data.Length - 32));
            BitConverter.TryWriteBytes(file.AsSpan(36), 4);
        }
        return file;
    }

    public static void PreloadArchive(string path)
    {
        using var tree = new MemoryStream();
        using var writer = new BinaryWriter(tree, Encoding.UTF8, leaveOpen: true);
        writer.Write(Encoding.UTF8.GetBytes("txt\0 \0sample\0"));
        writer.Write(DeadlockModelUpdater.VpkArchive.Crc([1, 2, 3, 4]));
        writer.Write((ushort)2);
        writer.Write((ushort)0x7FFF);
        writer.Write(0u);
        writer.Write(2u);
        writer.Write((ushort)0xFFFF);
        writer.Write(new byte[] { 1, 2 });
        writer.Write(new byte[] { 0, 0, 0 });
        using var output = File.Create(path);
        using var file = new BinaryWriter(output);
        file.Write(0x55AA1234u);
        file.Write(2u);
        file.Write((uint)tree.Length);
        file.Write(2u);
        file.Write(0u); file.Write(0u); file.Write(0u);
        file.Write(tree.ToArray());
        file.Write(new byte[] { 3, 4 });
    }
}
