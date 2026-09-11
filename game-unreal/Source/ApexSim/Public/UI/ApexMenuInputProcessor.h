#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

class UApexRootWidget;

/**
 * Sees every key, stick and mouse event before Slate routes it, on behalf of
 * the shell.
 *
 * Three jobs that cannot be done from inside the widget tree, because all are
 * about events that would never reach it, or must not:
 *
 * - Focus recovery. A click on the backdrop, or an input-mode switch, hands
 *   focus to the game viewport, and from then on every arrow and D-pad press
 *   goes nowhere. The first navigation press after that lands focus back on
 *   the active screen's default control instead of vanishing.
 * - The reverse while driving. Focus left anywhere in the shell puts the
 *   focusable root widget in the event path, and Slate's default handler
 *   there turns the left stick, D-pad and arrows into menu navigation and
 *   consumes them - the car never steers. Any event while driving first
 *   hands focus back to the viewport.
 * - The pause key during a race. Driving runs with focus on the viewport, so
 *   the root widget's own key handler never hears it.
 *
 * It also notices which device the player is using: the cursor is hidden
 * while a gamepad is driving the menu and comes back when the mouse moves.
 */
class FApexMenuInputProcessor : public IInputProcessor
{
public:
	explicit FApexMenuInputProcessor(UApexRootWidget* InOwner);

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override;
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("ApexMenuInputProcessor"); }

private:
	/** Brings focus back to the shell if it has drifted to the viewport. True if it did. */
	bool RecoverFocus(FSlateApplication& SlateApp, UApexRootWidget& Root) const;

	/** While driving, moves the user's focus to the game viewport if it is anywhere else. */
	void KeepFocusOnGame(FSlateApplication& SlateApp, uint32 UserIndex) const;

	/** A race is on screen with no overlay up: every key belongs to the car. */
	static bool IsDriving(const UApexRootWidget& Root);

	/** Hides the cursor while a pad drives the menu, shows it again for the mouse. */
	void SetGamepadActive(UApexRootWidget& Root, bool bActive);

	TWeakObjectPtr<UApexRootWidget> Owner;
	bool bGamepadActive = false;
};
