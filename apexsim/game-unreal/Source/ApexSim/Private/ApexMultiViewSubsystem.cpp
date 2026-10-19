#include "ApexMultiViewSubsystem.h"

#include "ApexGameViewportClient.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SWindow.h"

namespace
{
	FAutoConsoleCommandWithWorldAndArgs ScreensCommand(
		TEXT("apexsim.view.Screens"),
		TEXT("1 or 3: run on one monitor or as a triple (Settings > Graphics > Screens). No argument prints the current layout."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
			UApexMultiViewSubsystem* MultiView = GameInstance ? GameInstance->GetSubsystem<UApexMultiViewSubsystem>() : nullptr;
			if (!Settings || !MultiView)
			{
				return;
			}
			if (Args.Num() > 0)
			{
				Settings->SetScreens(FCString::Atoi(*Args[0]));
			}
			const FIntRect Span = MultiView->GetSpan();
			UE_LOG(LogApexSim, Display, TEXT("Screens: %s, window %d,%d %dx%d, centre fov %.1f"),
				MultiView->IsTriple() ? TEXT("triple") : TEXT("single"),
				Span.Min.X, Span.Min.Y, Span.Width(), Span.Height(), MultiView->CentreFovDeg());
		}));
}

void UApexMultiViewSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// The setting lives on the slot, and the slot is read by the settings
	// subsystem; this one has to be able to ask it from its first Apply.
	Collection.InitializeDependency<UApexSettingsSubsystem>();

	int32 Screens = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexScreens="), Screens) && (Screens == 1 || Screens == 3))
	{
		ForcedScreens = Screens;
		UE_LOG(LogApexSim, Log, TEXT("-ApexScreens=%d: %s for this run"), Screens,
			Screens == 3 ? TEXT("triple screen") : TEXT("single screen"));
	}

	// Nothing can be done yet — the window is created after the game
	// instance, the world after that — so the work is a ticker that keeps
	// trying until it can, and then keeps the window where it belongs.
	bPendingApply = true;
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UApexMultiViewSubsystem::Tick));
}

void UApexMultiViewSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Super::Deinitialize();
}

// --- What is asked for --------------------------------------------------------

int32 UApexMultiViewSubsystem::RequestedScreens() const
{
	if (ForcedScreens != 0)
	{
		return ForcedScreens;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
	return Values && Values->Screens == 3 ? 3 : 1;
}

ApexMultiView::FTripleGeometry UApexMultiViewSubsystem::Geometry() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	ApexMultiView::FTripleGeometry Geometry = Settings ? Settings->GetTripleGeometry() : ApexMultiView::FTripleGeometry();

	// The vertical field comes from the pixels: a third of the window's
	// width over its height is one panel.
	const UGameViewportClient* Viewport = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (Viewport && Viewport->Viewport)
	{
		const FIntPoint Size = Viewport->Viewport->GetSizeXY();
		if (Size.X > 0 && Size.Y > 0)
		{
			Geometry.AspectRatio = (static_cast<float>(Size.X) / 3.0f) / static_cast<float>(Size.Y);
		}
	}
	return Geometry;
}

float UApexMultiViewSubsystem::CentreFovDeg() const
{
	return ApexMultiView::CentreFovDeg(Geometry());
}

ApexMultiView::FSideView UApexMultiViewSubsystem::SideView(bool bLeft, float PrimaryFovDeg) const
{
	return ApexMultiView::SideView(Geometry(), PrimaryFovDeg, bLeft);
}

int32 UApexMultiViewSubsystem::SideOf(const ULocalPlayer* Player) const
{
	if (!Player)
	{
		return 0;
	}
	if (Viewers.Num() > 0 && Viewers[0] == Player) { return -1; }
	if (Viewers.Num() > 1 && Viewers[1] == Player) { return 1; }
	return 0;
}

void UApexMultiViewSubsystem::SetCentreWidget(UUserWidget* Widget)
{
	CentreWidget = Widget;
	PlaceCentreWidget();
}

// --- Applying ------------------------------------------------------------------

void UApexMultiViewSubsystem::Apply()
{
	// Done on the ticker rather than here: the settings subsystem calls this
	// from inside its own initialisation, before there is a window.
	bPendingApply = true;
	bSpanLogged = false;
}

bool UApexMultiViewSubsystem::Tick(float DeltaSeconds)
{
	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* Viewport = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!Viewport || !Viewport->GetWindow().IsValid())
	{
		return true;
	}

	if (bPendingApply)
	{
		const bool bWantTriple = RequestedScreens() == 3;
		if (bWantTriple)
		{
			MeasureSpan();
		}

		// The viewers need a world with a game mode to spawn their
		// controllers through; until then the window can still be spanned.
		const UWorld* World = GameInstance->GetWorld();
		const bool bWorldReady = World && World->GetAuthGameMode() && World->HasBegunPlay();

		if (bWantTriple != bTriple)
		{
			bTriple = bWantTriple;
			UE_LOG(LogApexSim, Log, TEXT("Screens: %s"), bTriple ? TEXT("triple") : TEXT("single"));
			if (!bTriple)
			{
				RemoveViewers();
				SetViewportLayout(false);
				UnspanWindow();
			}
		}

		if (bTriple)
		{
			SpanWindow();
			SetViewportLayout(true);
			if (bWorldReady)
			{
				EnsureViewers();
			}
		}
		PlaceCentreWidget();

		// Keep coming back until the viewers exist, so a triple asked for
		// before the map is up still gets them.
		bPendingApply = bTriple && Viewers.Num() < 2;
	}
	else if (bTriple)
	{
		// The engine puts a borderless window back onto one monitor on any
		// resolution change (FSceneViewport::ResizeFrame); this puts it back
		// over the row. Cheap: two rectangle compares a frame.
		SpanWindow();
	}
	return true;
}

void UApexMultiViewSubsystem::MeasureSpan()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UGameViewportClient* Viewport = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	const TSharedPtr<SWindow> Window = Viewport ? Viewport->GetWindow() : nullptr;
	if (!Window.IsValid())
	{
		return;
	}

	// Fresh, not cached: a monitor may have been plugged in since the last look.
	FDisplayMetrics Metrics;
	FSlateApplication::Get().GetDisplayMetrics(Metrics);
	TArray<ApexMultiView::FMonitorRect> Monitors;
	for (const FMonitorInfo& Monitor : Metrics.MonitorInfo)
	{
		ApexMultiView::FMonitorRect Rect;
		Rect.Rect = FIntRect(Monitor.DisplayRect.Left, Monitor.DisplayRect.Top, Monitor.DisplayRect.Right, Monitor.DisplayRect.Bottom);
		Rect.bPrimary = Monitor.bIsPrimary;
		Monitors.Add(Rect);
	}

	FIntRect Row;
	if (ApexMultiView::FindTripleRow(Monitors, Row))
	{
		Span = Row;
		bSpanIsWindow = false;
		if (!bSpanLogged)
		{
			UE_LOG(LogApexSim, Log, TEXT("Triple screen: three monitors in a row at %d,%d, %dx%d"),
				Span.Min.X, Span.Min.Y, Span.Width(), Span.Height());
		}
	}
	else
	{
		const FVector2D Position = Window->GetPositionInScreen();
		const FVector2D Size = Window->GetClientSizeInScreen();
		Span = FIntRect(FMath::RoundToInt(Position.X), FMath::RoundToInt(Position.Y),
			FMath::RoundToInt(Position.X + Size.X), FMath::RoundToInt(Position.Y + Size.Y));
		bSpanIsWindow = true;
		if (!bSpanLogged)
		{
			UE_LOG(LogApexSim, Warning,
				TEXT("Triple screen: no three monitors of one size in a row (%d monitor(s)); the three views share the window"),
				Monitors.Num());
		}
	}
	bSpanLogged = true;
}

void UApexMultiViewSubsystem::SpanWindow()
{
	if (bSpanIsWindow)
	{
		return;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	const UGameViewportClient* Viewport = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	const TSharedPtr<SWindow> Window = Viewport ? Viewport->GetWindow() : nullptr;
	if (!Window.IsValid() || Window->IsWindowMinimized())
	{
		return;
	}

	// Borderless first: a title bar would be part of the span, and an
	// exclusive fullscreen window belongs to one monitor by definition.
	if (Window->GetWindowMode() != EWindowMode::WindowedFullscreen)
	{
		Window->SetWindowMode(EWindowMode::WindowedFullscreen);
	}

	const FVector2D WantPosition(Span.Min.X, Span.Min.Y);
	const FVector2D WantSize(Span.Width(), Span.Height());
	const FVector2D Position = Window->GetPositionInScreen();
	const FVector2D Size = Window->GetClientSizeInScreen();
	if (!Position.Equals(WantPosition, 1.0f) || !Size.Equals(WantSize, 1.0f))
	{
		Window->ReshapeWindow(WantPosition, WantSize);
	}
}

void UApexMultiViewSubsystem::UnspanWindow()
{
	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* Viewport = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	FSceneViewport* SceneViewport = Viewport ? Viewport->GetGameViewport() : nullptr;
	const UGameUserSettings* User = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (!SceneViewport || !User)
	{
		return;
	}
	// The window was moved behind the engine's back, so its own idea of the
	// resolution is unchanged and ApplySettings would see nothing to do.
	const FIntPoint Resolution = User->GetScreenResolution();
	SceneViewport->ResizeFrame(Resolution.X, Resolution.Y, User->GetFullscreenMode());
}

// --- The viewers ------------------------------------------------------------------

void UApexMultiViewSubsystem::EnsureViewers()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}
	Viewers.RemoveAll([](const ULocalPlayer* Player) { return Player == nullptr; });
	while (Viewers.Num() < 2)
	{
		FString Error;
		// The game mode spawns the controller for a new local player; the flag
		// tells it this one is a viewer, not a second driver.
		TGuardValue<bool> Guard(bSpawningSideViewer, true);
		ULocalPlayer* Player = GameInstance->CreateLocalPlayer(/*ControllerId*/ -1, Error, /*bSpawnPlayerController*/ true);
		if (!Player)
		{
			UE_LOG(LogApexSim, Error, TEXT("Triple screen: could not add a side viewer: %s"), *Error);
			return;
		}
		// The horizontal field is the one the geometry speaks in.
		Player->AspectRatioAxisConstraint = AspectRatio_MaintainXFOV;
		Viewers.Add(Player);
	}
	if (ULocalPlayer* Primary = GameInstance->GetFirstGamePlayer())
	{
		Primary->AspectRatioAxisConstraint = AspectRatio_MaintainXFOV;
	}
	UE_LOG(LogApexSim, Log, TEXT("Triple screen: side viewers ready (%d local players)"), GameInstance->GetNumLocalPlayers());
}

void UApexMultiViewSubsystem::RemoveViewers()
{
	UGameInstance* GameInstance = GetGameInstance();
	for (ULocalPlayer* Player : Viewers)
	{
		if (Player && GameInstance)
		{
			GameInstance->RemoveLocalPlayer(Player);
		}
	}
	Viewers.Reset();
}

void UApexMultiViewSubsystem::SetViewportLayout(bool bEnable)
{
	const UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* Viewport = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (UApexGameViewportClient* Ours = Cast<UApexGameViewportClient>(Viewport))
	{
		Ours->SetTripleLayout(bEnable);
	}
	else if (bEnable && Viewport)
	{
		UE_LOG(LogApexSim, Error,
			TEXT("Triple screen: the viewport client is %s, not UApexGameViewportClient (DefaultEngine.ini "
				 "GameViewportClientClassName) - the three views will not be laid out side by side"),
			*Viewport->GetClass()->GetName());
	}
}

void UApexMultiViewSubsystem::PlaceCentreWidget()
{
	UUserWidget* Widget = CentreWidget.Get();
	if (!Widget || !Widget->IsInViewport())
	{
		return;
	}
	// Stretched anchors with zero offsets: the shell fills the middle third
	// of the window as a triple, the whole of it otherwise. UMG's DPI scale
	// goes by the window's height, so the shell is drawn at the size it
	// would be on one of the monitors alone.
	Widget->SetAnchorsInViewport(bTriple ? FAnchors(1.0f / 3.0f, 0.0f, 2.0f / 3.0f, 1.0f) : FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
}
