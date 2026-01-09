using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using ApexSim;
using MessagePack;
using MessagePack.Resolvers;
using Xunit;

namespace ApexSim.Tests;

public class ProceduralTerrainTests
{
    // Use array-based resolver to match Rust's rmp_serde::to_vec (not to_vec_named)
    // ContractlessStandardResolver uses map-based encoding, we need array-based
    private static readonly MessagePackSerializerOptions MsgPackOptions =
        MessagePackSerializerOptions.Standard.WithResolver(StandardResolver.Instance);

    [Fact]
    public void CanDeserializeTestProceduralTerrain()
    {
        // Arrange
        string terrainPath = Path.Combine(
            "..", "..", "..", "..", "..", // Navigate up from test bin directory
            "content", "tracks", "test_procedural.terrain.msgpack"
        );

        // Convert to absolute path
        terrainPath = Path.GetFullPath(terrainPath);

        Assert.True(File.Exists(terrainPath), $"Test terrain file not found at: {terrainPath}");

        // Act
        byte[] data = File.ReadAllBytes(terrainPath);
        ProceduralWorldData? terrainData = null;
        Exception? exception = null;

        try
        {
            terrainData = MessagePackSerializer.Deserialize<ProceduralWorldData>(data, MsgPackOptions);
        }
        catch (Exception ex)
        {
            exception = ex;
        }

        // Assert
        Assert.Null(exception);
        Assert.NotNull(terrainData);
        Assert.NotNull(terrainData!.Heightmap);

        var heightmap = terrainData.Heightmap!;
        Assert.True(heightmap.Width > 0, "Heightmap width should be greater than 0");
        Assert.True(heightmap.Height > 0, "Heightmap height should be greater than 0");
        Assert.True(heightmap.CellSizeM > 0, "Cell size should be greater than 0");
        Assert.NotNull(heightmap.Heights);
        Assert.Equal(heightmap.Width * heightmap.Height, heightmap.Heights.Count);
    }

    [Fact]
    public void CanDeserializeEnvironmentData()
    {
        // Arrange
        string terrainPath = Path.Combine(
            "..", "..", "..", "..", "..",
            "content", "tracks", "test_procedural.terrain.msgpack"
        );
        terrainPath = Path.GetFullPath(terrainPath);

        Assert.True(File.Exists(terrainPath), $"Test terrain file not found at: {terrainPath}");

        // Act
        byte[] data = File.ReadAllBytes(terrainPath);
        var terrainData = MessagePackSerializer.Deserialize<ProceduralWorldData>(data, MsgPackOptions);

        // Assert
        Assert.NotNull(terrainData);
        Assert.False(string.IsNullOrEmpty(terrainData!.EnvironmentType));
        Assert.NotNull(terrainData.Preset);
        Assert.NotNull(terrainData.Preset.GroundColor);
        Assert.Equal(3, terrainData.Preset.GroundColor.Length);
    }

    [Fact]
    public void HeightmapSamplingWorks()
    {
        // Arrange
        string terrainPath = Path.Combine(
            "..", "..", "..", "..", "..",
            "content", "tracks", "test_procedural.terrain.msgpack"
        );
        terrainPath = Path.GetFullPath(terrainPath);

        byte[] data = File.ReadAllBytes(terrainPath);
        var terrainData = MessagePackSerializer.Deserialize<ProceduralWorldData>(data, MsgPackOptions);
        var heightmap = terrainData!.Heightmap!;

        // Act - Sample at origin
        float heightAtOrigin = heightmap.Sample(heightmap.OriginX, heightmap.OriginY);

        // Assert
        Assert.True(heightAtOrigin >= 0, "Height at origin should be non-negative");

        // Act - Sample at a point within bounds
        float midX = heightmap.OriginX + (heightmap.Width / 2) * heightmap.CellSizeM;
        float midY = heightmap.OriginY + (heightmap.Height / 2) * heightmap.CellSizeM;
        float heightAtMiddle = heightmap.Sample(midX, midY);

        // Assert
        Assert.True(heightAtMiddle >= 0, "Height at middle should be non-negative");
    }

    [Fact]
    public void AllRealTracksWithTerrainCanBeDeserialized()
    {
        // Arrange
        string tracksDir = Path.Combine(
            "..", "..", "..", "..", "..",
            "content", "tracks", "real"
        );
        tracksDir = Path.GetFullPath(tracksDir);

        if (!Directory.Exists(tracksDir))
        {
            // Skip test if tracks directory doesn't exist
            return;
        }

        var terrainFiles = Directory.GetFiles(tracksDir, "*.terrain.msgpack");

        // We might not have any real tracks with terrain yet, so just check if we can process them
        if (terrainFiles.Length == 0)
        {
            // No terrain files to test - that's okay
            return;
        }

        // Act & Assert
        foreach (var terrainFile in terrainFiles)
        {
            byte[] data = File.ReadAllBytes(terrainFile);
            ProceduralWorldData? terrainData = null;
            Exception? exception = null;

            try
            {
                terrainData = MessagePackSerializer.Deserialize<ProceduralWorldData>(data, MsgPackOptions);
            }
            catch (Exception ex)
            {
                exception = ex;
            }

            Assert.Null(exception);
            Assert.NotNull(terrainData);
            Assert.NotNull(terrainData!.Heightmap);

            Console.WriteLine($"✓ Successfully deserialized: {Path.GetFileName(terrainFile)}");
            Console.WriteLine($"  - Size: {terrainData.Heightmap!.Width}x{terrainData.Heightmap.Height}");
            Console.WriteLine($"  - Environment: {terrainData.EnvironmentType}");
        }
    }

    [Fact]
    public void MessagePackAttributesAreCorrect()
    {
        // This test verifies that our C# classes have proper MessagePack attributes
        // for array-based serialization (matching Rust's rmp_serde::to_vec)

        var worldDataType = typeof(ProceduralWorldData);
        var heightmapType = typeof(TerrainHeightmap);
        var presetType = typeof(EnvironmentPreset);

        // All types should have [MessagePackObject] attribute
        Assert.NotNull(worldDataType.GetCustomAttributes(typeof(MessagePackObjectAttribute), false).FirstOrDefault());
        Assert.NotNull(heightmapType.GetCustomAttributes(typeof(MessagePackObjectAttribute), false).FirstOrDefault());
        Assert.NotNull(presetType.GetCustomAttributes(typeof(MessagePackObjectAttribute), false).FirstOrDefault());

        // Check that ProceduralWorldData has [Key(0)] through [Key(6)] attributes on properties
        var worldDataProps = worldDataType.GetProperties();
        var keysFound = new HashSet<int>();
        foreach (var prop in worldDataProps)
        {
            var keyAttr = prop.GetCustomAttributes(typeof(KeyAttribute), false).FirstOrDefault() as KeyAttribute;
            if (keyAttr?.IntKey != null)
            {
                keysFound.Add(keyAttr.IntKey.Value);
            }
        }

        // Should have keys 0-6 for the 7 fields in ProceduralWorldData
        Assert.Contains(0, keysFound);
        Assert.Contains(1, keysFound);
        Assert.Contains(2, keysFound);
        Assert.Contains(3, keysFound);
        Assert.Contains(4, keysFound);
        Assert.Contains(5, keysFound);
        Assert.Contains(6, keysFound);
    }
}
