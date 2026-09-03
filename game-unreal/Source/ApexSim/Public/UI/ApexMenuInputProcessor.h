#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

class UApexRootWidget;

/**
 * Sees every key, stick and mouse event before Slate routes it, on behalf of
 * the shell.
 *
 * Two jobs that cannot be done from inside the widget tree, because both are
 * about events that would never reach it:
 *
 * - Focus recovery. A click on the backdrop, or an input-mode switch, hands
 *   focus to the game viewport, and from then on every arrow and D-pad press
 *   goes nowhere. The first navigation press after that lands focus back on
 *   the active screen's default control instead of vanishing.
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

	/** Hides the cursor while a pad drives the menu, shows it again for the mouse. */
	void SetGamepadActive(UApexRootWidget& Root, bool bActive);

	TWeakObjectPtr<UApexRootWidget> Owner;
	bool bGamepadActive = false;
};
