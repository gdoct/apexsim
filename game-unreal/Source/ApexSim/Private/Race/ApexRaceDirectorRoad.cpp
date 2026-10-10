// The race director's road state: the server's rubber, marbles, dry line,
// water and debris drawn on the road (docs/server/conditions.md, "The road on
// screen"). The rest of the director is in ApexRaceDirector.cpp.

#include "Race/ApexRaceDirector.h"

#include "ApexSim.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Race/ApexRaceCoordinate.h"

namespace ApexRoadStateDraw
{
	/** A transient texture the road material samples: linear, wrapping round the lap. */
	UTexture2D* MakeTexture(int32 Width, int32 Height, EPixelFormat Format, TextureFilter Filter)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, Format);
		if (!Texture)
		{
			return nullptr;
		}
		Texture->SRGB = false;
		Texture->Filter = Filter;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Clamp;
		Texture->NeverStream = true;
		Texture->UpdateResource();
		return Texture;
	}

	/** Copy `Data` into the whole of `Texture` (the render thread frees the copy). */
	template <typename T>
	void Upload(UTexture2D* Texture, const TArray<T>& Data, int32 Width, int32 Height)
	{
		if (!Texture || Data.Num() != Width * Height)
		{
			return;
		}
		const int32 Bytes = Data.Num() * sizeof(T);
		uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Bytes));
		FMemory::Memcpy(Copy, Data.GetData(), Bytes);
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
		Texture->UpdateTextureRegions(0, 1, Region, Width * sizeof(T), sizeof(T), Copy,
			[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
			{
				FMemory::Free(SrcData);
				delete Regions;
			});
	}

	TAutoConsoleVariable<bool> CVarRoadDraw(TEXT("apexsim.road.Draw"), true,
		TEXT("Whether the race director draws the server's road state (rubber, marbles, dry line, water, debris) on the road."),
		ECVF_Default);

	/** A shard of debris: 25 x 12 x 3 cm, the engine cube scaled. */
	const FVector ShardScale(0.25, 0.12, 0.03);
	/** Traced down from this far above and to this far below the server's ground, cm. */
	constexpr double TraceReachCm = 100000.0;
}

void AApexRaceDirector::HandleRoadState(const FApexRoadState& Slice)
{
	// A slice from another source (the backdrop's stream, then a session of
	// our own, or the other way) starts the map again.
	if (Slice.SessionId != RoadSource)
	{
		RoadMap.Reset();
		RoadSource = Slice.SessionId;
	}
	if (!RoadMap.Apply(Slice))
	{
		UE_LOG(LogApexSim, Verbose, TEXT("Road state: a slice of %d byte(s) did not apply"), Slice.Rows.Num());
	}
}

void AApexRaceDirector::HandleDemoSessionChanged(bool bJoined)
{
	// Another backdrop begins: a stream's slices have no session id to tell
	// them from the last stream's. A live session's map is left alone (its
	// join burst may already be in); another session id resets it anyway.
	if (bJoined && RoadSource.IsEmpty())
	{
		RoadMap.Reset();
		RoadSource.Reset();
	}
}

void AApexRaceDirector::UpdateRoadState()
{
	// Textures follow the map's size; until the first slice there are none.
	if (RoadMap.TakeResized())
	{
		RoadStateTexture = nullptr;
		RoadGeometryTexture = nullptr;
		if (RoadMap.IsValid())
		{
			RoadStateTexture = ApexRoadStateDraw::MakeTexture(
				RoadMap.NumCells(), RoadMap.NumBins(), PF_B8G8R8A8, TF_Bilinear);
			// One texel a cell, read whole: the bins are measured from it.
			RoadGeometryTexture = ApexRoadStateDraw::MakeTexture(
				RoadMap.NumCells(), 1, PF_A32B32G32R32F, TF_Nearest);
			UE_LOG(LogApexSim, Log, TEXT("Road state: %d cells x %d bins over %.0f m"),
				RoadMap.NumCells(), RoadMap.NumBins(), RoadMap.GetLapM());
		}
		bRoadStateParamsStale = true;
	}
	if (RoadMap.TakeDirty() && RoadStateTexture && RoadGeometryTexture)
	{
		ApexRoadStateDraw::Upload(RoadStateTexture, RoadMap.GetState(), RoadMap.NumCells(), RoadMap.NumBins());
		ApexRoadStateDraw::Upload(RoadGeometryTexture, RoadMap.GetGeometry(), RoadMap.NumCells(), 1);
	}
	const bool bWanted = IsRoadStateLive() && ApexRoadStateDraw::CVarRoadDraw.GetValueOnGameThread();
	if (bWanted != bRoadStateShown)
	{
		bRoadStateParamsStale = true;
	}
	if (bRoadStateParamsStale)
	{
		PushRoadStateParameters();
	}
	UpdateRoadDebris();
}

void AApexRaceDirector::PushRoadStateParameters()
{
	static const FName StateParam(TEXT("RoadState"));
	static const FName GeometryParam(TEXT("RoadGeometry"));
	static const FName AmountParam(TEXT("RoadStateAmount"));
	static const FName UParam(TEXT("RoadStateU"));
	static const FName HalfSpanParam(TEXT("RoadStateHalfSpanM"));

	const bool bLive = IsRoadStateLive() && RoadStateTexture && RoadGeometryTexture
		&& ApexRoadStateDraw::CVarRoadDraw.GetValueOnGameThread();
	for (auto It = RoadStateMids.CreateIterator(); It; ++It)
	{
		UMaterialInstanceDynamic* Mid = It->Get();
		if (!Mid)
		{
			It.RemoveCurrent();
			continue;
		}
		if (bLive)
		{
			Mid->SetTextureParameterValue(StateParam, RoadStateTexture);
			Mid->SetTextureParameterValue(GeometryParam, RoadGeometryTexture);
			Mid->SetScalarParameterValue(UParam, RoadMap.UPerMetre());
			Mid->SetScalarParameterValue(HalfSpanParam, RoadMap.GetHalfSpanM());
		}
		Mid->SetScalarParameterValue(AmountParam, bLive ? 1.0f : 0.0f);
	}
	bRoadStateParamsStale = false;
	if (RoadStateMids.Num() > 0)
	{
		int32 Rubbered = 0;
		for (const FColor& Texel : RoadMap.GetState())
		{
			Rubbered += Texel.R > 200 ? 1 : 0;
		}
		UE_LOG(LogApexSim, Log, TEXT("Road state to %d road material(s): %s, %d of %d cells known, %d bins rubbered past 0.8"),
			RoadStateMids.Num(), bLive ? TEXT("drawn") : TEXT("off"), RoadMap.NumKnownCells(), RoadMap.NumCells(), Rubbered);
	}
	if (bRoadStateShown != bLive)
	{
		bRoadStateShown = bLive;
		// The lap's one wetness figure steps back from (or back onto) the
		// road's own materials.
		ApplyRoadWetness();
		UE_LOG(LogApexSim, Log, TEXT("Road state %s on %d road material(s)"),
			bLive ? TEXT("drawn") : TEXT("off"), RoadStateMids.Num());
	}
}

void AApexRaceDirector::UpdateRoadDebris()
{
	const bool bChanged = RoadMap.TakeDebrisChanged();
	if (!bChanged && bRoadDebrisLaid)
	{
		return;
	}
	if (!bTrackConditionsApplied)
	{
		// Laid once the track is in the world to trace onto.
		bRoadDebrisLaid = false;
		return;
	}
	const TArray<FVector2D>& Pieces = RoadMap.GetDebris();
	if (!RoadDebris && Pieces.Num() > 0)
	{
		RoadDebris = NewObject<UInstancedStaticMeshComponent>(this, TEXT("RoadDebris"));
		RoadDebris->SetUsingAbsoluteLocation(true);
		RoadDebris->SetUsingAbsoluteRotation(true);
		RoadDebris->SetUsingAbsoluteScale(true);
		RoadDebris->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RoadDebris->SetMobility(EComponentMobility::Movable);
		if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			RoadDebris->SetStaticMesh(Cube);
		}
		if (UMaterialInterface* Shape = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			// Carbon: near black, as a piece of bodywork's underside is.
			UMaterialInstanceDynamic* Carbon = UMaterialInstanceDynamic::Create(Shape, RoadDebris);
			Carbon->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.02f, 0.02f, 0.022f));
			RoadDebris->SetMaterial(0, Carbon);
		}
		RoadDebris->SetupAttachment(GetRootComponent());
		RoadDebris->RegisterComponent();
	}
	if (!RoadDebris)
	{
		bRoadDebrisLaid = true;
		return;
	}

	RoadDebris->ClearInstances();
	UWorld* World = GetWorld();
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ApexRoadDebris), /*bTraceComplex*/ false, this);
	const FCollisionObjectQueryParams Ground(ECC_WorldStatic);
	int32 Laid = 0;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const FVector At = ApexRace::ServerToUnrealPosition(FVector(Pieces[Index].X, Pieces[Index].Y, 0.0));
		FHitResult Hit;
		if (!World || !World->LineTraceSingleByObjectType(Hit,
				At + FVector(0.0, 0.0, ApexRoadStateDraw::TraceReachCm),
				At - FVector(0.0, 0.0, ApexRoadStateDraw::TraceReachCm), Ground, Query))
		{
			continue;
		}
		// Each piece lies its own way: a yaw hashed from where it is.
		const float Yaw = static_cast<float>(GetTypeHash(FIntPoint(FMath::RoundToInt(At.X), FMath::RoundToInt(At.Y))) % 360);
		const FTransform Shard(FRotator(0.0f, Yaw, 0.0f), Hit.ImpactPoint + FVector(0.0, 0.0, 1.5), ApexRoadStateDraw::ShardScale);
		RoadDebris->AddInstance(Shard, /*bWorldSpace*/ true);
		++Laid;
	}
	bRoadDebrisLaid = true;
	UE_LOG(LogApexSim, Verbose, TEXT("Road debris: %d of %d piece(s) laid"), Laid, Pieces.Num());
}

void AApexRaceDirector::ForgetRoadStateOnTrack()
{
	// The materials went back with the track; the textures and the map stay
	// for the next track view of the same session.
	RoadStateMids.Reset();
	bRoadStateParamsStale = true;
	bRoadStateShown = false;
	bRoadDebrisLaid = false;
	if (RoadDebris)
	{
		RoadDebris->ClearInstances();
	}
}
