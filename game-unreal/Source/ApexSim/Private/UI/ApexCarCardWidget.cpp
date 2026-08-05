#include "UI/ApexCarCardWidget.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"

void UApexCarCardWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (SelectButton)
	{
		SelectButton->OnClicked.AddDynamic(this, &UApexCarCardWidget::HandleClicked);
	}

	// The spec line ("GT3 · Posh · 2022 · 1280 kg · 375 kW") is wider than the
	// list column at small window sizes.
	if (CarNameText)  { CarNameText->SetAutoWrapText(true); }
	if (CarSpecsText) { CarSpecsText->SetAutoWrapText(true); }

	SetSelected(false);
}

void UApexCarCardWidget::SetCar(const FApexCarConfigSummary& Summary, const FApexCarCatalogRow& CatalogRow, bool bHasCatalogRow)
{
	CarId = Summary.Id;

	// The catalog name is the authored one and matches the content files; the
	// server's is derived. Prefer the catalog, fall back to the wire.
	DisplayName = (bHasCatalogRow && !CatalogRow.DisplayName.IsEmpty()) ? CatalogRow.DisplayName : Summary.Name;

	if (CarNameText)
	{
		CarNameText->SetText(FText::FromString(DisplayName));
	}

	if (CarSpecsText)
	{
		TArray<FString> Parts;
		if (bHasCatalogRow)
		{
			if (!CatalogRow.CarClass.IsEmpty()) { Parts.Add(CatalogRow.CarClass); }
			if (!CatalogRow.Brand.IsEmpty())    { Parts.Add(CatalogRow.Brand); }
			if (CatalogRow.ModelYear > 0)       { Parts.Add(FString::FromInt(CatalogRow.ModelYear)); }
		}

		// Mass always comes from the server: it is what the simulation will
		// actually use, catalog or not.
		if (Summary.MassKg > 0.0f)
		{
			Parts.Add(FString::Printf(TEXT("%.0f kg"), Summary.MassKg));
		}
		if (bHasCatalogRow && CatalogRow.MaxPowerKw > 0.0f)
		{
			Parts.Add(FString::Printf(TEXT("%.0f kW"), CatalogRow.MaxPowerKw));
		}

		CarSpecsText->SetText(FText::FromString(FString::Join(Parts, TEXT("  ·  "))));
	}
}

void UApexCarCardWidget::SetSelected(bool bInSelected)
{
	bSelected = bInSelected;
	if (HighlightBorder)
	{
		HighlightBorder->SetBrushColor(bSelected ? SelectedTint : UnselectedTint);
	}
}

void UApexCarCardWidget::HandleClicked()
{
	OnCardClicked.Broadcast(this);
}
