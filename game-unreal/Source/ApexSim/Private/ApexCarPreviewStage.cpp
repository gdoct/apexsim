#include "ApexCarPreviewStage.h"

#include "ApexSim.h"
#include "Components/RectLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Kismet/KismetRenderingLibrary.h"

AApexCarPreviewStage::AApexCarPreviewStage()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Turntable = CreateDefaultSubobject<USceneComponent>(TEXT("Turntable"));
	Turntable->SetupAttachment(Root);

	CarMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CarMesh"));
	CarMesh->SetupAttachment(Turntable);
	CarMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CarMesh->SetCastShadow(true);
	// The turntable car belongs to the car-select screen only. Without this it
	// also renders into the main view, where it shows up as a fifth car
	// spinning in mid-air next to the ones the server is driving.
	CarMesh->bVisibleInSceneCaptureOnly = true;

	Capture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("Capture"));
	Capture->SetupAttachment(Root);
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->bCaptureEveryFrame = true;
	Capture->bCaptureOnMovement = false;
	Capture->FOVAngle = CaptureFieldOfView;
	// A scene capture has no stable history for the temporal passes to
	// converge on, so with the car turning every frame TAA/TSR and motion
	// blur smear it into streaks. Plain spatial AA is what a turntable wants.
	Capture->ShowFlags.SetTemporalAA(false);
	Capture->ShowFlags.SetMotionBlur(false);

	KeyLight = CreateDefaultSubobject<URectLightComponent>(TEXT("KeyLight"));
	KeyLight->SetupAttachment(Root);
	KeyLight->SetRelativeLocation(FVector(200.0f, -250.0f, 260.0f));
	KeyLight->SetRelativeRotation(FRotator(-35.0f, 140.0f, 0.0f));
	KeyLight->SetIntensity(60000.0f);
	KeyLight->SourceWidth = 200.0f;
	KeyLight->SourceHeight = 200.0f;

	FillLight = CreateDefaultSubobject<URectLightComponent>(TEXT("FillLight"));
	FillLight->SetupAttachment(Root);
	FillLight->SetRelativeLocation(FVector(-200.0f, 250.0f, 180.0f));
	FillLight->SetRelativeRotation(FRotator(-20.0f, -40.0f, 0.0f));
	FillLight->SetIntensity(20000.0f);
	FillLight->SourceWidth = 300.0f;
	FillLight->SourceHeight = 200.0f;
}

void AApexCarPreviewStage::BeginPlay()
{
	Super::BeginPlay();

	// Transparent clear colour so the preview composites over whatever the
	// car-select screen puts behind it.
	PreviewRenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(
		this, PreviewWidth, PreviewHeight, RTF_RGBA8, FLinearColor::Transparent, /*bAutoGenerateMipMaps=*/false);

	if (PreviewRenderTarget)
	{
		Capture->TextureTarget = PreviewRenderTarget;
		Capture->ShowFlags.SetAtmosphere(false);
		Capture->ShowFlags.SetFog(false);
	}
	else
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("%s could not create its preview render target; the car preview will be blank"), *GetName());
	}

	Capture->FOVAngle = CaptureFieldOfView;
	FrameCurrentMesh();
}

AApexCarPreviewStage* AApexCarPreviewStage::Find(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (!World)
	{
		return nullptr;
	}

	for (TActorIterator<AApexCarPreviewStage> It(const_cast<UWorld*>(World)); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AApexCarPreviewStage::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bSpin && CarMesh->GetStaticMesh())
	{
		Turntable->AddLocalRotation(FRotator(0.0f, TurntableDegreesPerSecond * DeltaSeconds, 0.0f));
	}
}

void AApexCarPreviewStage::SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow)
{
	// Synchronous load: this fires from a click, the meshes are small, and a
	// blank preview for a frame or two reads as a bug.
	UStaticMesh* Loaded = MeshToShow.IsNull() ? nullptr : MeshToShow.LoadSynchronous();

	if (CarMesh->GetStaticMesh() == Loaded)
	{
		return;
	}

	CarMesh->SetStaticMesh(Loaded);
	Turntable->SetRelativeRotation(FRotator::ZeroRotator);
	FrameCurrentMesh();

	UE_LOG(LogApexSim, Verbose, TEXT("Preview mesh set to '%s' (requested '%s')"),
		Loaded ? *Loaded->GetName() : TEXT("none"),
		MeshToShow.IsNull() ? TEXT("null") : *MeshToShow.ToString());
}

void AApexCarPreviewStage::ResetTurntable()
{
	Turntable->SetRelativeRotation(FRotator::ZeroRotator);
}

void AApexCarPreviewStage::SetPreviewTransform(FVector Offset, FRotator Rotation, float Scale)
{
	PreviewOffset = Offset;
	PreviewRotation = Rotation;
	PreviewScale = FMath::IsNearlyZero(Scale) ? 1.0f : Scale;
	FrameCurrentMesh();
}

void AApexCarPreviewStage::FrameCurrentMesh()
{
	UStaticMesh* Mesh = CarMesh->GetStaticMesh();
	if (!Mesh)
	{
		Capture->SetRelativeLocation(FVector(-500.0f, 0.0f, 150.0f));
		Capture->SetRelativeRotation(FRotator::ZeroRotator);
		return;
	}

	CarMesh->SetRelativeScale3D(FVector(PreviewScale));
	CarMesh->SetRelativeRotation(PreviewRotation);

	// Recentre on the turntable so the car spins about itself rather than
	// orbiting its own pivot, which is rarely at the model's centre.
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const FVector ScaledOrigin = Bounds.Origin * PreviewScale;
	CarMesh->SetRelativeLocation(PreviewOffset - FVector(ScaledOrigin.X, ScaledOrigin.Y, 0.0f));

	// A three-quarter view: back, slightly to the side, slightly above.
	const FVector CameraDirection = FVector(-1.0f, -0.55f, 0.42f).GetSafeNormal();

	// Pull the camera back just far enough for the car to fit both axes of the
	// target, whatever the turntable's yaw. Cars are long and flat, so a
	// bounding-sphere fit leaves most of a landscape frame empty; the box
	// extents are what actually show. Horizontally the worst case over a full
	// turn is the XY diagonal; vertically it is the height plus however much
	// of that diagonal the camera's pitch tips into view. FOVAngle is the
	// horizontal field of view, the vertical one follows from the aspect.
	const FVector Extent = Bounds.BoxExtent * PreviewScale;
	const float HalfWidth = FMath::Max(FMath::Sqrt(Extent.X * Extent.X + Extent.Y * Extent.Y), 1.0f);
	const float Pitch = FMath::Asin(FMath::Clamp(CameraDirection.Z, -1.0f, 1.0f));
	const float HalfHeight = FMath::Max(Extent.Z * FMath::Cos(Pitch) + HalfWidth * FMath::Sin(Pitch), 1.0f);

	const float TanHalfHorizontal = FMath::Tan(FMath::DegreesToRadians(CaptureFieldOfView * 0.5f));
	const float Aspect = PreviewHeight > 0 ? static_cast<float>(PreviewWidth) / static_cast<float>(PreviewHeight) : 1.0f;
	const float TanHalfVertical = TanHalfHorizontal / FMath::Max(Aspect, KINDA_SMALL_NUMBER);
	const float Distance = FMath::Max(
		HalfWidth / FMath::Max(TanHalfHorizontal, KINDA_SMALL_NUMBER),
		HalfHeight / FMath::Max(TanHalfVertical, KINDA_SMALL_NUMBER)) * FramingMargin;

	const FVector CameraLocation = CameraDirection * Distance + FVector(0.0f, 0.0f, Extent.Z * 0.25f);

	Capture->SetRelativeLocation(CameraLocation);
	Capture->SetRelativeRotation((-CameraLocation).Rotation());
	Capture->FOVAngle = CaptureFieldOfView;
}
