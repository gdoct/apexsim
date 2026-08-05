#include "UI/ApexTrackSelectWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/WrapBox.h"
#include "Components/TextBlock.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexTrackCardWidget.h"

void UApexTrackSelectWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (ConfirmButton) { ConfirmButton->OnClicked.AddDynamic(this, &UApexTrackSelectWidget::HandleConfirmClicked); }
	if (BackButton)    { BackButton->OnClicked.AddDynamic(this, &UApexTrackSelectWidget::HandleBackClicked); }
	if (SearchBox)     { SearchBox->OnTextChanged.AddDynamic(this, &UApexTrackSelectWidget::HandleSearchChanged); }

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexTrackSelectWidget::HandleLobbyStateUpdated);
	}
}

void UApexTrackSelectWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexTrackSelectWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexTrackSelectWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	RebuildCards();

	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		if (Flow->HasPendingTrack())
		{
			SelectTrack(Flow->GetPendingTrackId());
		}
	}
}

void UApexTrackSelectWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RebuildCards();
}

void UApexTrackSelectWidget::RebuildCards()
{
	if (!TrackGrid || !TrackCardClass)
	{
		// A missing BindWidget leaves the screen silently blank, which is a
		// miserable thing to debug. Say which one, once.
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogApexSim, Warning,
				TEXT("Track select cannot build: TrackGrid=%s TrackCardClass=%s"),
				TrackGrid ? TEXT("ok") : TEXT("NULL (WrapBox named 'TrackGrid' not bound)"),
				TrackCardClass ? TEXT("ok") : TEXT("NULL (set it in the WBP defaults)"));
		}
		return;
	}

	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net)
	{
		return;
	}

	const TArray<FApexTrackConfigSummary>& Tracks = Net->GetCachedLobbyState().TrackConfigs;

	TArray<FString> IncomingIds;
	IncomingIds.Reserve(Tracks.Num());
	for (const FApexTrackConfigSummary& Track : Tracks)
	{
		IncomingIds.Add(Track.Id);
	}
	if (IncomingIds == BuiltTrackIds)
	{
		return;
	}
	BuiltTrackIds = MoveTemp(IncomingIds);

	TrackGrid->ClearChildren();
	Cards.Reset();

	for (const FApexTrackConfigSummary& Track : Tracks)
	{
		UApexTrackCardWidget* Card = CreateWidget<UApexTrackCardWidget>(this, TrackCardClass);
		if (!Card)
		{
			continue;
		}

		FApexTrackCatalogRow Row;
		const bool bHasRow = Flow && Flow->GetTrackCatalogRow(Track.Id, Row);
		Card->SetTrack(Track, Row, bHasRow);
		Card->OnCardClicked.AddDynamic(this, &UApexTrackSelectWidget::HandleCardClicked);

		TrackGrid->AddChild(Card);
		Cards.Add(Card);
	}

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(
			FString::Printf(TEXT("%d track%s available"), Cards.Num(), Cards.Num() == 1 ? TEXT("") : TEXT("s"))));
	}

	ApplyFilter();
}

void UApexTrackSelectWidget::HandleSearchChanged(const FText& Text)
{
	ApplyFilter();
}

void UApexTrackSelectWidget::ApplyFilter()
{
	const FString Filter = SearchBox ? SearchBox->GetText().ToString().TrimStartAndEnd() : FString();

	int32 VisibleCount = 0;
	for (UApexTrackCardWidget* Card : Cards)
	{
		const bool bMatches = Filter.IsEmpty()
			|| Card->GetDisplayName().Contains(Filter, ESearchCase::IgnoreCase);
		Card->SetVisibility(bMatches ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		VisibleCount += bMatches ? 1 : 0;
	}

	if (StatusText && !Filter.IsEmpty())
	{
		StatusText->SetText(FText::FromString(
			FString::Printf(TEXT("%d of %d track%s match '%s'"),
				VisibleCount, Cards.Num(), Cards.Num() == 1 ? TEXT("") : TEXT("s"), *Filter)));
	}
}

void UApexTrackSelectWidget::HandleCardClicked(UApexTrackCardWidget* Card)
{
	if (Card)
	{
		SelectTrack(Card->GetTrackId());
	}
}

void UApexTrackSelectWidget::SelectTrack(const FString& TrackId)
{
	SelectedTrackId = TrackId;
	for (UApexTrackCardWidget* Card : Cards)
	{
		Card->SetSelected(Card->GetTrackId().Equals(TrackId, ESearchCase::IgnoreCase));
	}
}

void UApexTrackSelectWidget::HandleConfirmClicked()
{
	if (SelectedTrackId.IsEmpty())
	{
		ShowToast(TEXT("Pick a track first"), true);
		return;
	}

	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->SetPendingTrack(SelectedTrackId);
	}

	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ReplaceScreen(Root->ScreenAfterTrackSelect);
	}
}

void UApexTrackSelectWidget::HandleBackClicked()
{
	GoBack();
}
