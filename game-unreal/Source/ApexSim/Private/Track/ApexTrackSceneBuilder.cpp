#include "Track/ApexTrackSceneBuilder.h"

#include "Algo/Count.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Race/ApexPropActors.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "Track/ApexGroundMaterials.h"
#include "Track/ApexPropLibrary.h"
#include "Track/ApexTrackCollisionComponent.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogApexTrack);

const FName FApexTrackSceneBuilder::PropTag(TEXT("ApexProp"));
const FName FApexTrackSceneBuilder::TrackMeshTag(TEXT("ApexTrackMesh"));
const FName FApexTrackSceneBuilder::StartLightsTag(TEXT("ApexStartLights"));
const FName FApexTrackSceneBuilder::StartLightTag(TEXT("ApexStartLight"));

namespace
{
	/** Engine placeholder used for prop kinds that have no generated mesh. */
	const TCHAR* kPlaceholderPropMesh = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** Default length of a grandstand the scene gives none for, metres. */
	constexpr float kDefaultStandLengthM = 30.0f;

	/** A text as an asset name fragment: lower case, one word. */
	FString TextKey(const FString& Text)
	{
		FString Key = Text.ToLower();
		for (TCHAR& C : Key)
		{
			if (!FChar::IsAlnum(C))
			{
				C = TEXT('_');
			}
		}
		return Key;
	}

	/** The three baked maps of one ground set, once loaded. */
	struct FGroundMapSet
	{
		UTexture* Albedo = nullptr;
		UTexture* Normal = nullptr;
		UTexture* Roughness = nullptr;

		bool IsComplete() const
		{
			return Albedo != nullptr && Normal != nullptr && Roughness != nullptr;
		}
	};

	/**
	 * A baked ground set from `/Game/Ground`, or an empty one when it has
	 * not been imported (`-run=ApexGroundTexImport`). The package is tested
	 * before it is loaded: a missing one is the normal case here, not a
	 * failure, and `LoadObject` would fill the log with errors about it.
	 */
	FGroundMapSet LoadGroundSet(const FString& Set)
	{
		auto Load = [](const FString& PackageName) -> UTexture* {
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}
			FString Left;
			FString Right;
			const FString ObjectName = PackageName.Split(TEXT("/"), &Left, &Right,
										   ESearchCase::CaseSensitive, ESearchDir::FromEnd)
				? Right
				: PackageName;
			return LoadObject<UTexture>(nullptr, *(PackageName + TEXT(".") + ObjectName));
		};
		FGroundMapSet Maps;
		Maps.Albedo = Load(ApexGround::TexturePath(Set, TEXT("col")));
		Maps.Normal = Load(ApexGround::TexturePath(Set, TEXT("nrm")));
		Maps.Roughness = Load(ApexGround::TexturePath(Set, TEXT("rough")));
		return Maps;
	}

	/**
	 * What a surface band fades toward at its edges: the dust and rubber a
	 * few hundred laps leave where the grass meets the road, not a lighter
	 * shade of the grass. Blended 70 % of the way from the band's own
	 * colour, so a gravel trap frays sandy and astroturf frays grey.
	 */
	const FLinearColor kEdgeDustColor(0.055f, 0.050f, 0.043f);

	/**
	 * How far the macro noise is allowed to move the fringe boundary. One
	 * exactly: the graph subtracts half before adding the recentred noise,
	 * so at this value the interior of a band is provably untouched and
	 * anything larger would start dirtying the middle of the grass.
	 */
	constexpr float kEdgeNoise = 1.0f;

	/**
	 * Rough blockout size per prop kind, in centimeters.
	 *
	 * Kinds without a generated stand-in (lights, cones, anything new) become
	 * a scaled engine cube. A pole-sized box in the right place is worth a
	 * great deal when you are checking whether a circuit feels right; a
	 * uniform cube everywhere is not.
	 */
	FVector PlaceholderPropSize(const FString& Kind)
	{
		if (Kind == TEXT("light"))
		{
			return FVector(40.0f, 40.0f, 1200.0f);
		}
		if (Kind == TEXT("cone"))
		{
			return FVector(40.0f, 40.0f, 60.0f);
		}
		return FVector(100.0f, 100.0f, 100.0f);
	}

	/** Cubes are 100 cm; placeholder sizes are absolute, so convert. */
	FVector PlaceholderPropScale(const FString& Kind, float PropScale)
	{
		return PlaceholderPropSize(Kind) * (PropScale / 100.0f);
	}

	/**
	 * Generated material for one prop kind, keyed `prop_<kind>`.
	 *
	 * Without these the placeholder cubes render in the engine's default
	 * checker grey, which is most of why a circuit lined with thousands of
	 * them reads as a paste-up rather than a place.
	 */
	struct FPropMaterialSpec
	{
		const TCHAR* Key;
		FLinearColor Color;
		float Roughness;
		float NoiseAmount;
		float NoiseScale;
		/** 0 disables the stripe. In the mesh's `u` units (see each recipe below). */
		float StripePeriod;
		FLinearColor Secondary;
		/**
		 * Per-instance colour swing: albedo is scaled by `1 + data0 * Tint`
		 * with `data0` in ±1 from the instanced component. Zero for
		 * everything that is not instanced foliage.
		 */
		FLinearColor InstanceTint;
	};

	const FLinearColor kNoTint(0.0f, 0.0f, 0.0f, 0.0f);

	const FPropMaterialSpec kPropMaterials[] = {
		// Tire walls: stacks of tyres, alternating red and white per stack
		// (each stack parks its `u` inside one stripe of period 0.26).
		{TEXT("prop_tire_wall"), FLinearColor(0.42f, 0.05f, 0.04f), 0.85f, 0.06f, 0.001f, 0.26f,
			FLinearColor(0.80f, 0.78f, 0.74f), kNoTint},
		// Armco: galvanised steel.
		{TEXT("prop_barrier"), FLinearColor(0.50f, 0.51f, 0.53f), 0.45f, 0.10f, 0.004f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_grandstand"), FLinearColor(0.30f, 0.31f, 0.34f), 0.60f, 0.12f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
		// Seat blocks alternate two colours block by block (`u` = block index).
		{TEXT("prop_grandstand_seats"), FLinearColor(0.05f, 0.12f, 0.40f), 0.50f, 0.00f, 0.002f,
			1.0f, FLinearColor(0.55f, 0.06f, 0.05f), kNoTint},
		{TEXT("prop_building"), FLinearColor(0.42f, 0.39f, 0.34f), 0.80f, 0.20f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_building_glass"), FLinearColor(0.03f, 0.04f, 0.05f), 0.15f, 0.00f, 0.002f,
			0.0f, FLinearColor::Black, kNoTint},
		// Foliage: each tree swings between a yellower and a bluer green.
		{TEXT("prop_tree"), FLinearColor(0.06f, 0.18f, 0.05f), 1.00f, 0.50f, 0.010f, 0.0f,
			FLinearColor::Black, FLinearColor(0.30f, 0.06f, -0.25f, 0.0f)},
		{TEXT("prop_tree_bark"), FLinearColor(0.16f, 0.11f, 0.07f), 0.95f, 0.30f, 0.010f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_sign"), FLinearColor(0.85f, 0.85f, 0.80f), 0.35f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_sign_post"), FLinearColor(0.30f, 0.30f, 0.32f), 0.50f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_light"), FLinearColor(0.30f, 0.30f, 0.32f), 0.50f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_cone"), FLinearColor(0.85f, 0.30f, 0.03f), 0.60f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
		{TEXT("prop_default"), FLinearColor(0.45f, 0.45f, 0.47f), 0.70f, 0.10f, 0.002f, 0.0f,
			FLinearColor::Black, kNoTint},
	};

	/**
	 * Vertex buffers for a generated mesh, one polygon group per material
	 * slot. Everything is flat-shaded with its own vertices per face except
	 * the lathes, which share a smooth ring; units are centimetres and the
	 * pivot is on the ground so props sit where the export seats them.
	 */
	struct FProcMesh
	{
		struct FSlot
		{
			FString MaterialKey;
			TArray<uint32> Indices;
		};

		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector2f> UVs;
		TArray<FSlot> Slots;

		int32 AddSlot(const FString& MaterialKey)
		{
			FSlot Slot;
			Slot.MaterialKey = MaterialKey;
			return Slots.Add(MoveTemp(Slot));
		}

		uint32 AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV)
		{
			Positions.Add(Position);
			Normals.Add(Normal);
			UVs.Add(UV);
			return Positions.Num() - 1;
		}

		/**
		 * Triangle wound to face `Normal`. Unreal's front face is clockwise
		 * seen from its normal — the mirror of the right-hand rule — so the
		 * corners are swapped whenever the cross product agrees with it.
		 */
		void AddTriangle(int32 Slot, uint32 A, uint32 B, uint32 C, const FVector3f& Normal)
		{
			const FVector3f Cross = FVector3f::CrossProduct(
				Positions[B] - Positions[A], Positions[C] - Positions[A]);
			if (FVector3f::DotProduct(Cross, Normal) > 0.0f)
			{
				Swap(B, C);
			}
			Slots[Slot].Indices.Append({A, B, C});
		}

		/** Flat triangle with its own vertices. */
		void AddFace(int32 Slot, const FVector3f& P0, const FVector3f& P1, const FVector3f& P2,
			const FVector3f& Normal, const FVector2f& UV0 = FVector2f::ZeroVector,
			const FVector2f& UV1 = FVector2f(1.0f, 0.0f),
			const FVector2f& UV2 = FVector2f(0.5f, 1.0f))
		{
			const uint32 A = AddVertex(P0, Normal, UV0);
			const uint32 B = AddVertex(P1, Normal, UV1);
			const uint32 C = AddVertex(P2, Normal, UV2);
			AddTriangle(Slot, A, B, C, Normal);
		}

		/** Flat quad with its own vertices; corners in perimeter order. */
		void AddQuad(int32 Slot, const FVector3f& P0, const FVector3f& P1, const FVector3f& P2,
			const FVector3f& P3, const FVector3f& Normal, const FVector2f& UV0 = FVector2f::ZeroVector,
			const FVector2f& UV1 = FVector2f(1.0f, 0.0f), const FVector2f& UV2 = FVector2f(1.0f, 1.0f),
			const FVector2f& UV3 = FVector2f(0.0f, 1.0f))
		{
			const uint32 A = AddVertex(P0, Normal, UV0);
			const uint32 B = AddVertex(P1, Normal, UV1);
			const uint32 C = AddVertex(P2, Normal, UV2);
			const uint32 D = AddVertex(P3, Normal, UV3);
			AddTriangle(Slot, A, B, C, Normal);
			AddTriangle(Slot, A, C, D, Normal);
		}

		/**
		 * Axis-aligned box, six flat faces. Each face's UV is its in-plane
		 * extent times `UvScale`, offset by `UvOffset`, so a box can be
		 * parked inside one stripe of the parent material by offsetting `u`.
		 */
		void AddBox(int32 Slot, const FVector3f& Min, const FVector3f& Max,
			const FVector2f& UvOffset = FVector2f::ZeroVector, float UvScale = 0.01f)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				for (float Sign : {1.0f, -1.0f})
				{
					const int32 U = (Axis + 1) % 3;
					const int32 V = (Axis + 2) % 3;
					FVector3f Normal = FVector3f::ZeroVector;
					Normal[Axis] = Sign;
					const float Plane = Sign > 0.0f ? Max[Axis] : Min[Axis];
					const float Us[4] = {Min[U], Max[U], Max[U], Min[U]};
					const float Vs[4] = {Min[V], Min[V], Max[V], Max[V]};
					FVector3f P[4];
					FVector2f T[4];
					for (int32 i = 0; i < 4; ++i)
					{
						P[i][Axis] = Plane;
						P[i][U] = Us[i];
						P[i][V] = Vs[i];
						T[i] = UvOffset + FVector2f(Us[i] - Min[U], Vs[i] - Min[V]) * UvScale;
					}
					AddQuad(Slot, P[0], P[1], P[2], P[3], Normal, T[0], T[1], T[2], T[3]);
				}
			}
		}

		/**
		 * Surface of revolution about a vertical axis through `Center`,
		 * between a bottom ring and a top ring, smooth-shaded; a cone when
		 * `RadiusTop` is 0. `u` runs `UvOffset.X` plus the angle fraction
		 * times `UvScale`; `v` is 0 at the bottom and 1 at the top.
		 */
		void AddLathe(int32 Slot, const FVector3f& Center, float RadiusBottom, float ZBottom,
			float RadiusTop, float ZTop, int32 Sides, bool bCapBottom, bool bCapTop,
			const FVector2f& UvOffset = FVector2f::ZeroVector, float UvScale = 1.0f)
		{
			const bool bCone = RadiusTop <= KINDA_SMALL_NUMBER;
			const float DZ = ZTop - ZBottom;
			const float DR = RadiusBottom - RadiusTop;
			TArray<uint32> Bottom;
			TArray<uint32> Top;
			TArray<FVector3f> RingNormals;
			for (int32 i = 0; i <= Sides; ++i)
			{
				const float Angle = 2.0f * PI * i / Sides;
				const float C = FMath::Cos(Angle);
				const float S = FMath::Sin(Angle);
				const FVector3f Normal = FVector3f(C * DZ, S * DZ, DR).GetSafeNormal();
				RingNormals.Add(Normal);
				const float U = UvOffset.X + UvScale * i / Sides;
				Bottom.Add(AddVertex(Center + FVector3f(RadiusBottom * C, RadiusBottom * S, ZBottom),
					Normal, FVector2f(U, UvOffset.Y)));
				Top.Add(AddVertex(Center + FVector3f(RadiusTop * C, RadiusTop * S, ZTop), Normal,
					FVector2f(U, UvOffset.Y + 1.0f)));
			}
			for (int32 i = 0; i < Sides; ++i)
			{
				// The winding test wants a normal the face agrees with; the
				// ring normal of the first corner is close enough to the
				// facet's own for that sign to be right.
				const FVector3f Facet = (RingNormals[i] + RingNormals[i + 1]).GetSafeNormal();
				AddTriangle(Slot, Bottom[i], Bottom[i + 1], Top[i], Facet);
				if (!bCone)
				{
					AddTriangle(Slot, Bottom[i + 1], Top[i + 1], Top[i], Facet);
				}
			}
			auto Cap = [&](float Radius, float Z, const FVector3f& Normal) {
				const uint32 Middle = AddVertex(Center + FVector3f(0.0f, 0.0f, Z), Normal,
					UvOffset + FVector2f(0.5f * UvScale, 0.5f));
				TArray<uint32> Ring;
				for (int32 i = 0; i <= Sides; ++i)
				{
					const float Angle = 2.0f * PI * i / Sides;
					Ring.Add(AddVertex(
						Center + FVector3f(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), Z),
						Normal,
						UvOffset
							+ FVector2f(UvScale * (0.5f + 0.5f * FMath::Cos(Angle)),
								0.5f + 0.5f * FMath::Sin(Angle))));
				}
				for (int32 i = 0; i < Sides; ++i)
				{
					AddTriangle(Slot, Middle, Ring[i], Ring[i + 1], Normal);
				}
			};
			if (bCapBottom)
			{
				Cap(RadiusBottom, ZBottom, FVector3f(0.0f, 0.0f, -1.0f));
			}
			if (bCapTop && !bCone)
			{
				Cap(RadiusTop, ZTop, FVector3f(0.0f, 0.0f, 1.0f));
			}
		}
	};

	/*
	 * Prop recipes. Local axes follow the export's yaw: +X is the prop's
	 * heading (for anything the groomer aligns to the road, that is the
	 * direction of travel), +Y is 90° clockwise from it seen from above.
	 */

	/** Tree: eight-sided trunk under two stacked canopy cones, ~8 m tall. */
	void BuildTreeMesh(FProcMesh& Mesh)
	{
		const int32 Bark = Mesh.AddSlot(TEXT("prop_tree_bark"));
		const int32 Foliage = Mesh.AddSlot(TEXT("prop_tree"));
		const FVector3f Origin = FVector3f::ZeroVector;
		Mesh.AddLathe(Bark, Origin, 40.0f, 0.0f, 30.0f, 250.0f, 8, false, false);
		Mesh.AddLathe(Foliage, Origin, 250.0f, 250.0f, 0.0f, 650.0f, 8, true, false);
		Mesh.AddLathe(Foliage, Origin, 180.0f, 450.0f, 0.0f, 800.0f, 8, true, false);
	}

	/**
	 * Tire wall: six stacks of tyres along X, 4 m long and 1 m tall at scale
	 * 1. Each stack sits inside its own stripe of the material's 0.26 period
	 * so stacks alternate red and white.
	 */
	void BuildTireWallMesh(FProcMesh& Mesh)
	{
		const int32 Slot = Mesh.AddSlot(TEXT("prop_tire_wall"));
		const int32 Stacks = 6;
		const float Pitch = 400.0f / Stacks;
		const float Radius = Pitch * 0.5f - 1.0f;
		for (int32 i = 0; i < Stacks; ++i)
		{
			const FVector3f Center(-200.0f + Pitch * (i + 0.5f), 0.0f, 0.0f);
			Mesh.AddLathe(Slot, Center, Radius, 0.0f, Radius, 100.0f, 8, false, true,
				FVector2f(0.26f * i + 0.05f, 0.0f), 0.1f);
		}
	}

	/**
	 * Armco barrier: a W-profile rail along X on two posts, 4 m long at scale
	 * 1. The rail's face bulges toward local +Y; the level turns each
	 * segment so that side is the one the road is on.
	 */
	void BuildBarrierMesh(FProcMesh& Mesh)
	{
		const int32 Slot = Mesh.AddSlot(TEXT("prop_barrier"));
		const float Half = 200.0f;
		// (y, z) cross-section, bottom to top: bulge, valley, bulge.
		const FVector2f Profile[] = {
			FVector2f(0.0f, 44.0f), FVector2f(6.0f, 52.0f), FVector2f(1.0f, 60.0f),
			FVector2f(6.0f, 68.0f), FVector2f(0.0f, 76.0f)};
		const int32 Segments = UE_ARRAY_COUNT(Profile) - 1;
		for (int32 i = 0; i < Segments; ++i)
		{
			const FVector2f& A = Profile[i];
			const FVector2f& B = Profile[i + 1];
			// Outward normal of the segment, in the (y, z) plane.
			const FVector2f Along = (B - A).GetSafeNormal();
			const FVector3f Front(0.0f, Along.Y, -Along.X);
			const float V0 = static_cast<float>(i) / Segments;
			const float V1 = static_cast<float>(i + 1) / Segments;
			Mesh.AddQuad(Slot, FVector3f(-Half, A.X, A.Y), FVector3f(Half, A.X, A.Y),
				FVector3f(Half, B.X, B.Y), FVector3f(-Half, B.X, B.Y), Front, FVector2f(0.0f, V0),
				FVector2f(4.0f, V0), FVector2f(4.0f, V1), FVector2f(0.0f, V1));
			// The back of the sheet, one centimetre behind, so the rail is
			// not see-through from the runoff side.
			const float Back = -1.0f;
			Mesh.AddQuad(Slot, FVector3f(-Half, A.X + Back, A.Y), FVector3f(Half, A.X + Back, A.Y),
				FVector3f(Half, B.X + Back, B.Y), FVector3f(-Half, B.X + Back, B.Y), -Front,
				FVector2f(0.0f, V0), FVector2f(4.0f, V0), FVector2f(4.0f, V1), FVector2f(0.0f, V1));
		}
		for (float X : {-100.0f, 100.0f})
		{
			Mesh.AddBox(Slot, FVector3f(X - 5.0f, -13.0f, 0.0f), FVector3f(X + 5.0f, -2.0f, 72.0f));
		}
	}

	/**
	 * Grandstand: six raked tiers with a row of seat blocks each, a back
	 * wall, and a roof on four posts. 30 m along X, 12 m deep, 9 m tall at
	 * scale 1. The open (seating) side faces local +Y; the level flips the
	 * stand 180° when the road turns out to lie on its -Y side.
	 */
	void BuildGrandstandMesh(FProcMesh& Mesh)
	{
		const int32 Structure = Mesh.AddSlot(TEXT("prop_grandstand"));
		const int32 Seats = Mesh.AddSlot(TEXT("prop_grandstand_seats"));
		const float HalfLength = 1500.0f;
		const float Front = 600.0f;
		const float Back = -600.0f;
		const int32 Tiers = 6;
		const float Rise = 90.0f;
		const float Run = 100.0f;
		for (int32 Tier = 0; Tier < Tiers; ++Tier)
		{
			const float FrontY = Front - Run * Tier;
			const float Top = Rise * (Tier + 1);
			Mesh.AddBox(Structure, FVector3f(-HalfLength, Back + 100.0f, 0.0f),
				FVector3f(HalfLength, FrontY, Top));
			// Seat blocks on the back half of the tread, 1 m pitch; `u` is
			// the block index so the material's stripe alternates colours.
			int32 Index = 0;
			for (float X = -HalfLength + 50.0f; X + 100.0f <= HalfLength - 50.0f; X += 100.0f, ++Index)
			{
				Mesh.AddBox(Seats, FVector3f(X + 5.0f, FrontY - 90.0f, Top),
					FVector3f(X + 95.0f, FrontY - 45.0f, Top + 45.0f), FVector2f(Index, 0.0f),
					0.005f);
			}
		}
		Mesh.AddBox(Structure, FVector3f(-HalfLength, Back, 0.0f),
			FVector3f(HalfLength, Back + 100.0f, 900.0f));
		Mesh.AddBox(Structure, FVector3f(-HalfLength - 50.0f, Back, 880.0f),
			FVector3f(HalfLength + 50.0f, Front + 50.0f, 900.0f));
		for (float X : {-1400.0f, -467.0f, 467.0f, 1400.0f})
		{
			Mesh.AddBox(Structure, FVector3f(X - 10.0f, Front - 40.0f, 0.0f),
				FVector3f(X + 10.0f, Front - 20.0f, 880.0f));
		}
	}

	/**
	 * Building: a 15 × 10 m block with a pitched roof (ridge along X) and a
	 * band of dark glazing on both long faces. 8 m to the ridge at scale 1.
	 */
	void BuildBuildingMesh(FProcMesh& Mesh)
	{
		const int32 Walls = Mesh.AddSlot(TEXT("prop_building"));
		const int32 Glass = Mesh.AddSlot(TEXT("prop_building_glass"));
		const FVector3f Min(-750.0f, -500.0f, 0.0f);
		const FVector3f Max(750.0f, 500.0f, 650.0f);
		Mesh.AddBox(Walls, Min, Max);

		const float Eave = 640.0f;
		const float Ridge = 800.0f;
		const float Overhang = 40.0f;
		const float X0 = Min.X - Overhang;
		const float X1 = Max.X + Overhang;
		for (float Side : {-1.0f, 1.0f})
		{
			const float EaveY = Side * (Max.Y + Overhang);
			const FVector3f Normal =
				FVector3f(0.0f, Side * (Ridge - Eave), Max.Y + Overhang).GetSafeNormal();
			Mesh.AddQuad(Walls, FVector3f(X0, EaveY, Eave), FVector3f(X1, EaveY, Eave),
				FVector3f(X1, 0.0f, Ridge), FVector3f(X0, 0.0f, Ridge), Normal);
			// Underside of the overhang, so the eave has thickness from below.
			Mesh.AddQuad(Walls, FVector3f(X0, EaveY, Eave - 10.0f), FVector3f(X1, EaveY, Eave - 10.0f),
				FVector3f(X1, EaveY, Eave), FVector3f(X0, EaveY, Eave),
				FVector3f(0.0f, Side, 0.0f));
		}
		for (float Side : {-1.0f, 1.0f})
		{
			const float X = Side > 0.0f ? Max.X : Min.X;
			Mesh.AddFace(Walls, FVector3f(X, Min.Y, Eave), FVector3f(X, Max.Y, Eave),
				FVector3f(X, 0.0f, Ridge), FVector3f(Side, 0.0f, 0.0f));
		}
		for (float Side : {-1.0f, 1.0f})
		{
			const float Y = Side * (Max.Y + 1.0f);
			Mesh.AddQuad(Glass, FVector3f(Min.X + 50.0f, Y, 300.0f), FVector3f(Max.X - 50.0f, Y, 300.0f),
				FVector3f(Max.X - 50.0f, Y, 450.0f), FVector3f(Min.X + 50.0f, Y, 450.0f),
				FVector3f(0.0f, Side, 0.0f));
		}
	}

	/**
	 * Distance board: a 2 × 1.2 m board on two posts, its bottom edge 1 m
	 * up, standing across the road (the board's plane is local YZ). The
	 * level adds the text on both faces.
	 */
	void BuildSignMesh(FProcMesh& Mesh)
	{
		const int32 Board = Mesh.AddSlot(TEXT("prop_sign"));
		const int32 Post = Mesh.AddSlot(TEXT("prop_sign_post"));
		Mesh.AddBox(Board, FVector3f(-3.0f, -100.0f, 100.0f), FVector3f(3.0f, 100.0f, 220.0f));
		Mesh.AddBox(Post, FVector3f(-5.0f, -85.0f, 0.0f), FVector3f(5.0f, -75.0f, 100.0f));
		Mesh.AddBox(Post, FVector3f(-5.0f, 75.0f, 0.0f), FVector3f(5.0f, 85.0f, 100.0f));
	}

	/*
	 * Start-light gantry, in the line's frame: +X is the direction of
	 * travel, the grid is behind at -X. Two posts either side of the road,
	 * a beam across the top, and a panel under the beam whose -X face
	 * carries the five lights (separate components, see SpawnStartLights).
	 */
	const float kGantryPostSize = 30.0f;
	const float kGantryHeight = 600.0f;
	const float kGantryBeam = 40.0f;
	const float kGantryPanelWidth = 500.0f;
	const float kGantryPanelHeight = 120.0f;
	const float kGantryPanelDepth = 25.0f;
	const float kGantryLightPitch = 90.0f;
	const float kGantryLightRadius = 20.0f;
	const float kGantryLightDepth = 6.0f;
	/** Centimetres past the line. */
	const float kGantryOffset = 1200.0f;
	/** Road width when the export has no line of its own. */
	const float kDefaultStartWidth = 1400.0f;

	void BuildStartGantryMesh(FProcMesh& Mesh, float RoadWidthCm)
	{
		const int32 Structure = Mesh.AddSlot(TEXT("prop_light"));
		const int32 Panel = Mesh.AddSlot(TEXT("prop_building_glass"));
		const float Half = RoadWidthCm * 0.5f + 100.0f;
		const float P = kGantryPostSize * 0.5f;
		for (float Side : {-1.0f, 1.0f})
		{
			const float Y = Side * Half;
			Mesh.AddBox(Structure, FVector3f(-P, Y - P, 0.0f), FVector3f(P, Y + P, kGantryHeight));
		}
		Mesh.AddBox(Structure, FVector3f(-kGantryBeam * 0.5f, -Half - P, kGantryHeight),
			FVector3f(kGantryBeam * 0.5f, Half + P, kGantryHeight + kGantryBeam));
		Mesh.AddBox(Panel,
			FVector3f(-kGantryPanelDepth * 0.5f, -kGantryPanelWidth * 0.5f,
				kGantryHeight - kGantryPanelHeight),
			FVector3f(kGantryPanelDepth * 0.5f, kGantryPanelWidth * 0.5f, kGantryHeight));
	}

	/** One lens: a short cylinder along -X with its face at the -X end. */
	void BuildStartLightMesh(FProcMesh& Mesh)
	{
		const int32 Slot = Mesh.AddSlot(TEXT("start_light"));
		const int32 Sides = 16;
		const float R = kGantryLightRadius;
		TArray<uint32> Front;
		TArray<uint32> Back;
		for (int32 i = 0; i <= Sides; ++i)
		{
			const float Angle = 2.0f * PI * i / Sides;
			const float C = FMath::Cos(Angle);
			const float S = FMath::Sin(Angle);
			const FVector3f Normal(0.0f, C, S);
			const float U = static_cast<float>(i) / Sides;
			Front.Add(Mesh.AddVertex(
				FVector3f(-kGantryLightDepth, R * C, R * S), Normal, FVector2f(U, 0.0f)));
			Back.Add(Mesh.AddVertex(FVector3f(0.0f, R * C, R * S), Normal, FVector2f(U, 1.0f)));
		}
		for (int32 i = 0; i < Sides; ++i)
		{
			const FVector3f Facet =
				(Mesh.Normals[Front[i]] + Mesh.Normals[Front[i + 1]]).GetSafeNormal();
			Mesh.AddTriangle(Slot, Front[i], Front[i + 1], Back[i], Facet);
			Mesh.AddTriangle(Slot, Front[i + 1], Back[i + 1], Back[i], Facet);
		}
		for (float X : {-kGantryLightDepth, 0.0f})
		{
			const FVector3f Normal(X < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
			const uint32 Middle =
				Mesh.AddVertex(FVector3f(X, 0.0f, 0.0f), Normal, FVector2f(0.5f, 0.5f));
			TArray<uint32> Ring;
			for (int32 i = 0; i <= Sides; ++i)
			{
				const float Angle = 2.0f * PI * i / Sides;
				Ring.Add(Mesh.AddVertex(
					FVector3f(X, R * FMath::Cos(Angle), R * FMath::Sin(Angle)), Normal,
					FVector2f(0.5f + 0.5f * FMath::Cos(Angle), 0.5f + 0.5f * FMath::Sin(Angle))));
			}
			for (int32 i = 0; i < Sides; ++i)
			{
				Mesh.AddTriangle(Slot, Middle, Ring[i], Ring[i + 1], Normal);
			}
		}
	}

	/**
	 * Where the start/finish line is. The export's own `start_finish` when it
	 * has one; otherwise grid slot 1, which the fallback grid layout puts on
	 * the line, at a default road width.
	 */
	bool ResolveStartFinish(const FApexTrackScene& Scene, FApexTrackStartFinish& Out)
	{
		if (Scene.StartFinish.IsSet())
		{
			Out = Scene.StartFinish.GetValue();
			if (Out.WidthCm <= 0.0f)
			{
				Out.WidthCm = kDefaultStartWidth;
			}
			return true;
		}
		for (const FApexTrackGridSlot& Slot : Scene.Grid)
		{
			if (Slot.Position == 1)
			{
				Out.Location = Slot.Location;
				Out.YawDeg = Slot.YawDeg;
				Out.WidthCm = kDefaultStartWidth;
				return true;
			}
		}
		return false;
	}

	struct FPropRecipe
	{
		const TCHAR* Kind;
		void (*Build)(FProcMesh&);
		/** Placed through one instanced component per level rather than an actor each. */
		bool bInstanced;
		/** Turned so local +Y faces the nearest road (stands, barriers). */
		bool bFaceRoad;
	};

	const FPropRecipe kPropRecipes[] = {
		{TEXT("tree"), &BuildTreeMesh, true, false},
		{TEXT("tire_wall"), &BuildTireWallMesh, true, false},
		{TEXT("barrier"), &BuildBarrierMesh, true, true},
		{TEXT("grandstand"), &BuildGrandstandMesh, false, true},
		{TEXT("building"), &BuildBuildingMesh, false, false},
		{TEXT("sign"), &BuildSignMesh, false, false},
	};

	const FPropRecipe* FindRecipe(const FString& Kind)
	{
		for (const FPropRecipe& Recipe : kPropRecipes)
		{
			if (Kind == Recipe.Kind)
			{
				return &Recipe;
			}
		}
		return nullptr;
	}

	/**
	 * Which side of a prop the road is on: +1 when the nearest centerline
	 * point lies toward the prop's local +Y, -1 toward -Y, 0 without a
	 * centerline. The export gives props the road's heading, not the side
	 * they stand on, so anything with a front has to work that out here.
	 */
	float RoadSideOf(const FApexTrackScene& Scene, const FApexTrackProp& Prop)
	{
		const FApexTrackCenterlinePoint* Nearest = nullptr;
		double BestDistance = TNumericLimits<double>::Max();
		for (const FApexTrackCenterlinePoint& Point : Scene.Centerline)
		{
			const double Distance = FVector::DistSquaredXY(Point.Location, Prop.Location);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Nearest = &Point;
			}
		}
		if (!Nearest)
		{
			return 0.0f;
		}
		const FVector Offset = Nearest->Location - Prop.Location;
		const float YawRad = FMath::DegreesToRadians(Prop.YawDeg);
		// Local +Y after a yaw rotation: (0, 1) -> (-sin, cos).
		const FVector Right(-FMath::Sin(YawRad), FMath::Cos(YawRad), 0.0);
		return FMath::Sign(static_cast<float>(FVector::DotProduct(Offset, Right)));
	}

	/** Road width at the nearest centerline point, metres; 0 without a centerline. */
	float RoadWidthAt(const FApexTrackScene& Scene, const FVector& Location)
	{
		const FApexTrackCenterlinePoint* Nearest = nullptr;
		double BestDistance = TNumericLimits<double>::Max();
		for (const FApexTrackCenterlinePoint& Point : Scene.Centerline)
		{
			const double Distance = FVector::DistSquaredXY(Point.Location, Location);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Nearest = &Point;
			}
		}
		return Nearest ? (Nearest->HalfLeftCm + Nearest->HalfRightCm) / 100.0f : 0.0f;
	}

	/** Deterministic ±1 per prop index, for per-instance colour swing. */
	float InstanceJitter(int32 Index)
	{
		return FMath::Frac(0.618034f * (Index + 1)) * 2.0f - 1.0f;
	}

	struct FMeshSlot
	{
		FString MaterialKey;
		TArrayView<const uint32> Indices;
	};

	/**
	 * Per-vertex tangents from the UV gradient, orthogonalised against the
	 * given normals, with the binormal's handedness as a sign. The editor's
	 * full mesh build computes its own; the runtime's fast build takes these
	 * as they are, and without them every normal-mapped ground set would be
	 * lit as if its bumps faced nowhere.
	 */
	void ComputeTangents(const TArray<FVector3f>& Positions, const TArray<FVector3f>& Normals,
		const TArray<FVector2f>& UVs, TArrayView<const FMeshSlot> Slots, TArray<FVector3f>& OutTangents,
		TArray<float>& OutSigns)
	{
		const int32 Count = Positions.Num();
		TArray<FVector3f> Tangents;
		TArray<FVector3f> Bitangents;
		Tangents.Init(FVector3f::ZeroVector, Count);
		Bitangents.Init(FVector3f::ZeroVector, Count);
		for (const FMeshSlot& Slot : Slots)
		{
			for (int32 Tri = 0; Tri + 2 < Slot.Indices.Num(); Tri += 3)
			{
				const uint32 A = Slot.Indices[Tri];
				const uint32 B = Slot.Indices[Tri + 1];
				const uint32 C = Slot.Indices[Tri + 2];
				const FVector3f E1 = Positions[B] - Positions[A];
				const FVector3f E2 = Positions[C] - Positions[A];
				const FVector2f D1 = UVs[B] - UVs[A];
				const FVector2f D2 = UVs[C] - UVs[A];
				const float Det = D1.X * D2.Y - D2.X * D1.Y;
				if (FMath::Abs(Det) < 1.0e-12f)
				{
					continue;
				}
				const float R = 1.0f / Det;
				const FVector3f T = (E1 * D2.Y - E2 * D1.Y) * R;
				const FVector3f Bt = (E2 * D1.X - E1 * D2.X) * R;
				for (const uint32 V : {A, B, C})
				{
					Tangents[V] += T;
					Bitangents[V] += Bt;
				}
			}
		}
		OutTangents.SetNumUninitialized(Count);
		OutSigns.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector3f& N = Normals[i];
			FVector3f T = Tangents[i] - N * FVector3f::DotProduct(N, Tangents[i]);
			if (!T.Normalize())
			{
				// No usable UV gradient (a degenerate strip, a cap's centre):
				// any direction in the surface will do.
				const FVector3f Axis = FMath::Abs(N.Z) < 0.9f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(1.0f, 0.0f, 0.0f);
				T = FVector3f::CrossProduct(Axis, N).GetSafeNormal();
			}
			OutTangents[i] = T;
			OutSigns[i] = FVector3f::DotProduct(FVector3f::CrossProduct(N, T), Bitangents[i]) < 0.0f ? -1.0f : 1.0f;
		}
	}

	/**
	 * Fill a mesh description from flat buffers, one polygon group per slot.
	 *
	 * One vertex instance per source vertex, shared by every triangle that
	 * uses it: the sources already carry one normal and one UV per vertex,
	 * and the runtime's fast build turns each instance into a render vertex,
	 * so an instance per corner would triple a circuit's vertex memory.
	 *
	 * `EdgeFactors`, when given, is one value per source vertex and goes
	 * into the red vertex channel: the seam fringe ramp for a surface band
	 * (see `ApexGround::EdgeFactors`). Everything else is left at the
	 * attribute's own default of white, which the material reads as "all
	 * interior" and so leaves alone.
	 */
	void FillMeshDescription(FMeshDescription& MeshDescription,
		const TArray<FVector3f>& SourcePositions, const TArray<FVector3f>& SourceNormals,
		const TArray<FVector2f>& SourceUVs, TArrayView<const FMeshSlot> Slots,
		TArrayView<const float> EdgeFactors = {})
	{
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();

		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const bool bHasEdgeFactors = EdgeFactors.Num() == SourcePositions.Num();

		TArray<FVector3f> SourceTangents;
		TArray<float> SourceSigns;
		ComputeTangents(SourcePositions, SourceNormals, SourceUVs, Slots, SourceTangents, SourceSigns);

		int32 IndexCount = 0;
		for (const FMeshSlot& Slot : Slots)
		{
			IndexCount += Slot.Indices.Num();
		}
		const int32 VertexCount = SourcePositions.Num();
		MeshDescription.ReserveNewVertices(VertexCount);
		MeshDescription.ReserveNewVertexInstances(VertexCount);
		MeshDescription.ReserveNewTriangles(IndexCount / 3);
		MeshDescription.ReserveNewPolygons(IndexCount / 3);

		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			const FVertexID VertexID = MeshDescription.CreateVertex();
			Positions[VertexID] = SourcePositions[i];
			const FVertexInstanceID InstanceID = MeshDescription.CreateVertexInstance(VertexID);
			Normals[InstanceID] = SourceNormals[i];
			Tangents[InstanceID] = SourceTangents[i];
			Signs[InstanceID] = SourceSigns[i];
			UVs.Set(InstanceID, 0, SourceUVs[i]);
			if (bHasEdgeFactors)
			{
				Colors[InstanceID] = FVector4f(EdgeFactors[i], 1.0f, 1.0f, 1.0f);
			}
			Instances.Add(InstanceID);
		}

		// Vertex order is taken exactly as given. The bake already wound
		// every triangle to Unreal's front-face convention and checks it in
		// its own test suite, and the recipes above do the same, so reversing
		// anything here would undo that.
		TArray<FVertexInstanceID> Corners;
		Corners.SetNum(3);
		for (const FMeshSlot& Slot : Slots)
		{
			const FPolygonGroupID PolygonGroup = MeshDescription.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroup] = FName(*Slot.MaterialKey);
			for (int32 Tri = 0; Tri + 2 < Slot.Indices.Num(); Tri += 3)
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					Corners[Corner] = Instances[Slot.Indices[Tri + Corner]];
				}
				MeshDescription.CreatePolygon(PolygonGroup, Corners);
			}
		}
	}

	FBox BoundsOf(const TArray<FVector3f>& Positions)
	{
		FBox Box(ForceInit);
		for (const FVector3f& P : Positions)
		{
			Box += FVector(P);
		}
		return Box;
	}

	/** A generated mesh as a prepared one, its slots named after its material keys. */
	TUniquePtr<FApexPreparedMesh> PrepareProcMesh(const FProcMesh& Proc, const FString& Name, const FString& Kind)
	{
		TUniquePtr<FApexPreparedMesh> Prepared = MakeUnique<FApexPreparedMesh>();
		Prepared->Name = Name;
		Prepared->PropKind = Kind;
		TArray<FMeshSlot> Slots;
		for (const FProcMesh::FSlot& Slot : Proc.Slots)
		{
			Slots.Add({Slot.MaterialKey, Slot.Indices});
			Prepared->Slots.Add(FName(*Slot.MaterialKey));
		}
		FillMeshDescription(Prepared->Description, Proc.Positions, Proc.Normals, Proc.UVs, Slots);
		Prepared->Bounds = BoundsOf(Proc.Positions);
		return Prepared;
	}
}	 // namespace

// --- Parents -------------------------------------------------------------------

FString ApexTrackMaterials::PackageName(const TCHAR* Name)
{
	return FString(Folder) / Name;
}

FString ApexTrackMaterials::ObjectPath(const TCHAR* Name)
{
	return FString::Printf(TEXT("%s/%s.%s"), Folder, Name, Name);
}

FApexTrackParents ApexTrackMaterials::LoadParents()
{
	auto Load = [](const TCHAR* Name) -> UMaterialInterface* {
		// Tested first: a missing parent is a fresh clone, not a failure,
		// and LoadObject would fill the log with errors about it.
		return FPackageName::DoesPackageExist(PackageName(Name))
			? LoadObject<UMaterialInterface>(nullptr, *ObjectPath(Name))
			: nullptr;
	};
	FApexTrackParents Parents;
	Parents.Base = Load(BaseName);
	Parents.Emissive = Load(EmissiveName);
	Parents.Brand = Load(BrandName);
	Parents.Decal = Load(DecalName);
	return Parents;
}

bool ApexTrackMaterials::HasGroundTextures(const UMaterialInterface* Base)
{
	if (!Base)
	{
		return false;
	}
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Base->GetAllScalarParameterInfo(Infos, Ids);
	static const FName TextureAmount(TEXT("TextureAmount"));
	return Infos.ContainsByPredicate([](const FMaterialParameterInfo& Info) { return Info.Name == TextureAmount; });
}

float FApexMaterialParams::FindScalar(const TCHAR* Name, float Default) const
{
	const FName Wanted(Name);
	for (const TPair<FName, float>& Pair : Scalars)
	{
		if (Pair.Key == Wanted)
		{
			return Pair.Value;
		}
	}
	return Default;
}

// --- Builder -------------------------------------------------------------------

FApexTrackSceneBuilder::FApexTrackSceneBuilder(IApexTrackAssetFactory& InFactory, const FApexTrackParents& InParents)
	: Factory(InFactory)
	, Parents(InParents)
	, bGroundTextures(ApexTrackMaterials::HasGroundTextures(InParents.Base))
	, PropsRoot(ApexProps::DefaultRoot)
{
}

void FApexTrackSceneBuilder::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Parents.Base);
	Collector.AddReferencedObject(Parents.Emissive);
	Collector.AddReferencedObject(Parents.Brand);
	Collector.AddReferencedObject(Parents.Decal);
	Collector.AddReferencedObject(PlaceholderMesh);
	for (TPair<FString, TObjectPtr<UMaterialInterface>>& Pair : Materials)
	{
		Collector.AddReferencedObject(Pair.Value);
	}
	for (TPair<FString, TObjectPtr<UStaticMesh>>& Pair : Meshes)
	{
		Collector.AddReferencedObject(Pair.Value);
	}
	for (TPair<FString, TObjectPtr<UStaticMesh>>& Pair : PropMeshes)
	{
		Collector.AddReferencedObject(Pair.Value);
	}
	for (TPair<FString, TObjectPtr<UStaticMesh>>& Pair : AuthoredMeshes)
	{
		Collector.AddReferencedObject(Pair.Value);
	}
	for (TPair<FString, TObjectPtr<UMaterialInterface>>& Pair : SlotMaterials)
	{
		Collector.AddReferencedObject(Pair.Value);
	}
}

void FApexTrackSceneBuilder::PrepareGeometry(const FApexTrackScene& Scene, FApexTrackGeometry& Out)
{
	// The centerline in the meshes' own frame (UE centimetres), for telling
	// a band's road-facing edge from its outer one. Which way `Across`
	// points does not matter as long as it is the same everywhere. Sorted
	// by station because the lookup is a binary search, and the exporter's
	// order is a convention rather than something the reader checks.
	TArray<ApexGround::FCenterSample> Center;
	Center.Reserve(Scene.Centerline.Num());
	for (const FApexTrackCenterlinePoint& Point : Scene.Centerline)
	{
		const float Yaw = FMath::DegreesToRadians(Point.YawDeg);
		ApexGround::FCenterSample& Sample = Center.AddDefaulted_GetRef();
		Sample.StationM = Point.StationCm / 100.0f;
		Sample.Location = FVector2f(Point.Location.X, Point.Location.Y);
		Sample.Across = FVector2f(-FMath::Sin(Yaw), FMath::Cos(Yaw));
	}
	Center.Sort([](const ApexGround::FCenterSample& A, const ApexGround::FCenterSample& B) {
		return A.StationM < B.StationM;
	});
	const float LapM = Scene.bClosedLoop ? Scene.LengthCm / 100.0f : 0.0f;

	Out.Meshes.Reset();
	Out.NextToBuild = 0;
	Out.BandVertices = 0;
	Out.FringeVertices = 0;
	Out.Meshes.Reserve(Scene.Meshes.Num() + UE_ARRAY_COUNT(kPropRecipes) + 2);
	for (int32 i = 0; i < Scene.Meshes.Num(); ++i)
	{
		const FApexTrackMesh& Source = Scene.Meshes[i];
		TUniquePtr<FApexPreparedMesh> Prepared = MakeUnique<FApexPreparedMesh>();
		Prepared->Name = Source.Name;
		Prepared->SceneMesh = i;
		Prepared->Slots.Add(FName(*Source.MaterialKey));
		const FMeshSlot Slot{Source.MaterialKey, Source.Indices};
		// A run-off band gets the seam fringe ramp in its vertex colours.
		// Only a band: the terrain is a grid whose UVs are world metres, so
		// the "how far across the strip is this" reading would be nonsense.
		TArray<float> EdgeFactors;
		if (ApexGround::IsSurfaceBand(Source.MaterialKey))
		{
			EdgeFactors = ApexGround::EdgeFactors(Source.UVs, Source.Positions, Center, LapM);
			Out.BandVertices += EdgeFactors.Num();
			Out.FringeVertices += Algo::CountIf(EdgeFactors, [](float F) { return F < 1.0f; });
		}
		FillMeshDescription(Prepared->Description, Source.Positions, Source.Normals, Source.UVs,
			MakeArrayView(&Slot, 1), EdgeFactors);
		Prepared->Bounds = BoundsOf(Source.Positions);
		Out.Meshes.Add(MoveTemp(Prepared));
	}

	// One stand-in per prop kind, generated rather than authored: what a
	// circuit falls back to where the kit has not been imported, so that a
	// stand is visibly a stand and a tree visibly a tree.
	for (const FPropRecipe& Recipe : kPropRecipes)
	{
		FProcMesh Proc;
		Recipe.Build(Proc);
		Out.Meshes.Add(PrepareProcMesh(Proc, FString(TEXT("Prop_")) + Recipe.Kind, Recipe.Kind));
	}

	// The gantry spans this track's road, so it is sized per track.
	FApexTrackStartFinish Start;
	if (ResolveStartFinish(Scene, Start))
	{
		FProcMesh Gantry;
		BuildStartGantryMesh(Gantry, Start.WidthCm);
		Out.Meshes.Add(PrepareProcMesh(Gantry, TEXT("Prop_start_gantry"), TEXT("start_gantry")));
		FProcMesh Light;
		BuildStartLightMesh(Light);
		Out.Meshes.Add(PrepareProcMesh(Light, TEXT("Prop_start_light"), TEXT("start_light")));
	}
}

bool FApexTrackSceneBuilder::BuildMaterials(const FApexTrackScene& Scene, FString& OutError)
{
	Dressing = Scene.Dressing;
	if (!Parents.Base)
	{
		OutError = FString::Printf(TEXT("no parent material at %s; run `-run=ApexMaterialBake` in the editor"),
			*ApexTrackMaterials::ObjectPath(ApexTrackMaterials::BaseName));
		return false;
	}
	if (!Parents.Emissive || !Parents.Brand || !Parents.Decal)
	{
		UE_LOG(LogApexTrack, Warning,
			TEXT("    some of the track parent materials under %s are missing (emissive %s, brand %s, decal %s); "
				 "run `-run=ApexMaterialBake`"),
			ApexTrackMaterials::Folder, Parents.Emissive ? TEXT("ok") : TEXT("missing"),
			Parents.Brand ? TEXT("ok") : TEXT("missing"), Parents.Decal ? TEXT("ok") : TEXT("missing"));
	}

	// One load per set, not one per key: a circuit has fifteen materials
	// and seven sets, and half of them are asphalt.
	TMap<FString, FGroundMapSet> GroundSets;
	auto GroundSetFor = [&GroundSets](const FString& Set) -> const FGroundMapSet& {
		if (FGroundMapSet* Existing = GroundSets.Find(Set))
		{
			return *Existing;
		}
		return GroundSets.Add(Set, LoadGroundSet(Set));
	};

	for (const FApexTrackMaterial& Source : Scene.Materials)
	{
		// A road decal is a picture, not a colour on the shared parent.
		if (Source.Family == TEXT("decal"))
		{
			if (UMaterialInterface* Decal = DecalMaterialFor(Source.Key))
			{
				Materials.Add(Source.Key, Decal);
			}
			continue;
		}

		FApexMaterialParams Params;

		// The exporter's colors are editor-viewport colors: bright, because
		// the viewport is flat-lit. Under a physically lit sun they render
		// as pastel — real asphalt reflects ~10% of what hits it, grass not
		// much more — so each family scales down toward plausible albedo.
		FLinearColor Base = Source.BaseColor;

		// Every key in a family gets the family's treatment, whatever it is
		// called (`wear_core`, `chequer_*` and the like are just more road
		// and marking): only the base colour comes from the export.
		//
		// The noise grain is the fallback graph's, so with the ground set
		// baked into the parent there are no `Detail*` parameters and these
		// would leave overrides matching nothing behind.
		auto SetDetail = [&](float Tiling, float Amount, float Roughness, float Normal) {
			if (bGroundTextures)
			{
				return;
			}
			Params.Scalar(TEXT("DetailTiling"), Tiling);
			Params.Scalar(TEXT("DetailAmount"), Amount);
			Params.Scalar(TEXT("DetailRoughness"), Roughness);
			Params.Scalar(TEXT("DetailNormal"), Normal);
		};
		const bool bRoadFamily = Source.Family == TEXT("road") || Source.Family == TEXT("pit_lane");
		if (bRoadFamily)
		{
			// Tarmac: patchy aggregate and uneven sheen, not one flat slab,
			// with 50 cm grain over the 8 m patches.
			Base *= 0.42f;
			Params.Scalar(TEXT("Roughness"), 0.9f);
			Params.Scalar(TEXT("NoiseAmount"), 0.35f);
			Params.Scalar(TEXT("NoiseScale"), 0.0012f);
			Params.Scalar(TEXT("RoughnessNoise"), 0.2f);
			SetDetail(2.0f, 0.35f, 0.25f, 0.3f);
		}
		else if (Source.Family == TEXT("curb"))
		{
			// The exporter's solid slab becomes 2 m stripes. Style keys name
			// their pair (`curb_red_white`, `curb_yellow_black`): the base
			// color is the first, so the alternate is white unless the key
			// says black.
			Base *= 0.7f;
			Params.Scalar(TEXT("StripePeriod"), 2.0f);
			const FLinearColor Alt = Source.Key.EndsWith(TEXT("black"))
				? FLinearColor(0.04f, 0.04f, 0.04f)
				: FLinearColor(0.62f, 0.60f, 0.57f);
			Params.Vector(TEXT("SecondaryColor"), Alt);
			Params.Scalar(TEXT("Roughness"), 0.6f);
			Params.Scalar(TEXT("NoiseAmount"), 0.08f);
			Params.Scalar(TEXT("NoiseScale"), 0.004f);
			SetDetail(4.0f, 0.15f, 0.1f, 0.15f);
		}
		else if (Source.Family == TEXT("surface"))
		{
			// Grass, gravel, sand and the terrain itself: desaturated a
			// touch, darkened a lot, and mottled in ~10 m patches — the
			// large scale is what still reads a hundred meters out — with
			// metre-scale grain for the near field.
			const float Luma = Base.GetLuminance();
			Base = FMath::Lerp(Base, FLinearColor(Luma, Luma, Luma), 0.15f) * 0.33f;
			Params.Scalar(TEXT("Roughness"), 0.95f);
			Params.Scalar(TEXT("NoiseAmount"), 0.5f);
			Params.Scalar(TEXT("NoiseScale"), 0.0008f);
			Params.Scalar(TEXT("RoughnessNoise"), 0.08f);
			SetDetail(1.0f, 0.45f, 0.1f, 0.3f);
		}
		else if (Source.Family == TEXT("structure"))
		{
			// Bridges and retaining walls: weathered concrete, and the painted
			// fascia on a deck. Stained in broad patches, little grain.
			Base *= 0.6f;
			Params.Scalar(TEXT("Roughness"), 0.8f);
			Params.Scalar(TEXT("NoiseAmount"), 0.18f);
			Params.Scalar(TEXT("NoiseScale"), 0.002f);
			Params.Scalar(TEXT("RoughnessNoise"), 0.1f);
			SetDetail(1.0f, 0.2f, 0.1f, 0.15f);
		}
		else if (Source.Family == TEXT("marking"))
		{
			// Painted lines read as paint, not asphalt — worn paint, so a
			// touch of the same mottling the road gets and grain that thins
			// the coat here and there.
			Base *= 0.85f;
			Params.Scalar(TEXT("Roughness"), 0.45f);
			Params.Scalar(TEXT("NoiseAmount"), 0.08f);
			SetDetail(2.0f, 0.25f, 0.15f, 0.2f);
		}

		// Which baked set this family samples, and how hard.
		const ApexGround::FSurfaceLook Look = ApexGround::LookFor(Source.Family, Source.Key);
		if (bGroundTextures && Look.Set[0] != TEXT('\0'))
		{
			const FGroundMapSet& Set = GroundSetFor(Look.Set);
			if (Set.IsComplete())
			{
				Params.Texture(TEXT("AlbedoMap"), Set.Albedo);
				Params.Texture(TEXT("NormalMap"), Set.Normal);
				Params.Texture(TEXT("RoughnessMap"), Set.Roughness);
				Params.Scalar(TEXT("TextureAmount"), 1.0f);
				Params.Scalar(TEXT("TextureTiling"), 1.0f / Look.TileM);
				Params.Scalar(TEXT("NormalStrength"), Look.NormalStrength);
				Params.Scalar(TEXT("RoughnessMapAmount"), Look.RoughnessMapAmount);
			}
			else
			{
				UE_LOG(LogApexTrack, Warning, TEXT("    the %s ground set is missing; %s keeps the parent's asphalt"),
					Look.Set, *Source.Key);
			}
		}
		// The fringe works either way: its ramp is in the vertex colours and
		// its parameters are outside the branch above.
		if (Look.EdgeBlend > 0.0f)
		{
			Params.Scalar(TEXT("EdgeBlend"), Look.EdgeBlend);
			Params.Scalar(TEXT("EdgeNoise"), kEdgeNoise);
			Params.Vector(TEXT("EdgeColor"), FMath::Lerp(Base, kEdgeDustColor, 0.7f));
		}

		Base.A = 1.0f;
		Params.Vector(TEXT("BaseColor"), Base);
		UMaterialInterface* Instance = Factory.MakeMaterial(Source.Key, Parents.Base, Params);
		if (!Instance)
		{
			OutError = FString::Printf(TEXT("could not create material %s"), *Source.Key);
			return false;
		}
		Materials.Add(Source.Key, Instance);
		if (bRoadFamily)
		{
			RoadMaterials.Add({Source.Key, Params.FindScalar(TEXT("Roughness"), 0.85f),
				Params.FindScalar(TEXT("RoughnessNoise"), 0.0f)});
		}
	}

	for (const FPropMaterialSpec& Spec : kPropMaterials)
	{
		FApexMaterialParams Params;
		Params.Vector(TEXT("BaseColor"), Spec.Color);
		Params.Scalar(TEXT("Roughness"), Spec.Roughness);
		Params.Scalar(TEXT("NoiseAmount"), Spec.NoiseAmount);
		Params.Scalar(TEXT("NoiseScale"), Spec.NoiseScale);
		if (Spec.StripePeriod > 0.0f)
		{
			Params.Scalar(TEXT("StripePeriod"), Spec.StripePeriod);
			Params.Vector(TEXT("SecondaryColor"), Spec.Secondary);
		}
		if (!Spec.InstanceTint.IsAlmostBlack())
		{
			Params.Vector(TEXT("InstanceTint"), Spec.InstanceTint);
		}
		UMaterialInterface* Instance = Factory.MakeMaterial(Spec.Key, Parents.Base, Params);
		if (!Instance)
		{
			OutError = FString::Printf(TEXT("could not create material %s"), Spec.Key);
			return false;
		}
		Materials.Add(Spec.Key, Instance);
	}

	// The lenses use the emissive parent itself, dark by default: the race
	// director gives each lens its own dynamic instance when it finds the
	// gantry, so they light one at a time.
	if (Parents.Emissive)
	{
		Materials.Add(TEXT("start_light"), Parents.Emissive);
	}

	UE_LOG(LogApexTrack, Display, TEXT("    %d material(s)%s"), Materials.Num(),
		bGroundTextures ? TEXT(" on the ground-textured parent") : TEXT(" on the noise-grain parent (no /Game/Ground when it was baked)"));
	return true;
}

UMaterialInterface* FApexTrackSceneBuilder::DecalMaterialFor(const FString& Key)
{
	FString Set;
	FString Name;
	if (!Parents.Decal || !ApexProps::ParseDecalKey(Key, Set, Name))
	{
		MissingDecals.Add(Key);
		return nullptr;
	}
	const FString TexturePath = ApexProps::DecalTextureObjectPath(PropsRoot, Set, Name);
	FString TexturePackage;
	FString TextureObject;
	TexturePath.Split(TEXT("."), &TexturePackage, &TextureObject, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	UTexture* Texture = FPackageName::DoesPackageExist(TexturePackage) ? LoadObject<UTexture>(nullptr, *TexturePath) : nullptr;
	if (!Texture)
	{
		// Paint with no picture would be a white slab across the road:
		// leave it out and say which import is missing.
		UE_LOG(LogApexTrack, Warning, TEXT("    %s is not imported (ApexPropImport -kind=decal); %s is left out"),
			*TexturePath, *Key);
		MissingDecals.Add(Key);
		return nullptr;
	}
	FApexMaterialParams Params;
	Params.Texture(TEXT("DecalTexture"), Texture);
	UMaterialInterface* Instance = Factory.MakeMaterial(Key, Parents.Decal, Params);
	if (!Instance)
	{
		MissingDecals.Add(Key);
	}
	return Instance;
}

bool FApexTrackSceneBuilder::BuildMeshes(FApexTrackGeometry& Geometry, double DeadlineSeconds, FString& OutError)
{
	while (!Geometry.IsBuilt())
	{
		FApexPreparedMesh& Prepared = *Geometry.Meshes[Geometry.NextToBuild];
		++Geometry.NextToBuild;
		const bool bTrackSurface = Prepared.SceneMesh != INDEX_NONE;
		if (bTrackSurface && MissingDecals.Contains(Prepared.Slots[0].ToString()))
		{
			Prepared.Description = FMeshDescription();
			continue;
		}
		TArray<UMaterialInterface*> MeshMaterials;
		for (const FName& Slot : Prepared.Slots)
		{
			MeshMaterials.Add(Materials.FindRef(Slot.ToString()));
		}
		UStaticMesh* Mesh = Factory.MakeMesh(
			Prepared.Name, Prepared.Description, Prepared.Slots, MeshMaterials, bTrackSurface, Prepared.Bounds);
		// Built (and, in the editor, committed as a copy): the source is done with.
		Prepared.Description = FMeshDescription();
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("could not build mesh %s"), *Prepared.Name);
			return false;
		}
		if (bTrackSurface)
		{
			Meshes.Add(Prepared.Name, Mesh);
		}
		else
		{
			PropMeshes.Add(Prepared.PropKind, Mesh);
		}
		if (FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			break;
		}
	}
	if (Geometry.IsBuilt())
	{
		const int32 SkippedDecals = Geometry.Meshes.Num() - Meshes.Num() - PropMeshes.Num();
		UE_LOG(LogApexTrack, Display, TEXT("    %d track mesh(es), %d prop mesh(es)"), Meshes.Num(), PropMeshes.Num());
		if (SkippedDecals > 0)
		{
			UE_LOG(LogApexTrack, Warning, TEXT("    %d road decal mesh(es) left out: their textures are not imported"),
				SkippedDecals);
		}
		if (Geometry.BandVertices > 0)
		{
			// Zero or everything means the inner-edge test has gone wrong (a
			// centerline in another frame, say), which nothing else would
			// show short of a screenshot.
			UE_LOG(LogApexTrack, Display, TEXT("    seam fringe on %d of %d run-off band vertices (%.0f%%)"),
				Geometry.FringeVertices, Geometry.BandVertices,
				100.0 * Geometry.FringeVertices / Geometry.BandVertices);
		}
	}
	return true;
}

bool FApexTrackSceneBuilder::ValidateMeshes(FString& OutError) const
{
	// Check what Unreal actually built, not what was handed to it. A mesh
	// with NaN or empty bounds is invisible — it fails every frustum test —
	// and nothing about the export would tell you why. Better to fail the
	// build than to ship a circuit with holes in it.
	TArray<TPair<FString, TObjectPtr<UStaticMesh>>> All;
	All.Append(Meshes.Array());
	All.Append(PropMeshes.Array());
	for (const TPair<FString, TObjectPtr<UStaticMesh>>& Entry : All)
	{
		const UStaticMesh* Mesh = Entry.Value;
		// Note `LODResources`, not `IsInitialized()`: the latter means the
		// RHI resources are live, which never happens in a commandlet.
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s built no render data"), *Entry.Key);
			return false;
		}
		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		if (Bounds.Origin.ContainsNaN() || Bounds.BoxExtent.ContainsNaN() || !FMath::IsFinite(Bounds.SphereRadius))
		{
			OutError = FString::Printf(TEXT("%s built with non-finite bounds"), *Entry.Key);
			return false;
		}
		if (Bounds.SphereRadius <= 0.0f)
		{
			OutError = FString::Printf(TEXT("%s built with empty bounds"), *Entry.Key);
			return false;
		}
		if (Mesh->GetNumTriangles(0) <= 0)
		{
			OutError = FString::Printf(TEXT("%s built with no triangles"), *Entry.Key);
			return false;
		}
	}
	return true;
}

UStaticMesh* FApexTrackSceneBuilder::FindAuthoredMesh(const FString& Kind, const FString& Asset)
{
	const FString ObjectPath = ApexProps::MeshObjectPath(PropsRoot, Kind, Asset);
	if (const TObjectPtr<UStaticMesh>* Cached = AuthoredMeshes.Find(ObjectPath))
	{
		return *Cached;
	}
	UStaticMesh* Mesh = nullptr;
	// Checked on disk first: a missing asset is the normal case for a kind
	// nobody has authored yet, and LoadObject would warn about each one.
	if (FPackageName::DoesPackageExist(ApexProps::MeshPackageName(PropsRoot, Kind, Asset)))
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
	}
	AuthoredMeshes.Add(ObjectPath, Mesh);
	return Mesh;
}

FApexTrackSceneBuilder::FResolvedProp FApexTrackSceneBuilder::ResolveProp(const FApexTrackProp& Prop)
{
	FResolvedProp Resolved;
	Resolved.Kind = Prop.Kind;
	Resolved.Asset = Prop.Asset;
	Resolved.Text = Prop.Text;
	ApexProps::ResolveAlias(Resolved.Kind, Resolved.Asset, Resolved.Text);

	Resolved.Mesh = FindAuthoredMesh(Resolved.Kind, Resolved.Asset);
	if (!Resolved.Mesh)
	{
		const FString Default = ApexProps::DefaultAssetFor(Resolved.Kind);
		if (!Default.IsEmpty() && Default != Resolved.Asset)
		{
			if (UStaticMesh* Mesh = FindAuthoredMesh(Resolved.Kind, Default))
			{
				Resolved.Mesh = Mesh;
				Resolved.Asset = Default;
			}
		}
	}
	if (Resolved.Mesh && Dressing.IsAutumn())
	{
		// The season's foliage where the tree has it.
		const FString Autumn = ApexProps::AutumnVariant(Resolved.Kind, Resolved.Asset);
		if (UStaticMesh* Mesh = Autumn.IsEmpty() ? nullptr : FindAuthoredMesh(Resolved.Kind, Autumn))
		{
			Resolved.Mesh = Mesh;
			Resolved.Asset = Autumn;
		}
	}
	if (Resolved.Mesh)
	{
		Resolved.bAuthored = true;
		Resolved.bFaceRoad = ApexProps::FacesRoad(Resolved.Kind, Resolved.Asset);
		Resolved.bFaceUpCourse = ApexProps::FacesUpCourse(Resolved.Kind, Resolved.Asset);
		// A board with a text face gets its own actor so the text component
		// can hang off it; instances cannot carry one.
		Resolved.bInstanced = ApexProps::IsInstancedKind(Resolved.Kind)
			&& !(ApexProps::HasTextFace(Resolved.Kind, Resolved.Asset) && !Resolved.Text.IsEmpty());
		return Resolved;
	}

	// Nothing authored: the prop's own kind and its generated stand-in, as
	// before the kit existed (an alias that changed the kind is undone, so
	// a `sign` still gets the board-and-post with its text on it).
	Resolved.Kind = Prop.Kind;
	Resolved.Asset = Prop.Asset;
	Resolved.Text = Prop.Text;
	if (const FPropRecipe* Recipe = FindRecipe(Prop.Kind))
	{
		Resolved.Mesh = PropMeshes.FindRef(Prop.Kind);
		Resolved.bFaceRoad = Recipe->bFaceRoad;
		Resolved.bInstanced = Recipe->bInstanced && Resolved.Mesh != nullptr;
	}
	return Resolved;
}

UMaterialInterface* FApexTrackSceneBuilder::TextureMaterialFor(const FString& Key, const FString& TexturePath)
{
	if (const TObjectPtr<UMaterialInterface>* Cached = SlotMaterials.Find(Key))
	{
		return *Cached;
	}
	UMaterialInterface* Result = nullptr;
	FString PackageName;
	FString ObjectName;
	TexturePath.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	UTexture* Texture = Parents.Brand && FPackageName::DoesPackageExist(PackageName)
		? LoadObject<UTexture>(nullptr, *TexturePath)
		: nullptr;
	if (Texture)
	{
		FApexMaterialParams Params;
		Params.Texture(TEXT("BrandTexture"), Texture);
		Result = Factory.MakeMaterial(Key, Parents.Brand, Params);
	}
	SlotMaterials.Add(Key, Result);
	return Result;
}

UMaterialInterface* FApexTrackSceneBuilder::EmissiveMaterialFor(FName Slot)
{
	// The lamps the race director drives are its own components on the
	// gantry; the authored lamp housing stays dark. The panels and screens
	// glow at a fixed level for now — there is no flag state on the wire
	// yet — and carry a tag so a director can find them later.
	struct FGlow
	{
		const TCHAR* Slot;
		FLinearColor Color;
		float Strength;
	};
	const FGlow Glows[] = {
		{TEXT("led_panel"), FLinearColor(0.1f, 1.0f, 0.2f), 40.0f},
		{TEXT("led_screen"), FLinearColor(0.35f, 0.5f, 1.0f), 15.0f},
		{TEXT("floodlight_lamp"), FLinearColor(1.0f, 0.95f, 0.8f), 0.0f},
		// The pit exit light shows green (pit open) until a director drives it.
		{TEXT("pit_light_green"), FLinearColor(0.1f, 1.0f, 0.25f), 30.0f},
		{TEXT("pit_light_red"), FLinearColor(1.0f, 0.1f, 0.1f), 0.0f},
	};
	const FString Key = TEXT("glow_") + Slot.ToString();
	if (const TObjectPtr<UMaterialInterface>* Cached = SlotMaterials.Find(Key))
	{
		return *Cached;
	}
	UMaterialInterface* Result = nullptr;
	for (const FGlow& Glow : Glows)
	{
		if (Slot != FName(Glow.Slot) || !Parents.Emissive)
		{
			continue;
		}
		FApexMaterialParams Params;
		Params.Vector(TEXT("BaseColor"), Glow.Color * 0.05f);
		Params.Vector(TEXT("EmissiveColor"), Glow.Color);
		Params.Scalar(TEXT("EmissiveStrength"), Glow.Strength);
		Result = Factory.MakeMaterial(Key, Parents.Emissive, Params);
	}
	SlotMaterials.Add(Key, Result);
	return Result;
}

void FApexTrackSceneBuilder::ApplyAuthoredSlots(
	UStaticMeshComponent* Component, const UStaticMesh* Mesh, const FString& Text)
{
	if (!Component || !Mesh)
	{
		return;
	}
	const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		// The slot name is the glTF material's, which is what every rule
		// below keys on. The imported name is editor-only data; a cooked
		// mesh carries the slot name, which the import sets to the same.
		FName Slot = Slots[i].MaterialSlotName;
#if WITH_EDITORONLY_DATA
		if (Slot.IsNone())
		{
			Slot = Slots[i].ImportedMaterialSlotName;
		}
#endif
		UMaterialInterface* Override = nullptr;
		if (ApexProps::IsBrandSlot(Slot) && !Text.IsEmpty())
		{
			const FString Key = TextKey(Text);
			Override = TextureMaterialFor(TEXT("brand_") + Key, ApexProps::BrandTextureObjectPath(PropsRoot, Key));
			if (!Override && !UnknownTexts.Contains(Key))
			{
				UnknownTexts.Add(Key);
				UE_LOG(LogApexTrack, Warning,
					TEXT("    no brand texture for text \"%s\" (T_brand_%s); boards keep their imported brand"), *Text,
					*Key);
			}
		}
		else if (ApexProps::IsFlagSlot(Slot) && !Text.IsEmpty())
		{
			const FString Key = TextKey(Text);
			Override = TextureMaterialFor(TEXT("flag_") + Key, ApexProps::FlagTextureObjectPath(PropsRoot, Key));
			if (!Override && !UnknownTexts.Contains(Key))
			{
				UnknownTexts.Add(Key);
				UE_LOG(LogApexTrack, Warning,
					TEXT("    no flag texture for text \"%s\" (T_flag_%s); the pole keeps its imported flag"), *Text,
					*Key);
			}
		}
		else if (ApexProps::IsMarkerSlot(Slot) && !Text.IsEmpty())
		{
			const FString Key = TextKey(Text);
			Override = TextureMaterialFor(TEXT("marker_") + Key, ApexProps::MarkerTextureObjectPath(PropsRoot, Key));
			if (!Override && !UnknownTexts.Contains(Key))
			{
				UnknownTexts.Add(Key);
				UE_LOG(LogApexTrack, Warning,
					TEXT("    no marker texture for text \"%s\" (T_marker_%s); the marker keeps its imported number"),
					*Text, *Key);
			}
		}
		else if (ApexProps::IsEmissiveSlot(Slot))
		{
			Override = EmissiveMaterialFor(Slot);
			if (Override)
			{
				Component->ComponentTags.AddUnique(FName(*(TEXT("ApexEmissive_") + Slot.ToString())));
			}
		}
		if (Override)
		{
			Component->SetMaterial(i, Override);
		}
	}
}

AActor* FApexTrackSceneBuilder::SpawnStartLights(const FApexTrackScene& Scene, UWorld* World, ULevel* Level)
{
	FApexTrackStartFinish Start;
	// The authored gantry when the kit has it, else the generated one. Both
	// stand on the road centre with the beam along Y and the lamp panel
	// facing -X, toward the cars on the grid.
	UStaticMesh* AuthoredGantry = FindAuthoredMesh(TEXT("bridge"), TEXT("start_gantry"));
	UStaticMesh* GantryMesh = AuthoredGantry ? AuthoredGantry : PropMeshes.FindRef(TEXT("start_gantry")).Get();
	UStaticMesh* LightMesh = PropMeshes.FindRef(TEXT("start_light"));
	if (!ResolveStartFinish(Scene, Start) || !GantryMesh || !LightMesh)
	{
		UE_LOG(LogApexTrack, Warning, TEXT("    no start/finish line and no grid — the track has no start lights"));
		return nullptr;
	}

	// 12 m past the line, facing the way the cars go; the lights hang on the
	// -X face, toward the grid.
	const FRotator Rotation(0.0f, Start.YawDeg, 0.0f);
	const FVector Location = Start.Location + Rotation.RotateVector(FVector(kGantryOffset, 0.0, 0.0));

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = Level;
	SpawnParams.ObjectFlags = RF_Transactional;
	SpawnParams.Name = MakeUniqueObjectName(Level, AStaticMeshActor::StaticClass(), FName(TEXT("StartLights")));
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
	if (!Actor)
	{
		return nullptr;
	}
	Factory.LabelActor(Actor, TEXT("StartLights"));
	Actor->Tags.Add(StartLightsTag);
	Actor->Tags.Add(PropTag);
	UStaticMeshComponent* Root = Actor->GetStaticMeshComponent();
	Root->SetMobility(EComponentMobility::Movable);
	Root->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
	Root->SetStaticMesh(GantryMesh);
	// The authored gantry is built for a 15 m road and stretched across
	// this one; the generated one was already sized to it.
	const float SpanScale = AuthoredGantry ? ApexProps::BridgeSpanScale(Start.WidthCm / 100.0f) : 1.0f;
	Actor->SetActorScale3D(FVector(1.0, SpanScale, 1.0));
	if (AuthoredGantry)
	{
		ApplyAuthoredSlots(Root, AuthoredGantry, FString());
	}

	// Left to right as seen from the grid, which looks along +X: left is -Y.
	// On the authored panel the five columns sit over its upper lamp row;
	// the lower row stays decoration. The lenses ride the span scale in
	// position (so do the housing's lamps) but not in shape.
	UMaterialInterface* LightMaterial = Materials.FindRef(TEXT("start_light"));
	const int32 Count = 5;
	for (int32 i = 0; i < Count; ++i)
	{
		UStaticMeshComponent* Light =
			NewObject<UStaticMeshComponent>(Actor, *FString::Printf(TEXT("Light%d"), i), RF_Transactional);
		Light->CreationMethod = EComponentCreationMethod::Instance;
		Light->SetMobility(EComponentMobility::Movable);
		Light->ComponentTags.Add(StartLightTag);
		Light->SetupAttachment(Root);
		const float Across = i - (Count - 1) * 0.5f;
		if (AuthoredGantry)
		{
			Light->SetRelativeLocation(
				FVector(ApexProps::GantryLampX, Across * ApexProps::GantryLampPitchY, ApexProps::GantryLampZ));
			Light->SetAbsolute(false, false, /*bAbsoluteScale*/ true);
			Light->SetWorldScale3D(FVector(1.0, ApexProps::GantryLampRadius / kGantryLightRadius,
				ApexProps::GantryLampRadius / kGantryLightRadius));
		}
		else
		{
			Light->SetRelativeLocation(FVector(-kGantryPanelDepth * 0.5f, Across * kGantryLightPitch,
				kGantryHeight - kGantryPanelHeight * 0.5f));
		}
		Light->SetStaticMesh(LightMesh);
		if (LightMaterial)
		{
			Light->SetMaterial(0, LightMaterial);
		}
		Actor->AddInstanceComponent(Light);
		Light->RegisterComponent();
	}
	UE_LOG(LogApexTrack, Display, TEXT("    start lights at (%.0f, %.0f, %.0f), %.1f m span%s"), Location.X,
		Location.Y, Location.Z, (Start.WidthCm + 200.0f) / 100.0f,
		AuthoredGantry ? TEXT(" (authored gantry)") : TEXT(" (generated gantry)"));
	return Actor;
}

AActor* FApexTrackSceneBuilder::SpawnGrandstand(UWorld* World, ULevel* Level, const FApexTrackProp& Prop,
	const FResolvedProp& Resolved, float YawDeg, int32 Index)
{
	// One prop is a whole stand: bays at 10 m pitch, capped at both ends,
	// wedges around a corner. The scene's `length_m` (30 m when it has
	// none) times the prop's scale is the stand's length; the bays
	// themselves are never scaled, so a legacy 30 m stand at scale 2.6
	// comes out as eight bays rather than one giant one.
	const float LengthM =
		(Prop.LengthM.IsSet() ? Prop.LengthM.GetValue() : kDefaultStandLengthM) * FMath::Max(Prop.Scale, 0.01f);
	// With spectators the `_crowd` twin of the module is used when it is
	// authored; the caps have no crowd and stay as they are.
	auto FindBay = [&](const FString& Asset) -> UStaticMesh* {
		if (Dressing.bSpectators)
		{
			const FString Crowd = ApexProps::CrowdVariant(TEXT("grandstand"), Asset);
			if (UStaticMesh* Mesh = Crowd.IsEmpty() ? nullptr : FindAuthoredMesh(TEXT("grandstand"), Crowd))
			{
				return Mesh;
			}
		}
		return FindAuthoredMesh(TEXT("grandstand"), Asset);
	};
	bool bWedge = false;
	ApexProps::FStandLayout Layout = ApexProps::LayoutGrandstand(Resolved.Asset, LengthM, Prop.RadiusM, &bWedge);
	UStaticMesh* BayMesh = FindBay(Layout.BayAsset);
	if (!BayMesh && bWedge)
	{
		// No wedge of that family authored: straight bays around the bend.
		Layout = ApexProps::LayoutGrandstand(Resolved.Asset, LengthM, TOptional<float>());
		BayMesh = FindBay(Layout.BayAsset);
	}
	if (!BayMesh)
	{
		BayMesh = Resolved.Mesh;
	}
	UStaticMesh* CapMesh = Layout.CapAsset.IsEmpty() ? nullptr : FindAuthoredMesh(TEXT("grandstand"), Layout.CapAsset);

	const FString Name = FString::Printf(TEXT("Grandstand_%d"), Index);
	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = Level;
	SpawnParams.ObjectFlags = RF_Transactional;
	SpawnParams.Name = MakeUniqueObjectName(Level, AActor::StaticClass(), FName(*Name));
	const FRotator Rotation(0.0f, YawDeg, 0.0f);
	AActor* Actor = World->SpawnActor<AActor>(Prop.Location, Rotation, SpawnParams);
	if (!Actor)
	{
		return nullptr;
	}
	Factory.LabelActor(Actor, Name);
	Actor->Tags.Add(PropTag);
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"), RF_Transactional);
	Root->CreationMethod = EComponentCreationMethod::Instance;
	Root->SetMobility(EComponentMobility::Movable);
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocationAndRotation(Prop.Location, Rotation);

	auto AddRow = [&](const TCHAR* RowName, UStaticMesh* Mesh, const TArray<FTransform>& Placements) {
		if (!Mesh || Placements.IsEmpty())
		{
			return;
		}
		UHierarchicalInstancedStaticMeshComponent* Row =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, RowName, RF_Transactional);
		Row->CreationMethod = EComponentCreationMethod::Instance;
		Row->SetMobility(EComponentMobility::Movable);
		Row->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
		Row->SetupAttachment(Root);
		Row->SetStaticMesh(Mesh);
		Row->SetNumCustomDataFloats(1);
		Actor->AddInstanceComponent(Row);
		Row->RegisterComponent();
		Row->AddInstances(Placements, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false);
	};
	AddRow(TEXT("Bays"), BayMesh, Layout.Bays);
	AddRow(TEXT("Caps"), CapMesh, Layout.Caps);
	return Actor;
}

void FApexTrackSceneBuilder::SpawnActors(
	const FApexTrackScene& Scene, UWorld* World, ULevel* Level, TArray<AActor*>& OutActors)
{
	check(World && Level);
	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = Level;
	SpawnParams.ObjectFlags = RF_Transactional;
	const bool bCollisionComponents = Factory.WantsCollisionComponents();

	// Track geometry: one static actor per baked mesh.
	int32 MeshActors = 0;
	for (const FApexTrackMesh& Source : Scene.Meshes)
	{
		UStaticMesh* Mesh = Meshes.FindRef(Source.Name);
		if (!Mesh)
		{
			continue;
		}
		SpawnParams.Name = MakeUniqueObjectName(Level, AStaticMeshActor::StaticClass(), FName(*Source.Name));
		AStaticMeshActor* Actor =
			World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!Actor)
		{
			continue;
		}
		Factory.LabelActor(Actor, Source.Name);
		// What the racing line and the cameras stand on.
		Actor->Tags.Add(TrackMeshTag);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		// Positions are baked in world space, so the actor stays at the
		// origin and the mesh carries the layout.
		// Movable, despite never moving. Static geometry wants baked lighting,
		// and nobody is going to run a lighting build on 26 regenerated
		// circuits — the level just loads complaining about hundreds of
		// unbuilt objects. Movable puts it on dynamic lighting instead, which
		// is what a procedurally generated level can actually rely on.
		Component->SetMobility(EComponentMobility::Movable);
		// Movable is a lighting decision, not a promise to move, and virtual
		// shadow maps read it as one: a movable primitive is cached as
		// dynamic, so a whole circuit — sixty-odd 384 m ground patches plus
		// the grass and gravel — re-marks its shadow pages every frame and
		// overflows the non-Nanite marking job queue (the "[VSM] Non-Nanite
		// Marking Job Queue overflow" warning on screen). Saying the shape
		// never moves puts it back in the static page cache.
		Component->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
		Component->ComponentTags.Add(TrackMeshTag);
		if (bCollisionComponents)
		{
			// The runtime mesh has no collision of its own; its triangles are
			// cooked by a component beside it instead.
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		Component->SetStaticMesh(Mesh);
		if (bCollisionComponents)
		{
			UApexTrackCollisionComponent* Collision =
				NewObject<UApexTrackCollisionComponent>(Actor, TEXT("Collision"), RF_Transactional);
			Collision->CreationMethod = EComponentCreationMethod::Instance;
			Collision->ComponentTags.Add(TrackMeshTag);
			Collision->SetupAttachment(Component);
			Actor->AddInstanceComponent(Collision);
			Collision->RegisterComponent();
			Collision->SetTriangles(Source.Positions, Source.Indices);
		}
		OutActors.Add(Actor);
		++MeshActors;
	}

	// Props. Each resolves to an authored mesh from the kit, the generated
	// stand-in for its kind, or a placeholder box (ResolveProp). The
	// numerous kinds go into one instanced component per resolved mesh and
	// text so a few thousand of them cost a few draw calls; stands become a
	// row of bays under one actor; the rest are an actor each.
	PlaceholderMesh = LoadObject<UStaticMesh>(nullptr, kPlaceholderPropMesh);
	if (!PlaceholderMesh)
	{
		UE_LOG(LogApexTrack, Warning, TEXT("    %s is missing — props without a generated mesh were skipped"),
			kPlaceholderPropMesh);
	}

	auto SettleComponent = [](UStaticMeshComponent* Component) {
		Component->SetMobility(EComponentMobility::Movable);
		// Props are scenery bolted to the ground; same shadow-cache reasoning
		// as the track meshes above.
		Component->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
	};

	// Instances are gathered per component and added in one go at the end:
	// one tree build per component instead of one per instance, which is
	// what makes fifteen thousand of them affordable at runtime.
	struct FInstanceBatch
	{
		UHierarchicalInstancedStaticMeshComponent* Component = nullptr;
		TArray<FTransform> Transforms;
		TArray<float> Jitter;
	};
	TMap<FString, FInstanceBatch> Instanced;
	auto InstancesFor = [&](const FString& Key, const FString& Label, UStaticMesh* Mesh,
							const FResolvedProp& Resolved) -> FInstanceBatch* {
		if (FInstanceBatch* Found = Instanced.Find(Key))
		{
			return Found;
		}
		if (!Mesh)
		{
			return nullptr;
		}
		SpawnParams.Name = MakeUniqueObjectName(Level, AActor::StaticClass(), FName(*Label));
		AActor* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!Actor)
		{
			return nullptr;
		}
		Factory.LabelActor(Actor, Label);
		Actor->Tags.Add(PropTag);
		UHierarchicalInstancedStaticMeshComponent* Component =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, TEXT("Instances"), RF_Transactional);
		Component->CreationMethod = EComponentCreationMethod::Instance;
		SettleComponent(Component);
		Component->SetStaticMesh(Mesh);
		// One float per instance for the material's colour swing.
		Component->SetNumCustomDataFloats(1);
		Actor->SetRootComponent(Component);
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		if (Resolved.bAuthored)
		{
			ApplyAuthoredSlots(Component, Mesh, Resolved.Text);
		}
		OutActors.Add(Actor);
		FInstanceBatch& Batch = Instanced.Add(Key);
		Batch.Component = Component;
		return &Batch;
	};

	// Both faces of a distance board, so the text reads coming and going.
	// A text component faces its own +X, so the face a driver approaching
	// from -X sees is the one turned 180°.
	auto AddSignText = [&](AStaticMeshActor* Actor, const FString& Text) {
		for (int32 Face = 0; Face < 2; ++Face)
		{
			const bool bFront = Face == 0;
			UTextRenderComponent* Label = NewObject<UTextRenderComponent>(
				Actor, bFront ? TEXT("TextFront") : TEXT("TextBack"), RF_Transactional);
			Label->CreationMethod = EComponentCreationMethod::Instance;
			Label->SetMobility(EComponentMobility::Movable);
			Label->SetupAttachment(Actor->GetStaticMeshComponent());
			Label->SetRelativeLocation(FVector(bFront ? -4.0 : 4.0, 0.0, 160.0));
			Label->SetRelativeRotation(FRotator(0.0, bFront ? 180.0 : 0.0, 0.0));
			Label->SetText(FText::FromString(Text));
			Label->SetHorizontalAlignment(EHTA_Center);
			Label->SetVerticalAlignment(EVRTA_TextCenter);
			Label->SetWorldSize(80.0f);
			Label->SetTextRenderColor(FColor::Black);
			Actor->AddInstanceComponent(Label);
			Label->RegisterComponent();
		}
	};

	// The corner name on a `corner_sign`: one text component just off the
	// board face, which the kit authors on local +Y between 1.8 and 2.6 m
	// (the builder already turns the board at the road). Long names shrink
	// to stay inside the 2.4 m board.
	auto AddBoardText = [&](AStaticMeshActor* Actor, const FString& Text) {
		UTextRenderComponent* Label = NewObject<UTextRenderComponent>(Actor, TEXT("BoardText"), RF_Transactional);
		Label->CreationMethod = EComponentCreationMethod::Instance;
		Label->SetMobility(EComponentMobility::Movable);
		Label->SetupAttachment(Actor->GetStaticMeshComponent());
		Label->SetRelativeLocation(FVector(0.0, 3.5, 220.0));
		Label->SetRelativeRotation(FRotator(0.0, 90.0, 0.0));
		Label->SetText(FText::FromString(Text));
		Label->SetHorizontalAlignment(EHTA_Center);
		Label->SetVerticalAlignment(EVRTA_TextCenter);
		const float Fit = 200.0f / FMath::Max(1, Text.Len()) / 0.62f;	// ~0.62 em advance per glyph
		Label->SetWorldSize(FMath::Clamp(Fit, 14.0f, 44.0f));
		Label->SetTextRenderColor(FColor(20, 20, 24));
		Actor->AddInstanceComponent(Label);
		Label->RegisterComponent();
	};

	auto SpawnMeshActor = [&](int32 Index, const FString& Label, const FVector& Location,
							  const FRotator& Rotation) -> AStaticMeshActor* {
		SpawnParams.Name = MakeUniqueObjectName(
			Level, AStaticMeshActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), Index)));
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
		if (Actor)
		{
			Factory.LabelActor(Actor, Label);
			Actor->Tags.Add(PropTag);
			SettleComponent(Actor->GetStaticMeshComponent());
			OutActors.Add(Actor);
		}
		return Actor;
	};

	int32 PropActors = 0;
	int32 PropInstances = 0;
	int32 AuthoredProps = 0;
	TSet<FString> FallbackKinds;
	for (int32 i = 0; i < Scene.Props.Num(); ++i)
	{
		const FApexTrackProp& Prop = Scene.Props[i];
		const FResolvedProp Resolved = ResolveProp(Prop);

		float YawDeg = Prop.YawDeg;
		if (Resolved.bFaceUpCourse)
		{
			// The export gives every prop the road's heading and the kit
			// authors a board's face on local +Y, which points a distance
			// board across the road at the crowd; turn it back up the
			// course, at the cars, from either side.
			YawDeg += ApexProps::UpCourseYawDeg;
		}
		else if (Resolved.bFaceRoad && RoadSideOf(Scene, Prop) < 0.0f)
		{
			YawDeg += 180.0f;
		}
		const FRotator Rotation(0.0f, YawDeg, 0.0f);

		if (Resolved.bAuthored)
		{
			++AuthoredProps;
			if (Resolved.Kind == TEXT("grandstand"))
			{
				if (AActor* Stand = SpawnGrandstand(World, Level, Prop, Resolved, YawDeg, i))
				{
					OutActors.Add(Stand);
					++PropActors;
				}
				continue;
			}
			if (Resolved.Kind == TEXT("sky"))
			{
				// Origin at the hull centre, `z` the altitude; it drifts from here.
				SpawnParams.Name = MakeUniqueObjectName(
					Level, AApexSkyDriftActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), i)));
				if (AApexSkyDriftActor* Actor =
						World->SpawnActor<AApexSkyDriftActor>(Prop.Location, Rotation, SpawnParams))
				{
					Factory.LabelActor(Actor, FString::Printf(TEXT("%s_%s"), *Resolved.Kind, *Resolved.Asset));
					Actor->Tags.Add(PropTag);
					Actor->GetMesh()->SetStaticMesh(Resolved.Mesh);
					Actor->GetMesh()->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
					Actor->SetActorScale3D(FVector(Prop.Scale));
					ApplyAuthoredSlots(Actor->GetMesh(), Resolved.Mesh, Resolved.Text);
					OutActors.Add(Actor);
					++PropActors;
				}
				continue;
			}
			if (Resolved.Kind == ApexProps::FerrisWheelKind && Resolved.Asset == ApexProps::FerrisWheelAsset)
			{
				SpawnParams.Name = MakeUniqueObjectName(
					Level, AApexRotorActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), i)));
				if (AApexRotorActor* Actor = World->SpawnActor<AApexRotorActor>(Prop.Location, Rotation, SpawnParams))
				{
					Factory.LabelActor(Actor, FString::Printf(TEXT("%s_%s"), *Resolved.Kind, *Resolved.Asset));
					Actor->Tags.Add(PropTag);
					Actor->GetMesh()->SetStaticMesh(Resolved.Mesh);
					Actor->GetMesh()->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
					Actor->GetRotor()->SetStaticMesh(
						FindAuthoredMesh(ApexProps::FerrisWheelKind, ApexProps::FerrisRotorAsset));
					Actor->GetRotor()->SetRelativeLocation(ApexProps::FerrisHubOffsetCm);
					Actor->SetActorScale3D(FVector(Prop.Scale));
					OutActors.Add(Actor);
					++PropActors;
				}
				continue;
			}

			FVector Scale(Prop.Scale);
			if (Resolved.Kind == TEXT("bridge"))
			{
				// Authored for a 15 m road; the span (local Y) stretches to this one.
				const float SpanM = Prop.SpanM.IsSet() ? Prop.SpanM.GetValue() : RoadWidthAt(Scene, Prop.Location);
				Scale.Y *= ApexProps::BridgeSpanScale(SpanM);
			}

			if (Resolved.bInstanced)
			{
				const FString Key = FString::Printf(TEXT("%s/%s/%s"), *Resolved.Kind, *Resolved.Asset, *Resolved.Text);
				FString Label = FString::Printf(TEXT("Props_%s_%s"), *Resolved.Kind, *Resolved.Asset);
				if (!Resolved.Text.IsEmpty())
				{
					Label += TEXT("_") + TextKey(Resolved.Text);
				}
				if (FInstanceBatch* Batch = InstancesFor(Key, Label, Resolved.Mesh, Resolved))
				{
					Batch->Transforms.Add(FTransform(Rotation, Prop.Location, Scale));
					Batch->Jitter.Add(InstanceJitter(i));
					++PropInstances;
				}
				continue;
			}

			if (AStaticMeshActor* Actor = SpawnMeshActor(
					i, FString::Printf(TEXT("%s_%s"), *Resolved.Kind, *Resolved.Asset), Prop.Location, Rotation))
			{
				UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
				Component->SetStaticMesh(Resolved.Mesh);
				Actor->SetActorScale3D(Scale);
				ApplyAuthoredSlots(Component, Resolved.Mesh, Resolved.Text);
				if (ApexProps::HasTextFace(Resolved.Kind, Resolved.Asset) && !Resolved.Text.IsEmpty())
				{
					AddBoardText(Actor, Resolved.Text);
				}
				++PropActors;
			}
			continue;
		}

		// Generated stand-in or placeholder, as before the kit.
		FallbackKinds.Add(Prop.Kind);
		if (Resolved.bInstanced)
		{
			if (FInstanceBatch* Batch =
					InstancesFor(Prop.Kind, FString::Printf(TEXT("Props_%s"), *Prop.Kind), Resolved.Mesh, Resolved))
			{
				Batch->Transforms.Add(FTransform(Rotation, Prop.Location, FVector(Prop.Scale)));
				Batch->Jitter.Add(InstanceJitter(i));
				++PropInstances;
			}
			continue;
		}

		FVector Location = Prop.Location;
		FVector Scale(Prop.Scale);
		if (!Resolved.Mesh)
		{
			if (!PlaceholderMesh)
			{
				continue;
			}
			Scale = PlaceholderPropScale(Prop.Kind, Prop.Scale) / 100.0f;
			// The box pivot is centred; lift it so props sit on the ground
			// rather than half-buried. Generated meshes pivot at the ground.
			Location.Z += Scale.Z * 50.0f;
		}
		AStaticMeshActor* Actor =
			SpawnMeshActor(i, FString::Printf(TEXT("%s_%s"), *Prop.Kind, *Prop.Asset), Location, Rotation);
		if (!Actor)
		{
			continue;
		}
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		if (Resolved.Mesh)
		{
			Component->SetStaticMesh(Resolved.Mesh);
		}
		else
		{
			Component->SetStaticMesh(PlaceholderMesh);
			UMaterialInterface* PropMaterial = Materials.FindRef(TEXT("prop_") + Prop.Kind);
			if (!PropMaterial)
			{
				PropMaterial = Materials.FindRef(TEXT("prop_default"));
			}
			if (PropMaterial)
			{
				Component->SetMaterial(0, PropMaterial);
			}
		}
		Actor->SetActorScale3D(Scale);
		if (Prop.Kind == TEXT("sign") && !Prop.Text.IsEmpty())
		{
			AddSignText(Actor, Prop.Text);
		}
		++PropActors;
	}

	for (TPair<FString, FInstanceBatch>& Pair : Instanced)
	{
		FInstanceBatch& Batch = Pair.Value;
		Batch.Component->AddInstances(Batch.Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
		for (int32 Index = 0; Index < Batch.Jitter.Num(); ++Index)
		{
			Batch.Component->SetCustomDataValue(Index, 0, Batch.Jitter[Index], /*bMarkRenderStateDirty*/ false);
		}
		Batch.Component->MarkRenderStateDirty();
	}

	if (!FallbackKinds.IsEmpty())
	{
		TArray<FString> Kinds = FallbackKinds.Array();
		Kinds.Sort();
		UE_LOG(LogApexTrack, Display, TEXT("    %d of %d prop(s) use the authored kit; generated stand-ins for: %s%s"),
			AuthoredProps, Scene.Props.Num(), *FString::Join(Kinds, TEXT(", ")),
			AuthoredProps == 0 ? TEXT(" (run ApexPropImport to bring the kit in)") : TEXT(""));
	}
	else if (Scene.Props.Num() > 0)
	{
		UE_LOG(LogApexTrack, Display, TEXT("    every prop uses the authored kit"));
	}

	if (AActor* Lights = SpawnStartLights(Scene, World, Level))
	{
		OutActors.Add(Lights);
	}

	// Deliberately no sun and no sky light.
	//
	// A track is streamed or built into the menu world rather than travelled
	// to, so it inherits that world's lighting. Adding its own gives two
	// directional lights in one scene, which Unreal resolves by picking one
	// and warning about the rest. The race director re-tunes that world's
	// sun instead.
	//
	// Fog and post-processing are different: the menu world has neither, and
	// both are what give a five-kilometer circuit any sense of depth.
	SpawnParams.Name = MakeUniqueObjectName(Level, AExponentialHeightFog::StaticClass(), FName(TEXT("TrackFog")));
	if (AExponentialHeightFog* Fog =
			World->SpawnActor<AExponentialHeightFog>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams))
	{
		Factory.LabelActor(Fog, TEXT("TrackFog"));
		UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
		// Thin: enough that the far end of a straight sits in haze rather
		// than pin-sharp against the sky, not enough to milk out the
		// mid-field (0.0045 did, and with it every color on screen).
		FogComponent->SetFogDensity(0.0015f);
		FogComponent->SetFogHeightFalloff(0.2f);
		// Keep the first ~30 m crisp; the haze belongs to the distance.
		FogComponent->SetStartDistance(3000.0f);
		OutActors.Add(Fog);
	}

	SpawnParams.Name =
		MakeUniqueObjectName(Level, APostProcessVolume::StaticClass(), FName(TEXT("TrackPostProcess")));
	if (APostProcessVolume* PostProcess =
			World->SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams))
	{
		Factory.LabelActor(PostProcess, TEXT("TrackPostProcess"));
		PostProcess->bUnbound = true;
		FPostProcessSettings& Settings = PostProcess->Settings;
		Settings.bOverride_BloomIntensity = true;
		Settings.BloomIntensity = 0.3f;
		// A touch under-exposed: the neutral metering renders daylight as
		// high-key pastel; racing footage sits deeper.
		Settings.bOverride_AutoExposureBias = true;
		Settings.AutoExposureBias = -0.4f;
		// Quick adaptation: the view flips between open sky and
		// self-shadowed cockpit constantly.
		Settings.bOverride_AutoExposureSpeedUp = true;
		Settings.AutoExposureSpeedUp = 5.0f;
		Settings.bOverride_AutoExposureSpeedDown = true;
		Settings.AutoExposureSpeedDown = 2.0f;
		// Clamped to daylight (EV100): the race director drives the sun at
		// real sunlight intensities, and without a floor the auto-exposure
		// would normalize any scene to mid-grey — which is exactly the
		// washed-out look this whole volume exists to prevent.
		Settings.bOverride_AutoExposureMinBrightness = true;
		Settings.AutoExposureMinBrightness = 10.0f;
		Settings.bOverride_AutoExposureMaxBrightness = true;
		Settings.AutoExposureMaxBrightness = 15.0f;
		Settings.bOverride_VignetteIntensity = true;
		Settings.VignetteIntensity = 0.3f;
		OutActors.Add(PostProcess);
	}

	UE_LOG(LogApexTrack, Display,
		TEXT("    %d track mesh actor(s), %d prop actor(s), %d prop instance(s) in %d component(s)"), MeshActors,
		PropActors, PropInstances, Instanced.Num());
}
