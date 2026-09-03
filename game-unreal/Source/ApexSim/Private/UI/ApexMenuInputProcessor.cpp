#include "UI/ApexMenuInputProcessor.h"

#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"

namespace
{
	/** Stick deflection that counts as "the player is using the pad". */
	constexpr float AnalogIntentThreshold = 0.5f;
}

FApexMenuInputProcessor::FApexMenuInputProcessor(UApexRootWidget* InOwner)
	: Owner(InOwner)
{
}

bool FApexMenuInputProcessor::RecoverFocus(FSlateApplication& SlateApp, UApexRootWidget& Root) const
{
	// A dropdown's list lives in its own window, outside the root's tree, so
	// an open menu is not lost focus — stealing it back would close the list.
	if (SlateApp.AnyMenusVisible() || Root.HasFocusedDescendants())
	{
		return false;
	}

	UE_LOG(LogApexSim, Verbose, TEXT("Menu focus had drifted to the viewport; restoring it"));

	// In a navigation scope: the press was the player's, and hearing the
	// focus land is how they learn the menu is listening again.
	ApexNav::FNavigationScope Scope;
	Root.FocusDefault();
	return true;
}

void FApexMenuInputProcessor::SetGamepadActive(UApexRootWidget& Root, bool bActive)
{
	if (bGamepadActive == bActive)
	{
		return;
	}
	bGamepadActive = bActive;

	if (APlayerController* PlayerController = Root.GetOwningPlayer())
	{
		PlayerController->SetShowMouseCursor(!bActive);
	}
}

bool FApexMenuInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	UApexRootWidget* Root = Owner.Get();
	if (!Root)
	{
		return false;
	}

	const FKey Key = InKeyEvent.GetKey();
	if (Key.IsGamepadKey())
	{
		SetGamepadActive(*Root, true);
	}

	// While settings is up every key may be a rebind in progress; nothing here
	// is allowed to get in front of that.
	if (Root->IsSettingsOpen())
	{
		return false;
	}

	if (Root->IsRaceViewActive())
	{
		const UApexSettingsSubsystem* Settings =
			Root->GetGameInstance() ? Root->GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
		const bool bPauseKey = Settings ? Settings->IsPauseKey(Key) : Key == EKeys::Escape;

		if (bPauseKey && !InKeyEvent.IsRepeat())
		{
			const bool bOpening = !Root->IsPaused();
			Root->SetPaused(bOpening);
			ApexUiAudio::Play(Root, bOpening ? EApexUiSound::Accept : EApexUiSound::Back);
			return true;
		}

		if (!Root->IsPaused())
		{
			// Driving: the keys belong to the car.
			return false;
		}
	}

	const bool bNavigationPress = ApexNav::DirectionFromKey(InKeyEvent) != EUINavigation::Invalid
		|| ApexNav::IsAccept(InKeyEvent)
		|| ApexNav::IsBack(InKeyEvent);
	if (bNavigationPress && RecoverFocus(SlateApp, *Root))
	{
		// The press was spent on finding the menu again; the next one moves.
		return true;
	}

	return false;
}

bool FApexMenuInputProcessor::HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent)
{
	UApexRootWidget* Root = Owner.Get();
	if (!Root || !ApexNav::IsAnalogNavigationKey(InAnalogInputEvent.GetKey()))
	{
		return false;
	}
	if (FMath::Abs(InAnalogInputEvent.GetAnalogValue()) < AnalogIntentThreshold)
	{
		return false;
	}

	SetGamepadActive(*Root, true);

	if (Root->IsSettingsOpen() || (Root->IsRaceViewActive() && !Root->IsPaused()))
	{
		return false;
	}

	return RecoverFocus(SlateApp, *Root);
}

bool FApexMenuInputProcessor::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	// Synthesized moves carry no delta; only a hand on the mouse brings the
	// cursor back.
	if (UApexRootWidget* Root = Owner.Get())
	{
		if (!FVector2D(MouseEvent.GetCursorDelta()).IsNearlyZero())
		{
			SetGamepadActive(*Root, false);
		}
	}
	return false;
}

bool FApexMenuInputProcessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (UApexRootWidget* Root = Owner.Get())
	{
		SetGamepadActive(*Root, false);
	}
	return false;
}
