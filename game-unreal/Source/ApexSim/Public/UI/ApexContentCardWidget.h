#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexContentCardWidget.generated.h"

class UApexContentCardWidget;
class UBorder;
class UTextBlock;
class UTexture2D;
class UVerticalBox;
class UWidget;

/** What a card shows. Content-agnostic: a track, a car, anything with art and a name. */
USTRUCT(BlueprintType)
struct FApexCardSpec
{
	GENERATED_BODY()

	/** The content's UUID, so the owning screen can map a click back to data. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString Id;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString Title;

	/** The dotted line under the title: "GERMANY · 4.57 km · F1". */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString Meta;

	/** Third line, usually a personal best or its absence. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString Footnote;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor FootnoteColour = FLinearColor(0.43f, 0.43f, 0.46f);

	/** Null draws the placeholder instead. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	TObjectPtr<UTexture2D> Preview;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	FString PlaceholderCaption = TEXT("No preview");

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|UI")
	float PreviewHeight = 150.0f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnCardActivated, UApexContentCardWidget*, Card);

/**
 * A picture, a name and two lines of metadata, in a box that can be focused,
 * hovered and selected.
 *
 * Selection and focus are different things here: focus follows the keyboard or
 * the last click, while selection is what the screen's detail panel is showing.
 * They usually coincide, but a click somewhere else must not silently change
 * what the panel describes.
 */
UCLASS()
class APEXSIM_API UApexContentCardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexContentCardWidget(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnCardActivated OnActivated;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Setup(const FApexCardSpec& InSpec);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetSelected(bool bInSelected);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetCardId() const { return Spec.Id; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetTitle() const { return Spec.Title; }

	/** Everything the screen's search box should match against. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	FString GetSearchText() const { return Spec.Title + TEXT(" ") + Spec.Meta; }

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;

private:
	void ApplyState();

	UPROPERTY()
	FApexCardSpec Spec;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> Frame;

	/** Rebuilt on Setup: the art changes size and kind between specs. */
	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> Body;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> MetaText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> FootnoteText;

	bool bHovered = false;
	bool bFocused = false;
	bool bSelected = false;
};
