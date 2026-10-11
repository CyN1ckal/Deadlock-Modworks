using System.IO.Hashing;
using System.Security.Cryptography;
using System.Text;

namespace DeadlockModelUpdater;

/// <summary>
/// Source 2 directory VPK. Reads chunked game archives and writes a single embedded mod archive.
/// </summary>
public sealed class VpkArchive
{
    const uint Signature = 0x55AA1234;
    const ushort EmbeddedArchive = 0x7FFF;

    readonly string _directoryPath;
    readonly string _baseName;
    readonly uint _fileDataSize;
    readonly int _treeSize;

    VpkArchive(string directoryPath, List<VpkEntry> entries, uint fileDataSize, int treeSize)
    {
        _directoryPath = directoryPath;
        _baseName = Path.GetFileNameWithoutExtension(directoryPath);
        if (_baseName.EndsWith("_dir", StringComparison.OrdinalIgnoreCase))
            _baseName = _baseName[..^4];
        Entries = entries;
        _fileDataSize = fileDataSize;
        _treeSize = treeSize;
    }

    public List<VpkEntry> Entries { get; }

    public static VpkArchive Open(string directoryPath)
    {
        using var stream = File.Open(directoryPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        Span<byte> header = stackalloc byte[28];
        stream.ReadExactly(header);
        var signature = BitConverter.ToUInt32(header);
        var version = BitConverter.ToUInt32(header[4..]);
        var treeSize = BitConverter.ToInt32(header[8..]);
        var fileDataSize = BitConverter.ToUInt32(header[12..]);
        if (signature != Signature || version != 2)
            throw new InvalidDataException($"{Path.GetFileName(directoryPath)} is not a Source 2 VPK.");
        // Bytes 16..28 are the archive MD5, other MD5, and signature sizes.
        // Chunked game archives fill those in. The tree still begins after this header,
        // and Write() emits a new embedded mod archive instead of copying those sections.

        var tree = new byte[treeSize];
        stream.ReadExactly(tree);
        return new VpkArchive(directoryPath, ParseTree(tree), fileDataSize, treeSize);
    }

    public byte[] Read(VpkEntry entry)
    {
        if (entry.PreloadBytes != 0)
            throw new InvalidDataException($"{entry.FullPath} uses preload data.");

        using var stream = OpenData(entry);
        stream.Seek(DataOffset(entry), SeekOrigin.Begin);
        var data = new byte[entry.Length];
        stream.ReadExactly(data);
        return data;
    }

    public void CopyTo(VpkEntry entry, Stream destination)
    {
        if (entry.PreloadBytes != 0)
            throw new InvalidDataException($"{entry.FullPath} uses preload data.");

        using var stream = OpenData(entry);
        stream.Seek(DataOffset(entry), SeekOrigin.Begin);
        var remaining = entry.Length;
        var buffer = new byte[1024 * 1024];
        while (remaining > 0)
        {
            var n = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
            if (n == 0)
                throw new EndOfStreamException(entry.FullPath);
            destination.Write(buffer, 0, n);
            remaining -= (uint)n;
        }
    }

    /// <summary>
    /// Writes a new embedded directory VPK. A null change removes a file. A path that is not
    /// already in the archive is added.
    /// </summary>
    public void Write(string destinationPath, IReadOnlyDictionary<string, byte[]?> changes)
    {
        var byPath = Entries.ToDictionary(entry => entry.FullPath, StringComparer.OrdinalIgnoreCase);
        var output = new List<(string Path, VpkEntry? Source, byte[]? Replacement)>();
        foreach (var entry in Entries)
        {
            if (changes.TryGetValue(entry.FullPath, out var replacement))
            {
                if (replacement != null)
                    output.Add((entry.FullPath, null, replacement));
            }
            else
            {
                output.Add((entry.FullPath, entry, null));
            }
        }

        foreach (var (path, data) in changes)
        {
            if (data == null || byPath.ContainsKey(path))
                continue;
            output.Add((Normalize(path), null, data));
        }

        var planned = output.Select(item =>
        {
            var length = item.Replacement?.Length ?? (int)item.Source!.Length;
            var (extension, directory, name) = Split(item.Path);
            return new PlannedFile(item.Path, extension, directory, name, item.Source, item.Replacement, length);
        }).ToList();

        uint offset = 0;
        var located = new List<LocatedFile>(planned.Count);
        foreach (var file in planned
                     .OrderBy(file => file.Extension, StringComparer.Ordinal)
                     .ThenBy(file => file.Directory, StringComparer.Ordinal)
                     .ThenBy(file => file.Name, StringComparer.Ordinal))
        {
            located.Add(new LocatedFile(file, offset));
            offset += (uint)file.Length;
        }

        var tree = BuildTree(located);
        var fileDataSize = offset;
        var header = new byte[28];
        BitConverter.TryWriteBytes(header.AsSpan(0), Signature);
        BitConverter.TryWriteBytes(header.AsSpan(4), 2u);
        BitConverter.TryWriteBytes(header.AsSpan(8), (uint)tree.Length);
        BitConverter.TryWriteBytes(header.AsSpan(12), fileDataSize);
        BitConverter.TryWriteBytes(header.AsSpan(20), 48u);

        using var dest = File.Create(destinationPath);
        var whole = new Md5Builder();
        WriteHashed(dest, whole, header);
        WriteHashed(dest, whole, tree);
        foreach (var file in located)
        {
            if (file.Plan.Replacement != null)
            {
                WriteHashed(dest, whole, file.Plan.Replacement);
            }
            else
            {
                using var copy = new HashingStream(dest, whole);
                CopyTo(file.Plan.Source!, copy);
            }
        }

        var treeMd5 = MD5.HashData(tree);
        var emptyMd5 = MD5.HashData([]);
        WriteHashed(dest, whole, treeMd5);
        WriteHashed(dest, whole, emptyMd5);
        dest.Write(whole.Hash());
    }

    Stream OpenData(VpkEntry entry)
    {
        if (entry.ArchiveIndex == EmbeddedArchive)
            return File.Open(_directoryPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);

        var chunk = Path.Combine(Path.GetDirectoryName(_directoryPath)!, $"{_baseName}_{entry.ArchiveIndex:000}.vpk");
        if (!File.Exists(chunk))
            throw new FileNotFoundException($"Missing VPK chunk {Path.GetFileName(chunk)}.", chunk);
        return File.Open(chunk, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
    }

    long DataOffset(VpkEntry entry)
    {
        if (entry.ArchiveIndex == EmbeddedArchive)
            return 28L + _treeSize + entry.Offset;
        return entry.Offset;
    }

    static List<VpkEntry> ParseTree(byte[] tree)
    {
        var entries = new List<VpkEntry>();
        var cursor = 0;
        while (cursor < tree.Length)
        {
            var extension = ReadString(tree, ref cursor);
            if (extension.Length == 0)
                break;
            while (true)
            {
                var directory = ReadString(tree, ref cursor);
                if (directory.Length == 0)
                    break;
                while (true)
                {
                    var name = ReadString(tree, ref cursor);
                    if (name.Length == 0)
                        break;
                    var crc = BitConverter.ToUInt32(tree, cursor);
                    var preload = BitConverter.ToUInt16(tree, cursor + 4);
                    var archive = BitConverter.ToUInt16(tree, cursor + 6);
                    var offset = BitConverter.ToUInt32(tree, cursor + 8);
                    var length = BitConverter.ToUInt32(tree, cursor + 12);
                    var term = BitConverter.ToUInt16(tree, cursor + 16);
                    if (term != 0xFFFF)
                        throw new InvalidDataException("Broken VPK directory entry.");
                    cursor += 18 + preload;
                    // A single space is the Source 2 marker for the archive root.
                    if (directory == " ")
                        directory = "";
                    var full = directory.Length == 0 ? $"{name}.{extension}" : $"{directory}/{name}.{extension}";
                    entries.Add(new VpkEntry(full, crc, preload, archive, offset, length));
                }
            }
        }

        return entries;
    }

    static byte[] BuildTree(List<LocatedFile> files)
    {
        using var stream = new MemoryStream();
        Span<byte> meta = stackalloc byte[18];
        foreach (var extensionGroup in files.GroupBy(file => file.Plan.Extension, StringComparer.Ordinal))
        {
            WriteString(stream, extensionGroup.Key);
            foreach (var directoryGroup in extensionGroup.GroupBy(file => file.Plan.Directory, StringComparer.Ordinal))
            {
                WriteString(stream, directoryGroup.Key.Length == 0 ? " " : directoryGroup.Key);
                foreach (var file in directoryGroup)
                {
                    WriteString(stream, file.Plan.Name);
                    meta.Clear();
                    var crc = file.Plan.Replacement == null
                        ? file.Plan.Source!.Crc32
                        : Crc(file.Plan.Replacement);
                    BitConverter.TryWriteBytes(meta, crc);
                    BitConverter.TryWriteBytes(meta[6..], EmbeddedArchive);
                    BitConverter.TryWriteBytes(meta[8..], file.Offset);
                    BitConverter.TryWriteBytes(meta[12..], (uint)file.Plan.Length);
                    BitConverter.TryWriteBytes(meta[16..], (ushort)0xFFFF);
                    stream.Write(meta);
                }

                stream.WriteByte(0);
            }

            stream.WriteByte(0);
        }

        stream.WriteByte(0);
        return stream.ToArray();
    }

    public static uint Crc(byte[] data)
    {
        var crc = new Crc32();
        crc.Append(data);
        return crc.GetCurrentHashAsUInt32();
    }

    public static string Normalize(string path) => path.Replace('\\', '/').TrimStart('/');

    static (string Extension, string Directory, string Name) Split(string path)
    {
        path = Normalize(path);
        var slash = path.LastIndexOf('/');
        var directory = slash < 0 ? "" : path[..slash];
        var file = slash < 0 ? path : path[(slash + 1)..];
        var dot = file.LastIndexOf('.');
        if (dot <= 0)
            throw new InvalidDataException($"Cannot split VPK path {path}.");
        return (file[(dot + 1)..], directory, file[..dot]);
    }

    static string ReadString(byte[] data, ref int cursor)
    {
        var end = Array.IndexOf(data, (byte)0, cursor);
        if (end < 0)
            throw new InvalidDataException("Unterminated VPK string.");
        var text = Encoding.UTF8.GetString(data, cursor, end - cursor);
        cursor = end + 1;
        return text;
    }

    static void WriteString(Stream stream, string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        stream.Write(bytes);
        stream.WriteByte(0);
    }

    static void WriteHashed(Stream stream, Md5Builder hash, byte[] data)
    {
        stream.Write(data);
        hash.Append(data);
    }

    sealed class PlannedFile(string path, string extension, string directory, string name, VpkEntry? source, byte[]? replacement, int length)
    {
        public string Path { get; } = path;
        public string Extension { get; } = extension;
        public string Directory { get; } = directory;
        public string Name { get; } = name;
        public VpkEntry? Source { get; } = source;
        public byte[]? Replacement { get; } = replacement;
        public int Length { get; } = length;
    }

    sealed class LocatedFile(PlannedFile plan, uint offset)
    {
        public PlannedFile Plan { get; } = plan;
        public uint Offset { get; } = offset;
    }

    sealed class HashingStream(Stream inner, Md5Builder hash) : Stream
    {
        public override bool CanWrite => true;
        public override void Write(byte[] buffer, int offset, int count)
        {
            inner.Write(buffer, offset, count);
            hash.Append(buffer.AsSpan(offset, count));
        }

        public override void Flush() => inner.Flush();
        public override bool CanRead => false;
        public override bool CanSeek => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            // The destination stays open so the caller can write the checksum trailer.
            base.Dispose(disposing);
        }
    }

    sealed class Md5Builder
    {
        readonly IncrementalHash _hash = IncrementalHash.CreateHash(HashAlgorithmName.MD5);

        public void Append(ReadOnlySpan<byte> data) => _hash.AppendData(data);

        public byte[] Hash() => _hash.GetHashAndReset();
    }
}

public sealed record VpkEntry(string FullPath, uint Crc32, ushort PreloadBytes, ushort ArchiveIndex, uint Offset, uint Length);
