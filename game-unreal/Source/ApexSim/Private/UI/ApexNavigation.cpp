#include "UI/ApexNavigation.h"

#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Framework/Application/SlateApplication.h"
#include "UI/ApexButtonWidget.h"

namespace ApexNav
{
	EUINavigation DirectionFromKey(const FKeyEvent& Event)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return EUINavigation::Invalid;
		}

		// Arrows, the D-pad and Tab come from the navigation config, which is
		// also what Slate's own fallback consults — so the two never disagree.
		const EUINavigation Direction = FSlateApplication::Get().GetNavigationDirectionFromKey(Event);
		if (Direction != EUINavigation::Invalid)
		{
			return Direction;
		}

		// The shoulders are Tab and Shift+Tab for a pad: every screen with more
		// than one region uses Next/Previous to hop between them.
		const FKey Key = Event.GetKey();
		if (Key == EKeys::Gamepad_LeftShoulder)
		{
			return EUINavigation::Previous;
		}
		if (Key == EKeys::Gamepad_RightShoulder)
		{
			return EUINavigation::Next;
		}
		return EUINavigation::Invalid;
	}

	bool IsAnalogNavigationKey(const FKey& Key)
	{
		return Key == EKeys::Gamepad_LeftX || Key == EKeys::Gamepad_LeftY;
	}

	EUINavigation DirectionFromAnalog(const FAnalogInputEvent& Event)
	{
		if (!FSlateApplication::IsInitialized() || !IsAnalogNavigationKey(Event.GetKey()))
		{
			return EUINavigation::Invalid;
		}
		// Stateful on purpose: the config remembers when it last stepped for
		// this stick and direction, which is what gives a held stick a sane
		// repeat rate instead of one move per input sample.
		return FSlateApplication::Get().GetNavigationDirectionFromAnalog(Event);
	}

	bool IsAccept(const FKeyEvent& Event)
	{
		return FSlateApplication::IsInitialized()
			&& FSlateApplication::Get().GetNavigationActionFromKey(Event) == EUINavigationAction::Accept;
	}

	bool IsBack(const FKeyEvent& Event)
	{
		return FSlateApplication::IsInitialized()
			&& FSlateApplication::Get().GetNavigationActionFromKey(Event) == EUINavigationAction::Back;
	}

	bool IsSequential(EUINavigation Direction)
	{
		return Direction == EUINavigation::Next || Direction == EUINavigation::Previous;
	}

	UApexNavigableWidget* FindHost(const UWidget* Leaf)
	{
		// Outer, not parent: a leaf's slot chain ends at the root of its own
		// user widget's tree, while the outer chain steps straight out to the
		// user widget that built it, and on to the one that built that.
		for (UUserWidget* Owner = Leaf ? Leaf->GetTypedOuter<UUserWidget>() : nullptr;
			Owner;
			Owner = Owner->GetTypedOuter<UUserWidget>())
		{
			if (UApexNavigableWidget* Host = Cast<UApexNavigableWidget>(Owner))
			{
				return Host;
			}
		}
		return nullptr;
	}

	namespace
	{
		/** The next widget up: the slot's parent, or the owning user widget at the top of a tree. */
		const UWidget* StepUp(const UWidget* Widget)
		{
			if (const UWidget* Parent = Widget->GetParent())
			{
				return Parent;
			}
			return Widget->GetTypedOuter<UUserWidget>();
		}

		/** Visible here and all the way up. UWidget::IsVisible only answers for the widget itself. */
		bool IsShown(const UWidget* Widget)
		{
			for (const UWidget* At = Widget; At; At = StepUp(At))
			{
				if (!At->IsVisible())
				{
					return false;
				}
			}
			return true;
		}
	}

	bool CanFocus(const UWidget* Widget)
	{
		if (!Widget)
		{
			return false;
		}

		// A locked button is focusable at the Slate level only until Setup has
		// run; this covers the window in between as well.
		if (const UApexButtonWidget* Button = Cast<UApexButtonWidget>(Widget))
		{
			if (!Button->IsInteractive())
			{
				return false;
			}
		}

		if (!Widget->GetIsEnabled() || !IsShown(Widget))
		{
			return false;
		}

		const TSharedPtr<SWidget> Slate = Widget->GetCachedWidget();
		return Slate.IsValid() && Slate->SupportsKeyboardFocus();
	}

	bool Focus(UWidget* Widget)
	{
		if (!CanFocus(Widget))
		{
			return false;
		}

		Widget->SetKeyboardFocus();

		// Focus that lands below the fold of a list is invisible focus.
		for (UWidget* At = Widget->GetParent(); At; At = At->GetParent())
		{
			if (UScrollBox* Scroll = Cast<UScrollBox>(At))
			{
				Scroll->ScrollWidgetIntoView(Widget, /*AnimateScroll*/ true);
				break;
			}
		}
		return true;
	}

	UWidget* FindFirstFocusable(const UUserWidget* Container)
	{
		if (!Container || !Container->WidgetTree)
		{
			return nullptr;
		}

		UWidget* Found = nullptr;
		Container->WidgetTree->ForEachWidget([&Found](UWidget* Widget)
		{
			if (!Found && CanFocus(Widget))
			{
				Found = Widget;
			}
		});
		return Found;
	}

	FReply RouteFromLeaf(UWidget* Leaf, EUINavigation Direction, ENavigationGenesis Genesis)
	{
		if (UApexNavigableWidget* Host = FindHost(Leaf))
		{
			FNavigationScope Scope;
			if (Host->HandleNavigation(Direction, Leaf))
			{
				return FReply::Handled();
			}
		}

		// Exactly the reply SWidget::OnKeyDown would have produced: Slate
		// searches from the leaf's own geometry. That move is reported as
		// EFocusCause::Navigation, so it needs no scope.
		return FReply::Handled().SetNavigation(Direction, Genesis);
	}

	namespace
	{
		/** Scopes nest — a host may route on to another host — so this counts rather than flags. */
		int32 NavigationDepth = 0;
	}

	FNavigationScope::FNavigationScope()
	{
		++NavigationDepth;
	}

	FNavigationScope::~FNavigationScope()
	{
		--NavigationDepth;
	}

	bool IsNavigating()
	{
		return NavigationDepth > 0;
	}
}

// ---------------------------------------------------------------------------
// UApexNavigableWidget
// ---------------------------------------------------------------------------

void UApexNavigableWidget::FocusDefault()
{
	if (!ApexNav::Focus(ApexNav::FindFirstFocusable(this)))
	{
		// Nothing to land on: keep the keys coming here, so Back still works.
		SetKeyboardFocus();
	}
}

bool UApexNavigableWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	return false;
}

bool UApexNavigableWidget::HandleBack()
{
	return false;
}

bool UApexNavigableWidget::HandleAccept()
{
	return false;
}

FReply UApexNavigableWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// A direction reaching this widget means no control below took it: either
	// this widget itself has focus, or a Slate control (a text box, say) let it
	// through. Either way the answer is to put focus somewhere useful.
	const EUINavigation Direction = ApexNav::DirectionFromKey(InKeyEvent);
	if (Direction != EUINavigation::Invalid)
	{
		ApexNav::FNavigationScope Scope;
		if (!HandleNavigation(Direction, this))
		{
			FocusDefault();
		}
		return FReply::Handled();
	}

	if (ApexNav::IsBack(InKeyEvent) && HandleBack())
	{
		// Only a Back that did something: a surface with nowhere to go returns
		// false and the press falls through, silently.
		ApexUiAudio::Play(this, EApexUiSound::Back);
		return FReply::Handled();
	}

	if (ApexNav::IsAccept(InKeyEvent) && HandleAccept())
	{
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UApexNavigableWidget::NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent)
{
	const EUINavigation Direction = ApexNav::DirectionFromAnalog(InAnalogEvent);
	if (Direction != EUINavigation::Invalid)
	{
		ApexNav::FNavigationScope Scope;
		if (!HandleNavigation(Direction, this))
		{
			FocusDefault();
		}
		return FReply::Handled();
	}

	if (ApexNav::IsAnalogNavigationKey(InAnalogEvent.GetKey()))
	{
		// Centred, or throttled by the repeat rate. Consumed either way, or
		// Slate would ask the config a second time and step on its own.
		return FReply::Handled();
	}

	return Super::NativeOnAnalogValueChanged(InGeometry, InAnalogEvent);
}
