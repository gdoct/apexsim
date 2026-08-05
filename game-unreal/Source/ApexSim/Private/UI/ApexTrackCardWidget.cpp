#include "UI/ApexTrackCardWidget.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"

void UApexTrackCardWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (SelectButton)
	{
		SelectButton->OnClicked.AddDynamic(this, &UApexTrackCardWidget::HandleClicked);
	}

	// Track names are long ("Autódromo José Carlos Pace"). Wrapping them made
	// card heights uneven, so rows of the grid overlapped each other; a single
	// elided line keeps every card the same height.
	for (UTextBlock* Line : {ToRawPtr(TrackNameText), ToRawPtr(TrackInfoText)})
	{
		if (Line)
		{
			Line->SetAutoWrapText(false);
			Line->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		}
	}

	SetSelected(false);
}

void UApexTrackCardWidget::SetTrack(const FApexTrackConfigSummary& Summary, const FApexTrackCatalogRow& CatalogRow, bool bHasCatalogRow)
{
	TrackId = Summary.Id;
	DisplayName = (bHasCatalogRow && !CatalogRow.DisplayName.IsEmpty()) ? CatalogRow.DisplayName : Summary.Name;

	if (TrackNameText)
	{
		TrackNameText->SetText(FText::FromString(Clamp(DisplayName)));
	}

	if (TrackInfoText)
	{
		TArray<FString> Parts;
		if (bHasCatalogRow)
		{
			if (!CatalogRow.Country.IsEmpty()) { Parts.Add(CatalogRow.Country); }
			if (CatalogRow.LengthM > 0.0f)     { Parts.Add(FString::Printf(TEXT("%.2f km"), CatalogRow.LengthM / 1000.0f)); }
			if (!CatalogRow.Category.IsEmpty()) { Parts.Add(CatalogRow.Category); }
		}
		TrackInfoText->SetText(FText::FromString(Clamp(FString::Join(Parts, TEXT("  ·  ")))));
	}

	if (PreviewImage)
	{
		// The preview can only be reached through the catalog: the wire gives
		// us "Circuit of The Americas" while the file is Austin.png.
		UTexture2D* Preview = nullptr;
		if (bHasCatalogRow && !CatalogRow.PreviewImage.IsNull())
		{
			Preview = CatalogRow.PreviewImage.LoadSynchronous();
		}
		if (!Preview)
		{
			Preview = PlaceholderPreview;
		}

		if (Preview)
		{
			// The size goes on the brush rather than through
			// SetDesiredSizeOverride: cards are populated before they are added
			// to the panel, and the override silently no-ops until the
			// underlying Slate widget exists. Brush values live on the UObject
			// and survive into it. Source previews are 400x300, so keep 4:3.
			FSlateBrush Brush = PreviewImage->GetBrush();
			Brush.SetResourceObject(Preview);
			Brush.DrawAs = ESlateBrushDrawType::Image;
			Brush.ImageSize = FVector2D(PreviewWidth, PreviewWidth * 0.75f);
			PreviewImage->SetBrush(Brush);
			PreviewImage->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			PreviewImage->SetVisibility(ESlateVisibility::Hidden);
		}
	}
}

FString UApexTrackCardWidget::Clamp(const FString& Value) const
{
	// The card is only as wide as its art, and the text block has no width
	// constraint to elide against, so long names would run over the neighbouring
	// card. Clamping by character count keeps every card the same shape.
	if (Value.Len() <= MaxCaptionChars)
	{
		return Value;
	}
	return Value.Left(FMath::Max(MaxCaptionChars - 1, 1)).TrimEnd() + TEXT("…");
}

void UApexTrackCardWidget::SetSelected(bool bInSelected)
{
	bSelected = bInSelected;
	if (HighlightBorder)
	{
		HighlightBorder->SetBrushColor(bSelected ? SelectedTint : UnselectedTint);
	}
}

void UApexTrackCardWidget::HandleClicked()
{
	OnCardClicked.Broadcast(this);
}
