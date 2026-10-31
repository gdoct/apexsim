#include "Race/ApexGrassMarks.h"

#include "ApexSim.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Race/ApexRaceCarActor.h"
#include "Race/ApexRaceDirector.h"
#include "Track/ApexTrackSceneBuilder.h"

namespace ApexGrassMarks
{
	bool Advance(FTrail& Trail, const FVector& Point, const FVector& Normal, bool bOnGrass, bool bEarth, FMark& Out)
	{
		if (!bOnGrass)
		{
			Trail.bOpen = false;
			return false;
		}
		const double Run = FVector::Dist(Trail.Last, Point);
		if (!Trail.bOpen || Run > BreakCm)
		{
			Trail.Last = Point;
			Trail.bOpen = true;
			return false;
		}
		if (Run < StepCm)
		{
			return false;
		}
		Out.From = Trail.Last;
		Out.To = Point;
		Out.Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		Out.bEarth = bEarth;
		Trail.Last = Point;
		return true;
	}

	FTransform MarkTransform(const FMark& Mark, float Width)
	{
		const FVector Up = Mark.Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		FVector Along = Mark.To - Mark.From;
		// Laid along the ground: the run less its part along the normal.
		Along -= Up * FVector::DotProduct(Along, Up);
		const double Length = Along.Size();
		const FVector X = Length > UE_KINDA_SMALL_NUMBER ? Along / Length : FVector::ForwardVector;
		const FRotator Rotation = FRotationMatrix::MakeFromXZ(X, Up).Rotator();
		const FVector Centre = (Mark.From + Mark.To) * 0.5 + Up * LiftCm;
		// The plane is a metre square; a tenth more length closes the seams.
		const FVector Scale((Length + 10.0) / 100.0, Width / 100.0, 1.0);
		return FTransform(Rotation, Centre, Scale);
	}

	float SlipDeg(const FVector& Velocity, const FVector& Forward)
	{
		const FVector2D Going(Velocity.X, Velocity.Y);
		const FVector2D Facing(Forward.X, Forward.Y);
		if (Going.Size() < MinSpeedMps * 100.0f || Facing.IsNearlyZero())
		{
			return 0.0f;
		}
		const double Cos = FVector2D::DotProduct(Going.GetSafeNormal(), Facing.GetSafeNormal());
		return static_cast<float>(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cos, -1.0, 1.0))));
	}

	bool TearsEarth(float SlipAngleDeg, float DecelMps2)
	{
		return SlipAngleDeg > EarthSlipDeg || DecelMps2 > EarthDecelMps2;
	}

	FName GrassTag()
	{
		static const FName Tag(TEXT("ApexGrass"));
		return Tag;
	}

	TArray<FColor> MarkPixels(int32 Size)
	{
		const int32 Across = FMath::Max(Size, 4);
		const int32 Along = Across * 4;
		const auto Hash = [](uint32 X) {
			X ^= X >> 16;
			X *= 0x7feb352dU;
			X ^= X >> 15;
			X *= 0x846ca68bU;
			X ^= X >> 16;
			return static_cast<float>(X & 0xffffU) / 65535.0f;
		};
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Along * Across);
		for (int32 Y = 0; Y < Across; ++Y)
		{
			// 0 down the middle of the print, 1 at its edges.
			const float Edge = FMath::Abs((Y + 0.5f) / Across - 0.5f) * 2.0f;
			for (int32 X = 0; X < Along; ++X)
			{
				// Ragged edges: the blades the tyre missed, wandering along it.
				const float Rag = 0.72f + 0.2f * Hash(static_cast<uint32>(X / 3) * 7919U);
				// Tread bars across the print, and the odd tuft standing back up.
				const bool bBar = FMath::Frac(static_cast<float>(X) / (Along / 9.0f)) < 0.22f;
				const float Speck = Hash(static_cast<uint32>(X * 131 + Y * 977));
				float Alpha = Edge < Rag ? 0.7f + 0.3f * Speck : 0.0f;
				Alpha *= bBar ? 0.8f : 1.0f;
				Alpha *= Speck < 0.05f ? 0.3f : 1.0f;
				// Ragged ends: a trail's first and last marks have no square
				// edge, and the overlap with the next mark hides the rest.
				const float U = (X + 0.5f) / Along;
				const float End = 0.04f + 0.05f * Hash(static_cast<uint32>(Y / 2) * 40503U);
				Alpha *= FMath::SmoothStep(0.0f, End, U) * FMath::SmoothStep(0.0f, End, 1.0f - U);
				// The blades laid along the print: streaks of light and dark
				// running its whole length, so the tint is not one flat band.
				const float Blade = 0.7f + 0.3f * Hash(static_cast<uint32>(Y) * 2654435761U);
				const uint8 Shade = static_cast<uint8>(FMath::Clamp(Blade, 0.0f, 1.0f) * 255.0f);
				Pixels[Y * Along + X] = FColor(Shade, Shade, Shade, static_cast<uint8>(FMath::Clamp(Alpha, 0.0f, 1.0f) * 255.0f));
			}
		}
		return Pixels;
	}
}

namespace
{
	/** A pass a twentieth of a second: a mark is 70 cm, under a pass's run from 14 m/s. */
	constexpr float SampleSeconds = 1.0f / 20.0f;
	/** The trace under a wheel: from above the patch to under it, cm. */
	constexpr float TraceUpCm = 60.0f;
	constexpr float TraceDownCm = 150.0f;
	/** How far the grass may lie from the tyre's contact patch and still be under it, cm. */
	constexpr float ContactToleranceCm = 20.0f;
	/** Flattened grass: a darker green than the turf beside it, the blades laid over in their own shadow. */
	const FLinearColor GrassTint(0.02f, 0.032f, 0.01f);
	/** Torn turf: damp earth. */
	const FLinearColor EarthTint(0.056f, 0.039f, 0.024f);
	constexpr int32 TextureAcross = 32;

	FAutoConsoleCommandWithWorldAndArgs GrassMarksLookCommand(
		TEXT("apexsim.road.GrassMarksLook"),
		TEXT("Park the shot camera behind and over the newest tyre mark in the grass: [back m] [up m] (default 7 and 3)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AApexGrassMarksActor* Marks = nullptr;
			for (TActorIterator<AApexGrassMarksActor> It(World); It && !Marks; ++It)
			{
				Marks = *It;
			}
			ApexGrassMarks::FMark Mark;
			AApexRaceDirector* Director = AApexRaceDirector::Find(World);
			if (!Marks || !Director || !Marks->GetNewestMark(Mark))
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.road.GrassMarksLook: no mark in the grass yet"));
				return;
			}
			const double Back = (Args.Num() > 0 ? FCString::Atod(*Args[0]) : 7.0) * 100.0;
			const double Up = (Args.Num() > 1 ? FCString::Atod(*Args[1]) : 3.0) * 100.0;
			const FVector Along = (Mark.To - Mark.From).GetSafeNormal2D();
			const FVector Eye = Mark.To - Along * Back + FVector(0.0, 0.0, Up);
			Director->SetShotCameraPose(Eye, (Mark.To - Eye).Rotation());
			UE_LOG(LogApexSim, Log, TEXT("Grass marks: looking at the newest of %d at %s"), Marks->NumMarks(), *Mark.To.ToCompactString());
		}));
}

AApexGrassMarksActor::AApexGrassMarksActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AApexGrassMarksActor::EnsureComponents()
{
	if (bComponentsTried)
	{
		return;
	}
	bComponentsTried = true;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ApexTrackMaterials::ObjectPath(ApexTrackMaterials::DecalName));
	if (!Plane || !Parent)
	{
		UE_LOG(LogApexSim, Warning, TEXT("Grass marks: no %s (bake it with -run=ApexMaterialBake); none are drawn"),
			ApexTrackMaterials::DecalName);
		return;
	}

	const TArray<FColor> Pixels = ApexGrassMarks::MarkPixels(TextureAcross);
	const int32 Along = TextureAcross * 4;
	MarkTexture = UTexture2D::CreateTransient(Along, TextureAcross, PF_B8G8R8A8);
	if (MarkTexture)
	{
		MarkTexture->SRGB = true;
		MarkTexture->Filter = TF_Bilinear;
		MarkTexture->AddressX = TA_Wrap;
		MarkTexture->AddressY = TA_Clamp;
		MarkTexture->NeverStream = true;
		void* Data = MarkTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
		MarkTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
		MarkTexture->UpdateResource();
	}

	const auto MakeLayer = [this, Plane, Parent](const TCHAR* Name, const FLinearColor& Tint) {
		UInstancedStaticMeshComponent* Layer = NewObject<UInstancedStaticMeshComponent>(this, Name);
		Layer->SetUsingAbsoluteLocation(true);
		Layer->SetUsingAbsoluteRotation(true);
		Layer->SetUsingAbsoluteScale(true);
		Layer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Layer->SetMobility(EComponentMobility::Movable);
		// Flat on the ground: a shadow would be a dark seam under every mark.
		Layer->SetCastShadow(false);
		Layer->SetStaticMesh(Plane);
		UMaterialInstanceDynamic* Look = UMaterialInstanceDynamic::Create(Parent, Layer);
		if (MarkTexture)
		{
			Look->SetTextureParameterValue(TEXT("DecalTexture"), MarkTexture);
		}
		Look->SetVectorParameterValue(TEXT("Tint"), Tint);
		Look->SetScalarParameterValue(TEXT("Roughness"), 0.95f);
		Layer->SetMaterial(0, Look);
		Layer->SetupAttachment(GetRootComponent());
		Layer->RegisterComponent();
		return Layer;
	};
	Grass = MakeLayer(TEXT("GrassMarks"), GrassTint);
	Earth = MakeLayer(TEXT("EarthMarks"), EarthTint);
}

void AApexGrassMarksActor::Sample(const TMap<int32, TObjectPtr<AApexRaceCarActor>>& Cars, float DeltaSeconds)
{
	SinceSample += DeltaSeconds;
	if (SinceSample < SampleSeconds)
	{
		return;
	}
	const float Interval = SinceSample;
	SinceSample = 0.0f;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FCollisionQueryParams Query(SCENE_QUERY_STAT(ApexGrassMarks), /*bTraceComplex*/ false, this);
	const FCollisionObjectQueryParams Ground(ECC_WorldStatic);
	for (const TPair<int32, TObjectPtr<AApexRaceCarActor>>& Pair : Cars)
	{
		const AApexRaceCarActor* Car = Pair.Value.Get();
		if (!Car || Car->IsHidden())
		{
			continue;
		}
		const float Speed = Car->GetSpeedMps();
		const float Before = PrevSpeed.FindRef(Pair.Key);
		PrevSpeed.Add(Pair.Key, Speed);
		const bool bMoving = Speed >= ApexGrassMarks::MinSpeedMps;
		const bool bEarth = ApexGrassMarks::TearsEarth(
			ApexGrassMarks::SlipDeg(Car->GetVelocity(), Car->GetActorForwardVector()), (Before - Speed) / FMath::Max(Interval, 1e-3f));
		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			ApexGrassMarks::FTrail& Trail = Trails.FindOrAdd(Pair.Key * 4 + Wheel);
			FVector Patch = FVector::ZeroVector;
			FHitResult Hit;
			const bool bOnGrass = bMoving && Car->GetContactPatch(Wheel, Patch)
				&& World->LineTraceSingleByObjectType(Hit, Patch + FVector(0.0, 0.0, TraceUpCm),
					Patch - FVector(0.0, 0.0, TraceDownCm), Ground, Query)
				&& Hit.GetComponent() && Hit.GetComponent()->ComponentHasTag(ApexGrassMarks::GrassTag())
				// The terrain runs on under the road: a hit well under the
				// tyre is the ground beneath a surface the trace passed.
				&& FMath::Abs(Hit.ImpactPoint.Z - Patch.Z) < ContactToleranceCm
				// The carved face beside a road is no verge to lie in.
				&& Hit.ImpactNormal.Z >= ApexGrassMarks::MinGroundUp;
			ApexGrassMarks::FMark Mark;
			if (ApexGrassMarks::Advance(Trail, bOnGrass ? FVector(Hit.ImpactPoint) : Patch, Hit.ImpactNormal, bOnGrass, bEarth, Mark))
			{
				Lay(Mark);
			}
		}
	}
}

void AApexGrassMarksActor::Lay(const ApexGrassMarks::FMark& Mark)
{
	EnsureComponents();
	UInstancedStaticMeshComponent* Layer = Mark.bEarth ? Earth.Get() : Grass.Get();
	if (!Layer)
	{
		return;
	}
	if (NumMarks() >= ApexGrassMarks::MaxMarks)
	{
		// The session's oldest marks go, a tenth at a time, from the busier layer.
		UInstancedStaticMeshComponent* Busier = Grass && Earth && Earth->GetInstanceCount() > Grass->GetInstanceCount() ? Earth.Get() : Grass.Get();
		TArray<int32> Oldest;
		for (int32 Index = 0; Index < FMath::Min(Busier->GetInstanceCount(), ApexGrassMarks::MaxMarks / 10); ++Index)
		{
			Oldest.Add(Index);
		}
		Busier->RemoveInstances(Oldest);
	}
	Layer->AddInstance(ApexGrassMarks::MarkTransform(Mark), /*bWorldSpace*/ true);
	Newest = Mark;
	bHasNewest = true;
	if (NumMarks() / 100 > LoggedHundreds)
	{
		LoggedHundreds = NumMarks() / 100;
		UE_LOG(LogApexSim, Log, TEXT("Grass marks: %d in the grass (%d torn earth)"), NumMarks(), Earth ? Earth->GetInstanceCount() : 0);
	}
}

bool AApexGrassMarksActor::GetNewestMark(ApexGrassMarks::FMark& Out) const
{
	Out = Newest;
	return bHasNewest;
}

void AApexGrassMarksActor::Clear()
{
	Trails.Reset();
	PrevSpeed.Reset();
	SinceSample = 0.0f;
	bHasNewest = false;
	LoggedHundreds = 0;
	if (Grass)
	{
		Grass->ClearInstances();
	}
	if (Earth)
	{
		Earth->ClearInstances();
	}
}

int32 AApexGrassMarksActor::NumMarks() const
{
	return (Grass ? Grass->GetInstanceCount() : 0) + (Earth ? Earth->GetInstanceCount() : 0);
}
