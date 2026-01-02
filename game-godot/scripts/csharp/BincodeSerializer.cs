using System;
using System.IO;
using System.Text;

namespace ApexSim;

/// <summary>
/// Bincode serializer compatible with Rust's bincode format.
/// Implements the default bincode configuration (little-endian, variable-length encoding).
/// </summary>
public class BincodeWriter
{
    private readonly MemoryStream _stream;
    private readonly BinaryWriter _writer;

    public BincodeWriter()
    {
        _stream = new MemoryStream();
        _writer = new BinaryWriter(_stream);
    }

    public byte[] ToArray() => _stream.ToArray();

    // Primitive types
    public void WriteU8(byte value) => _writer.Write(value);
    public void WriteU16(ushort value) => _writer.Write(value);
    public void WriteU32(uint value) => _writer.Write(value);
    public void WriteU64(ulong value) => _writer.Write(value);
    public void WriteI8(sbyte value) => _writer.Write(value);
    public void WriteI16(short value) => _writer.Write(value);
    public void WriteI32(int value) => _writer.Write(value);
    public void WriteI64(long value) => _writer.Write(value);
    public void WriteF32(float value) => _writer.Write(value);
    public void WriteF64(double value) => _writer.Write(value);
    public void WriteBool(bool value) => _writer.Write((byte)(value ? 1 : 0));

    // String (length-prefixed UTF-8)
    public void WriteString(string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        WriteU64((ulong)bytes.Length);
        _writer.Write(bytes);
    }

    // Option<T> (0 = None, 1 = Some(T))
    public void WriteOption<T>(T? value, Action<T> writeValue) where T : struct
    {
        if (value.HasValue)
        {
            WriteU8(1);
            writeValue(value.Value);
        }
        else
        {
            WriteU8(0);
        }
    }

    // Option<String> (0 = None, 1 = Some(String))
    public void WriteOptionString(string? value)
    {
        if (value != null)
        {
            WriteU8(1);
            WriteString(value);
        }
        else
        {
            WriteU8(0);
        }
    }

    // Vec<T> (length-prefixed)
    public void WriteVec<T>(T[] array, Action<T> writeItem)
    {
        WriteU64((ulong)array.Length);
        foreach (var item in array)
        {
            writeItem(item);
        }
    }

    // Enum variant index (u32)
    public void WriteVariantIndex(uint index) => WriteU32(index);

    // UUID - Rust's uuid crate with bincode serializes as [u32 length, 16 bytes]
    public void WriteUuid(string uuid)
    {
        // Parse UUID string (like "550e8400-e29b-41d4-a716-446655440000")
        var guid = Guid.Parse(uuid);

        // Write length prefix (always 16 for UUID)
        WriteU32(16);

        // Write UUID bytes in the same format as Rust's uuid crate
        // Guid.ToByteArray() matches the uuid crate's byte order
        var bytes = guid.ToByteArray();
        _writer.Write(bytes);
    }
}

/// <summary>
/// Bincode deserializer compatible with Rust's bincode format.
/// </summary>
public class BincodeReader
{
    private readonly BinaryReader _reader;

    public BincodeReader(byte[] data)
    {
        _reader = new BinaryReader(new MemoryStream(data));
    }

    public byte ReadU8() => _reader.ReadByte();
    public ushort ReadU16() => _reader.ReadUInt16();
    public uint ReadU32() => _reader.ReadUInt32();
    public ulong ReadU64() => _reader.ReadUInt64();
    public sbyte ReadI8() => _reader.ReadSByte();
    public short ReadI16() => _reader.ReadInt16();
    public int ReadI32() => _reader.ReadInt32();
    public long ReadI64() => _reader.ReadInt64();
    public float ReadF32() => _reader.ReadSingle();
    public double ReadF64() => _reader.ReadDouble();
    public bool ReadBool() => _reader.ReadByte() != 0;

    public string ReadString()
    {
        var posBefore = _reader.BaseStream.Position;
        var length = ReadU64();

        if (length > int.MaxValue)
        {
            throw new Exception($"String length {length} exceeds maximum");
        }
        if (length > 1000000) // Sanity check: strings shouldn't be > 1MB
        {
            throw new Exception($"String length {length} seems unreasonably large");
        }

        var bytes = _reader.ReadBytes((int)length);
        if (bytes.Length != (int)length)
        {
            throw new Exception($"Expected {length} bytes but only read {bytes.Length}");
        }
        var result = Encoding.UTF8.GetString(bytes);
        return result;
    }

    public T? ReadOption<T>(Func<T> readValue) where T : struct
    {
        var hasValue = ReadU8();
        if (hasValue == 1)
        {
            return readValue();
        }
        return null;
    }

    public string? ReadOptionString()
    {
        var hasValue = ReadU8();
        if (hasValue == 1)
        {
            return ReadString();
        }
        return null;
    }

    public T[] ReadVec<T>(Func<T> readItem)
    {
        var length = ReadU64();
        var array = new T[length];
        for (ulong i = 0; i < length; i++)
        {
            array[i] = readItem();
        }
        return array;
    }

    public uint ReadVariantIndex() => ReadU32();

    // UUID - Rust's uuid crate with bincode serializes as [u32 length, 16 bytes]
    public string ReadUuid()
    {
        var posBefore = _reader.BaseStream.Position;

        // Read length prefix (always 16 for UUID)
        var length = ReadU32();
        if (length != 16)
        {
            throw new Exception($"UUID length {length} is not 16");
        }

        // Read 16 UUID bytes (in Rust's native byte order, which matches .NET Guid for the uuid crate)
        var uuidBytes = _reader.ReadBytes(16);

        // Debug: print the raw bytes
        var hexStr = BitConverter.ToString(uuidBytes).Replace("-", " ");

        // Rust's uuid crate stores bytes in the same order as .NET Guid.ToByteArray()
        // No byte swapping needed!
        var guid = new Guid(uuidBytes);
        return guid.ToString();
    }

    // Option<UUID>
    public string? ReadOptionUuid()
    {
        var hasValue = ReadU8();
        if (hasValue == 1)
        {
            return ReadUuid();
        }
        else if (hasValue == 0)
        {
            return null;
        }
        else
        {
            throw new Exception($"Invalid Option discriminant: {hasValue} (expected 0 or 1)");
        }
    }

    // Get current position in stream (for debugging)
    public long Position => _reader.BaseStream.Position;

    // Get total length of stream (for debugging)
    public long Length => _reader.BaseStream.Length;
}
