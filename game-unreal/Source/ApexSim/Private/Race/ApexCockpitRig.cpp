#include "Race/ApexCockpitRig.h"

#include "ApexSim.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Race/ApexRaceCarActor.h"
#include "Race/ApexRaceCoordinate.h"
#include "UI/ApexCockpitDashWidget.h"
#include "UI/ApexMirrorWidget.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/**
	 * Emissive multiplier on the cockpit screens. The race is exposed for
	 * a 50 klux sun (EV100 10–15), where a surface emitting 1.0 is black;
	 * the start lights read at 4000. A knob rather than a constant so it can
	 * be tuned against a screenshot without a rebuild.
	 */
	TAutoConsoleVariable<float> CVarScreenNits(
		TEXT("apexsim.cockpit.ScreenNits"),
		4500.0f,
		TEXT("Brightness of the cockpit display and mirror faces, as an emissive multiplier"),
		ECVF_Default);

	/** Render-target scale per mirror-quality bucket. */
	constexpr float QualityScale[] = { 0.5f, 0.75f, 1.0f };

	/** Width of the wheel display in centimetres; the height follows the widget's aspect. */
	constexpr float DashWidthCm = 21.0f;

	void ConfigureCapture(USceneCaptureComponent2D& Capture)
	{
		Capture.CaptureSource = SCS_FinalColorLDR;
		// Refreshed by the rig, round-robin, not by the engine every frame.
		Capture.bCaptureEveryFrame = false;
		Capture.bCaptureOnMovement = false;
		// Keeps exposure and anti-aliasing history between refreshes, so a
		// mirror does not re-adapt from black every time its turn comes.
		Capture.bAlwaysPersistRenderingState = true;
		Capture.ProjectionType = ECameraProjectionMode::Perspective;
		Capture.MaxViewDistanceOverride = 30000.0f;
		Capture.bUseRayTracingIfEnabled = false;

		// A mirror is glanced at, not studied: global illumination and the
		// screen-space effects are most of a capture's cost and none of its
		// legibility.
		Capture.ShowFlags.SetLumenGlobalIllumination(false);
		Capture.ShowFlags.SetLumenReflections(false);
		Capture.ShowFlags.SetScreenSpaceReflections(false);
		Capture.ShowFlags.SetAmbientOcclusion(false);
		Capture.ShowFlags.SetMotionBlur(false);
		Capture.ShowFlags.SetBloom(false);
		Capture.ShowFlags.SetDepthOfField(false);
		Capture.ShowFlags.SetVolumetricFog(false);
		Capture.ShowFlags.SetVignette(false);
		Capture.ShowFlags.SetLensFlares(false);
	}

	void SetupPart(UStaticMeshComponent& Part, UStaticMesh* Mesh)
	{
		Part.SetStaticMesh(Mesh);
		Part.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part.SetGenerateOverlapEvents(false);
		Part.SetCanEverAffectNavigation(false);
	}

	void SetupFace(UWidgetComponent& Face, TSubclassOf<UUserWidget> WidgetClass, const FVector2D& DrawSize)
	{
		Face.SetWidgetSpace(EWidgetSpace::World);
		Face.SetWidgetClass(WidgetClass);
		Face.SetDrawSize(DrawSize);
		Face.SetDrawAtDesiredSize(false);
		Face.SetBlendMode(EWidgetBlendMode::Opaque);
		Face.SetTwoSided(false);
		Face.SetTickWhenOffscreen(true);
		Face.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Face.SetGenerateOverlapEvents(false);
		Face.SetCastShadow(false);
		Face.SetReceivesDecals(false);
	}

	void SetFaceBrightness(UWidgetComponent* Face, float Nits)
	{
		if (!Face)
		{
			return;
		}
		const FLinearColor Tint(Nits, Nits, Nits, 1.0f);
		Face->SetTintColorAndOpacity(Tint);
		// The component pushes its tint into the pass-through material; set
		// it on the instance as well in case the proxy was built before the
		// tint changed.
		if (UMaterialInstanceDynamic* Mid = Face->GetMaterialInstance())
		{
			Mid->SetVectorParameterValue(TEXT("TintColorAndOpacity"), Tint);
		}
	}
}

AApexCockpitRig::AApexCockpitRig()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the car it rides, so the parts are placed from this frame's
	// smoothed transform rather than last frame's.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// Engine primitives, referenced from the class default object so the
	// cooker carries them into a packaged build.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = CubeFinder.Object;
	CylinderMesh = CylinderFinder.Object;
	ShapeMaterial = MaterialFinder.Object;

	BuildWheel();

	// Resolutions are the glass sizes' aspects at roughly 20 px/cm.
	Centre = BuildMirror(TEXT("Centre"), FIntPoint(512, 160), 50.0f, /*bWithFace*/ true);
	Left = BuildMirror(TEXT("Left"), FIntPoint(384, 240), 38.0f, /*bWithFace*/ true);
	Right = BuildMirror(TEXT("Right"), FIntPoint(384, 240), 38.0f, /*bWithFace*/ true);
	Virtual = BuildMirror(TEXT("Virtual"), FIntPoint(640, 200), 70.0f, /*bWithFace*/ false);
}

void AApexCockpitRig::BuildWheel()
{
	WheelPivot = CreateDefaultSubobject<USceneComponent>(TEXT("WheelPivot"));
	WheelPivot->SetupAttachment(Root);

	auto MakePart = [this](const TCHAR* Name, UStaticMesh* Mesh) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(WheelPivot);
		SetupPart(*Part, Mesh);
		return Part;
	};

	WheelHub = MakePart(TEXT("WheelHub"), CubeMesh);
	GripLeft = MakePart(TEXT("GripLeft"), CylinderMesh);
	GripRight = MakePart(TEXT("GripRight"), CylinderMesh);
	SpokeTop = MakePart(TEXT("SpokeTop"), CubeMesh);
	SpokeBottom = MakePart(TEXT("SpokeBottom"), CubeMesh);

	Dash = CreateDefaultSubobject<UWidgetComponent>(TEXT("Dash"));
	Dash->SetupAttachment(WheelPivot);
	SetupFace(*Dash, UApexCockpitDashWidget::StaticClass(),
		FVector2D(UApexCockpitDashWidget::DrawWidth, UApexCockpitDashWidget::DrawHeight));
}

AApexCockpitRig::FMirror AApexCockpitRig::BuildMirror(
	const TCHAR* Name, const FIntPoint& BaseResolution, float FovDeg, bool bWithFace)
{
	FMirror Mirror;
	Mirror.BaseResolution = BaseResolution;
	Mirror.FovDeg = FovDeg;

	Mirror.Mount = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("Mirror%sMount"), Name));
	Mirror.Mount->SetupAttachment(Root);

	if (bWithFace)
	{
		Mirror.Bezel = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Mirror%sBezel"), Name));
		Mirror.Bezel->SetupAttachment(Mirror.Mount);
		SetupPart(*Mirror.Bezel, CubeMesh);
		Mirror.Bezel->SetCastShadow(false);

		Mirror.Face = CreateDefaultSubobject<UWidgetComponent>(*FString::Printf(TEXT("Mirror%sFace"), Name));
		Mirror.Face->SetupAttachment(Mirror.Mount);
		SetupFace(*Mirror.Face, UApexMirrorWidget::StaticClass(), FVector2D(BaseResolution));
	}

	// The camera is not on the mount: the mount faces the driver, the
	// camera faces the road behind.
	Mirror.Capture = CreateDefaultSubobject<USceneCaptureComponent2D>(*FString::Printf(TEXT("Mirror%sCapture"), Name));
	Mirror.Capture->SetupAttachment(Root);
	ConfigureCapture(*Mirror.Capture);
	Mirror.Capture->FOVAngle = FovDeg;

	return Mirror;
}

void AApexCockpitRig::BeginPlay()
{
	Super::BeginPlay();

	// Dark carbon for the hub and spokes, a shade lighter for the grips, so
	// the rim reads as a shape against the hub. Instances are made here
	// rather than in the constructor, which is too early for dynamic
	// materials to be safe.
	auto Paint = [this](UStaticMeshComponent* Part, float Grey)
	{
		if (!Part || !ShapeMaterial)
		{
			return;
		}
		if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(ShapeMaterial, this))
		{
			Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor(Grey, Grey, Grey * 1.05f, 1.0f));
			Part->SetMaterial(0, Mid);
			Owned.Add(Mid);
		}
	};
	Paint(WheelHub, 0.015f);
	Paint(SpokeTop, 0.02f);
	Paint(SpokeBottom, 0.02f);
	Paint(GripLeft, 0.045f);
	Paint(GripRight, 0.045f);
	for (FMirror* Mirror : { &Centre, &Left, &Right })
	{
		Paint(Mirror->Bezel, 0.012f);
	}

	ApplyScreenBrightness();
	ApplyVisibility();
}

void AApexCockpitRig::AttachToCar(AApexRaceCarActor* InCar)
{
	Car = InCar;
	if (!Car)
	{
		DetachFromCar();
		return;
	}

	Layout = Car->GetCockpitLayout();
	AttachToActor(Car, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	// The parts ride the attachment, but the display reads the car's
	// telemetry: same frame's values, after the car has taken them.
	AddTickPrerequisiteActor(Car);
	SetActorRelativeTransform(FTransform::Identity);

	// Nothing on the rig belongs in a mirror — the centre one looks back
	// through the cabin, straight at the wheel. The virtual mirror is the
	// clear rear view a HUD strip wants, so it hides the bodywork too.
	for (FMirror* Mirror : { &Centre, &Left, &Right, &Virtual })
	{
		Mirror->Capture->ClearHiddenComponents();
		Mirror->Capture->HideActorComponents(this);
	}
	if (UStaticMeshComponent* Body = Car->GetMeshComponent())
	{
		Virtual.Capture->HideComponent(Body);
	}

	ObservedMaxRpm = 8000.0f;
	bPlaced = false;
	PlaceParts();
	ApplyVisibility();

	UE_LOG(LogApexSim, Log, TEXT("Cockpit rig in car %d: %s, eye (%.0f, %.0f, %.0f) cm, %s"),
		Car->GetCarIndex(), Layout.bOpenWheel ? TEXT("open cockpit") : TEXT("closed cabin"),
		Layout.Eye.X, Layout.Eye.Y, Layout.Eye.Z,
		Layout.bCentreMirror ? TEXT("three mirrors") : TEXT("two mirrors"));
}

void AApexCockpitRig::DetachFromCar()
{
	Car = nullptr;
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	ApplyVisibility();
}

void AApexCockpitRig::SetEyeLocal(const FVector& InEyeLocal)
{
	if (EyeLocal.Equals(InEyeLocal, 0.01f))
	{
		return;
	}
	EyeLocal = InEyeLocal;
	if (Car)
	{
		PlaceParts();
	}
}

void AApexCockpitRig::SetFeatures(const FApexCockpitFeatures& InFeatures)
{
	Features = InFeatures;
	ApplyVisibility();
}

UTextureRenderTarget2D* AApexCockpitRig::GetVirtualMirrorTexture() const
{
	return Virtual.bWanted ? Virtual.Target.Get() : nullptr;
}

// --- Layout -----------------------------------------------------------------

void AApexCockpitRig::PlaceParts()
{
	bPlaced = true;

	// The wheel: a flat-bottomed racing rim — hub in the middle, a grip
	// either side, spokes joining them — in the pivot's frame, where +X is
	// the nose and the driver sits at -X.
	const float Half = Layout.WheelHalfWidthCm;
	WheelPivot->SetRelativeLocation(Layout.Wheel);
	WheelPivot->SetRelativeRotation(FRotator(Layout.WheelRakeDeg, 0.0f, WheelRollDeg));

	// Basic shapes are 100 cm; scale is size in metres.
	WheelHub->SetRelativeLocation(FVector::ZeroVector);
	WheelHub->SetRelativeScale3D(FVector(0.035f, (2.0f * Half * 0.74f) / 100.0f, 0.15f));

	GripLeft->SetRelativeLocation(FVector(0.0f, -Half, 0.0f));
	GripLeft->SetRelativeRotation(FRotator(0.0f, 0.0f, -8.0f));
	GripLeft->SetRelativeScale3D(FVector(0.055f, 0.055f, 0.18f));
	GripRight->SetRelativeLocation(FVector(0.0f, Half, 0.0f));
	GripRight->SetRelativeRotation(FRotator(0.0f, 0.0f, 8.0f));
	GripRight->SetRelativeScale3D(FVector(0.055f, 0.055f, 0.18f));

	SpokeTop->SetRelativeLocation(FVector(0.0f, 0.0f, 5.5f));
	SpokeTop->SetRelativeScale3D(FVector(0.025f, (2.0f * Half) / 100.0f, 0.035f));
	SpokeBottom->SetRelativeLocation(FVector(0.0f, 0.0f, -5.5f));
	SpokeBottom->SetRelativeScale3D(FVector(0.025f, (2.0f * Half) / 100.0f, 0.035f));

	// The display sits proud of the hub on the driver's side and faces them.
	Dash->SetRelativeLocation(FVector(-2.2f, 0.0f, 0.5f));
	Dash->SetRelativeRotation(FRotator(0.0f, 180.0f, 0.0f));
	Dash->SetRelativeScale3D(FVector(DashWidthCm / UApexCockpitDashWidget::DrawWidth));

	// Mirrors: each camera looks back, the side ones toed out a little so the
	// glass shows the flank of the car and the lane beside it. Pitch is a
	// touch down: the interesting things behind are on the road.
	PlaceMirror(Centre, Layout.MirrorCentre, Layout.CentreMirrorSizeCm, FRotator(-2.0f, 180.0f, 0.0f));
	PlaceMirror(Left, Layout.MirrorLeft, Layout.SideMirrorSizeCm, FRotator(-2.0f, 190.0f, 0.0f));
	PlaceMirror(Right, Layout.MirrorRight, Layout.SideMirrorSizeCm, FRotator(-2.0f, 170.0f, 0.0f));

	// The virtual mirror looks back from just above the roofline, behind
	// the driver, which is the view a rear-facing camera on the car gives.
	Virtual.Capture->SetRelativeLocationAndRotation(
		FVector(Layout.Eye.X - 30.0f, 0.0f, Layout.RoofZ + 25.0f), FRotator(-4.0f, 180.0f, 0.0f));
}

void AApexCockpitRig::PlaceMirror(FMirror& Mirror, const FVector& Local, const FVector2D& SizeCm, const FRotator& CaptureRotation)
{
	Mirror.Mount->SetRelativeLocation(Local);

	// The glass faces the eye: a widget face draws toward its +X.
	const FVector ToEye = EyeLocal - Local;
	Mirror.Mount->SetRelativeRotation(
		ToEye.IsNearlyZero() ? FRotator(0.0f, 180.0f, 0.0f) : FRotationMatrix::MakeFromX(ToEye).Rotator());

	if (Mirror.Face)
	{
		// Width in centimetres over width in pixels; the resolution was chosen
		// to match the glass's aspect, so the height follows.
		const float Scale = SizeCm.X / static_cast<float>(Mirror.BaseResolution.X);
		Mirror.Face->SetDrawSize(FVector2D(Mirror.BaseResolution));
		Mirror.Face->SetRelativeScale3D(FVector(Scale));
		Mirror.Face->SetRelativeLocation(FVector(0.8f, 0.0f, 0.0f));

		if (UApexMirrorWidget* Widget = Cast<UApexMirrorWidget>(Mirror.Face->GetUserWidgetObject()))
		{
			Widget->SetFaceSize(FVector2D(Mirror.BaseResolution));
		}
	}
	if (Mirror.Bezel)
	{
		const float Scale = SizeCm.X / static_cast<float>(Mirror.BaseResolution.X);
		const float HeightCm = Mirror.BaseResolution.Y * Scale;
		Mirror.Bezel->SetRelativeLocation(FVector(-0.4f, 0.0f, 0.0f));
		Mirror.Bezel->SetRelativeScale3D(FVector(0.012f, (SizeCm.X + 2.4f) / 100.0f, (HeightCm + 2.4f) / 100.0f));
	}

	Mirror.Capture->SetRelativeLocationAndRotation(Local, CaptureRotation);
	Mirror.Capture->FOVAngle = Mirror.FovDeg;
}

void AApexCockpitRig::ApplyVisibility()
{
	const bool bInside = Features.bCockpitActive && Car != nullptr;

	WheelPivot->SetVisibility(bInside && Features.bWheel, /*bPropagateToChildren*/ true);

	Centre.bWanted = bInside && Features.bMirrors && Layout.bCentreMirror;
	Left.bWanted = bInside && Features.bMirrors;
	Right.bWanted = bInside && Features.bMirrors;
	Virtual.bWanted = Features.bVirtualMirror && Car != nullptr;

	for (FMirror* Mirror : { &Centre, &Left, &Right })
	{
		Mirror->Mount->SetVisibility(Mirror->bWanted, /*bPropagateToChildren*/ true);
	}
}

void AApexCockpitRig::EnsureRenderTargets()
{
	const int32 Quality = FMath::Clamp(Features.MirrorQuality, 0, 2);
	if (BuiltQuality == Quality)
	{
		return;
	}
	BuiltQuality = Quality;

	// Everything the old targets were referenced from is reassigned below,
	// so dropping them from the keep-alive list lets them go.
	Owned.RemoveAll([](const TObjectPtr<UObject>& Object) { return Object && Object->IsA<UTextureRenderTarget2D>(); });

	for (FMirror* Mirror : { &Centre, &Left, &Right, &Virtual })
	{
		const FIntPoint Size(
			FMath::Max(64, FMath::RoundToInt(Mirror->BaseResolution.X * QualityScale[Quality])),
			FMath::Max(32, FMath::RoundToInt(Mirror->BaseResolution.Y * QualityScale[Quality])));

		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(this);
		// The capture writes tone-mapped, display-ready colour; an sRGB
		// target keeps Slate from gamma-correcting it a second time.
		Target->RenderTargetFormat = RTF_RGBA8_SRGB;
		Target->ClearColor = FLinearColor::Black;
		Target->bAutoGenerateMips = false;
		Target->InitAutoFormat(Size.X, Size.Y);
		Target->UpdateResourceImmediate(true);
		Owned.Add(Target);

		Mirror->Target = Target;
		Mirror->Capture->TextureTarget = Target;
		// Shadows are the other big cost; the low bucket does without.
		Mirror->Capture->ShowFlags.SetDynamicShadows(Quality > 0);

		if (Mirror->Face)
		{
			if (UApexMirrorWidget* Widget = Cast<UApexMirrorWidget>(Mirror->Face->GetUserWidgetObject()))
			{
				Widget->SetTexture(Target);
			}
		}
	}
}

void AApexCockpitRig::ApplyScreenBrightness()
{
	const float Nits = FMath::Max(1.0f, CVarScreenNits.GetValueOnGameThread());
	if (FMath::IsNearlyEqual(Nits, AppliedNits))
	{
		return;
	}
	AppliedNits = Nits;

	SetFaceBrightness(Dash, Nits);
	for (FMirror* Mirror : { &Centre, &Left, &Right })
	{
		SetFaceBrightness(Mirror->Face, Nits);
	}
}

// --- Per frame --------------------------------------------------------------

void AApexCockpitRig::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!Car)
	{
		return;
	}
	if (!bPlaced)
	{
		PlaceParts();
	}

	ApplyScreenBrightness();

	if (WheelPivot->IsVisible())
	{
		UpdateWheel(DeltaSeconds);
		PushDash();
	}

	EnsureRenderTargets();
	RunCaptures();
}

void AApexCockpitRig::UpdateWheel(float DeltaSeconds)
{
	// Steering telemetry steps at the broadcast rate; the rim eases onto it
	// the way the car eases onto its position, so the wheel never snaps.
	const float Target = ApexCockpit::WheelRollDeg(Car->GetSteering(), Layout.WheelLockDeg);
	WheelRollDeg = FMath::FInterpTo(WheelRollDeg, Target, DeltaSeconds, 14.0f);
	WheelPivot->SetRelativeRotation(FRotator(Layout.WheelRakeDeg, 0.0f, WheelRollDeg));
}

void AApexCockpitRig::PushDash()
{
	UApexCockpitDashWidget* Widget = Cast<UApexCockpitDashWidget>(Dash->GetUserWidgetObject());
	if (!Widget)
	{
		return;
	}

	// The wire never states a redline; the bar's full scale is the highest
	// the engine has been seen to rev, which it reaches by the first shift.
	ObservedMaxRpm = FMath::Max(ObservedMaxRpm, Car->GetEngineRpm());
	Widget->SetValues(Car->GetGear(), ApexRace::MpsToKph(Car->GetSpeedMps()), Features.bMetric,
		Car->GetEngineRpm() / ObservedMaxRpm, Car->GetCurrentLap(), Car->GetCurrentLapTimeMs(), CountdownMs);
}

void AApexCockpitRig::RunCaptures()
{
	TArray<FMirror*, TInlineAllocator<4>> Active;
	for (FMirror* Mirror : { &Centre, &Left, &Right, &Virtual })
	{
		if (Mirror->bWanted && Mirror->Target)
		{
			Active.Add(Mirror);
		}
	}
	if (Active.Num() == 0)
	{
		return;
	}

	// Low: one mirror every other frame. Medium: one a frame. High: all of
	// them, every frame — which with three mirrors is three extra scene
	// renders and is priced accordingly in the settings text.
	const int32 Quality = FMath::Clamp(Features.MirrorQuality, 0, 2);
	if (Quality == 0)
	{
		if (++CaptureSkip < 2)
		{
			return;
		}
		CaptureSkip = 0;
	}

	const int32 Count = Quality >= 2 ? Active.Num() : 1;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FMirror* Mirror = Active[CaptureCursor % Active.Num()];
		++CaptureCursor;
		Mirror->Capture->CaptureScene();
	}
	CaptureCursor %= Active.Num();
}
