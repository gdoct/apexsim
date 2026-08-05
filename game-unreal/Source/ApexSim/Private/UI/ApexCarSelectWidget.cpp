#include "UI/ApexCarSelectWidget.h"

#include "ApexCarPreviewStage.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UI/ApexCarCardWidget.h"
#include "UI/ApexRootWidget.h"

void UApexCarSelectWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (ConfirmButton) { ConfirmButton->OnClicked.AddDynamic(this, &UApexCarSelectWidget::HandleConfirmClicked); }
	if (BackButton)    { BackButton->OnClicked.AddDynamic(this, &UApexCarSelectWidget::HandleBackClicked); }
	if (SearchBox)     { SearchBox->OnTextChanged.AddDynamic(this, &UApexCarSelectWidget::HandleSearchChanged); }

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexCarSelectWidget::HandleLobbyStateUpdated);
	}
}

void UApexCarSelectWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexCarSelectWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexCarSelectWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	RebuildCards();

	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		if (Flow->HasPendingCar())
		{
			SelectCar(Flow->GetPendingCarId());
		}
	}

	if (AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this))
	{
		Stage->SetTurntableEnabled(true);

		// A UImage brush accepts a render target directly, so the turntable
		// needs no intermediate UI material.
		if (CarPreviewImage)
		{
			if (UTextureRenderTarget2D* RenderTarget = Stage->GetPreviewRenderTarget())
			{
				// Size goes on the brush, not via SetDesiredSizeOverride — see
				// the note in ApexTrackCardWidget::SetTrack. The render target
				// is square and carries no desired size of its own.
				FSlateBrush Brush = CarPreviewImage->GetBrush();
				Brush.SetResourceObject(RenderTarget);
				Brush.DrawAs = ESlateBrushDrawType::Image;
				Brush.ImageSize = FVector2D(PreviewSize, PreviewSize);
				CarPreviewImage->SetBrush(Brush);
				CarPreviewImage->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
		}
	}
	else if (CarPreviewImage)
	{
		// No stage in the level: hide the frame rather than show an empty box.
		CarPreviewImage->SetVisibility(ESlateVisibility::Hidden);
		UE_LOG(LogApexSim, Warning, TEXT("No AApexCarPreviewStage in the level; the car preview is disabled"));
	}
}

void UApexCarSelectWidget::OnScreenDeactivated()
{
	// Stop spending a scene capture per frame on a preview nobody is looking at.
	if (AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this))
	{
		Stage->SetTurntableEnabled(false);
	}
	Super::OnScreenDeactivated();
}

void UApexCarSelectWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RebuildCards();
}

void UApexCarSelectWidget::RebuildCards()
{
	if (!CarList || !CarCardClass)
	{
		return;
	}

	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net)
	{
		return;
	}

	const TArray<FApexCarConfigSummary>& Cars = Net->GetCachedLobbyState().CarConfigs;

	// The catalog is static, so only rebuild when the set of car IDs actually
	// changes — otherwise the 2s LobbyState broadcast would recreate the list
	// under the user's cursor.
	TArray<FString> IncomingIds;
	IncomingIds.Reserve(Cars.Num());
	for (const FApexCarConfigSummary& Car : Cars)
	{
		IncomingIds.Add(Car.Id);
	}
	if (IncomingIds == BuiltCarIds)
	{
		return;
	}
	BuiltCarIds = MoveTemp(IncomingIds);

	CarList->ClearChildren();
	Cards.Reset();

	for (const FApexCarConfigSummary& Car : Cars)
	{
		UApexCarCardWidget* Card = CreateWidget<UApexCarCardWidget>(this, CarCardClass);
		if (!Card)
		{
			continue;
		}

		FApexCarCatalogRow Row;
		const bool bHasRow = Flow && Flow->GetCarCatalogRow(Car.Id, Row);
		Card->SetCar(Car, Row, bHasRow);
		Card->OnCardClicked.AddDynamic(this, &UApexCarSelectWidget::HandleCardClicked);

		CarList->AddChild(Card);
		Cards.Add(Card);
	}

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(
			FString::Printf(TEXT("%d car%s available"), Cards.Num(), Cards.Num() == 1 ? TEXT("") : TEXT("s"))));
	}

	// Preselect here rather than only on activation: the list is usually built
	// when LobbyState arrives, which is after the screen was activated. Without
	// this the preview stays empty until the user clicks a card.
	if (SelectedCarId.IsEmpty() && Cards.Num() > 0)
	{
		SelectCar(Cards[0]->GetCarId());
	}

	ApplyFilter();
}

void UApexCarSelectWidget::HandleSearchChanged(const FText& Text)
{
	ApplyFilter();
}

void UApexCarSelectWidget::ApplyFilter()
{
	const FString Filter = SearchBox ? SearchBox->GetText().ToString().TrimStartAndEnd() : FString();

	for (UApexCarCardWidget* Card : Cards)
	{
		const bool bMatches = Filter.IsEmpty()
			|| Card->GetDisplayName().Contains(Filter, ESearchCase::IgnoreCase);
		Card->SetVisibility(bMatches ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UApexCarSelectWidget::HandleCardClicked(UApexCarCardWidget* Card)
{
	if (Card)
	{
		SelectCar(Card->GetCarId());
	}
}

void UApexCarSelectWidget::SelectCar(const FString& CarId)
{
	SelectedCarId = CarId;

	for (UApexCarCardWidget* Card : Cards)
	{
		Card->SetSelected(Card->GetCarId().Equals(CarId, ESearchCase::IgnoreCase));
	}

	UpdatePreview();
}

void UApexCarSelectWidget::UpdatePreview()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net)
	{
		return;
	}

	FApexCarConfigSummary Summary;
	if (!Net->FindCarById(SelectedCarId, Summary))
	{
		return;
	}

	FApexCarCatalogRow Row;
	const bool bHasRow = Flow && Flow->GetCarCatalogRow(SelectedCarId, Row);

	if (SpecsText)
	{
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Name:  %s"),
			*((bHasRow && !Row.DisplayName.IsEmpty()) ? Row.DisplayName : Summary.Name)));
		if (bHasRow)
		{
			if (!Row.Brand.IsEmpty())    { Lines.Add(FString::Printf(TEXT("Brand:  %s"), *Row.Brand)); }
			if (!Row.CarClass.IsEmpty()) { Lines.Add(FString::Printf(TEXT("Class:  %s"), *Row.CarClass)); }
			if (Row.ModelYear > 0)       { Lines.Add(FString::Printf(TEXT("Year:  %d"), Row.ModelYear)); }
		}
		Lines.Add(FString::Printf(TEXT("Mass:  %.0f kg"), Summary.MassKg));
		if (bHasRow && Row.MaxPowerKw > 0.0f)
		{
			Lines.Add(FString::Printf(TEXT("Power:  %.0f kW"), Row.MaxPowerKw));
		}
		SpecsText->SetText(FText::FromString(FString::Join(Lines, TEXT("\n"))));
	}

	if (AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this))
	{
		if (bHasRow)
		{
			Stage->SetPreviewTransform(Row.PreviewOffset, Row.PreviewRotation, Row.PreviewScale);
			Stage->SetCarMesh(Row.Mesh);
		}
		else
		{
			// No catalog row means no mesh reference exists at all — the
			// server's ModelPath is a dead path (broadcast.rs:150).
			Stage->SetCarMesh(nullptr);
			UE_LOG(LogApexSim, Verbose, TEXT("No catalog row for car %s; showing no preview mesh"), *SelectedCarId);
		}
	}
}

void UApexCarSelectWidget::HandleConfirmClicked()
{
	if (SelectedCarId.IsEmpty())
	{
		ShowToast(TEXT("Pick a car first"), true);
		return;
	}

	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->SetPendingCar(SelectedCarId);
	}
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->SelectCar(SelectedCarId);
	}

	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ReplaceScreen(Root->ScreenAfterCarSelect);
	}
}

void UApexCarSelectWidget::HandleBackClicked()
{
	GoBack();
}
