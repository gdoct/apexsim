#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UApexRootWidget;
class UUserWidget;
class UWidget;

/**
 * Walks a menu surface by pad alone, as a player with only a controller
 * would: focus lands where the surface puts it, then the D-pad is pressed
 * from every control reached, and the right shoulder turns the pages (tabs),
 * through Slate's own input path (preprocessors, focus routing, navigation
 * replies), until nothing new is reached. A control a pad could focus but
 * never reaches is a trap. Accept is never pressed, so nothing is chosen;
 * Left/Right is not pressed on a slider, so no value moves (a livery, a page
 * or the menu's own selection may).
 *
 * A page shown by a shoulder press, or rows scrolled into view, have no
 * layout until they have been drawn, and the moves are measured on it, so
 * the walk runs over frames as a player's presses do: FWalker::Advance once
 * a frame until it says it is done.
 *
 * Used by `ApexSim.UI.PadWalk` (in a running game, `-game`) and the console
 * command `apexsim.ui.PadWalk`. docs/game/client.md, "Pad walk".
 */
namespace ApexPadWalk
{
	struct FResult
	{
		/** What was walked, for the report. */
		FString Surface;
		/** Every control a pad could focus on the surface, on any page seen. */
		TArray<FString> Focusable;
		/** Those the walk never reached. */
		TArray<FString> Unreached;
		/** Where focus landed on arrival; empty when it was not on the surface. */
		FString InitialFocus;
		/** Everything wrong, one line each; empty when the surface passes. */
		TArray<FString> Problems;

		bool Passed() const { return Problems.Num() == 0; }
		/** A line for the surface, then one per problem, for the log. */
		FString Report() const;
	};

	/** Walks the surface in front of the root (UApexRootWidget::GetFrontSurface). */
	class APEXSIM_API FWalker
	{
	public:
		FWalker(UApexRootWidget& InRoot, const FString& Name);

		/**
		 * One control's moves, or the next page shown; call once a frame. True
		 * when the walk is over and Result is final.
		 */
		bool Advance();

		/** The last Advance showed a new page: give it time to be drawn. */
		bool TurnedPage() const { return bTurnedPage; }

		const FResult& GetResult() const { return Result; }

	private:
		void Start();
		void Finish();
		/** Press Key from From and note where focus went. */
		UWidget* Step(UWidget* From, const FKey& Key);
		void Enqueue(UWidget* Widget, bool bEvenIfReached);

		TWeakObjectPtr<UApexRootWidget> Root;
		TWeakObjectPtr<UUserWidget> Surface;
		FResult Result;
		TArray<TWeakObjectPtr<UWidget>> Seen;
		TSet<TWeakObjectPtr<UWidget>> Reached;
		TArray<TWeakObjectPtr<UWidget>> Queue;
		bool bStarted = false;
		bool bDone = false;
		int32 Flips = 0;
		int32 FlipsWithoutNews = 0;
		int32 ReachedAtPageStart = 0;
		bool bPageShown = false;
		bool bTurnedPage = false;
	};

	/**
	 * Press Back (pad B) on the front surface and say whether it left: the
	 * front surface or the current screen changed.
	 */
	APEXSIM_API bool BackLeaves(UApexRootWidget& Root);

	/** One pad key, down and up, through FSlateApplication as a pad sends it. True when handled. */
	APEXSIM_API bool PressKey(const FKey& Key);

	/** The controls on Surface a pad could focus now, nested user widgets included, in tree order. */
	APEXSIM_API void GatherFocusables(UUserWidget* Surface, TArray<UWidget*>& Out);

	/** A readable name for a control: its class, label and name. */
	APEXSIM_API FString Describe(const UWidget* Widget);
}
