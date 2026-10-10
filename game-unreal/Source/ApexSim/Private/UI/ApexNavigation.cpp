#include "UI/ApexNavigation.h"

#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/WidgetSwitcher.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
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

		/**
		 * Visible here and all the way up. UWidget::IsVisible only answers for
		 * the widget itself, and a switcher's other pages are visible by that
		 * measure while not drawn at all.
		 */
		bool IsShown(const UWidget* Widget)
		{
			const UWidget* Below = nullptr;
			for (const UWidget* At = Widget; At; Below = At, At = StepUp(At))
			{
				if (!At->IsVisible())
				{
					return false;
				}
				if (const UWidgetSwitcher* Switcher = Cast<UWidgetSwitcher>(At); Switcher && Below
					&& Below->GetParent() == Switcher && Switcher->GetActiveWidget() != Below)
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

	namespace
	{
		void GatherInto(const UUserWidget* Container, TArray<UWidget*>& Out, int32 Depth)
		{
			if (!Container || !Container->WidgetTree || Depth > 16)
			{
				return;
			}
			Container->WidgetTree->ForEachWidget([&Out, Depth](UWidget* Widget)
			{
				if (CanFocus(Widget))
				{
					Out.Add(Widget);
				}
				// A button is a user widget too, but nothing inside it takes focus.
				if (const UUserWidget* Nested = Cast<UUserWidget>(Widget); Nested && !Cast<UApexButtonWidget>(Nested))
				{
					GatherInto(Nested, Out, Depth + 1);
				}
			});
		}

		/**
		 * Where a widget is laid out now, in desktop space. Arranged afresh
		 * down its path rather than read from its last paint: a row scrolled
		 * out of a scroll box is not drawn, so its cached geometry is where it
		 * was last seen, or nowhere.
		 */
		bool LaidOut(const UWidget* Widget, FBox2D& Out)
		{
			const TSharedPtr<SWidget> Slate = Widget ? Widget->GetCachedWidget() : nullptr;
			if (!Slate.IsValid() || !FSlateApplication::IsInitialized())
			{
				return false;
			}
			FWidgetPath Path;
			if (!FSlateApplication::Get().FindPathToWidget(Slate.ToSharedRef(), Path, EVisibility::Visible) || !Path.IsValid())
			{
				return false;
			}
			const FGeometry& Geometry = Path.Widgets.Last().Geometry;
			const FVector2D Min = Geometry.GetAbsolutePosition();
			const FVector2D Size = Geometry.GetAbsoluteSize();
			Out = FBox2D(Min, Min + Size);
			return Size.X > 0.0f && Size.Y > 0.0f;
		}

		/**
		 * Where a widget is laid out now, in desktop space; where it was last
		 * drawn when it is not laid out (see LaidOut).
		 */
		FBox2D RectOfImpl(const UWidget* Widget)
		{
			FBox2D Rect(ForceInit);
			if (LaidOut(Widget, Rect))
			{
				return Rect;
			}
			const FGeometry& Geometry = Widget->GetCachedGeometry();
			const FVector2D Min = Geometry.GetAbsolutePosition();
			return FBox2D(Min, Min + Geometry.GetAbsoluteSize());
		}

		/** Space between two intervals; zero where they overlap. */
		float IntervalGap(float AMin, float AMax, float BMin, float BMax)
		{
			return FMath::Max(0.0f, FMath::Max(BMin - AMax, AMin - BMax));
		}
	}

	FBox2D LayoutRect(const UWidget* Widget)
	{
		return Widget ? RectOfImpl(Widget) : FBox2D(ForceInit);
	}

	void GatherFocusables(const UUserWidget* Container, TArray<UWidget*>& Out)
	{
		GatherInto(Container, Out, 0);
	}

	UWidget* FocusedAmong(const TArray<UWidget*>& Candidates)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		for (TSharedPtr<SWidget> At = FSlateApplication::Get().GetUserFocusedWidget(0); At.IsValid(); At = At->GetParentWidget())
		{
			for (UWidget* Candidate : Candidates)
			{
				if (Candidate && Candidate->GetCachedWidget() == At)
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	namespace
	{
		/** The scroll box a widget scrolls in, if any. */
		const UScrollBox* ScrollBoxOf(const UWidget* Widget)
		{
			for (const UWidget* At = Widget ? StepUp(Widget) : nullptr; At; At = StepUp(At))
			{
				if (const UScrollBox* Scroll = Cast<UScrollBox>(At))
				{
					return Scroll;
				}
			}
			return nullptr;
		}

		UWidget* NearestAmong(EUINavigation Direction, const UWidget* Source, const TArray<UWidget*>& Candidates);
	}

	UWidget* NearestToward(EUINavigation Direction, const UWidget* Source, const TArray<UWidget*>& Candidates)
	{
		if (!Source)
		{
			return nullptr;
		}
		// Along a scroll box, its own rows first and what lies past it (a
		// footer) only at the list's end: a row scrolled out of view is
		// further away than the footer drawn over the box's edge.
		const UScrollBox* Scroll = ScrollBoxOf(Source);
		const bool bAlong = Scroll && (Scroll->GetOrientation() == Orient_Vertical
			? Direction == EUINavigation::Up || Direction == EUINavigation::Down
			: Direction == EUINavigation::Left || Direction == EUINavigation::Right);
		if (bAlong)
		{
			TArray<UWidget*> Inside;
			for (UWidget* Candidate : Candidates)
			{
				if (Candidate && ScrollBoxOf(Candidate) == Scroll)
				{
					Inside.Add(Candidate);
				}
			}
			if (UWidget* Best = NearestAmong(Direction, Source, Inside))
			{
				return Best;
			}
			// Nothing laid out that way: the next row in the list's own order,
			// which the box scrolls into view as it takes focus.
			const bool bForward = Direction == EUINavigation::Down || Direction == EUINavigation::Right;
			const int32 At = Inside.IndexOfByKey(Source);
			FBox2D Ignored(ForceInit);
			if (At != INDEX_NONE)
			{
				for (int32 Index = At + (bForward ? 1 : -1); Inside.IsValidIndex(Index); Index += bForward ? 1 : -1)
				{
					if (!LaidOut(Inside[Index], Ignored))
					{
						return Inside[Index];
					}
				}
			}
		}
		return NearestAmong(Direction, Source, Candidates);
	}

	namespace
	{
		UWidget* NearestAmong(EUINavigation Direction, const UWidget* Source, const TArray<UWidget*>& Candidates)
		{
			constexpr float Slack = 4.0f;
			const FBox2D From = RectOfImpl(Source);
			const FVector2D FromCentre = From.GetCenter();

			UWidget* Best = nullptr;
			float BestScore = TNumericLimits<float>::Max();
			for (UWidget* Candidate : Candidates)
			{
				if (!Candidate || Candidate == Source || !CanFocus(Candidate))
				{
					continue;
				}
				// A row a scroll box has not laid out (scrolled out of view) has
				// no place to measure; NearestToward reaches it by list order.
				FBox2D To(ForceInit);
				if (!LaidOut(Candidate, To))
				{
					continue;
				}
				const FVector2D ToCentre = To.GetCenter();
				float Along = 0.0f;
				float Across = 0.0f;
				float CentreAcross = 0.0f;
				switch (Direction)
				{
				case EUINavigation::Right:
					if (To.Min.X < From.Max.X - Slack) { continue; }
					Along = To.Min.X - From.Max.X;
					Across = IntervalGap(From.Min.Y, From.Max.Y, To.Min.Y, To.Max.Y);
					CentreAcross = FMath::Abs(ToCentre.Y - FromCentre.Y);
					break;
				case EUINavigation::Left:
					if (To.Max.X > From.Min.X + Slack) { continue; }
					Along = From.Min.X - To.Max.X;
					Across = IntervalGap(From.Min.Y, From.Max.Y, To.Min.Y, To.Max.Y);
					CentreAcross = FMath::Abs(ToCentre.Y - FromCentre.Y);
					break;
				case EUINavigation::Down:
					if (To.Min.Y < From.Max.Y - Slack) { continue; }
					Along = To.Min.Y - From.Max.Y;
					Across = IntervalGap(From.Min.X, From.Max.X, To.Min.X, To.Max.X);
					CentreAcross = FMath::Abs(ToCentre.X - FromCentre.X);
					break;
				case EUINavigation::Up:
					if (To.Max.Y > From.Min.Y + Slack) { continue; }
					Along = From.Min.Y - To.Max.Y;
					Across = IntervalGap(From.Min.X, From.Max.X, To.Min.X, To.Max.X);
					CentreAcross = FMath::Abs(ToCentre.X - FromCentre.X);
					break;
				default:
					return nullptr;
				}
				const float Score = FMath::Max(0.0f, Along) + 2.0f * Across + 0.05f * CentreAcross;
				if (Score < BestScore)
				{
					BestScore = Score;
					Best = Candidate;
				}
			}
			return Best;
		}
	}

	bool MoveToward(UUserWidget* Surface, EUINavigation Direction, const UWidget* Source)
	{
		if (!Surface || IsSequential(Direction) || Direction == EUINavigation::Invalid)
		{
			return false;
		}
		TArray<UWidget*> Candidates;
		GatherFocusables(Surface, Candidates);
		if (!Source || Source == Surface)
		{
			Source = FocusedAmong(Candidates);
		}
		if (!Source)
		{
			return false;
		}
		if (UWidget* Target = NearestToward(Direction, Source, Candidates))
		{
			Focus(Target);
		}
		return true;
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

FReply UApexNavigableWidget::NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const EUINavigation Direction = ApexNav::DirectionFromKey(InKeyEvent);
	if (Direction == EUINavigation::Up || Direction == EUINavigation::Down)
	{
		TArray<UWidget*> Controls;
		ApexNav::GatherFocusables(this, Controls);
		UWidget* Focused = ApexNav::FocusedAmong(Controls);
		// Only a slider that is this surface's own, not one in a surface on top.
		if (Focused && Focused->IsA<USlider>() && ApexNav::FindHost(Focused) == this)
		{
			ApexNav::FNavigationScope Scope;
			ApexNav::MoveToward(this, Direction, Focused);
			return FReply::Handled();
		}
	}
	return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
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
