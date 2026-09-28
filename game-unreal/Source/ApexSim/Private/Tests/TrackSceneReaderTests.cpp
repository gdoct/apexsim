#include "ApexTestCommon.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Track/ApexDdsReader.h"
#include "Track/ApexTrackSceneData.h"
#include "Track/ApexTrackSceneReader.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * `GOLDEN_TRIANGLE_BLOB` from track-core's `the_mesh_blob_layout_is_pinned`
	 * (track-editor/core/tests/ue_export.rs): one uncompressed mesh, `tri`
	 * with material `road`, three vertices, indices 0 2 1. If the exporter's
	 * layout changes, both pins move together.
	 */
	const TCHAR* GoldenTriangleBlobHex =
		TEXT("415045584d45534801000000010000000300000074726904000000726f61640300000003000000000000006c000000")
		TEXT("1f85454131c887c2b6f39d3ffe64e443e3a5bbc11283fc409c440a42be8f9043dd249240f085493c11c73abde9b77f3f")
		TEXT("5396a13d9e5e193f88f44b3f6744193f4a7b03bd76e04c3f6de7fb3dd578e93edd2492409318643fd7a3b03efa7e3240")
		TEXT("000000000200000001000000");

	TArray<uint8> TrackReaderHexBytes(const FString& Hex)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Hex.Len() / 2);
		HexToBytes(Hex, Out.GetData());
		return Out;
	}

	/** A scratch folder under Saved/, emptied first. */
	FString TrackReaderScratchDir()
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("TrackReader"));
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}
}	 // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTrackReaderGoldenBlobTest, "ApexSim.Track.Reader.GoldenBlob", ApexTestFlags)

bool FApexTrackReaderGoldenBlobTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Blob = TrackReaderHexBytes(GoldenTriangleBlobHex);
	TestEqual(TEXT("the golden blob is 155 bytes"), Blob.Num(), 155);

	TArray<FApexTrackMesh> Meshes;
	FString Error;
	if (!TestTrue(TEXT("parses"), FApexTrackSceneReader::ParseMeshBlob(Blob, Meshes, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("one mesh"), Meshes.Num(), 1))
	{
		return false;
	}
	const FApexTrackMesh& Mesh = Meshes[0];
	TestEqual(TEXT("name"), Mesh.Name, FString(TEXT("tri")));
	TestEqual(TEXT("material key"), Mesh.MaterialKey, FString(TEXT("road")));
	TestEqual(TEXT("three vertices"), Mesh.Positions.Num(), 3);
	TestEqual(TEXT("three normals"), Mesh.Normals.Num(), 3);
	TestEqual(TEXT("three uvs"), Mesh.UVs.Num(), 3);
	TestEqual(TEXT("one triangle"), Mesh.NumTriangles(), 1);
	TestTrue(TEXT("first position"), Mesh.Positions[0].Equals(FVector3f(12.345f, -67.891f, 1.234f), 1.0e-4f));
	TestTrue(TEXT("last position"), Mesh.Positions[2].Equals(FVector3f(34.567f, 289.123f, 4.567f), 1.0e-4f));
	TestTrue(TEXT("second normal"), Mesh.Normals[1].Equals(FVector3f(0.0789f, 0.5991f, 0.7967f), 1.0e-5f));
	TestTrue(TEXT("third uv"), Mesh.UVs[2].Equals(FVector2f(0.345f, 2.789f), 1.0e-5f));
	TestTrue(TEXT("winding kept"), Mesh.Indices == TArray<uint32>({0u, 2u, 1u}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTrackReaderRejectsTest, "ApexSim.Track.Reader.Rejects", ApexTestFlags)

bool FApexTrackReaderRejectsTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Golden = TrackReaderHexBytes(GoldenTriangleBlobHex);
	TArray<FApexTrackMesh> Meshes;
	FString Error;

	TArray<uint8> BadMagic = Golden;
	BadMagic[0] = 'X';
	TestFalse(TEXT("bad magic"), FApexTrackSceneReader::ParseMeshBlob(BadMagic, Meshes, Error));

	TArray<uint8> Truncated = Golden;
	Truncated.SetNum(Golden.Num() - 5);
	TestFalse(TEXT("truncated payload"), FApexTrackSceneReader::ParseMeshBlob(Truncated, Meshes, Error));

	TArray<uint8> Trailing = Golden;
	Trailing.Add(0);
	TestFalse(TEXT("trailing bytes"), FApexTrackSceneReader::ParseMeshBlob(Trailing, Meshes, Error));

	TArray<uint8> FutureVersion = Golden;
	FutureVersion[8] = 2;
	TestFalse(TEXT("a newer blob version"), FApexTrackSceneReader::ParseMeshBlob(FutureVersion, Meshes, Error));

	// The last index (1, the final four bytes) pointed past the three vertices.
	TArray<uint8> OutOfRange = Golden;
	OutOfRange[OutOfRange.Num() - 4] = 7;
	TestFalse(TEXT("an index past the vertices"), FApexTrackSceneReader::ParseMeshBlob(OutOfRange, Meshes, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTrackReaderManifestTest, "ApexSim.Track.Reader.Manifest", ApexTestFlags)

bool FApexTrackReaderManifestTest::RunTest(const FString& Parameters)
{
	// A version 2 manifest beside the golden blob, keys in the exporter's order.
	const FString Dir = TrackReaderScratchDir();
	const FString ScenePath = FPaths::Combine(Dir, TEXT("Tiny.uescene.json"));
	const FString BlobPath = FPaths::Combine(Dir, TEXT("Tiny.uemesh"));
	const FString Manifest = TEXT(
		"{\"format\":\"apex-ue-scene\",\"version\":2,\"track_id\":\"tiny-1\",\"track_name\":\"Tiny Ring\","
		"\"track_display_name\":\"Teeny Ring\","
		"\"source_track\":\"Tiny.yaml\",\"source_crc\":3921426583,\"closed_loop\":true,\"length_cm\":123400.0,"
		"\"metadata\":{\"country\":\"Netherlands\",\"city\":\"Zandvoort\",\"category\":\"F1\",\"environment_type\":\"coastal\","
		"\"description\":\"Modelled on a tiny ring.\"},"
		"\"dressing\":{\"season\":\"autumn\",\"spectators\":false},\"mesh_blob\":\"Tiny.uemesh\","
		"\"materials\":[{\"key\":\"road\",\"family\":\"road\",\"base_color\":[0.2,0.2,0.2,1.0]}],"
		"\"meshes\":[{\"name\":\"tri\",\"material_key\":\"road\",\"vertex_count\":3,\"index_count\":3}],"
		"\"props\":[],\"grid\":[],\"centerline\":[]}");
	TestTrue(TEXT("wrote the manifest"), FFileHelper::SaveStringToFile(Manifest, *ScenePath));
	TestTrue(TEXT("wrote the blob"), FFileHelper::SaveArrayToFile(TrackReaderHexBytes(GoldenTriangleBlobHex), *BlobPath));

	FApexTrackSceneHeader Header;
	FString Error;
	if (TestTrue(TEXT("header reads"), FApexTrackSceneReader::LoadHeader(ScenePath, Header, Error)))
	{
		TestEqual(TEXT("version"), Header.Version, 2);
		TestEqual(TEXT("track id"), Header.TrackId, FString(TEXT("tiny-1")));
		TestEqual(TEXT("name"), Header.TrackName, FString(TEXT("Tiny Ring")));
		TestEqual(TEXT("display name"), Header.DisplayName, FString(TEXT("Teeny Ring")));
		TestEqual(TEXT("description"), Header.Description, FString(TEXT("Modelled on a tiny ring.")));
		TestEqual(TEXT("checksum, all 32 bits"), Header.SourceCrc, int64(3921426583LL));
		TestEqual(TEXT("length"), Header.LengthCm, 123400.0f);
		TestTrue(TEXT("closed"), Header.bClosedLoop);
		TestEqual(TEXT("country from the metadata"), Header.Country, FString(TEXT("Netherlands")));
		TestEqual(TEXT("category"), Header.Category, FString(TEXT("F1")));
		TestEqual(TEXT("blob"), Header.MeshBlob, FString(TEXT("Tiny.uemesh")));
	}
	else
	{
		AddError(Error);
	}
	TestEqual(TEXT("stem"), FApexTrackSceneReader::StemOf(ScenePath), FString(TEXT("Tiny")));

	FApexTrackScene Scene;
	if (TestTrue(TEXT("scene reads with its blob"), FApexTrackSceneReader::LoadFromFile(ScenePath, Scene, Error)))
	{
		TestEqual(TEXT("one mesh from the blob"), Scene.Meshes.Num(), 1);
		TestEqual(TEXT("checksum"), Scene.SourceCrc, int64(3921426583LL));
		TestTrue(TEXT("autumn"), Scene.Dressing.IsAutumn());
		TestFalse(TEXT("no spectators"), Scene.Dressing.bSpectators);
	}
	else
	{
		AddError(Error);
	}

	// A manifest that disagrees with its blob is refused, not half-loaded.
	const FString Renamed = Manifest.Replace(TEXT("\"name\":\"tri\""), TEXT("\"name\":\"quad\""));
	TestTrue(TEXT("wrote the bad manifest"), FFileHelper::SaveStringToFile(Renamed, *ScenePath));
	FApexTrackScene Rejected;
	TestFalse(TEXT("a renamed mesh is refused"), FApexTrackSceneReader::LoadFromFile(ScenePath, Rejected, Error));

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}


namespace
{
	/**
	 * A 4 x 4 DXT1 texture with two mips as `scripts/ac_import` writes it:
	 * the 128-byte header, an 8-byte block for level 0 and one for level 1
	 * (a 2 x 2 mip still takes a whole block).
	 */
	TArray<uint8> TrackReaderTinyDds()
	{
		TArray<uint8> Out;
		auto U32 = [&Out](uint32 V) {
			Out.Add(uint8(V & 0xFF));
			Out.Add(uint8((V >> 8) & 0xFF));
			Out.Add(uint8((V >> 16) & 0xFF));
			Out.Add(uint8((V >> 24) & 0xFF));
		};
		Out.Append({'D', 'D', 'S', ' '});
		U32(124);
		U32(0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | 0x20000);	 // caps, height, width, pixelformat, linear size, mip count
		U32(4);														 // height
		U32(4);														 // width
		U32(8);														 // linear size
		U32(0);														 // depth
		U32(2);														 // mip count
		for (int32 i = 0; i < 11; ++i)
		{
			U32(0);
		}
		U32(32);							// pixel format size
		U32(0x4);							// FOURCC
		Out.Append({'D', 'X', 'T', '1'});
		U32(0);
		U32(0);
		U32(0);
		U32(0);
		U32(0);
		U32(0x1000 | 0x400000 | 0x8);	// caps
		U32(0);
		U32(0);
		U32(0);
		U32(0);
		check(Out.Num() == 128);
		// Level 0: colour 0 = red, colour 1 = blue, every index 0.
		Out.Append({0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0});
		// Level 1: white/black, indices 1.
		Out.Append({0xFF, 0xFF, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55});
		return Out;
	}
}	 // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTrackDdsParseTest, "ApexSim.Track.Dds.Parse", ApexTestFlags)

bool FApexTrackDdsParseTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Dds = TrackReaderTinyDds();
	FApexTrackTexture Texture;
	FString Error;
	if (!TestTrue(TEXT("parses"), ApexDds::Parse(Dds, Texture, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("width"), Texture.Width, 4);
	TestEqual(TEXT("height"), Texture.Height, 4);
	TestTrue(TEXT("DXT1"), Texture.Format == PF_DXT1);
	TestEqual(TEXT("two mips"), Texture.Mips.Num(), 2);
	TestEqual(TEXT("level 0 is one block"), Texture.Mips[0].Num(), 8);
	TestEqual(TEXT("level 1 is one block"), Texture.Mips[1].Num(), 8);
	TestEqual(TEXT("level 0 bytes"), int32(Texture.Mips[0][1]), 0xF8);
	TestEqual(TEXT("level 1 bytes"), int32(Texture.Mips[1][4]), 0x55);
	TestEqual(TEXT("block bytes"), ApexDds::MipBytes(PF_DXT5, 6, 6), int64(64));
	TestEqual(TEXT("uncompressed bytes"), ApexDds::MipBytes(PF_B8G8R8A8, 3, 2), int64(24));

	// A truncated lower mip ends the chain; a truncated top level fails.
	TArray<uint8> Short = Dds;
	Short.SetNum(Dds.Num() - 3);
	TestTrue(TEXT("short chain parses"), ApexDds::Parse(Short, Texture, Error));
	TestEqual(TEXT("with one mip"), Texture.Mips.Num(), 1);
	Short.SetNum(130);
	TestFalse(TEXT("a truncated level 0 fails"), ApexDds::Parse(Short, Texture, Error));
	TArray<uint8> NotDds = Dds;
	NotDds[0] = 'X';
	TestFalse(TEXT("not a DDS"), ApexDds::Parse(NotDds, Texture, Error));

	// 24-bit uncompressed becomes BGRA8.
	TArray<uint8> Rgb;
	auto U32 = [&Rgb](uint32 V) {
		Rgb.Add(uint8(V & 0xFF));
		Rgb.Add(uint8((V >> 8) & 0xFF));
		Rgb.Add(uint8((V >> 16) & 0xFF));
		Rgb.Add(uint8((V >> 24) & 0xFF));
	};
	Rgb.Append({'D', 'D', 'S', ' '});
	U32(124);
	U32(0x1 | 0x2 | 0x4 | 0x1000);
	U32(1);
	U32(2);
	U32(6);
	U32(0);
	U32(1);
	for (int32 i = 0; i < 11; ++i)
	{
		U32(0);
	}
	U32(32);
	U32(0x40);	 // RGB
	U32(0);
	U32(24);
	U32(0x00FF0000);
	U32(0x0000FF00);
	U32(0x000000FF);
	U32(0);
	U32(0x1000);
	U32(0);
	U32(0);
	U32(0);
	U32(0);
	Rgb.Append({10, 20, 30, 40, 50, 60});
	if (TestTrue(TEXT("rgb24 parses"), ApexDds::Parse(Rgb, Texture, Error)))
	{
		TestTrue(TEXT("as BGRA8"), Texture.Format == PF_B8G8R8A8);
		TestEqual(TEXT("two pixels of four bytes"), Texture.Mips[0].Num(), 8);
		TestEqual(TEXT("blue first"), int32(Texture.Mips[0][0]), 10);
		TestEqual(TEXT("opaque"), int32(Texture.Mips[0][3]), 255);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTrackReaderVersion3Test, "ApexSim.Track.Reader.Version3", ApexTestFlags)

bool FApexTrackReaderVersion3Test::RunTest(const FString& Parameters)
{
	// A version 3 manifest as the AC importer writes one: a textured
	// scenery material, a surface key naming its ground set, a mesh with a
	// draw distance and no collision, the blob beside it, the DDS in the
	// textures folder.
	const FString Dir = TrackReaderScratchDir();
	const FString BlobPath = FPaths::Combine(Dir, TEXT("Imported.uemesh"));
	TestTrue(TEXT("wrote the blob"), FFileHelper::SaveArrayToFile(TrackReaderHexBytes(GoldenTriangleBlobHex), *BlobPath));
	const FString TexDir = FPaths::Combine(Dir, TEXT("Imported.textures"));
	IFileManager::Get().MakeDirectory(*TexDir, true);
	TestTrue(TEXT("wrote the texture"), FFileHelper::SaveArrayToFile(TrackReaderTinyDds(), *FPaths::Combine(TexDir, TEXT("fence.dds"))));
	const FString Manifest =
		TEXT("{\"format\":\"apex-ue-scene\",\"version\":3,\"track_id\":\"id-3\",\"track_name\":\"Imported\",")
		TEXT("\"source_track\":\"Imported.yaml\",\"source_crc\":1,\"closed_loop\":true,\"length_cm\":100.0,")
		TEXT("\"metadata\":{\"country\":\"NL\"},\"dressing\":{\"season\":\"summer\",\"spectators\":true},\"imported\":\"ac\",")
		TEXT("\"mesh_blob\":\"Imported.uemesh\",")
		TEXT("\"materials\":[{\"key\":\"road\",\"family\":\"road\",\"base_color\":[0.2,0.2,0.2,1.0]},")
		TEXT("{\"key\":\"ac_gravel\",\"family\":\"surface\",\"base_color\":[0.6,0.5,0.4,1.0],\"ground_set\":\"gravel\"},")
		TEXT("{\"key\":\"scenery_fence\",\"family\":\"scenery\",\"base_color\":[1.0,1.0,1.0,1.0],")
		TEXT("\"texture\":\"Imported.textures/fence.dds\",\"blend\":\"masked\",\"two_sided\":true,\"roughness\":0.7,\"alpha_cutoff\":0.4},")
		TEXT("{\"key\":\"scenery_gone\",\"family\":\"scenery\",\"base_color\":[0.3,0.3,0.3,1.0],\"texture\":\"Imported.textures/missing.dds\",\"blend\":\"opaque\"}],")
		TEXT("\"meshes\":[{\"name\":\"tri\",\"material_key\":\"road\",\"vertex_count\":3,\"index_count\":3,\"draw_distance_m\":400.0,\"collision\":false}],")
		TEXT("\"props\":[],\"grid\":[],\"centerline\":[],\"pit_lane\":null,\"start_finish\":null}");
	const FString ManifestPath = FPaths::Combine(Dir, TEXT("Imported.uescene.json"));
	TestTrue(TEXT("wrote the manifest"), FFileHelper::SaveStringToFile(Manifest, *ManifestPath));

	FApexTrackSceneHeader Header;
	FString Error;
	if (!TestTrue(TEXT("header reads"), FApexTrackSceneReader::LoadHeader(ManifestPath, Header, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("version 3"), Header.Version, 3);
	TestEqual(TEXT("imported marker"), Header.Imported, FString(TEXT("ac")));
	TestEqual(TEXT("blob name"), Header.MeshBlob, FString(TEXT("Imported.uemesh")));

	FApexTrackScene Scene;
	if (!TestTrue(TEXT("scene reads"), FApexTrackSceneReader::LoadFromFile(ManifestPath, Scene, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("imported"), Scene.Imported, FString(TEXT("ac")));
	TestEqual(TEXT("base dir"), Scene.BaseDir, FPaths::GetPath(ManifestPath));
	TestEqual(TEXT("one mesh"), Scene.Meshes.Num(), 1);
	TestTrue(TEXT("draw distance in cm"), FMath::IsNearlyEqual(Scene.Meshes[0].DrawDistanceCm, 40000.0f));
	TestFalse(TEXT("no collision"), Scene.Meshes[0].bCollision);
	const FApexTrackMaterial* Gravel = Scene.FindMaterial(TEXT("ac_gravel"));
	TestTrue(TEXT("ground set"), Gravel && Gravel->GroundSet == TEXT("gravel"));
	const FApexTrackMaterial* Fence = Scene.FindMaterial(TEXT("scenery_fence"));
	if (TestNotNull(TEXT("fence material"), Fence))
	{
		TestTrue(TEXT("scenery"), Fence->IsScenery());
		TestEqual(TEXT("blend"), Fence->Blend, FString(TEXT("masked")));
		TestTrue(TEXT("two sided"), Fence->bTwoSided);
		TestTrue(TEXT("roughness"), FMath::IsNearlyEqual(Fence->Roughness, 0.7f));
		TestTrue(TEXT("cutoff"), FMath::IsNearlyEqual(Fence->AlphaCutoff, 0.4f));
		TestEqual(TEXT("texture path kept"), Fence->Texture, FString(TEXT("Imported.textures/fence.dds")));
		const FApexTrackTexture* Texture = Scene.Textures.Find(Fence->Texture);
		TestTrue(TEXT("texture parsed with its two mips"), Texture && Texture->Mips.Num() == 2 && Texture->Format == PF_DXT1);
	}
	const FApexTrackMaterial* Gone = Scene.FindMaterial(TEXT("scenery_gone"));
	TestTrue(TEXT("a missing texture leaves the material flat"), Gone && Gone->Texture.IsEmpty());
	TestEqual(TEXT("only the readable texture is kept"), Scene.Textures.Num(), 1);

	// A version 2 mesh keeps the defaults the builder relied on.
	FApexTrackMesh Plain;
	TestTrue(TEXT("always drawn"), Plain.DrawDistanceCm == 0.0f);
	TestTrue(TEXT("collides"), Plain.bCollision);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
