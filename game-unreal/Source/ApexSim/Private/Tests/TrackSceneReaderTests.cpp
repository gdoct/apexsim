#include "ApexTestCommon.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
		"\"source_track\":\"Tiny.yaml\",\"source_crc\":3921426583,\"closed_loop\":true,\"length_cm\":123400.0,"
		"\"metadata\":{\"country\":\"Netherlands\",\"city\":\"Zandvoort\",\"category\":\"F1\",\"environment_type\":\"coastal\"},"
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

#endif	  // WITH_DEV_AUTOMATION_TESTS
