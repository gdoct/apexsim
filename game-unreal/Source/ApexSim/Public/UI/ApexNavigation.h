#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "Types/SlateEnums.h"

#include "ApexNavigation.generated.h"

class UApexNavigableWidget;
class UWidget;

/**
 * Keyboard and gamepad navigation, in one vocabulary for the whole shell.
 *
 * Slate already turns arrows, the D-pad and the left stick into directional
 * focus moves, and picks the target geometrically. That is the fallback here,
 * not the design: a geometric search cannot know that the rail wraps, that a
 * car row should describe itself when reached, or that the last card in a grid
 * row leads to the detail panel. So every interactive leaf (buttons, cards)
 * first offers a move to the nearest UApexNavigableWidget above it, and only
 * when that has no opinion does Slate's search run.
 *
 * Accept and Back are read through Slate's navigation config rather than as
 * literal keys, so Enter, Space and the gamepad's face buttons behave the same
 * everywhere without every screen listing them.
 */
namespace ApexNav
{
	/**
	 * Arrows, D-pad, Tab / Shift+Tab (Next / Previous) and the shoulder buttons
	 * (Previous / Next). Invalid for anything else — including the stick-as-
	 * button keys some platforms send, which the analog path already covers.
	 */
	APEXSIM_API EUINavigation DirectionFromKey(const FKeyEvent& Event);

	/**
	 * The left stick, throttled the way Slate throttles it, so a held stick
	 * steps rather than sprints. Invalid for other axes or a centred stick.
	 */
	APEXSIM_API EUINavigation DirectionFromAnalog(const FAnalogInputEvent& Event);

	/** True for the left stick's axes, whatever their value. */
	APEXSIM_API bool IsAnalogNavigationKey(const FKey& Key);

	/** Enter, Space, gamepad A. */
	APEXSIM_API bool IsAccept(const FKeyEvent& Event);

	/** Escape, gamepad B. */
	APEXSIM_API bool IsBack(const FKeyEvent& Event);

	/** True for Next and Previous: the "move to the next region" pair. */
	APEXSIM_API bool IsSequential(EUINavigation Direction);

	/** The nearest navigable widget that owns Leaf, walking out through user widget boundaries. */
	APEXSIM_API UApexNavigableWidget* FindHost(const UWidget* Leaf);

	/**
	 * Whether focus could usefully land here: visible up the hierarchy,
	 * enabled, focusable at the Slate level, and not a locked button.
	 */
	APEXSIM_API bool CanFocus(const UWidget* Widget);

	/**
	 * Focuses Widget if CanFocus, scrolling any enclosing scroll box to show
	 * it. Returns whether it did, so a caller can fall through to another
	 * target.
	 */
	APEXSIM_API bool Focus(UWidget* Widget);

	/** First widget in Container's tree order that CanFocus. */
	APEXSIM_API UWidget* FindFirstFocusable(const UUserWidget* Container);

	/**
	 * What a leaf does with a direction: offer it to the host, and if the host
	 * declines, hand it to Slate's own search from the leaf's position.
	 */
	APEXSIM_API FReply RouteFromLeaf(UWidget* Leaf, EUINavigation Direction, ENavigationGenesis Genesis);

	/**
	 * Marks the extent of one navigation input being handled.
	 *
	 * Slate reports focus it moves itself as EFocusCause::Navigation, but a
	 * host that answers a direction with ApexNav::Focus produces the same
	 * SetDirectly a screen makes when it opens. Wrapping the handling in one
	 * of these is how the shell tells the two apart: focus that moves inside
	 * a scope was moved by the player, and sounds like it (see the root
	 * widget's focus hook); focus set outside one is plumbing, and is silent.
	 */
	struct APEXSIM_API FNavigationScope
	{
		FNavigationScope();
		~FNavigationScope();
		FNavigationScope(const FNavigationScope&) = delete;
		FNavigationScope& operator=(const FNavigationScope&) = delete;
	};

	/** True while inside any FNavigationScope. */
	APEXSIM_API bool IsNavigating();

	/** Index of Widget in a list of widget pointers, or INDEX_NONE. */
	template <class T>
	int32 IndexOf(const TArray<TObjectPtr<T>>& List, const UWidget* Widget)
	{
		return List.IndexOfByPredicate([Widget](const TObjectPtr<T>& Entry) { return Entry.Get() == Widget; });
	}
}

/**
 * A surface that owns navigation for the controls inside it: every screen in
 * the switcher, and the race overlays.
 *
 * Subclasses answer three questions — where focus should land by default,
 * what a directional move from a given control means, and what Back does.
 * Anything they leave unanswered falls through to Slate's defaults, so a
 * screen with a plain vertical stack of buttons needs no overrides at all.
 */
UCLASS()
class APEXSIM_API UApexNavigableWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Puts focus on this surface's default control. Called whenever the
	 * surface becomes the active one, and whenever focus has to be recovered
	 * after it was lost to the viewport.
	 */
	virtual void FocusDefault();

	/**
	 * A directional move requested from Source — a leaf inside this widget,
	 * or this widget itself when nothing inside it has focus. Return true when
	 * the move was dealt with, including a deliberate "stay put"; false hands
	 * it to Slate's geometric search.
	 */
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source);

	/** Escape or gamepad B reached this surface. Return true if consumed. */
	virtual bool HandleBack();

	/** Enter or gamepad A that no control below consumed. Return true if consumed. */
	virtual bool HandleAccept();

protected:
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;
};
