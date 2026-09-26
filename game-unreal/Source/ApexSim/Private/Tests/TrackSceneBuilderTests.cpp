#include "ApexTestCommon.h"
#include "StaticMeshAttributes.h"
#include "Track/ApexTrackSceneBuilder.h"
#include "Track/ApexTrackSceneData.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A 10 m x 4 m quad of road in the export's frame: `u` along +X in
	 * metres, `v` across, normal up, wound as the bake winds.
	 */
	FApexTrackScene TrackBuilderQuadScene()
	{
		FApexTrackScene Scene;
		Scene.TrackName = TEXT("Quad");
		FApexTrackMaterial Road;
		Road.Key = TEXT("road");
		Road.Family = TEXT("road");
		Scene.Materials.Add(Road);
		FApexTrackMesh Mesh;
		Mesh.Name = TEXT("road_000");
		Mesh.MaterialKey = TEXT("road");
		Mesh.Positions = {FVector3f(0, -200, 0), FVector3f(1000, -200, 0), FVector3f(1000, 200, 0), FVector3f(0, 200, 0)};
		Mesh.Normals.Init(FVector3f(0, 0, 1), 4);
		Mesh.UVs = {FVector2f(0, 0), FVector2f(10, 0), FVector2f(10, 4), FVector2f(0, 4)};
		Mesh.Indices = {0, 2, 1, 0, 3, 2};
		Scene.Meshes.Add(Mesh);
		return Scene;
	}
}	 // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTrackBuilderPrepareTest, "ApexSim.Track.Builder.Prepare", ApexTestFlags)

bool FApexTrackBuilderPrepareTest::RunTest(const FString& Parameters)
{
	const FApexTrackScene Scene = TrackBuilderQuadScene();
	FApexTrackGeometry Geometry;
	FApexTrackSceneBuilder::PrepareGeometry(Scene, Geometry);

	// The scene's mesh first, then one stand-in per recipe; no line and no
	// grid, so no gantry.
	if (!TestTrue(TEXT("the scene mesh and the stand-ins"), Geometry.Meshes.Num() > 1))
	{
		return false;
	}
	TestFalse(TEXT("nothing built yet"), Geometry.IsBuilt());
	const FApexPreparedMesh& Road = *Geometry.Meshes[0];
	TestEqual(TEXT("named after the scene mesh"), Road.Name, FString(TEXT("road_000")));
	TestEqual(TEXT("points back at it"), Road.SceneMesh, 0);
	TestEqual(TEXT("one slot, the material key"), Road.Slots.Num(), 1);
	TestTrue(TEXT("stand-ins know their kind"), !Geometry.Meshes[1]->PropKind.IsEmpty());
	TestFalse(TEXT("gantry needs a line or a grid"),
		Geometry.Meshes.ContainsByPredicate([](const TUniquePtr<FApexPreparedMesh>& M) { return M->PropKind == TEXT("start_gantry"); }));

	// One vertex instance per source vertex: the runtime's fast build makes a
	// render vertex of each, so one per corner would be 1.5x here and 3x on
	// a real circuit.
	const FMeshDescription& Description = Road.Description;
	TestEqual(TEXT("vertices"), Description.Vertices().Num(), 4);
	TestEqual(TEXT("vertex instances shared by the triangles"), Description.VertexInstances().Num(), 4);
	TestEqual(TEXT("triangles"), Description.Triangles().Num(), 2);
	TestTrue(TEXT("bounds span the quad"),
		Road.Bounds.IsValid && FMath::IsNearlyEqual(Road.Bounds.GetSize().X, 1000.0) && FMath::IsNearlyEqual(Road.Bounds.GetSize().Y, 400.0));

	// Tangents follow `u`, which runs along +X, and sit in the surface.
	FStaticMeshConstAttributes Attributes(Description);
	const TVertexInstanceAttributesConstRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	const TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	for (const FVertexInstanceID Instance : Description.VertexInstances().GetElementIDs())
	{
		const FVector3f T = Tangents[Instance];
		TestTrue(TEXT("unit tangent"), FMath::IsNearlyEqual(T.Size(), 1.0f, 1.0e-3f));
		TestTrue(TEXT("tangent along u"), T.Equals(FVector3f(1, 0, 0), 1.0e-3f));
		TestTrue(TEXT("normal kept"), Normals[Instance].Equals(FVector3f(0, 0, 1), 1.0e-5f));
	}
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
