#include "ApexStartupSplashSubsystem.h"

#include "ApexDemoModeSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "ApexStartupSplash.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "Race/ApexRaceDirector.h"
#include "Widgets/SWindow.h"

namespace
{
	TAutoConsoleVariable<float> CVarSplashMaxSeconds(
		TEXT("apexsim.splash.MaxSeconds"),
		20.0f,
		TEXT("Longest the startup splash waits for the menu's demo race before showing the menu anyway."),
		ECVF_Default);

	/** How long the splash takes to fade off the finished menu. */
	constexpr float FadeSeconds = 0.35f;

	/**
	 * Grace before "no demo is coming" is believed: the connection is only
	 * asked for as the menu map loads, and a refused connect takes a moment to
	 * be reported.
	 */
	constexpr float NoDemoGraceSeconds = 0.5f;
}

void UApexStartupSplashSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// The window mode comes from settings.yml, applied by the settings subsystem.
	Collection.InitializeDependency<UApexSettingsSubsystem>();
	Collection.InitializeDependency<UApexDemoModeSubsystem>();

	if (!ApexStartupSplash::IsHolding())
	{
		return;
	}

	const TCHAR* Skip = nullptr;
	if (UApexDemoModeSubsystem::IsDemoDisabled())
	{
		Skip = TEXT("there is no demo race to wait for");
	}
	else if (GEngine && GEngine->GetGameUserSettings()
		&& GEngine->GetGameUserSettings()->GetFullscreenMode() == EWindowMode::Fullscreen)
	{
		// Exclusive fullscreen takes the display through the swap chain, which a
		// window the compositor is hiding cannot do. The engine switches to it
		// after this, as it attaches the viewport.
		Skip = TEXT("exclusive fullscreen");
	}
	if (Skip)
	{
		UE_LOG(LogApexSim, Log, TEXT("Startup splash: not held (%s)"), Skip);
		ApexStartupSplash::Release();
		return;
	}

	ApexStartupSplash::Claim();

	// The engine may have made more than one top-level window by now; the one
	// the viewport lives in is the one that has to stay hidden.
	ViewportCreatedHandle = UGameViewportClient::OnViewportCreated().AddWeakLambda(this, [this]()
	{
		// The viewport client is not told its window until later; the engine's
		// own pointer is set by now.
		const UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
		const TSharedPtr<SWindow> Window = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr;
		void* const Handle = Window && Window->GetNativeWindow() ? Window->GetNativeWindow()->GetOSWindowHandle() : nullptr;
		if (!ApexStartupSplash::ConfirmGameWindow(Handle))
		{
			// The game window would show over the splash regardless.
			UE_LOG(LogApexSim, Warning, TEXT("Startup splash: the game window could not be hidden; not holding"));
			ApexStartupSplash::Release();
			StopWatching();
		}
	});

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UApexStartupSplashSubsystem::Tick));
	UE_LOG(LogApexSim, Log, TEXT("Startup splash: held until the demo race is on screen"));
}

void UApexStartupSplashSubsystem::Deinitialize()
{
	StopWatching();
	ApexStartupSplash::Release();
	Super::Deinitialize();
}

bool UApexStartupSplashSubsystem::Tick(float DeltaSeconds)
{
	if (!ApexStartupSplash::IsHolding())
	{
		TickerHandle.Reset();
		StopWatching();
		return false;
	}
	// The first frame after a long load reports all of it as one delta.
	HeldFor += FMath::Min(DeltaSeconds, 0.25f);

	const UGameInstance* GameInstance = GetGameInstance();
	const AApexRaceDirector* Director = GameInstance ? AApexRaceDirector::Find(GameInstance->GetWorld()) : nullptr;
	const UApexDemoModeSubsystem* Demo = GameInstance ? GameInstance->GetSubsystem<UApexDemoModeSubsystem>() : nullptr;

	if (Director && Director->IsDemoReady())
	{
		Reveal(TEXT("the demo race is on screen"));
	}
	else if (HeldFor > NoDemoGraceSeconds && (!Demo || !Demo->IsDemoExpected()))
	{
		Reveal(TEXT("no demo race is coming"));
	}
	else if (HeldFor > CVarSplashMaxSeconds.GetValueOnGameThread())
	{
		Reveal(TEXT("the demo race took too long"));
	}
	else
	{
		return true;
	}
	// Returning false removes this ticker; StopWatching must not remove it too.
	TickerHandle.Reset();
	return false;
}

void UApexStartupSplashSubsystem::Reveal(const TCHAR* Why)
{
	UE_LOG(LogApexSim, Log, TEXT("Startup splash: showing the menu after %.1f s because %s"), HeldFor, Why);
	// The boot module runs the fade and closes the splash when it is done.
	ApexStartupSplash::Reveal(FadeSeconds);
	StopWatching();
}

void UApexStartupSplashSubsystem::StopWatching()
{
	if (ViewportCreatedHandle.IsValid())
	{
		UGameViewportClient::OnViewportCreated().Remove(ViewportCreatedHandle);
		ViewportCreatedHandle.Reset();
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}
