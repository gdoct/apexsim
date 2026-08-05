#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"
#include "Catalog/ApexCatalogRows.h"

#include "ApexTrackCardWidget.generated.h"

class UApexTrackCardWidget;
class UBorder;
class UButton;
class UImage;
class UTextBlock;
class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnTrackCardClicked, UApexTrackCardWidget*, Card);

/** A 400x300 tile in the track grid: preview image, name, and country/length/category. */
UCLASS(Abstract)
class APEXSIM_API UApexTrackCardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetTrack(const FApexTrackConfigSummary& Summary, const FApexTrackCatalogRow& CatalogRow, bool bHasCatalogRow);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetSelected(bool bInSelected);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetTrackId() const { return TrackId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetDisplayName() const { return DisplayName; }

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnTrackCardClicked OnCardClicked;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> SelectButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UBorder> HighlightBorder;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UImage> PreviewImage;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> TrackNameText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> TrackInfoText;

	/** Used for tracks with no preview art — Le Mans is the current example. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	TObjectPtr<UTexture2D> PlaceholderPreview;

	/** Card art width in slate units; the height follows the source 4:3 art. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	float PreviewWidth = 300.0f;

	/** Captions longer than this are clipped with an ellipsis to fit the card. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	int32 MaxCaptionChars = 25;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor SelectedTint = FLinearColor(0.15f, 0.45f, 0.75f, 0.95f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor UnselectedTint = FLinearColor(0.08f, 0.08f, 0.10f, 0.70f);

private:
	UFUNCTION()
	void HandleClicked();

	/** Shortens a caption to MaxCaptionChars, appending an ellipsis. */
	FString Clamp(const FString& Value) const;

	FString TrackId;
	FString DisplayName;
	bool bSelected = false;
};
