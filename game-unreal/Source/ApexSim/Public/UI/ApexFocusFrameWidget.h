#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexFocusFrameWidget.generated.h"

class UBorder;
class UOverlay;

/**
 * Draws the shell's focus ring around a composite control while focus is
 * anywhere inside it. Slate sliders and similar leaves have no focus brush a
 * pad player can see; wrapping the row in one of these is how they get one.
 * Built by ApexUI::MakeFocusFrame.
 */
UCLASS()
class APEXSIM_API UApexFocusFrameWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetFramedContent(UWidget* Content, const FMargin& RingPadding);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent) override;

private:
	void Build();

	UPROPERTY(Transient) TObjectPtr<UWidget> PendingContent;
	UPROPERTY(Transient) TObjectPtr<UOverlay> Host;
	UPROPERTY(Transient) TObjectPtr<UBorder> Ring;
	FMargin PendingPadding;
};
