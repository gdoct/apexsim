#include "UI/ApexPadWalk.h"

#include "ApexSim.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "Components/Slider.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexStepperWidget.h"

// Every step of a walk, from -> key -> to: -LogCmds="LogApexPadWalk Verbose".
DEFINE_LOG_CATEGORY_STATIC(LogApexPadWalk, Log, All);

namespace ApexPadWalk
{
	namespace
	{
		/** The D-pad: every way a pad moves focus on a page. */
		const FKey DirectionKeys[] = {
			EKeys::Gamepad_DPad_Up, EKeys::Gamepad_DPad_Down,
			EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Right,
		};

		/** Pages a surface may hold (settings has seven); a cap, not an expectation. */
		constexpr int32 MaxPageFlips = 12;
		/** A whole cycle of pages with nothing new ends the walk. */
		constexpr int32 FlipsWithoutNewsToStop = 8;

		/** Still part of Surface: not dropped from it by a rebuild since it was seen. */
		bool IsAttached(const UWidget* Widget, const UUserWidget* Surface)
		{
			const UWidget* At = Widget;
			for (int32 Depth = 0; At && Depth < 64; ++Depth)
			{
				if (At == Surface)
				{
					return true;
				}
				while (At->GetParent())
				{
					At = At->GetParent();
				}
				const UUserWidget* Owner = At->GetTypedOuter<UUserWidget>();
				if (!Owner || !Owner->WidgetTree || Owner->WidgetTree->RootWidget != At)
				{
					return false;
				}
				At = Owner;
			}
			return false;
		}

		bool IsSlider(const UWidget* Widget)
		{
			return Widget && Widget->IsA<USlider>();
		}

		FString FocusedTypeName()
		{
			if (FSlateApplication::IsInitialized())
			{
				if (const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(0))
				{
					return Focused->GetTypeAsString();
				}
			}
			return TEXT("nothing");
		}
	}

	FString Describe(const UWidget* Widget)
	{
		if (!Widget)
		{
			return TEXT("(none)");
		}
		FString Text;
		if (const UApexButtonWidget* Button = Cast<UApexButtonWidget>(Widget))
		{
			Text = Button->GetLabel();
			if (Text.IsEmpty() && !Button->GetActionId().IsNone())
			{
				Text = Button->GetActionId().ToString();
			}
		}
		else if (const UEditableTextBox* Box = Cast<UEditableTextBox>(Widget))
		{
			Text = Box->GetHintText().ToString();
		}
		const UUserWidget* Owner = Widget->GetTypedOuter<UUserWidget>();
		FString OwnerName = Owner ? Owner->GetClass()->GetName() : TEXT("?");
		// A stepper's pills share their names with every other stepper's.
		if (const UApexStepperWidget* Stepper = Cast<UApexStepperWidget>(Owner))
		{
			OwnerName += FString::Printf(TEXT(" %s#%d"), *Stepper->ControlId.ToString(), Stepper->Index);
		}
		return FString::Printf(TEXT("%s \"%s\" (%s in %s)"),
			*Widget->GetClass()->GetName(), *Text.Left(40), *Widget->GetName(), *OwnerName);
	}

	bool PressKey(const FKey& Key)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		// As FSlateApplication::OnControllerButtonPressed builds a pad's: user 0, no character.
		const FKeyEvent Event(Key, FModifierKeysState(), 0u, false, 0u, 0u);
		const bool bHandled = Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
		return bHandled;
	}

	void GatherFocusables(UUserWidget* Surface, TArray<UWidget*>& Out)
	{
		ApexNav::GatherFocusables(Surface, Out);
	}

	FString FResult::Report() const
	{
		FString Out = FString::Printf(TEXT("%s: %s, %d of %d controls reached, focus on arrival %s"),
			*Surface, Passed() ? TEXT("PASS") : TEXT("FAIL"),
			Focusable.Num() - Unreached.Num(), Focusable.Num(),
			InitialFocus.IsEmpty() ? TEXT("(off the surface)") : *InitialFocus);
		for (const FString& Problem : Problems)
		{
			Out += TEXT("\n    ") + Problem;
		}
		return Out;
	}

	FWalker::FWalker(UApexRootWidget& InRoot, const FString& Name)
		: Root(&InRoot)
	{
		Result.Surface = Name;
	}

	void FWalker::Enqueue(UWidget* Widget, bool bEvenIfReached)
	{
		if (!Widget)
		{
			return;
		}
		const bool bNew = !Reached.Contains(Widget);
		Reached.Add(Widget);
		if ((bNew || bEvenIfReached) && !Queue.Contains(Widget))
		{
			Queue.Add(Widget);
		}
	}

	UWidget* FWalker::Step(UWidget* From, const FKey& Key)
	{
		UApexRootWidget* RootWidget = Root.Get();
		UUserWidget* On = Surface.Get();
		if (!RootWidget || !On || !ApexNav::Focus(From))
		{
			return nullptr;
		}
		PressKey(Key);
		if (RootWidget->GetFrontSurface() != On)
		{
			Result.Problems.AddUnique(FString::Printf(TEXT("%s from %s left the surface"),
				*Key.GetDisplayName().ToString(), *Describe(From)));
			return nullptr;
		}
		TArray<UWidget*> After;
		GatherFocusables(On, After);
		for (UWidget* Widget : After)
		{
			Seen.AddUnique(Widget);
		}
		UWidget* To = ApexNav::FocusedAmong(After);
		UE_LOG(LogApexPadWalk, Verbose, TEXT("%s @%s -%s-> %s @%s"),
			*Describe(From), *From->GetCachedGeometry().GetAbsolutePosition().ToString(), *Key.GetDisplayName().ToString(),
			To ? *Describe(To) : *FocusedTypeName(), To ? *To->GetCachedGeometry().GetAbsolutePosition().ToString() : TEXT("-"));
		Enqueue(To, false);
		return To;
	}

	void FWalker::Start()
	{
		bStarted = true;
		UApexRootWidget* RootWidget = Root.Get();
		UUserWidget* On = RootWidget ? RootWidget->GetFrontSurface() : nullptr;
		Surface = On;
		if (!On)
		{
			Result.Problems.Add(TEXT("nothing in front: the viewport owns the keys"));
			bDone = true;
			return;
		}

		RootWidget->FocusDefault();
		TArray<UWidget*> Now;
		GatherFocusables(On, Now);
		for (UWidget* Widget : Now)
		{
			Seen.AddUnique(Widget);
		}
		if (Now.Num() == 0)
		{
			// Nothing to walk: the keys must still reach the surface, or Back is dead.
			if (On->HasKeyboardFocus() || On->HasUserFocusedDescendants(On->GetOwningPlayer()))
			{
				Result.InitialFocus = On->GetClass()->GetName();
			}
			else
			{
				Result.Problems.Add(FString::Printf(TEXT("no control, and focus is not on the surface (%s)"), *FocusedTypeName()));
			}
			bDone = true;
			return;
		}

		UWidget* First = ApexNav::FocusedAmong(Now);
		if (First)
		{
			Result.InitialFocus = Describe(First);
		}
		else
		{
			Result.Problems.Add(FString::Printf(TEXT("focus on arrival is not on a control (%s)"), *FocusedTypeName()));
			First = Now[0];
		}
		Enqueue(First, true);
		ReachedAtPageStart = 0;
	}

	bool FWalker::Advance()
	{
		if (bDone)
		{
			return true;
		}
		bTurnedPage = false;
		if (!bStarted)
		{
			Start();
			return bDone;
		}
		UUserWidget* On = Surface.Get();
		if (!On)
		{
			Finish();
			return true;
		}
		if (bPageShown)
		{
			// The page the last shoulder press showed has been drawn: start it
			// from wherever focus landed, reached before or not, and walk again
			// the controls that stay on show from page to page (tabs, a rail,
			// the footer), which have new neighbours on it.
			bPageShown = false;
			TArray<UWidget*> Now;
			GatherFocusables(On, Now);
			for (UWidget* Widget : Now)
			{
				Seen.AddUnique(Widget);
			}
			Enqueue(ApexNav::FocusedAmong(Now), true);
			for (UWidget* Widget : Now)
			{
				if (Reached.Contains(Widget))
				{
					Enqueue(Widget, true);
				}
			}
			ReachedAtPageStart = Reached.Num();
		}

		// One control a frame: a move that scrolls a list lays out the rows it
		// brings into view only when the frame is drawn, as for a player.
		while (Queue.Num() > 0)
		{
			UWidget* From = Queue[0].Get();
			Queue.RemoveAt(0);
			if (!From)
			{
				continue;
			}
			for (const FKey& Key : DirectionKeys)
			{
				// Left and right move a slider's value, not the focus.
				if (IsSlider(From) && (Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Gamepad_DPad_Right))
				{
					continue;
				}
				Step(From, Key);
			}
			return false;
		}

		// The page is done: the next one, from a control on show that was reached.
		FlipsWithoutNews = Reached.Num() > ReachedAtPageStart ? 0 : FlipsWithoutNews + 1;
		UWidget* From = nullptr;
		if (Flips < MaxPageFlips && FlipsWithoutNews < FlipsWithoutNewsToStop)
		{
			TArray<UWidget*> Shown;
			GatherFocusables(On, Shown);
			for (UWidget* Widget : Shown)
			{
				if (Reached.Contains(Widget))
				{
					From = Widget;
					break;
				}
			}
		}
		if (!From)
		{
			Finish();
			return true;
		}
		++Flips;
		Step(From, EKeys::Gamepad_RightShoulder);
		bPageShown = true;
		bTurnedPage = true;
		return false;
	}

	void FWalker::Finish()
	{
		bDone = true;
		// A control a screen rebuilt (the create screen's summary links, on
		// every lobby broadcast) is a new object in the old one's place: it
		// counts as reached when the one it replaced was. Same type, label and
		// owner; the old one no longer on the surface.
		auto Key = [](const UWidget* Widget)
		{
			const UApexButtonWidget* Button = Cast<UApexButtonWidget>(Widget);
			const UUserWidget* Owner = Widget->GetTypedOuter<UUserWidget>();
			return FString::Printf(TEXT("%s|%s|%s"), *Widget->GetClass()->GetName(),
				Button ? *Button->GetLabel() : TEXT(""), Owner ? *Owner->GetName() : TEXT(""));
		};
		TSet<FString> Replaced;
		for (const TWeakObjectPtr<UWidget>& Weak : Seen)
		{
			if (UWidget* Widget = Weak.Get(); Widget && Reached.Contains(Widget) && !IsAttached(Widget, Surface.Get()))
			{
				Replaced.Add(Key(Widget));
			}
		}
		for (const TWeakObjectPtr<UWidget>& Weak : Seen)
		{
			UWidget* Widget = Weak.Get();
			if (!Widget || !IsAttached(Widget, Surface.Get()))
			{
				continue;
			}
			const FString Text = Describe(Widget);
			Result.Focusable.Add(Text);
			if (!Reached.Contains(Widget) && !Replaced.Contains(Key(Widget)))
			{
				Result.Unreached.Add(Text);
				Result.Problems.Add(FString::Printf(TEXT("unreachable by pad: %s at %s"),
					*Text, *ApexNav::LayoutRect(Widget).ToString()));
			}
		}
		// Leave focus where the surface wants it, for whatever comes next.
		if (UApexRootWidget* RootWidget = Root.Get())
		{
			RootWidget->FocusDefault();
		}
	}

	bool BackLeaves(UApexRootWidget& Root)
	{
		UUserWidget* Before = Root.GetFrontSurface();
		const EApexScreen Screen = Root.GetCurrentScreen();
		Root.FocusDefault();
		PressKey(EKeys::Gamepad_FaceButton_Right);
		return Root.GetFrontSurface() != Before || Root.GetCurrentScreen() != Screen;
	}

	namespace
	{
		UApexRootWidget* FindRoot(UWorld* World)
		{
			for (TObjectIterator<UApexRootWidget> It; It; ++It)
			{
				if (It->GetWorld() == World && It->IsInViewport())
				{
					return *It;
				}
			}
			return nullptr;
		}

		FAutoConsoleCommandWithWorld PadWalkCommand(
			TEXT("apexsim.ui.PadWalk"),
			TEXT("Walk the surface in front by pad alone (D-pad, shoulder pages) and log any control it never reaches."),
			FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
			{
				UApexRootWidget* Root = FindRoot(World);
				if (!Root)
				{
					UE_LOG(LogApexSim, Warning, TEXT("apexsim.ui.PadWalk: no menu shell in this world"));
					return;
				}
				TSharedRef<FWalker> Walker = MakeShared<FWalker>(*Root, TEXT("front surface"));
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Walker](float)
				{
					if (!Walker->Advance())
					{
						return true;
					}
					UE_LOG(LogApexSim, Display, TEXT("apexsim.ui.PadWalk %s"), *Walker->GetResult().Report());
					return false;
				}), 0.0f);
			}));
	}
}
