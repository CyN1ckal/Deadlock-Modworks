using System.Text;
using ValveResourceFormat.ResourceTypes;

namespace DeadlockModelUpdater;

/// <summary>
/// Replaces the DATA block of a compiled Source 2 resource and patches the block table.
/// </summary>
public static class ResourceData
{
    public static byte[] ReplaceDataBlock(byte[] file, byte[] newData)
    {
        if (file.Length < 16)
            throw new InvalidDataException("Resource is too small.");
        if (BitConverter.ToUInt16(file, 4) != 12)
            throw new InvalidDataException("Resource header is not version 12.");
        if (newData.Length < 4 || !BinaryKV3.IsBinaryKV3(BitConverter.ToUInt32(newData)))
            throw new InvalidDataException("Replacement DATA is not KV3.");

        var fileSize = BitConverter.ToUInt32(file, 0);
        var blockOffset = BitConverter.ToUInt32(file, 8);
        var blockCount = BitConverter.ToInt32(file, 12);
        if (blockCount <= 0 || blockOffset < 8)
            throw new InvalidDataException("Resource block table is invalid.");

        var tableStart = 16 + (int)blockOffset - 8;
        var tableBytes = blockCount * 12;
        if (tableStart < 16 || tableStart > file.Length || tableBytes > file.Length - tableStart)
            throw new InvalidDataException("Resource block table is outside the file.");

        var dataEntry = -1;
        var dataAbs = 0;
        var dataSize = 0;
        for (var i = 0; i < blockCount; i++)
        {
            var pos = tableStart + i * 12;
            var type = Encoding.ASCII.GetString(file, pos, 4);
            if (type != "DATA")
                continue;
            if (dataEntry >= 0)
                throw new InvalidDataException("Resource has more than one DATA block.");

            var rel = BitConverter.ToUInt32(file, pos + 4);
            var size = BitConverter.ToInt32(file, pos + 8);
            var abs = (long)pos + 4 + rel;
            if (size < 0 || abs > int.MaxValue)
                throw new InvalidDataException("DATA block location is invalid.");
            dataEntry = pos;
            dataAbs = (int)abs;
            dataSize = size;
        }

        if (dataEntry < 0)
            throw new InvalidDataException("Resource has no DATA block.");
        if (dataAbs > file.Length || dataSize > file.Length - dataAbs)
            throw new InvalidDataException("DATA block is outside the file.");

        var tableEnd = tableStart + tableBytes;
        if (dataAbs < tableEnd)
            throw new InvalidDataException("DATA block starts before the end of the block table.");

        var dataEnd = dataAbs + dataSize;
        for (var i = 0; i < blockCount; i++)
        {
            var pos = tableStart + i * 12;
            if (pos == dataEntry)
                continue;
            var rel = BitConverter.ToUInt32(file, pos + 4);
            var size = BitConverter.ToInt32(file, pos + 8);
            if (size < 0)
                throw new InvalidDataException("A block size is negative.");
            var abs = (long)pos + 4 + rel;
            var end = abs + size;
            if (abs < dataEnd && end > dataAbs)
                throw new InvalidDataException("A block overlaps the DATA block.");
        }

        var delta = newData.Length - dataSize;
        var newLength = (long)file.Length + delta;
        if (newLength is < 0 or > int.MaxValue)
            throw new InvalidDataException("Replacing DATA made the resource an invalid size.");

        var result = new byte[newLength];
        file.AsSpan(0, dataAbs).CopyTo(result);
        newData.CopyTo(result.AsSpan(dataAbs));
        file.AsSpan(dataEnd).CopyTo(result.AsSpan(dataAbs + newData.Length));

        var newFileSize = fileSize + (long)delta;
        if (newFileSize is < 0 or > uint.MaxValue)
            throw new InvalidDataException("Resource file size does not fit.");
        BitConverter.TryWriteBytes(result.AsSpan(0), (uint)newFileSize);
        BitConverter.TryWriteBytes(result.AsSpan(dataEntry + 8), (uint)newData.Length);

        for (var i = 0; i < blockCount; i++)
        {
            var pos = tableStart + i * 12;
            if (pos == dataEntry)
                continue;
            var rel = BitConverter.ToUInt32(file, pos + 4);
            var abs = (long)pos + 4 + rel;
            if (abs < dataEnd)
                continue;
            var newRel = rel + (long)delta;
            if (newRel is < 0 or > uint.MaxValue)
                throw new InvalidDataException("A block offset does not fit after the DATA replacement.");
            BitConverter.TryWriteBytes(result.AsSpan(pos + 4), (uint)newRel);
        }

        return result;
    }
}
