#include "ApexTrackAssetBuilder.h"

#include "ApexGroundMaterials.h"
#include "ApexPropLibrary.h"
#include "ApexTrackEditorModule.h"
#include "ApexTrackSceneData.h"
#include "Race/ApexPropActors.h"
#include "Algo/Count.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "StaticMeshResources.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerStart.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/** Engine placeholder used for prop kinds that have no generated mesh. */
	const TCHAR* kPlaceholderPropMesh = TEXT("/Engine/BasicShapes/Cube.Cube");

	/**
	 * Every prop actor carries this tag, so the racing line's ground snap
	 * and the TV camera's ground trace know a bridge deck or a garage roof
	 * is not the road.
	 */
	const FName kPropTag(TEXT("ApexProp"));

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

	/**
	 * Tiling grayscale noise, the surface grain of last resort. An engine
	 * asset, so every project has it; used only when the baked ground set
	 * below has not been imported, which is the state of a fresh clone.
	 */
	const TCHAR* kDetailTexture =
		TEXT("/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq.Good64x64TilingNoiseHighFreq");

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

	/** Object name from an asset path, i.e. the part after the last slash. */
	FString ObjectNameOf(const FString& PackageName)
	{
		FString Left;
		FString Right;
		return PackageName.Split(TEXT("/"), &Left, &Right, ESearchCase::CaseSensitive,
				   ESearchDir::FromEnd)
			? Right
			: PackageName;
	}

	/**
	 * A package ready to receive a freshly generated asset.
	 *
	 * Re-importing a track after editing it is the normal case, not the
	 * exception, so an existing asset of the same name has to get out of the
	 * way. Worlds are the sharp edge: `UWorld::CreateWorld` into a package
	 * that already holds one fails outright trying to name its
	 * `WorldSettings`, and takes the process down with it.
	 */
	UPackage* MakePackage(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}
		Package->FullyLoad();

		const FString ObjectName = ObjectNameOf(PackageName);
		if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *ObjectName))
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Existing->MarkAsGarbage();
		}
		return Package;
	}

	/** Create a material expression and register it with the material. */
	template <typename T>
	T* AddExpr(UMaterial* Material)
	{
		T* Expression = NewObject<T>(Material);
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
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
	 * Fill a mesh description from flat buffers, one polygon group per slot.
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
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const bool bHasEdgeFactors = EdgeFactors.Num() == SourcePositions.Num();

		int32 IndexCount = 0;
		for (const FMeshSlot& Slot : Slots)
		{
			IndexCount += Slot.Indices.Num();
		}
		const int32 VertexCount = SourcePositions.Num();
		MeshDescription.ReserveNewVertices(VertexCount);
		MeshDescription.ReserveNewVertexInstances(IndexCount);
		MeshDescription.ReserveNewTriangles(IndexCount / 3);
		MeshDescription.ReserveNewPolygons(IndexCount / 3);

		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			const FVertexID VertexID = MeshDescription.CreateVertex();
			Positions[VertexID] = SourcePositions[i];
			VertexIDs.Add(VertexID);
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
					const uint32 Index = Slot.Indices[Tri + Corner];
					const FVertexInstanceID InstanceID =
						MeshDescription.CreateVertexInstance(VertexIDs[Index]);
					Normals[InstanceID] = SourceNormals[Index];
					UVs.Set(InstanceID, 0, SourceUVs[Index]);
					if (bHasEdgeFactors)
					{
						Colors[InstanceID] =
							FVector4f(EdgeFactors[Index], 1.0f, 1.0f, 1.0f);
					}
					Corners[Corner] = InstanceID;
				}
				MeshDescription.CreatePolygon(PolygonGroup, Corners);
			}
		}
	}

}	 // namespace

FApexTrackAssetBuilder::FApexTrackAssetBuilder(const FString& DestRoot, const FString& TrackStem)
	: TrackFolder(DestRoot / TrackStem)
	, TrackName(TrackStem)
	, PropsRoot(ApexProps::DefaultRoot)
{
	LevelPackage = TrackFolder / (TEXT("L_") + TrackStem);
}

bool FApexTrackAssetBuilder::Build(const FApexTrackScene& Scene, FString& OutError)
{
	PurgeExistingAssets();
	Dressing = Scene.Dressing;
	return BuildMaterials(Scene, OutError) && BuildMeshes(Scene, OutError)
		&& BuildLevel(Scene, OutError) && SaveTouchedPackages(OutError);
}

void FApexTrackAssetBuilder::PurgeExistingAssets()
{
	// Delete the track's whole content folder before regenerating it.
	//
	// Mesh names are derived from the material keys and section spans in the
	// export, so editing a track can retire names as well as add them.
	// Overwriting in place would leave those orphans behind forever, still
	// referenced by nothing and still cooked into builds. This runs before
	// anything from the folder is loaded, so there is no in-memory state to
	// invalidate — which is also why it only makes sense in the commandlet,
	// on a fresh process.
	const FString Folder = FPackageName::LongPackageNameToFilename(TrackFolder);
	if (IFileManager::Get().DirectoryExists(*Folder))
	{
		IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		UE_LOG(LogApexTrackImport, Display, TEXT("    cleared previous assets in %s"), *TrackFolder);
	}
}

bool FApexTrackAssetBuilder::BuildMaterials(const FApexTrackScene& Scene, FString& OutError)
{
	// One parent material, then an instance per key. The project has no
	// authored track materials yet, so the parent carries enough procedural
	// detail to keep a circuit from reading as poster paint: an optional
	// stripe over the `u` coordinate (curbs, tire walls), and world-space
	// noise breaking up albedo and roughness. Swap `ParentMaterial` for a
	// hand-authored asset later and the instances keep working, as long as
	// it exposes the same parameters.
	const FString ParentPackageName = TrackFolder / TEXT("M_ApexTrackBase");
	UPackage* ParentPackage = MakePackage(ParentPackageName);
	if (!ParentPackage)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *ParentPackageName);
		return false;
	}

	UMaterial* Parent = NewObject<UMaterial>(
		ParentPackage, *ObjectNameOf(ParentPackageName), RF_Public | RF_Standalone);
	if (!Parent)
	{
		OutError = TEXT("could not create the parent material");
		return false;
	}

	UMaterialExpressionVectorParameter* ColorParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	ColorParam->ParameterName = TEXT("BaseColor");
	ColorParam->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

	UMaterialExpressionVectorParameter* SecondaryParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	SecondaryParam->ParameterName = TEXT("SecondaryColor");
	SecondaryParam->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

	UMaterialExpressionScalarParameter* RoughnessParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessParam->ParameterName = TEXT("Roughness");
	RoughnessParam->DefaultValue = 0.85f;

	// A period `u` never reaches, so stripes are off unless an instance
	// asks for them (0 would divide by zero in the graph below).
	UMaterialExpressionScalarParameter* StripeParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	StripeParam->ParameterName = TEXT("StripePeriod");
	StripeParam->DefaultValue = 1.0e6f;

	UMaterialExpressionScalarParameter* NoiseAmountParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	NoiseAmountParam->ParameterName = TEXT("NoiseAmount");
	NoiseAmountParam->DefaultValue = 0.0f;

	// Cycles per centimeter of world space.
	UMaterialExpressionScalarParameter* NoiseScaleParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	NoiseScaleParam->ParameterName = TEXT("NoiseScale");
	NoiseScaleParam->DefaultValue = 0.002f;

	UMaterialExpressionScalarParameter* RoughnessNoiseParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessNoiseParam->ParameterName = TEXT("RoughnessNoise");
	RoughnessNoiseParam->DefaultValue = 0.0f;

	// Stripe mask: fmod(floor(u / period), 2) alternates 0/1 along `u`,
	// which the bake emits as meters of station for track strips and which
	// is plain 0..1 face UVs on the placeholder prop cubes.
	UMaterialExpressionTextureCoordinate* TexCoord =
		AddExpr<UMaterialExpressionTextureCoordinate>(Parent);
	UMaterialExpressionComponentMask* MaskU = AddExpr<UMaterialExpressionComponentMask>(Parent);
	MaskU->Input.Expression = TexCoord;
	MaskU->R = 1;
	MaskU->G = 0;
	MaskU->B = 0;
	MaskU->A = 0;
	UMaterialExpressionDivide* StripeU = AddExpr<UMaterialExpressionDivide>(Parent);
	StripeU->A.Expression = MaskU;
	StripeU->B.Expression = StripeParam;
	UMaterialExpressionFloor* StripeIndex = AddExpr<UMaterialExpressionFloor>(Parent);
	StripeIndex->Input.Expression = StripeU;
	UMaterialExpressionConstant* Two = AddExpr<UMaterialExpressionConstant>(Parent);
	Two->R = 2.0f;
	UMaterialExpressionFmod* Stripe = AddExpr<UMaterialExpressionFmod>(Parent);
	Stripe->A.Expression = StripeIndex;
	Stripe->B.Expression = Two;
	UMaterialExpressionLinearInterpolate* Paint =
		AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
	Paint->A.Expression = ColorParam;
	Paint->B.Expression = SecondaryParam;
	Paint->Alpha.Expression = Stripe;

	// World-space turbulence, recentred to ±0.5 so instances can scale it.
	UMaterialExpressionWorldPosition* WorldPos = AddExpr<UMaterialExpressionWorldPosition>(Parent);
	UMaterialExpressionMultiply* NoisePos = AddExpr<UMaterialExpressionMultiply>(Parent);
	NoisePos->A.Expression = WorldPos;
	NoisePos->B.Expression = NoiseScaleParam;
	UMaterialExpressionNoise* NoiseExpr = AddExpr<UMaterialExpressionNoise>(Parent);
	NoiseExpr->Position.Expression = NoisePos;
	NoiseExpr->Scale = 1.0f;
	NoiseExpr->bTurbulence = true;
	NoiseExpr->Levels = 3;
	NoiseExpr->OutputMin = 0.0f;
	NoiseExpr->OutputMax = 1.0f;
	UMaterialExpressionAdd* NoiseCentered = AddExpr<UMaterialExpressionAdd>(Parent);
	NoiseCentered->A.Expression = NoiseExpr;
	NoiseCentered->ConstB = -0.5f;

	// Albedo: the striped paint, brightened or darkened by the noise.
	UMaterialExpressionMultiply* Mottle = AddExpr<UMaterialExpressionMultiply>(Parent);
	Mottle->A.Expression = NoiseCentered;
	Mottle->B.Expression = NoiseAmountParam;
	UMaterialExpressionAdd* Brightness = AddExpr<UMaterialExpressionAdd>(Parent);
	Brightness->A.Expression = Mottle;
	Brightness->ConstB = 1.0f;
	UMaterialExpressionMultiply* Albedo = AddExpr<UMaterialExpressionMultiply>(Parent);
	Albedo->A.Expression = Paint;
	Albedo->B.Expression = Brightness;

	// Roughness: the base value, wobbled by the same noise so sheen varies
	// with the mottling instead of against it.
	UMaterialExpressionMultiply* RoughWobble = AddExpr<UMaterialExpressionMultiply>(Parent);
	RoughWobble->A.Expression = NoiseCentered;
	RoughWobble->B.Expression = RoughnessNoiseParam;
	UMaterialExpressionAdd* RoughSum = AddExpr<UMaterialExpressionAdd>(Parent);
	RoughSum->A.Expression = RoughnessParam;
	RoughSum->B.Expression = RoughWobble;

	// Per-instance colour swing for instanced foliage: `1 + data0 * Tint`,
	// where the tint is zero (no effect) unless an instance sets it and the
	// custom data reads as its default 0 on anything that is not instanced.
	UMaterialExpressionVectorParameter* TintParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	TintParam->ParameterName = TEXT("InstanceTint");
	TintParam->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	UMaterialExpressionPerInstanceCustomData* InstanceData =
		AddExpr<UMaterialExpressionPerInstanceCustomData>(Parent);
	InstanceData->DataIndex = 0;
	InstanceData->ConstDefaultValue = 0.0f;
	UMaterialExpressionMultiply* TintSwing = AddExpr<UMaterialExpressionMultiply>(Parent);
	TintSwing->A.Expression = TintParam;
	TintSwing->B.Expression = InstanceData;
	UMaterialExpressionAdd* TintFactor = AddExpr<UMaterialExpressionAdd>(Parent);
	TintFactor->A.Expression = TintSwing;
	TintFactor->ConstB = 1.0f;
	UMaterialExpressionMultiply* Tinted = AddExpr<UMaterialExpressionMultiply>(Parent);
	Tinted->A.Expression = Albedo;
	Tinted->B.Expression = TintFactor;

	/** Output 1 of a texture sample or a vertex colour is its red channel. */
	constexpr int32 kRedOutput = 1;

	// The seam fringe.
	//
	// A run-off band is its own mesh at its own height beside the road, so
	// grass meets asphalt as a geometric line between two flat colours —
	// the single most "painted on" thing about a circuit after the surfaces
	// themselves. Nothing in the material knows where that line is, so the
	// builder measures each vertex's distance from the band's edge
	// (`ApexGround::EdgeFactors`) and puts the ramp in the red vertex
	// channel: 0 at the edge, 1 a couple of metres in. Here it is broken
	// with the macro noise and the outer stretch taken toward dust, so the
	// boundary wanders and frays instead of ruling a line.
	//
	// It is a fringe, not a real blend: the two surfaces still meet where
	// they met. Blending properly would mean the bands and the ground
	// sharing a mesh, which is the exporter's business, not this one's.
	//
	// The ×2 −0.5 is what keeps the interior clean. At a vertex colour of 1
	// the mask is saturate(−0.5 + noise × EdgeNoise), and the recentred
	// noise never exceeds +0.5, so for any EdgeNoise up to 1 the middle of
	// the band is untouched. Meshes that carry no colours read as white,
	// which is that same interior case, so this costs them nothing.
	UMaterialExpressionVertexColor* VertexColor = AddExpr<UMaterialExpressionVertexColor>(Parent);
	UMaterialExpressionScalarParameter* EdgeBlendParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	EdgeBlendParam->ParameterName = TEXT("EdgeBlend");
	EdgeBlendParam->DefaultValue = 0.0f;
	UMaterialExpressionScalarParameter* EdgeNoiseParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	EdgeNoiseParam->ParameterName = TEXT("EdgeNoise");
	EdgeNoiseParam->DefaultValue = 0.0f;
	UMaterialExpressionVectorParameter* EdgeColorParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	EdgeColorParam->ParameterName = TEXT("EdgeColor");
	EdgeColorParam->DefaultValue = kEdgeDustColor;

	UMaterialExpressionSubtract* FromEdge = AddExpr<UMaterialExpressionSubtract>(Parent);
	FromEdge->ConstA = 1.0f;
	FromEdge->B.Expression = VertexColor;
	FromEdge->B.OutputIndex = kRedOutput;
	UMaterialExpressionMultiply* EdgeRamp = AddExpr<UMaterialExpressionMultiply>(Parent);
	EdgeRamp->A.Expression = FromEdge;
	EdgeRamp->ConstB = 2.0f;
	UMaterialExpressionSubtract* EdgeBias = AddExpr<UMaterialExpressionSubtract>(Parent);
	EdgeBias->A.Expression = EdgeRamp;
	EdgeBias->ConstB = 0.5f;
	UMaterialExpressionMultiply* EdgeWobble = AddExpr<UMaterialExpressionMultiply>(Parent);
	EdgeWobble->A.Expression = NoiseCentered;
	EdgeWobble->B.Expression = EdgeNoiseParam;
	UMaterialExpressionAdd* EdgeRagged = AddExpr<UMaterialExpressionAdd>(Parent);
	EdgeRagged->A.Expression = EdgeBias;
	EdgeRagged->B.Expression = EdgeWobble;
	UMaterialExpressionClamp* EdgeMask = AddExpr<UMaterialExpressionClamp>(Parent);
	EdgeMask->Input.Expression = EdgeRagged;
	EdgeMask->MinDefault = 0.0f;
	EdgeMask->MaxDefault = 1.0f;
	UMaterialExpressionMultiply* Fringe = AddExpr<UMaterialExpressionMultiply>(Parent);
	Fringe->A.Expression = EdgeMask;
	Fringe->B.Expression = EdgeBlendParam;
	UMaterialExpressionLinearInterpolate* Edged =
		AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
	Edged->A.Expression = Tinted;
	Edged->B.Expression = EdgeColorParam;
	Edged->Alpha.Expression = Fringe;

	// The surface itself. Track strips carry metres in their UVs (u along
	// the station, v across; ground tiles use world x/y), so every tiling
	// parameter below is repeats per metre.
	//
	// Two graphs, chosen once per track at bake time rather than branched at
	// runtime, because this material is generated per track anyway and a
	// texture sample multiplied by zero still costs a texture sample:
	//
	//  - the baked ground set from `/Game/Ground`, when it has been imported
	//    (`-run=ApexGroundTexImport` after `scripts/bake_ground_textures.py`);
	//  - otherwise the engine's 64-texel noise tile as plain grain, which is
	//    all this material had before the set existed and is what a fresh
	//    clone still gets.
	UMaterialExpression* FinalAlbedo = Edged;
	UMaterialExpression* FinalRoughness = RoughSum;
	UMaterialExpression* FinalNormal = nullptr;
	// Asphalt is the parent's default set. Every instance overrides the
	// three maps with its own, so which set supplies the defaults only
	// matters for a material that asks for none.
	const FGroundMapSet DefaultGround = LoadGroundSet(TEXT("asphalt"));
	const bool bGroundTextures = DefaultGround.IsComplete();
	if (bGroundTextures)
	{
		// Every map is sampled twice — at the instance's own tiling and at a
		// deliberately non-integer multiple of it — and mixed by a
		// world-space noise. One scale alone shows its repeat by the third
		// row of quads; two mixed at twenty metres do not, for one extra
		// sample per map. The same mix is then pushed all the way to the
		// coarse sample with distance, so the far field is low-frequency
		// texture rather than a metre of grain per pixel, which is what
		// shimmers, and the normal is flattened over the same range because
		// a tangent-space normal at a grazing angle is pure aliasing.
		UMaterialExpressionScalarParameter* TilingParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		TilingParam->ParameterName = TEXT("TextureTiling");
		TilingParam->DefaultValue = 1.0f / ApexGround::TextureTileM;
		// Off by default, all three: the prop stand-ins and anything else
		// that shares this parent without asking for a surface would
		// otherwise come out in asphalt grain and asphalt bumps. Only an
		// instance with a ground look turns them on.
		UMaterialExpressionScalarParameter* TextureAmountParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		TextureAmountParam->ParameterName = TEXT("TextureAmount");
		TextureAmountParam->DefaultValue = 0.0f;
		UMaterialExpressionScalarParameter* NormalStrengthParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		NormalStrengthParam->ParameterName = TEXT("NormalStrength");
		NormalStrengthParam->DefaultValue = 0.0f;
		UMaterialExpressionScalarParameter* RoughMapParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		RoughMapParam->ParameterName = TEXT("RoughnessMapAmount");
		RoughMapParam->DefaultValue = 0.0f;
		// Cycles per centimetre of world space, as `NoiseScale`: 0.0005 is
		// a twenty-metre patch, large enough that the eye reads it as the
		// ground varying rather than as the texture changing scale.
		UMaterialExpressionScalarParameter* MacroScaleParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		MacroScaleParam->ParameterName = TEXT("MacroBlendScale");
		MacroScaleParam->DefaultValue = 0.0005f;
		// Centimetres of pixel depth: fully coarse by 200 m out.
		UMaterialExpressionScalarParameter* FarStartParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		FarStartParam->ParameterName = TEXT("FarFadeStart");
		FarStartParam->DefaultValue = 4000.0f;
		UMaterialExpressionScalarParameter* FarRangeParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		FarRangeParam->ParameterName = TEXT("FarFadeRange");
		FarRangeParam->DefaultValue = 16000.0f;

		UMaterialExpressionMultiply* NearUV = AddExpr<UMaterialExpressionMultiply>(Parent);
		NearUV->A.Expression = TexCoord;
		NearUV->B.Expression = TilingParam;
		UMaterialExpressionMultiply* CoarseUV = AddExpr<UMaterialExpressionMultiply>(Parent);
		CoarseUV->A.Expression = NearUV;
		// Nothing near a ratio of small integers, or the two scales line up
		// again every few tiles and the repeat comes straight back.
		CoarseUV->ConstB = 0.371f;

		UMaterialExpressionMultiply* MacroPos = AddExpr<UMaterialExpressionMultiply>(Parent);
		MacroPos->A.Expression = WorldPos;
		MacroPos->B.Expression = MacroScaleParam;
		UMaterialExpressionNoise* MacroNoise = AddExpr<UMaterialExpressionNoise>(Parent);
		MacroNoise->Position.Expression = MacroPos;
		MacroNoise->Scale = 1.0f;
		MacroNoise->Levels = 2;
		MacroNoise->OutputMin = 0.0f;
		MacroNoise->OutputMax = 1.0f;

		UMaterialExpressionPixelDepth* Depth = AddExpr<UMaterialExpressionPixelDepth>(Parent);
		UMaterialExpressionSubtract* PastStart = AddExpr<UMaterialExpressionSubtract>(Parent);
		PastStart->A.Expression = Depth;
		PastStart->B.Expression = FarStartParam;
		UMaterialExpressionDivide* FarRaw = AddExpr<UMaterialExpressionDivide>(Parent);
		FarRaw->A.Expression = PastStart;
		FarRaw->B.Expression = FarRangeParam;
		UMaterialExpressionClamp* Far = AddExpr<UMaterialExpressionClamp>(Parent);
		Far->Input.Expression = FarRaw;
		Far->MinDefault = 0.0f;
		Far->MaxDefault = 1.0f;
		UMaterialExpressionLinearInterpolate* Mix =
			AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
		Mix->A.Expression = MacroNoise;
		Mix->ConstB = 1.0f;
		Mix->Alpha.Expression = Far;

		// The sampler type is baked into the node, not the override, so an
		// instance may only swap a map for one of the same class. That is
		// what the importer's fixed per-suffix settings are for: every
		// `_col` is sRGB colour, every `_nrm` a normal map and every
		// `_rough` grayscale, on every set.
		auto SampleMap = [&](const TCHAR* Name, UTexture* Texture, UMaterialExpression* Coords) {
			UMaterialExpressionTextureSampleParameter2D* Sample =
				AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
			Sample->ParameterName = Name;
			Sample->Texture = Texture;
			Sample->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
			Sample->Coordinates.Expression = Coords;
			return Sample;
		};
		auto BlendMap = [&](const TCHAR* Name, UTexture* Texture, int32 Output) {
			UMaterialExpressionLinearInterpolate* Blend =
				AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
			Blend->A.Expression = SampleMap(Name, Texture, NearUV);
			Blend->A.OutputIndex = Output;
			Blend->B.Expression = SampleMap(Name, Texture, CoarseUV);
			Blend->B.OutputIndex = Output;
			Blend->Alpha.Expression = Mix;
			return Blend;
		};
		UMaterialExpressionLinearInterpolate* AlbedoMap =
			BlendMap(TEXT("AlbedoMap"), DefaultGround.Albedo, 0);
		UMaterialExpressionLinearInterpolate* NormalMap =
			BlendMap(TEXT("NormalMap"), DefaultGround.Normal, 0);
		UMaterialExpressionLinearInterpolate* RoughMap =
			BlendMap(TEXT("RoughnessMap"), DefaultGround.Roughness, kRedOutput);

		// The maps are baked to a per-channel mean of 0.5 so the exporter's
		// own per-key colour still decides what a surface is — a red kerb
		// from a yellow one, this circuit's asphalt from its pit lane (see
		// the note in apex_tex.py) — and doubling brings the level back.
		UMaterialExpressionMultiply* MapDoubled = AddExpr<UMaterialExpressionMultiply>(Parent);
		MapDoubled->A.Expression = AlbedoMap;
		MapDoubled->ConstB = 2.0f;
		UMaterialExpressionLinearInterpolate* MapGain =
			AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
		MapGain->ConstA = 1.0f;
		MapGain->B.Expression = MapDoubled;
		MapGain->Alpha.Expression = TextureAmountParam;
		UMaterialExpressionMultiply* TexturedAlbedo = AddExpr<UMaterialExpressionMultiply>(Parent);
		TexturedAlbedo->A.Expression = Edged;
		TexturedAlbedo->B.Expression = MapGain;
		FinalAlbedo = TexturedAlbedo;

		UMaterialExpressionAdd* RoughCentered = AddExpr<UMaterialExpressionAdd>(Parent);
		RoughCentered->A.Expression = RoughMap;
		RoughCentered->ConstB = -0.5f;
		UMaterialExpressionMultiply* RoughSwing = AddExpr<UMaterialExpressionMultiply>(Parent);
		RoughSwing->A.Expression = RoughCentered;
		RoughSwing->B.Expression = RoughMapParam;
		UMaterialExpressionAdd* TexturedRoughness = AddExpr<UMaterialExpressionAdd>(Parent);
		TexturedRoughness->A.Expression = RoughSum;
		TexturedRoughness->B.Expression = RoughSwing;
		FinalRoughness = TexturedRoughness;

		UMaterialExpressionSubtract* NearFactor = AddExpr<UMaterialExpressionSubtract>(Parent);
		NearFactor->ConstA = 1.0f;
		NearFactor->B.Expression = Far;
		UMaterialExpressionMultiply* SlopeScale = AddExpr<UMaterialExpressionMultiply>(Parent);
		SlopeScale->A.Expression = NormalStrengthParam;
		SlopeScale->B.Expression = NearFactor;
		UMaterialExpressionComponentMask* SlopeXY =
			AddExpr<UMaterialExpressionComponentMask>(Parent);
		SlopeXY->Input.Expression = NormalMap;
		SlopeXY->R = 1;
		SlopeXY->G = 1;
		SlopeXY->B = 0;
		SlopeXY->A = 0;
		UMaterialExpressionComponentMask* SlopeZ =
			AddExpr<UMaterialExpressionComponentMask>(Parent);
		SlopeZ->Input.Expression = NormalMap;
		SlopeZ->R = 0;
		SlopeZ->G = 0;
		SlopeZ->B = 1;
		SlopeZ->A = 0;
		UMaterialExpressionMultiply* ScaledXY = AddExpr<UMaterialExpressionMultiply>(Parent);
		ScaledXY->A.Expression = SlopeXY;
		ScaledXY->B.Expression = SlopeScale;
		UMaterialExpressionAppendVector* NormalXYZ =
			AddExpr<UMaterialExpressionAppendVector>(Parent);
		NormalXYZ->A.Expression = ScaledXY;
		NormalXYZ->B.Expression = SlopeZ;
		UMaterialExpressionNormalize* NormalOut = AddExpr<UMaterialExpressionNormalize>(Parent);
		NormalOut->VectorInput.Expression = NormalXYZ;
		FinalNormal = NormalOut;
	}
	else if (UTexture* DetailTexture = LoadObject<UTexture>(nullptr, kDetailTexture))
	{
		// The fallback: two samples of the noise tile at coprime scales to
		// hide its 64-texel repeat, and a finite difference of the first
		// turned into a bump so the grain catches light rather than just
		// tinting it. All off (0) by default; the family branches below
		// switch it on.
		UMaterialExpressionScalarParameter* DetailTilingParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		DetailTilingParam->ParameterName = TEXT("DetailTiling");
		DetailTilingParam->DefaultValue = 0.0f;
		UMaterialExpressionScalarParameter* DetailAmountParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		DetailAmountParam->ParameterName = TEXT("DetailAmount");
		DetailAmountParam->DefaultValue = 0.0f;
		UMaterialExpressionScalarParameter* DetailRoughnessParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		DetailRoughnessParam->ParameterName = TEXT("DetailRoughness");
		DetailRoughnessParam->DefaultValue = 0.0f;
		UMaterialExpressionScalarParameter* DetailNormalParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		DetailNormalParam->ParameterName = TEXT("DetailNormal");
		DetailNormalParam->DefaultValue = 0.0f;

		UMaterialExpressionMultiply* DetailUV = AddExpr<UMaterialExpressionMultiply>(Parent);
		DetailUV->A.Expression = TexCoord;
		DetailUV->B.Expression = DetailTilingParam;

		const EMaterialSamplerType SamplerType =
			MaterialExpressionUtils::GetSamplerTypeForTexture(DetailTexture);
		auto SampleDetail = [&](UMaterialExpression* Coordinates) {
			UMaterialExpressionTextureSampleParameter2D* Sample =
				AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
			Sample->ParameterName = TEXT("DetailTexture");
			Sample->Texture = DetailTexture;
			Sample->SamplerType = SamplerType;
			Sample->Coordinates.Expression = Coordinates;
			return Sample;
		};
		auto OffsetUV = [&](float DU, float DV) {
			UMaterialExpressionConstant2Vector* Delta =
				AddExpr<UMaterialExpressionConstant2Vector>(Parent);
			Delta->R = DU;
			Delta->G = DV;
			UMaterialExpressionAdd* Shifted = AddExpr<UMaterialExpressionAdd>(Parent);
			Shifted->A.Expression = DetailUV;
			Shifted->B.Expression = Delta;
			return Shifted;
		};
		UMaterialExpressionTextureSampleParameter2D* Fine = SampleDetail(DetailUV);
		UMaterialExpressionMultiply* CoarseUV = AddExpr<UMaterialExpressionMultiply>(Parent);
		CoarseUV->A.Expression = DetailUV;
		CoarseUV->ConstB = 0.137f;
		UMaterialExpressionTextureSampleParameter2D* Coarse = SampleDetail(CoarseUV);
		UMaterialExpressionAdd* DetailSum = AddExpr<UMaterialExpressionAdd>(Parent);
		DetailSum->A.Expression = Fine;
		DetailSum->A.OutputIndex = kRedOutput;
		DetailSum->B.Expression = Coarse;
		DetailSum->B.OutputIndex = kRedOutput;
		// Average of the two, recentred to ±0.5 like the macro noise.
		UMaterialExpressionMultiply* DetailMean = AddExpr<UMaterialExpressionMultiply>(Parent);
		DetailMean->A.Expression = DetailSum;
		DetailMean->ConstB = 0.5f;
		UMaterialExpressionAdd* DetailCentered = AddExpr<UMaterialExpressionAdd>(Parent);
		DetailCentered->A.Expression = DetailMean;
		DetailCentered->ConstB = -0.5f;

		UMaterialExpressionMultiply* DetailMottle = AddExpr<UMaterialExpressionMultiply>(Parent);
		DetailMottle->A.Expression = DetailCentered;
		DetailMottle->B.Expression = DetailAmountParam;
		UMaterialExpressionAdd* DetailBrightness = AddExpr<UMaterialExpressionAdd>(Parent);
		DetailBrightness->A.Expression = DetailMottle;
		DetailBrightness->ConstB = 1.0f;
		UMaterialExpressionMultiply* DetailedAlbedo = AddExpr<UMaterialExpressionMultiply>(Parent);
		DetailedAlbedo->A.Expression = Edged;
		DetailedAlbedo->B.Expression = DetailBrightness;
		FinalAlbedo = DetailedAlbedo;

		UMaterialExpressionMultiply* DetailRough = AddExpr<UMaterialExpressionMultiply>(Parent);
		DetailRough->A.Expression = DetailCentered;
		DetailRough->B.Expression = DetailRoughnessParam;
		UMaterialExpressionAdd* DetailedRoughness = AddExpr<UMaterialExpressionAdd>(Parent);
		DetailedRoughness->A.Expression = RoughSum;
		DetailedRoughness->B.Expression = DetailRough;
		FinalRoughness = DetailedRoughness;

		// Bump from the fine sample: height falling along +u tilts the
		// normal toward +u, so slope = h(uv) - h(uv + d). One texel of the
		// 64 × 64 source per step; `DetailNormal` sets the strength.
		const float Texel = 1.0f / 64.0f;
		UMaterialExpressionTextureSampleParameter2D* FineU = SampleDetail(OffsetUV(Texel, 0.0f));
		UMaterialExpressionTextureSampleParameter2D* FineV = SampleDetail(OffsetUV(0.0f, Texel));
		UMaterialExpressionSubtract* SlopeU = AddExpr<UMaterialExpressionSubtract>(Parent);
		SlopeU->A.Expression = Fine;
		SlopeU->A.OutputIndex = kRedOutput;
		SlopeU->B.Expression = FineU;
		SlopeU->B.OutputIndex = kRedOutput;
		UMaterialExpressionSubtract* SlopeV = AddExpr<UMaterialExpressionSubtract>(Parent);
		SlopeV->A.Expression = Fine;
		SlopeV->A.OutputIndex = kRedOutput;
		SlopeV->B.Expression = FineV;
		SlopeV->B.OutputIndex = kRedOutput;
		UMaterialExpressionMultiply* BumpU = AddExpr<UMaterialExpressionMultiply>(Parent);
		BumpU->A.Expression = SlopeU;
		BumpU->B.Expression = DetailNormalParam;
		UMaterialExpressionMultiply* BumpV = AddExpr<UMaterialExpressionMultiply>(Parent);
		BumpV->A.Expression = SlopeV;
		BumpV->B.Expression = DetailNormalParam;
		UMaterialExpressionAppendVector* BumpUV = AddExpr<UMaterialExpressionAppendVector>(Parent);
		BumpUV->A.Expression = BumpU;
		BumpUV->B.Expression = BumpV;
		UMaterialExpressionConstant* One = AddExpr<UMaterialExpressionConstant>(Parent);
		One->R = 1.0f;
		UMaterialExpressionAppendVector* BumpXYZ = AddExpr<UMaterialExpressionAppendVector>(Parent);
		BumpXYZ->A.Expression = BumpUV;
		BumpXYZ->B.Expression = One;
		UMaterialExpressionNormalize* Bump = AddExpr<UMaterialExpressionNormalize>(Parent);
		Bump->VectorInput.Expression = BumpXYZ;
		FinalNormal = Bump;
	}
	else
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    no ground textures and %s is missing — surfaces have no grain at all"),
			kDetailTexture);
	}

	UMaterialExpressionClamp* RoughOut = AddExpr<UMaterialExpressionClamp>(Parent);
	RoughOut->Input.Expression = FinalRoughness;
	RoughOut->MinDefault = 0.05f;
	RoughOut->MaxDefault = 1.0f;

	UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
	EditorOnly->BaseColor.Expression = FinalAlbedo;
	EditorOnly->Roughness.Expression = RoughOut;
	if (FinalNormal)
	{
		EditorOnly->Normal.Expression = FinalNormal;
	}
	// The prop stand-ins go through instanced components; without the flag
	// a cooked build draws them with the default material.
	Parent->bUsedWithInstancedStaticMeshes = true;
	Parent->PostEditChange();

	FAssetRegistryModule::AssetCreated(Parent);
	ParentPackage->MarkPackageDirty();
	TouchedPackages.Add(ParentPackage);
	ParentMaterial = Parent;

	auto MakeInstance = [&](const FString& Key) -> UMaterialInstanceConstant* {
		const FString PackageName = TrackFolder / (TEXT("MI_") + Key);
		UPackage* Package = MakePackage(PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
			return nullptr;
		}
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
			Package, *ObjectNameOf(PackageName), RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(Parent);
		return Instance;
	};
	auto FinishInstance = [&](UMaterialInstanceConstant* Instance, const FString& Key) {
		Instance->PostEditChange();
		FAssetRegistryModule::AssetCreated(Instance);
		UPackage* Package = Instance->GetOutermost();
		Package->MarkPackageDirty();
		TouchedPackages.Add(Package);
		Materials.Add(Key, Instance);
	};
	auto SetScalar = [](UMaterialInstanceConstant* Instance, const TCHAR* Name, float Value) {
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
	};
	auto SetVector = [](
						 UMaterialInstanceConstant* Instance, const TCHAR* Name, FLinearColor Value) {
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
	};
	auto SetTexture = [](UMaterialInstanceConstant* Instance, const TCHAR* Name, UTexture* Value) {
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
	};
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

	if (!BuildDecalMaterial(OutError))
	{
		return false;
	}
	for (const FApexTrackMaterial& Source : Scene.Materials)
	{
		// A road decal is a picture, not a colour on the shared parent.
		if (Source.Family == TEXT("decal"))
		{
			if (UMaterialInterface* Decal = DecalMaterialFor(Source.Key, OutError))
			{
				Materials.Add(Source.Key, Decal);
			}
			else if (!OutError.IsEmpty())
			{
				return false;
			}
			continue;
		}
		UMaterialInstanceConstant* Instance = MakeInstance(Source.Key);
		if (!Instance)
		{
			return false;
		}

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
		// imported the parent has no `Detail*` parameters and these calls
		// would leave overrides matching nothing behind.
		auto SetDetail = [&](float Tiling, float Amount, float Roughness, float Normal) {
			if (bGroundTextures)
			{
				return;
			}
			SetScalar(Instance, TEXT("DetailTiling"), Tiling);
			SetScalar(Instance, TEXT("DetailAmount"), Amount);
			SetScalar(Instance, TEXT("DetailRoughness"), Roughness);
			SetScalar(Instance, TEXT("DetailNormal"), Normal);
		};
		if (Source.Family == TEXT("road") || Source.Family == TEXT("pit_lane"))
		{
			// Tarmac: patchy aggregate and uneven sheen, not one flat slab,
			// with 50 cm grain over the 8 m patches.
			Base *= 0.42f;
			SetScalar(Instance, TEXT("Roughness"), 0.9f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.35f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.0012f);
			SetScalar(Instance, TEXT("RoughnessNoise"), 0.2f);
			SetDetail(2.0f, 0.35f, 0.25f, 0.3f);
		}
		else if (Source.Family == TEXT("curb"))
		{
			// The exporter's solid slab becomes 2 m stripes. Style keys name
			// their pair (`curb_red_white`, `curb_yellow_black`): the base
			// color is the first, so the alternate is white unless the key
			// says black.
			Base *= 0.7f;
			SetScalar(Instance, TEXT("StripePeriod"), 2.0f);
			const FLinearColor Alt = Source.Key.EndsWith(TEXT("black"))
				? FLinearColor(0.04f, 0.04f, 0.04f)
				: FLinearColor(0.62f, 0.60f, 0.57f);
			SetVector(Instance, TEXT("SecondaryColor"), Alt);
			SetScalar(Instance, TEXT("Roughness"), 0.6f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.08f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.004f);
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
			SetScalar(Instance, TEXT("Roughness"), 0.95f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.5f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.0008f);
			SetScalar(Instance, TEXT("RoughnessNoise"), 0.08f);
			SetDetail(1.0f, 0.45f, 0.1f, 0.3f);
		}
		else if (Source.Family == TEXT("structure"))
		{
			// Bridges and retaining walls: weathered concrete, and the painted
			// fascia on a deck. Stained in broad patches, little grain.
			Base *= 0.6f;
			SetScalar(Instance, TEXT("Roughness"), 0.8f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.18f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.002f);
			SetScalar(Instance, TEXT("RoughnessNoise"), 0.1f);
			SetDetail(1.0f, 0.2f, 0.1f, 0.15f);
		}
		else if (Source.Family == TEXT("marking"))
		{
			// Painted lines read as paint, not asphalt — worn paint, so a
			// touch of the same mottling the road gets and grain that thins
			// the coat here and there.
			Base *= 0.85f;
			SetScalar(Instance, TEXT("Roughness"), 0.45f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.08f);
			SetDetail(2.0f, 0.25f, 0.15f, 0.2f);
		}

		// Which baked set this family samples, and how hard.
		const ApexGround::FSurfaceLook Look =
			ApexGround::LookFor(Source.Family, Source.Key);
		if (bGroundTextures && Look.Set[0] != TEXT('\0'))
		{
			const FGroundMapSet& Set = GroundSetFor(Look.Set);
			if (Set.IsComplete())
			{
				SetTexture(Instance, TEXT("AlbedoMap"), Set.Albedo);
				SetTexture(Instance, TEXT("NormalMap"), Set.Normal);
				SetTexture(Instance, TEXT("RoughnessMap"), Set.Roughness);
				SetScalar(Instance, TEXT("TextureAmount"), 1.0f);
				SetScalar(Instance, TEXT("TextureTiling"), 1.0f / Look.TileM);
				SetScalar(Instance, TEXT("NormalStrength"), Look.NormalStrength);
				SetScalar(Instance, TEXT("RoughnessMapAmount"), Look.RoughnessMapAmount);
			}
			else
			{
				UE_LOG(LogApexTrackImport, Warning,
					TEXT("    the %s ground set is missing; %s keeps the parent's asphalt"),
					Look.Set, *Source.Key);
			}
		}
		// The fringe works either way: its ramp is in the vertex colours and
		// its parameters are outside the branch above.
		if (Look.EdgeBlend > 0.0f)
		{
			SetScalar(Instance, TEXT("EdgeBlend"), Look.EdgeBlend);
			SetScalar(Instance, TEXT("EdgeNoise"), kEdgeNoise);
			SetVector(Instance, TEXT("EdgeColor"), FMath::Lerp(Base, kEdgeDustColor, 0.7f));
		}

		Base.A = 1.0f;
		SetVector(Instance, TEXT("BaseColor"), Base);
		FinishInstance(Instance, Source.Key);
	}

	for (const FPropMaterialSpec& Spec : kPropMaterials)
	{
		UMaterialInstanceConstant* Instance = MakeInstance(Spec.Key);
		if (!Instance)
		{
			return false;
		}
		SetVector(Instance, TEXT("BaseColor"), Spec.Color);
		SetScalar(Instance, TEXT("Roughness"), Spec.Roughness);
		SetScalar(Instance, TEXT("NoiseAmount"), Spec.NoiseAmount);
		SetScalar(Instance, TEXT("NoiseScale"), Spec.NoiseScale);
		if (Spec.StripePeriod > 0.0f)
		{
			SetScalar(Instance, TEXT("StripePeriod"), Spec.StripePeriod);
			SetVector(Instance, TEXT("SecondaryColor"), Spec.Secondary);
		}
		if (!Spec.InstanceTint.IsAlmostBlack())
		{
			SetVector(Instance, TEXT("InstanceTint"), Spec.InstanceTint);
		}
		FinishInstance(Instance, Spec.Key);
	}

	if (!BuildEmissiveMaterial(OutError) || !BuildBrandMaterial(OutError))
	{
		return false;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("    generated %d material instance(s)"),
		Materials.Num());
	return true;
}

bool FApexTrackAssetBuilder::BuildEmissiveMaterial(FString& OutError)
{
	// A second, tiny parent: the track material has no emissive input and
	// the start lights are driven at runtime through `EmissiveStrength` on
	// dynamic instances, so they want a graph of their own rather than
	// another branch in the big one.
	const FString ParentPackageName = TrackFolder / TEXT("M_ApexEmissive");
	UPackage* ParentPackage = MakePackage(ParentPackageName);
	if (!ParentPackage)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *ParentPackageName);
		return false;
	}
	UMaterial* Parent = NewObject<UMaterial>(
		ParentPackage, *ObjectNameOf(ParentPackageName), RF_Public | RF_Standalone);

	UMaterialExpressionVectorParameter* ColorParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	ColorParam->ParameterName = TEXT("BaseColor");
	ColorParam->DefaultValue = FLinearColor(0.05f, 0.01f, 0.01f, 1.0f);
	UMaterialExpressionScalarParameter* RoughnessParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessParam->ParameterName = TEXT("Roughness");
	RoughnessParam->DefaultValue = 0.4f;
	UMaterialExpressionVectorParameter* EmissiveColor =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	EmissiveColor->ParameterName = TEXT("EmissiveColor");
	EmissiveColor->DefaultValue = FLinearColor(1.0f, 0.02f, 0.02f, 1.0f);
	UMaterialExpressionScalarParameter* EmissiveStrength =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	EmissiveStrength->ParameterName = TEXT("EmissiveStrength");
	EmissiveStrength->DefaultValue = 0.0f;
	UMaterialExpressionMultiply* Emissive = AddExpr<UMaterialExpressionMultiply>(Parent);
	Emissive->A.Expression = EmissiveColor;
	Emissive->B.Expression = EmissiveStrength;

	UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
	EditorOnly->BaseColor.Expression = ColorParam;
	EditorOnly->Roughness.Expression = RoughnessParam;
	EditorOnly->EmissiveColor.Expression = Emissive;
	// Its instances land on the kit's instanced boards and Nanite screens.
	Parent->bUsedWithInstancedStaticMeshes = true;
	Parent->bUsedWithNanite = true;
	Parent->PostEditChange();
	FAssetRegistryModule::AssetCreated(Parent);
	ParentPackage->MarkPackageDirty();
	TouchedPackages.Add(ParentPackage);
	EmissiveParent = Parent;

	const FString Key = TEXT("start_light");
	const FString PackageName = TrackFolder / (TEXT("MI_") + Key);
	UPackage* Package = MakePackage(PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
		return false;
	}
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
		Package, *ObjectNameOf(PackageName), RF_Public | RF_Standalone);
	Instance->SetParentEditorOnly(Parent);
	Instance->PostEditChange();
	FAssetRegistryModule::AssetCreated(Instance);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);
	Materials.Add(Key, Instance);
	return true;
}

bool FApexTrackAssetBuilder::BuildBrandMaterial(FString& OutError)
{
	// The authored boards carry a default brand baked into their material;
	// a prop's `text` swaps that slot for an instance of this, which is
	// nothing but a texture on a matte surface.
	UTexture* Placeholder = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	if (!Placeholder)
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    the engine's DefaultTexture is missing; boards keep their imported brands"));
		return true;
	}

	const FString ParentPackageName = TrackFolder / TEXT("M_ApexBrand");
	UPackage* ParentPackage = MakePackage(ParentPackageName);
	if (!ParentPackage)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *ParentPackageName);
		return false;
	}
	UMaterial* Parent = NewObject<UMaterial>(
		ParentPackage, *ObjectNameOf(ParentPackageName), RF_Public | RF_Standalone);

	UMaterialExpressionTextureSampleParameter2D* Sample =
		AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
	Sample->ParameterName = TEXT("BrandTexture");
	Sample->Texture = Placeholder;
	Sample->SamplerType = SAMPLERTYPE_Color;
	UMaterialExpressionScalarParameter* RoughnessParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessParam->ParameterName = TEXT("Roughness");
	RoughnessParam->DefaultValue = 0.45f;

	UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
	EditorOnly->BaseColor.Expression = Sample;
	EditorOnly->Roughness.Expression = RoughnessParam;
	// Brands go on instanced boards and on Nanite bridges and garages.
	Parent->bUsedWithInstancedStaticMeshes = true;
	Parent->bUsedWithNanite = true;
	Parent->PostEditChange();
	FAssetRegistryModule::AssetCreated(Parent);
	ParentPackage->MarkPackageDirty();
	TouchedPackages.Add(ParentPackage);
	BrandParent = Parent;
	return true;
}

bool FApexTrackAssetBuilder::BuildDecalMaterial(FString& OutError)
{
	// Paint on the road: the texture's colour at a paint's sheen, its alpha
	// as the mask, so the asphalt shows through wherever nothing is painted
	// (and through the gaps the texture leaves in a worn stroke).
	UTexture* Placeholder = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	if (!Placeholder)
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    the engine's DefaultTexture is missing; road decals are left out"));
		return true;
	}

	const FString ParentPackageName = TrackFolder / TEXT("M_ApexDecal");
	UPackage* ParentPackage = MakePackage(ParentPackageName);
	if (!ParentPackage)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *ParentPackageName);
		return false;
	}
	UMaterial* Parent = NewObject<UMaterial>(
		ParentPackage, *ObjectNameOf(ParentPackageName), RF_Public | RF_Standalone);

	UMaterialExpressionTextureSampleParameter2D* Sample =
		AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
	Sample->ParameterName = TEXT("DecalTexture");
	Sample->Texture = Placeholder;
	Sample->SamplerType = SAMPLERTYPE_Color;
	// Road paint is a matte coat; the tint dims the texture's white to what
	// a sprayed white reflects under the race's sun.
	UMaterialExpressionVectorParameter* TintParam = AddExpr<UMaterialExpressionVectorParameter>(Parent);
	TintParam->ParameterName = TEXT("Tint");
	TintParam->DefaultValue = FLinearColor(0.72f, 0.72f, 0.70f, 1.0f);
	UMaterialExpressionMultiply* Tinted = AddExpr<UMaterialExpressionMultiply>(Parent);
	Tinted->A.Expression = Sample;
	Tinted->B.Expression = TintParam;
	UMaterialExpressionScalarParameter* RoughnessParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessParam->ParameterName = TEXT("Roughness");
	RoughnessParam->DefaultValue = 0.6f;

	UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
	EditorOnly->BaseColor.Expression = Tinted;
	EditorOnly->Roughness.Expression = RoughnessParam;
	// Output 4 of a texture sample is its alpha.
	EditorOnly->OpacityMask.Connect(4, Sample);
	Parent->BlendMode = BLEND_Masked;
	Parent->OpacityMaskClipValue = 0.4f;
	Parent->PostEditChange();
	FAssetRegistryModule::AssetCreated(Parent);
	ParentPackage->MarkPackageDirty();
	TouchedPackages.Add(ParentPackage);
	DecalParent = Parent;
	return true;
}

UMaterialInterface* FApexTrackAssetBuilder::DecalMaterialFor(const FString& Key, FString& OutError)
{
	FString Set;
	FString Name;
	if (!DecalParent || !ApexProps::ParseDecalKey(Key, Set, Name))
	{
		MissingDecals.Add(Key);
		return nullptr;
	}
	const FString TexturePath = ApexProps::DecalTextureObjectPath(PropsRoot, Set, Name);
	FString TexturePackage;
	FString TextureObject;
	TexturePath.Split(TEXT("."), &TexturePackage, &TextureObject, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	UTexture* Texture = FPackageName::DoesPackageExist(TexturePackage)
		? LoadObject<UTexture>(nullptr, *TexturePath)
		: nullptr;
	if (!Texture)
	{
		// Paint with no picture would be a white slab across the road:
		// leave it out and say which import is missing.
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    %s is not imported (ApexPropImport -kind=decal); %s is left out"),
			*TexturePath, *Key);
		MissingDecals.Add(Key);
		return nullptr;
	}
	const FString PackagePath = TrackFolder / (TEXT("MI_") + Key);
	UPackage* Package = MakePackage(PackagePath);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *PackagePath);
		return nullptr;
	}
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
		Package, *ObjectNameOf(PackagePath), RF_Public | RF_Standalone);
	Instance->SetParentEditorOnly(DecalParent);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("DecalTexture")), Texture);
	Instance->PostEditChange();
	FAssetRegistryModule::AssetCreated(Instance);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);
	return Instance;
}

UStaticMesh* FApexTrackAssetBuilder::FindAuthoredMesh(const FString& Kind, const FString& Asset)
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

FApexTrackAssetBuilder::FResolvedProp FApexTrackAssetBuilder::ResolveProp(const FApexTrackProp& Prop)
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

UMaterialInterface* FApexTrackAssetBuilder::TextureMaterialFor(const FString& Key, const FString& TexturePath)
{
	if (const TObjectPtr<UMaterialInterface>* Cached = SlotMaterials.Find(Key))
	{
		return *Cached;
	}
	UMaterialInterface* Result = nullptr;
	FString PackageName;
	FString ObjectName;
	TexturePath.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	UTexture* Texture = BrandParent && FPackageName::DoesPackageExist(PackageName)
		? LoadObject<UTexture>(nullptr, *TexturePath)
		: nullptr;
	if (Texture)
	{
		const FString PackagePath = TrackFolder / (TEXT("MI_") + Key);
		if (UPackage* Package = MakePackage(PackagePath))
		{
			UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
				Package, *ObjectNameOf(PackagePath), RF_Public | RF_Standalone);
			Instance->SetParentEditorOnly(BrandParent);
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BrandTexture")), Texture);
			Instance->PostEditChange();
			FAssetRegistryModule::AssetCreated(Instance);
			Package->MarkPackageDirty();
			TouchedPackages.Add(Package);
			Result = Instance;
		}
	}
	SlotMaterials.Add(Key, Result);
	return Result;
}

UMaterialInterface* FApexTrackAssetBuilder::EmissiveMaterialFor(FName Slot)
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
		if (Slot != FName(Glow.Slot) || !EmissiveParent)
		{
			continue;
		}
		const FString PackagePath = TrackFolder / (TEXT("MI_") + Key);
		if (UPackage* Package = MakePackage(PackagePath))
		{
			UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
				Package, *ObjectNameOf(PackagePath), RF_Public | RF_Standalone);
			Instance->SetParentEditorOnly(EmissiveParent);
			Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), Glow.Color * 0.05f);
			Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveColor")), Glow.Color);
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveStrength")), Glow.Strength);
			Instance->PostEditChange();
			FAssetRegistryModule::AssetCreated(Instance);
			Package->MarkPackageDirty();
			TouchedPackages.Add(Package);
			Result = Instance;
		}
	}
	SlotMaterials.Add(Key, Result);
	return Result;
}

void FApexTrackAssetBuilder::ApplyAuthoredSlots(
	UStaticMeshComponent* Component, const UStaticMesh* Mesh, const FString& Text)
{
	if (!Component || !Mesh)
	{
		return;
	}
	const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		const FName Slot = Slots[i].MaterialSlotName.IsNone() ? Slots[i].ImportedMaterialSlotName
																: Slots[i].MaterialSlotName;
		UMaterialInterface* Override = nullptr;
		if (ApexProps::IsBrandSlot(Slot) && !Text.IsEmpty())
		{
			const FString Key = TextKey(Text);
			Override = TextureMaterialFor(TEXT("brand_") + Key, ApexProps::BrandTextureObjectPath(PropsRoot, Key));
			if (!Override && !UnknownTexts.Contains(Key))
			{
				UnknownTexts.Add(Key);
				UE_LOG(LogApexTrackImport, Warning,
					TEXT("    no brand texture for text \"%s\" (T_brand_%s); boards keep their imported brand"),
					*Text, *Key);
			}
		}
		else if (ApexProps::IsFlagSlot(Slot) && !Text.IsEmpty())
		{
			const FString Key = TextKey(Text);
			Override = TextureMaterialFor(TEXT("flag_") + Key, ApexProps::FlagTextureObjectPath(PropsRoot, Key));
			if (!Override && !UnknownTexts.Contains(Key))
			{
				UnknownTexts.Add(Key);
				UE_LOG(LogApexTrackImport, Warning,
					TEXT("    no flag texture for text \"%s\" (T_flag_%s); the pole keeps its imported flag"),
					*Text, *Key);
			}
		}
		else if (ApexProps::IsMarkerSlot(Slot) && !Text.IsEmpty())
		{
			const FString Key = TextKey(Text);
			Override = TextureMaterialFor(TEXT("marker_") + Key, ApexProps::MarkerTextureObjectPath(PropsRoot, Key));
			if (!Override && !UnknownTexts.Contains(Key))
			{
				UnknownTexts.Add(Key);
				UE_LOG(LogApexTrackImport, Warning,
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

UStaticMesh* FApexTrackAssetBuilder::CreateStaticMesh(const FString& Name,
	FMeshDescription& MeshDescription, const TArray<FString>& MaterialKeys, bool bSimpleCollision,
	FString& OutError)
{
	const FString PackageName = TrackFolder / (TEXT("SM_") + Name);
	UPackage* Package = MakePackage(PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
		return nullptr;
	}

	UStaticMesh* Mesh =
		NewObject<UStaticMesh>(Package, *ObjectNameOf(PackageName), RF_Public | RF_Standalone);
	for (const FString& Key : MaterialKeys)
	{
		// The slot name matches the polygon group's, which is how the build
		// maps sections onto slots.
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials.FindRef(Key), FName(*Key)));
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	// Simple collision would be a box around a 250 m ribbon, which is worse
	// than none. The road needs its actual surface, so it uses the render
	// geometry directly (set below). Props are small enough for the box.
	BuildParams.bBuildSimpleCollision = bSimpleCollision;
	// Not the fast path. It leaves some meshes with NaN bounds — three of
	// Monza's ground patches, with perfectly finite vertices — and a mesh
	// with NaN bounds is culled from every view, so the level comes out with
	// holes that nothing in the export explains. The full build costs a
	// second or so per circuit and computes bounds and tangents properly.
	// `ValidateMeshes` keeps it honest either way.
	BuildParams.bFastBuild = false;
	BuildParams.bMarkPackageDirty = false;
	BuildParams.bCommitMeshDescription = true;
	Mesh->BuildFromMeshDescriptions({&MeshDescription}, BuildParams);

	if (!bSimpleCollision)
	{
		if (UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
		}
	}
	// Nanite is deliberately left off (it is off by default, so there is
	// nothing to set). A whole circuit is around 35k triangles — the entire
	// point of these meshes is that they are cheap — and Nanite would add
	// build time and a memory floor for nothing. Revisit when the ribbons
	// carry real displaced detail.

	Mesh->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mesh);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);
	return Mesh;
}

bool FApexTrackAssetBuilder::BuildMeshes(const FApexTrackScene& Scene, FString& OutError)
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

	// How much of the bands ended up as fringe, for the log. A figure of
	// zero or of everything means the inner-edge test has gone wrong (a
	// centerline in another frame, say), which nothing else would show
	// short of a screenshot.
	int32 BandVertices = 0;
	int32 FringeVertices = 0;
	int32 SkippedDecals = 0;
	for (const FApexTrackMesh& Source : Scene.Meshes)
	{
		if (MissingDecals.Contains(Source.MaterialKey))
		{
			++SkippedDecals;
			continue;
		}
		FMeshDescription MeshDescription;
		const FMeshSlot Slot{Source.MaterialKey, Source.Indices};
		// A run-off band gets the seam fringe ramp in its vertex colours.
		// Only a band: the terrain is a grid whose UVs are world metres, so
		// the "how far across the strip is this" reading would be nonsense.
		TArray<float> EdgeFactors;
		if (ApexGround::IsSurfaceBand(Source.MaterialKey))
		{
			EdgeFactors = ApexGround::EdgeFactors(Source.UVs, Source.Positions, Center, LapM);
			BandVertices += EdgeFactors.Num();
			FringeVertices += Algo::CountIf(EdgeFactors, [](float F) { return F < 1.0f; });
		}
		FillMeshDescription(MeshDescription, Source.Positions, Source.Normals, Source.UVs,
			MakeArrayView(&Slot, 1), EdgeFactors);
		UStaticMesh* Mesh = CreateStaticMesh(Source.Name, MeshDescription, {Source.MaterialKey},
			/*bSimpleCollision*/ false, OutError);
		if (!Mesh)
		{
			return false;
		}
		Meshes.Add(Source.Name, Mesh);
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("    generated %d static mesh(es)"), Meshes.Num());
	if (SkippedDecals > 0)
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    %d road decal mesh(es) left out: their textures are not imported"), SkippedDecals);
	}
	if (BandVertices > 0)
	{
		UE_LOG(LogApexTrackImport, Display,
			TEXT("    seam fringe on %d of %d run-off band vertices (%.0f%%)"), FringeVertices,
			BandVertices, 100.0 * FringeVertices / BandVertices);
	}
	return BuildPropMeshes(Scene, OutError) && ValidateMeshes(OutError);
}

bool FApexTrackAssetBuilder::BuildPropMeshes(const FApexTrackScene& Scene, FString& OutError)
{
	// One stand-in per prop kind, generated rather than authored: nothing in
	// the project is art yet, but a stand that is visibly a stand and a tree
	// that is visibly a tree are what make a screenshot read as a circuit.
	// Saved alongside the track's other meshes so a level is self-contained.
	for (const FPropRecipe& Recipe : kPropRecipes)
	{
		FProcMesh Proc;
		Recipe.Build(Proc);

		TArray<FMeshSlot> Slots;
		TArray<FString> Keys;
		for (const FProcMesh::FSlot& Slot : Proc.Slots)
		{
			Slots.Add({Slot.MaterialKey, Slot.Indices});
			Keys.Add(Slot.MaterialKey);
		}
		FMeshDescription MeshDescription;
		FillMeshDescription(MeshDescription, Proc.Positions, Proc.Normals, Proc.UVs, Slots);
		UStaticMesh* Mesh = CreateStaticMesh(FString(TEXT("Prop_")) + Recipe.Kind, MeshDescription,
			Keys, /*bSimpleCollision*/ true, OutError);
		if (!Mesh)
		{
			return false;
		}
		PropMeshes.Add(Recipe.Kind, Mesh);
	}

	// The gantry spans this track's road, so it is sized per level.
	FApexTrackStartFinish Start;
	if (ResolveStartFinish(Scene, Start))
	{
		struct FGantryPart
		{
			const TCHAR* Name;
			TFunction<void(FProcMesh&)> Build;
		};
		const FGantryPart Parts[] = {
			{TEXT("start_gantry"),
				[&Start](FProcMesh& M) { BuildStartGantryMesh(M, Start.WidthCm); }},
			{TEXT("start_light"), [](FProcMesh& M) { BuildStartLightMesh(M); }},
		};
		for (const FGantryPart& Part : Parts)
		{
			FProcMesh Proc;
			Part.Build(Proc);
			TArray<FMeshSlot> Slots;
			TArray<FString> Keys;
			for (const FProcMesh::FSlot& Slot : Proc.Slots)
			{
				Slots.Add({Slot.MaterialKey, Slot.Indices});
				Keys.Add(Slot.MaterialKey);
			}
			FMeshDescription MeshDescription;
			FillMeshDescription(MeshDescription, Proc.Positions, Proc.Normals, Proc.UVs, Slots);
			UStaticMesh* Mesh = CreateStaticMesh(FString(TEXT("Prop_")) + Part.Name,
				MeshDescription, Keys, /*bSimpleCollision*/ true, OutError);
			if (!Mesh)
			{
				return false;
			}
			PropMeshes.Add(Part.Name, Mesh);
		}
	}
	UE_LOG(LogApexTrackImport, Display, TEXT("    generated %d prop mesh(es)"), PropMeshes.Num());
	return true;
}

void FApexTrackAssetBuilder::SpawnStartLights(const FApexTrackScene& Scene, UWorld* World)
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
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    no start/finish line and no grid — the level has no start lights"));
		return;
	}

	// 12 m past the line, facing the way the cars go; the lights hang on the
	// -X face, toward the grid.
	const FRotator Rotation(0.0f, Start.YawDeg, 0.0f);
	const FVector Location =
		Start.Location + Rotation.RotateVector(FVector(kGantryOffset, 0.0, 0.0));

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = World->PersistentLevel;
	SpawnParams.ObjectFlags = RF_Transactional;
	SpawnParams.Name = MakeUniqueObjectName(
		World->PersistentLevel, AStaticMeshActor::StaticClass(), FName(TEXT("StartLights")));
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
	if (!Actor)
	{
		return;
	}
	Actor->SetActorLabel(TEXT("StartLights"));
	Actor->Tags.Add(FName(TEXT("ApexStartLights")));
	Actor->Tags.Add(kPropTag);
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
		UStaticMeshComponent* Light = NewObject<UStaticMeshComponent>(
			Actor, *FString::Printf(TEXT("Light%d"), i), RF_Transactional);
		Light->CreationMethod = EComponentCreationMethod::Instance;
		Light->SetMobility(EComponentMobility::Movable);
		Light->ComponentTags.Add(FName(TEXT("ApexStartLight")));
		Light->SetupAttachment(Root);
		const float Across = i - (Count - 1) * 0.5f;
		if (AuthoredGantry)
		{
			Light->SetRelativeLocation(FVector(ApexProps::GantryLampX, Across * ApexProps::GantryLampPitchY,
				ApexProps::GantryLampZ));
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
	UE_LOG(LogApexTrackImport, Display,
		TEXT("    start lights at (%.0f, %.0f, %.0f), %.1f m span%s"), Location.X, Location.Y,
		Location.Z, (Start.WidthCm + 200.0f) / 100.0f,
		AuthoredGantry ? TEXT(" (authored gantry)") : TEXT(" (generated gantry)"));
}

void FApexTrackAssetBuilder::SpawnGrandstand(
	UWorld* World, const FApexTrackProp& Prop, const FResolvedProp& Resolved, float YawDeg, int32 Index)
{
	// One prop is a whole stand: bays at 10 m pitch, capped at both ends,
	// wedges around a corner. The scene's `length_m` (30 m when it has
	// none) times the prop's scale is the stand's length; the bays
	// themselves are never scaled, so a legacy 30 m stand at scale 2.6
	// comes out as eight bays rather than one giant one.
	const float LengthM = (Prop.LengthM.IsSet() ? Prop.LengthM.GetValue() : kDefaultStandLengthM)
		* FMath::Max(Prop.Scale, 0.01f);
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
	UStaticMesh* CapMesh =
		Layout.CapAsset.IsEmpty() ? nullptr : FindAuthoredMesh(TEXT("grandstand"), Layout.CapAsset);

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = World->PersistentLevel;
	SpawnParams.ObjectFlags = RF_Transactional;
	SpawnParams.Name = MakeUniqueObjectName(
		World->PersistentLevel, AActor::StaticClass(), FName(*FString::Printf(TEXT("Grandstand_%d"), Index)));
	const FRotator Rotation(0.0f, YawDeg, 0.0f);
	AActor* Actor = World->SpawnActor<AActor>(Prop.Location, Rotation, SpawnParams);
	if (!Actor)
	{
		return;
	}
	Actor->SetActorLabel(FString::Printf(TEXT("Grandstand_%d"), Index));
	Actor->Tags.Add(kPropTag);
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"), RF_Transactional);
	Root->CreationMethod = EComponentCreationMethod::Instance;
	Root->SetMobility(EComponentMobility::Movable);
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocationAndRotation(Prop.Location, Rotation);

	auto AddRow = [&](const TCHAR* Name, UStaticMesh* Mesh, const TArray<FTransform>& Placements) {
		if (!Mesh || Placements.IsEmpty())
		{
			return;
		}
		UHierarchicalInstancedStaticMeshComponent* Row =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, Name, RF_Transactional);
		Row->CreationMethod = EComponentCreationMethod::Instance;
		Row->SetMobility(EComponentMobility::Movable);
		Row->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
		Row->SetupAttachment(Root);
		Row->SetStaticMesh(Mesh);
		Row->SetNumCustomDataFloats(1);
		Actor->AddInstanceComponent(Row);
		Row->RegisterComponent();
		for (const FTransform& Placement : Placements)
		{
			Row->AddInstance(Placement, /*bWorldSpace*/ false);
		}
	};
	AddRow(TEXT("Bays"), BayMesh, Layout.Bays);
	AddRow(TEXT("Caps"), CapMesh, Layout.Caps);
}

bool FApexTrackAssetBuilder::ValidateMeshes(FString& OutError)
{
	// Check what Unreal actually built, not what was handed to it. A mesh
	// with NaN or empty bounds is invisible at runtime — it fails every
	// frustum test — and nothing about the export would tell you why. Better
	// to fail the import than to ship a circuit with holes in it.
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
		if (Bounds.Origin.ContainsNaN() || Bounds.BoxExtent.ContainsNaN()
			|| !FMath::IsFinite(Bounds.SphereRadius))
		{
			OutError = FString::Printf(TEXT("%s built with non-finite bounds"), *Entry.Key);
			return false;
		}
		if (Bounds.SphereRadius <= 0.0f)
		{
			OutError = FString::Printf(TEXT("%s built with empty bounds"), *Entry.Key);
			return false;
		}

		const int32 Triangles = Mesh->GetNumTriangles(0);
		if (Triangles <= 0)
		{
			OutError = FString::Printf(TEXT("%s built with no triangles"), *Entry.Key);
			return false;
		}
	}
	return true;
}

bool FApexTrackAssetBuilder::BuildLevel(const FApexTrackScene& Scene, FString& OutError)
{
	UPackage* Package = MakePackage(LevelPackage);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *LevelPackage);
		return false;
	}

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, /*bInformEngineOfWorld*/ false,
		*ObjectNameOf(LevelPackage), Package, /*bAddToRoot*/ false);
	if (!World)
	{
		OutError = FString::Printf(TEXT("could not create world %s"), *LevelPackage);
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	LevelWorld = World;

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = World->PersistentLevel;
	SpawnParams.ObjectFlags = RF_Transactional;

	// Track geometry: one static actor per baked mesh.
	int32 MeshActors = 0;
	for (const FApexTrackMesh& Source : Scene.Meshes)
	{
		UStaticMesh* Mesh = Meshes.FindRef(Source.Name);
		if (!Mesh)
		{
			continue;
		}
		SpawnParams.Name = MakeUniqueObjectName(
			World->PersistentLevel, AStaticMeshActor::StaticClass(), FName(*Source.Name));
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!Actor)
		{
			continue;
		}
		Actor->SetActorLabel(Source.Name);
		// Positions are baked in world space, so the actor stays at the
		// origin and the mesh carries the layout.
		// Movable, despite never moving. Static geometry wants baked lighting,
		// and nobody is going to run a lighting build on 26 regenerated
		// circuits — the level just loads complaining about hundreds of
		// unbuilt objects. Movable puts it on dynamic lighting instead, which
		// is what a procedurally generated level can actually rely on.
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		// Movable is a lighting decision, not a promise to move, and virtual
		// shadow maps read it as one: a movable primitive is cached as
		// dynamic, so a whole circuit — sixty-odd 384 m ground patches plus
		// the grass and gravel — re-marks its shadow pages every frame and
		// overflows the non-Nanite marking job queue (the "[VSM] Non-Nanite
		// Marking Job Queue overflow" warning on screen). Saying the shape
		// never moves puts it back in the static page cache.
		Actor->GetStaticMeshComponent()->ShadowCacheInvalidationBehavior =
			EShadowCacheInvalidationBehavior::Static;
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		++MeshActors;
	}

	// Props. Each resolves to an authored mesh from the kit, the generated
	// stand-in for its kind, or a placeholder box (ResolveProp). The
	// numerous kinds go into one instanced component per resolved mesh and
	// text so a few thousand of them cost a few draw calls; stands become a
	// row of bays under one actor; the rest are an actor each.
	UStaticMesh* PlaceholderMesh = LoadObject<UStaticMesh>(nullptr, kPlaceholderPropMesh);
	if (!PlaceholderMesh)
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    %s is missing — props without a generated mesh were skipped"),
			kPlaceholderPropMesh);
	}

	auto SettleComponent = [](UStaticMeshComponent* Component) {
		Component->SetMobility(EComponentMobility::Movable);
		// Props are scenery bolted to the ground; same shadow-cache reasoning
		// as the track meshes above.
		Component->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
	};

	TMap<FString, UHierarchicalInstancedStaticMeshComponent*> Instanced;
	auto InstancesFor = [&](const FString& Key, const FString& Label, UStaticMesh* Mesh,
						   const FResolvedProp& Resolved) -> UHierarchicalInstancedStaticMeshComponent* {
		if (UHierarchicalInstancedStaticMeshComponent** Found = Instanced.Find(Key))
		{
			return *Found;
		}
		if (!Mesh)
		{
			return nullptr;
		}
		SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel, AActor::StaticClass(), FName(*Label));
		AActor* Actor =
			World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		Actor->Tags.Add(kPropTag);
		UHierarchicalInstancedStaticMeshComponent* Component =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(
				Actor, TEXT("Instances"), RF_Transactional);
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
		Instanced.Add(Key, Component);
		return Component;
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
	// (the importer already turns the board at the road). Long names shrink
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
		const float Fit = 200.0f / FMath::Max(1, Text.Len()) / 0.62f;   // ~0.62 em advance per glyph
		Label->SetWorldSize(FMath::Clamp(Fit, 14.0f, 44.0f));
		Label->SetTextRenderColor(FColor(20, 20, 24));
		Actor->AddInstanceComponent(Label);
		Label->RegisterComponent();
	};

	auto SpawnMeshActor = [&](int32 Index, const FString& Label, const FVector& Location,
							  const FRotator& Rotation) -> AStaticMeshActor* {
		SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
			AStaticMeshActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), Index)));
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->Tags.Add(kPropTag);
			SettleComponent(Actor->GetStaticMeshComponent());
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
				SpawnGrandstand(World, Prop, Resolved, YawDeg, i);
				++PropActors;
				continue;
			}
			if (Resolved.Kind == TEXT("sky"))
			{
				// Origin at the hull centre, `z` the altitude; it drifts from here.
				SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
					AApexSkyDriftActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), i)));
				if (AApexSkyDriftActor* Actor = World->SpawnActor<AApexSkyDriftActor>(Prop.Location, Rotation, SpawnParams))
				{
					Actor->SetActorLabel(FString::Printf(TEXT("%s_%s"), *Resolved.Kind, *Resolved.Asset));
					Actor->Tags.Add(kPropTag);
					Actor->GetMesh()->SetStaticMesh(Resolved.Mesh);
					Actor->GetMesh()->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
					Actor->SetActorScale3D(FVector(Prop.Scale));
					ApplyAuthoredSlots(Actor->GetMesh(), Resolved.Mesh, Resolved.Text);
					++PropActors;
				}
				continue;
			}
			if (Resolved.Kind == ApexProps::FerrisWheelKind && Resolved.Asset == ApexProps::FerrisWheelAsset)
			{
				SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
					AApexRotorActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), i)));
				if (AApexRotorActor* Actor = World->SpawnActor<AApexRotorActor>(Prop.Location, Rotation, SpawnParams))
				{
					Actor->SetActorLabel(FString::Printf(TEXT("%s_%s"), *Resolved.Kind, *Resolved.Asset));
					Actor->Tags.Add(kPropTag);
					Actor->GetMesh()->SetStaticMesh(Resolved.Mesh);
					Actor->GetMesh()->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
					Actor->GetRotor()->SetStaticMesh(
						FindAuthoredMesh(ApexProps::FerrisWheelKind, ApexProps::FerrisRotorAsset));
					Actor->GetRotor()->SetRelativeLocation(ApexProps::FerrisHubOffsetCm);
					Actor->SetActorScale3D(FVector(Prop.Scale));
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
				if (UHierarchicalInstancedStaticMeshComponent* Component =
						InstancesFor(Key, Label, Resolved.Mesh, Resolved))
				{
					const int32 Index = Component->AddInstance(
						FTransform(Rotation, Prop.Location, Scale), /*bWorldSpace*/ true);
					Component->SetCustomDataValue(Index, 0, InstanceJitter(i));
					++PropInstances;
				}
				continue;
			}

			if (AStaticMeshActor* Actor = SpawnMeshActor(i,
					FString::Printf(TEXT("%s_%s"), *Resolved.Kind, *Resolved.Asset), Prop.Location, Rotation))
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
			if (UHierarchicalInstancedStaticMeshComponent* Component = InstancesFor(
					Prop.Kind, FString::Printf(TEXT("Props_%s"), *Prop.Kind), Resolved.Mesh, Resolved))
			{
				const int32 Index = Component->AddInstance(
					FTransform(Rotation, Prop.Location, FVector(Prop.Scale)), /*bWorldSpace*/ true);
				Component->SetCustomDataValue(Index, 0, InstanceJitter(i));
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
	if (!FallbackKinds.IsEmpty())
	{
		TArray<FString> Kinds = FallbackKinds.Array();
		Kinds.Sort();
		UE_LOG(LogApexTrackImport, Display,
			TEXT("    %d of %d prop(s) use the authored kit; generated stand-ins for: %s%s"), AuthoredProps,
			Scene.Props.Num(), *FString::Join(Kinds, TEXT(", ")),
			AuthoredProps == 0 ? TEXT(" (run ApexPropImport to bring the kit in)") : TEXT(""));
	}
	else if (Scene.Props.Num() > 0)
	{
		UE_LOG(LogApexTrackImport, Display, TEXT("    every prop uses the authored kit"));
	}

	// Starting grid. These match the slots the server computes, so a car
	// spawned here sits where the simulation thinks it is.
	for (const FApexTrackGridSlot& Slot : Scene.Grid)
	{
		SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
			APlayerStart::StaticClass(), FName(*FString::Printf(TEXT("Grid_%d"), Slot.Position)));
		APlayerStart* Start = World->SpawnActor<APlayerStart>(
			Slot.Location, FRotator(0.0f, Slot.YawDeg, 0.0f), SpawnParams);
		if (Start)
		{
			Start->SetActorLabel(FString::Printf(TEXT("GridSlot_%02d"), Slot.Position));
		}
	}

	SpawnStartLights(Scene, World);

	// Deliberately no sun and no sky light.
	//
	// A track level is streamed into the menu world rather than travelled to,
	// so it inherits that world's lighting. Adding its own gives two
	// directional lights in one scene, which Unreal resolves by picking one
	// and warning about the rest. The race director re-tunes that world's
	// sun instead. If these levels ever get opened standalone they will need
	// lighting of their own, but not from here.
	//
	// Fog and post-processing are different: the menu world has neither, and
	// both are what give a five-kilometer circuit any sense of depth.
	SpawnParams.Name = MakeUniqueObjectName(
		World->PersistentLevel, AExponentialHeightFog::StaticClass(), FName(TEXT("TrackFog")));
	if (AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams))
	{
		Fog->SetActorLabel(TEXT("TrackFog"));
		UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
		// Thin: enough that the far end of a straight sits in haze rather
		// than pin-sharp against the sky, not enough to milk out the
		// mid-field (0.0045 did, and with it every color on screen).
		FogComponent->FogDensity = 0.0015f;
		FogComponent->FogHeightFalloff = 0.2f;
		// Keep the first ~30 m crisp; the haze belongs to the distance.
		FogComponent->StartDistance = 3000.0f;
	}

	SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
		APostProcessVolume::StaticClass(), FName(TEXT("TrackPostProcess")));
	if (APostProcessVolume* PostProcess = World->SpawnActor<APostProcessVolume>(
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams))
	{
		PostProcess->SetActorLabel(TEXT("TrackPostProcess"));
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
	}

	World->PostEditChange();
	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);

	UE_LOG(LogApexTrackImport, Display,
		TEXT("    level %s: %d track mesh actor(s), %d prop actor(s), %d prop instance(s) in %d "
			 "component(s), %d grid slot(s)"),
		*LevelPackage, MeshActors, PropActors, PropInstances, Instanced.Num(), Scene.Grid.Num());
	return true;
}

bool FApexTrackAssetBuilder::SaveTouchedPackages(FString& OutError)
{
	for (UPackage* Package : TouchedPackages)
	{
		if (!Package)
		{
			continue;
		}
		const bool bIsMap = Package->GetName() == LevelPackage;
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), bIsMap ? FPackageName::GetMapPackageExtension()
									   : FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		// A map package has to be saved with its world as the asset, or it
		// lands on disk without one and the editor will not open it.
		UObject* Asset = bIsMap ? LevelWorld.Get() : nullptr;
		if (!UPackage::SavePackage(Package, Asset, *FileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("failed to save %s"), *FileName);
			return false;
		}
	}
	UE_LOG(LogApexTrackImport, Display, TEXT("    saved %d package(s)"), TouchedPackages.Num());
	return true;
}
