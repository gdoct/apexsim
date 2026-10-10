#include "ApexTestCommon.h"
#include "ApexSim.h"
#include "ApexMenuFlowSubsystem.h"
#include "UI/ApexSettingsWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Tests/AutomationCommon.h"
#include "UI/ApexPadWalk.h"
#include "UI/ApexRootWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Every menu screen and overlay walked by pad alone (ApexPadWalk): focus
 * lands on a control, the D-pad and shoulders reach every control, and Back
 * leaves where it should. Needs the running game, so the menu shell is on a
 * viewport:
 *
 *   UnrealEditor-Cmd.exe ApexSim.uproject -game -ApexNoDemo -windowed -ResX=1920 -ResY=1080
 *       -ExecCmds="Automation RunTests ApexSim.UI.PadWalk; Quit" -unattended -nosplash -log
 *
 * In the headless editor run (no game viewport) it passes with a note.
 */
namespace ApexPadWalkTest
{
	struct FPadWalkStop
	{
		const TCHAR* Name;
		/** A screen to show from the main menu, or none for an overlay. */
		TOptional<EApexScreen> Screen;
		/** Back (pad B) should leave it. */
		bool bBackLeaves = true;
		/** Opens an overlay over the main menu. */
		TFunction<void(UApexRootWidget&)> Open;
		/** Puts things back if Back did not. */
		TFunction<void(UApexRootWidget&)> Close;
	};

	UApexRootWidget* FindRoot()
	{
		UWorld* World = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
		if (!World)
		{
			return nullptr;
		}
		for (TObjectIterator<UApexRootWidget> It; It; ++It)
		{
			if (It->GetWorld() == World && It->IsInViewport())
			{
				return *It;
			}
		}
		return nullptr;
	}

	TArray<FPadWalkStop> Stops()
	{
		TArray<FPadWalkStop> Out;
		// The main menu is home: Back steps up its own pages and never leaves.
		Out.Add({ TEXT("Main menu"), EApexScreen::MainMenu, false });
		Out.Add({ TEXT("Connect"), EApexScreen::ConnectDialog });
		Out.Add({ TEXT("Session browser"), EApexScreen::SessionBrowser });
		Out.Add({ TEXT("Create session"), EApexScreen::SessionCreate });
		Out.Add({ TEXT("Car select"), EApexScreen::CarSelect });
		Out.Add({ TEXT("Track select"), EApexScreen::TrackSelect });
		// Leaving the lobby is a deliberate button, and the results have no history.
		Out.Add({ TEXT("Session lobby"), EApexScreen::SessionLobby, false });
		Out.Add({ TEXT("Loading"), EApexScreen::Loading });
		Out.Add({ TEXT("Results"), EApexScreen::SessionResults, false });
		Out.Add({ TEXT("Replays"), EApexScreen::Replays });
		Out.Add({ TEXT("Settings"), {}, true,
			[](UApexRootWidget& Root) { Root.OpenSettings(EApexSettingsTab::Gameplay); },
			[](UApexRootWidget& Root)
			{
				if (Root.IsSettingsOpen())
				{
					Root.FocusDefault();
					ApexPadWalk::PressKey(EKeys::Escape);
				}
			} });
		Out.Add({ TEXT("Pause menu"), {}, true,
			[](UApexRootWidget& Root) { Root.SetPaused(true); },
			[](UApexRootWidget& Root) { Root.SetPaused(false); } });
		Out.Add({ TEXT("Car setups"), {}, true,
			[](UApexRootWidget& Root) { Root.OpenSetupEditor(); },
			[](UApexRootWidget& Root) { Root.CloseSetupEditor(); } });
		return Out;
	}

	/** Shared between the latent steps of one run. */
	struct FPadWalkRun
	{
		TArray<FPadWalkStop> Stops;
		int32 Index = 0;
		bool bOpened = false;
		/** When the next step may run: a stop lays out, a page is drawn. */
		double WaitUntil = 0.0;
		TUniquePtr<ApexPadWalk::FWalker> Walker;
	};
}



/** One stop at a time: open it, give it a moment to lay out and focus, walk it, close it. */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FApexPadWalkStep, FAutomationTestBase*, Test, TSharedPtr<ApexPadWalkTest::FPadWalkRun>, Run);

bool FApexPadWalkStep::Update()
{
	UApexRootWidget* Root = ApexPadWalkTest::FindRoot();
	if (!Root)
	{
		Test->AddError(TEXT("the menu shell went away"));
		return true;
	}
	if (FPlatformTime::Seconds() < Run->WaitUntil)
	{
		return false;
	}
	if (Run->Index >= Run->Stops.Num())
	{
		Root->ReplaceScreen(EApexScreen::MainMenu);
		return true;
	}
	const ApexPadWalkTest::FPadWalkStop& Stop = Run->Stops[Run->Index];
	if (!Run->bOpened)
	{
		Root->ReplaceScreen(EApexScreen::MainMenu);
		if (Stop.Screen.IsSet())
		{
			if (Stop.Screen.GetValue() != EApexScreen::MainMenu)
			{
				Root->ShowScreen(Stop.Screen.GetValue());
			}
		}
		else if (Stop.Open)
		{
			Stop.Open(*Root);
		}
		Run->bOpened = true;
		// Focus lands a tick after a switch; layout needs a frame or two more.
		Run->WaitUntil = FPlatformTime::Seconds() + 0.75;
		return false;
	}
	if (!Run->Walker)
	{
		Run->Walker = MakeUnique<ApexPadWalk::FWalker>(*Root, Stop.Name);
	}
	if (!Run->Walker->Advance())
	{
		// A new page is up: let it be drawn before walking it. Otherwise the
		// next frame will do (a scrolled list is drawn by then).
		Run->WaitUntil = FPlatformTime::Seconds() + (Run->Walker->TurnedPage() ? 0.2 : 0.0);
		return false;
	}

	const ApexPadWalk::FResult& Result = Run->Walker->GetResult();
	UE_LOG(LogApexSim, Display, TEXT("PadWalk %s"), *Result.Report());
	for (const FString& Problem : Result.Problems)
	{
		Test->AddError(FString::Printf(TEXT("%s: %s"), Stop.Name, *Problem));
	}
	if (Stop.bBackLeaves && !ApexPadWalk::BackLeaves(*Root))
	{
		Test->AddError(FString::Printf(TEXT("%s: pad B does not leave it"), Stop.Name));
	}
	if (Stop.Close)
	{
		Stop.Close(*Root);
	}
	Run->Walker.Reset();
	++Run->Index;
	Run->bOpened = false;
	Run->WaitUntil = FPlatformTime::Seconds() + 0.3;
	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexPadWalkTest,
	"ApexSim.UI.PadWalk",
	ApexTestFlags)

bool FApexPadWalkTest::RunTest(const FString& Parameters)
{
	if (!ApexPadWalkTest::FindRoot())
	{
		AddInfo(TEXT("No menu shell on a game viewport: run the game (-game) to walk the screens."));
		return true;
	}
	TSharedPtr<ApexPadWalkTest::FPadWalkRun> Run = MakeShared<ApexPadWalkTest::FPadWalkRun>();
	Run->Stops = ApexPadWalkTest::Stops();
	// -ApexPadWalkOnly=<name> walks the stops whose name contains it.
	FString Only;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexPadWalkOnly="), Only))
	{
		Run->Stops.RemoveAll([&Only](const ApexPadWalkTest::FPadWalkStop& Stop) { return !FString(Stop.Name).Contains(Only); });
	}
	ADD_LATENT_AUTOMATION_COMMAND(FApexPadWalkStep(this, Run));
	return true;
}

#endif
