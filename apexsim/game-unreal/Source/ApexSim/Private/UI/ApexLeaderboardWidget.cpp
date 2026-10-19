#include "UI/ApexLeaderboardWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionLeaderboardTrack(TEXT("Leaderboard.Track"));
	const FName ActionLeaderboardCar(TEXT("Leaderboard.Car"));
	const FName ActionLeaderboardBack(TEXT("Leaderboard.Back"));

	constexpr float LeaderboardCardWidth = 1040.0f;

	/** "2026-10-06T12:00:00Z" as "2026-10-06"; whatever is not a date comes back as it is. */
	FString DateOf(const FString& Rfc3339)
	{
		return Rfc3339.Len() >= 10 ? Rfc3339.Left(10) : Rfc3339;
	}

	FString LapText(int32 Ms)
	{
		return UApexMenuFlowSubsystem::FormatLapTime(Ms / 1000.0f);
	}
}

UApexLeaderboardWidget::UApexLeaderboardWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

TArray<UApexLeaderboardWidget::FRow> UApexLeaderboardWidget::MakeRows(const TArray<FApexLeaderboardEntry>& Entries,
	const FString& Me, TFunctionRef<FString(const FString& CarId)> CarName)
{
	TArray<FRow> Rows;
	Rows.Reserve(Entries.Num());
	const int32 Best = Entries.Num() > 0 ? Entries[0].LapTimeMs : 0;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const FApexLeaderboardEntry& Entry = Entries[Index];
		FRow& Row = Rows.AddDefaulted_GetRef();
		Row.Rank = Index + 1;
		Row.Driver = Entry.PlayerName;
		Row.Car = CarName(Entry.CarConfigId);
		Row.Time = LapText(Entry.LapTimeMs);
		Row.Gap = Index == 0 ? FString() : FString::Printf(TEXT("+%.3f"), (Entry.LapTimeMs - Best) / 1000.0f);
		Row.When = DateOf(Entry.RecordedAt);
		Row.bMine = !Me.IsEmpty() && Entry.PlayerName.Equals(Me, ESearchCase::IgnoreCase);
	}
	return Rows;
}

void UApexLeaderboardWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexLeaderboardWidget::BuildLayout()
{
	using namespace ApexUI;
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Card, MakeLabel(*WidgetTree, TEXT("Track records"), Palette::Accent));
	AddV(Card, MakeText(*WidgetTree, TEXT("Leaderboards"), Font::Display(42.0f), Palette::TextPrimary), FMargin(0.0f, 10.0f, 0.0f, 6.0f));
	StatusText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	AddV(Card, StatusText, FMargin(0.0f, 0.0f, 0.0f, 18.0f));

	auto MakePicker = [this](FName Action, const TCHAR* SubLabel)
	{
		FApexButtonSpec Spec;
		Spec.Label = TEXT("-");
		Spec.SubLabel = SubLabel;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.LabelSize = 19.0f;
		Spec.Height = 64.0f;
		Spec.ActionId = Action;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexLeaderboardWidget::HandleButtonActivated);
		return Button;
	};
	TrackButton = MakePicker(ActionLeaderboardTrack, TEXT("CIRCUIT  ·  LEFT / RIGHT CHANGES IT"));
	CarButton = MakePicker(ActionLeaderboardCar, TEXT("CARS  ·  ENTER SWITCHES"));
	UHorizontalBox* Pickers = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Pickers, TrackButton, FMargin(), VAlign_Fill, 1.0f);
	AddH(Pickers, CarButton, FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	AddV(Card, Pickers, FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	List = WidgetTree->ConstructWidget<UScrollBox>();
	AddV(Card, MakeSized(*WidgetTree, List, -1.0f, 460.0f));

	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.Variant = EApexButtonVariant::Ghost;
	BackSpec.bCentreLabel = true;
	BackSpec.LabelSize = 17.0f;
	BackSpec.Height = 56.0f;
	BackSpec.ActionId = ActionLeaderboardBack;
	BackSpec.Sound = EApexUiSound::Back;
	BackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackButton->Setup(BackSpec);
	BackButton->OnActivated.AddDynamic(this, &UApexLeaderboardWidget::HandleButtonActivated);
	AddV(Card, MakeSized(*WidgetTree, BackButton, 180.0f, -1.0f), FMargin(0.0f, 18.0f, 0.0f, 0.0f), HAlign_Left);

	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree,
		MakePanel(*WidgetTree, Card, FMargin(44.0f, 40.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f)),
		LeaderboardCardWidth, -1.0f), FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.2f);
	WidgetTree->RootWidget = MakePanel(*WidgetTree, Centre, FMargin(), MakeBrush(Palette::Background));
}

void UApexLeaderboardWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	UApexNetSubsystem* Net = GetNet();
	if (Net && !bBound)
	{
		Net->OnLeaderboard.AddDynamic(this, &UApexLeaderboardWidget::HandleLeaderboard);
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexLeaderboardWidget::HandleLobbyState);
		bBound = true;
	}
	bOneCar = false;
	LoadTracks();
	Request();
	Rebuild();
	FocusDefault();
}

void UApexLeaderboardWidget::LoadTracks()
{
	// The circuits this machine can name, by name, starting on the one the
	// garage has picked.
	UApexNetSubsystem* Net = GetNet();
	TrackIds.Reset();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (Net && Flow)
	{
		TArray<TPair<FString, FString>> Named;
		for (const FApexTrackConfigSummary& Track : Net->GetCachedLobbyState().TrackConfigs)
		{
			FApexTrackCatalogRow Row;
			if (Flow->GetTrackCatalogRow(Track.Id, Row) && !Row.DisplayName.IsEmpty())
			{
				Named.Emplace(Row.DisplayName, Track.Id);
			}
		}
		Named.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0; });
		for (const TPair<FString, FString>& Entry : Named)
		{
			TrackIds.Add(Entry.Value);
		}
		const int32 Pending = TrackIds.IndexOfByPredicate([Flow](const FString& Id) { return Id.Equals(Flow->GetPendingTrackId(), ESearchCase::IgnoreCase); });
		TrackIndex = Pending != INDEX_NONE ? Pending : 0;
	}
}

void UApexLeaderboardWidget::HandleLobbyState(const FApexLobbyState& LobbyState)
{
	if (IsActiveScreen() && TrackIds.Num() == 0 && LobbyState.TrackConfigs.Num() > 0)
	{
		LoadTracks();
		Request();
		Rebuild();
	}
}

void UApexLeaderboardWidget::OnScreenDeactivated()
{
	Super::OnScreenDeactivated();
	if (UApexNetSubsystem* Net = GetNet())
	{
		if (bBound)
		{
			Net->OnLeaderboard.RemoveDynamic(this, &UApexLeaderboardWidget::HandleLeaderboard);
			Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexLeaderboardWidget::HandleLobbyState);
			bBound = false;
		}
	}
}

void UApexLeaderboardWidget::FocusDefault()
{
	if (!(TrackButton && ApexNav::Focus(TrackButton)))
	{
		SetKeyboardFocus();
	}
}

FReply UApexLeaderboardWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const bool bOnTrack = TrackButton && (TrackButton->HasAnyUserFocus() || TrackButton->HasFocusedDescendants());
	const bool bOnCar = CarButton && (CarButton->HasAnyUserFocus() || CarButton->HasFocusedDescendants());
	if ((bOnTrack || bOnCar) && (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right))
	{
		if (bOnTrack)
		{
			StepTrack(Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left ? -1 : 1);
		}
		else
		{
			ToggleCar();
		}
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexLeaderboardWidget::StepTrack(int32 Direction)
{
	if (TrackIds.Num() == 0)
	{
		return;
	}
	TrackIndex = ((TrackIndex + Direction) % TrackIds.Num() + TrackIds.Num()) % TrackIds.Num();
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	Request();
	Rebuild();
}

void UApexLeaderboardWidget::ToggleCar()
{
	bOneCar = !bOneCar;
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
	Request();
	Rebuild();
}

void UApexLeaderboardWidget::Request()
{
	UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Net->IsAuthenticated() || !TrackIds.IsValidIndex(TrackIndex))
	{
		return;
	}
	Net->RequestLeaderboard(TrackIds[TrackIndex], bOneCar && Flow ? Flow->GetPendingCarId() : FString());
}

void UApexLeaderboardWidget::HandleLeaderboard(const FString& TrackId, const FString& CarId)
{
	if (IsActiveScreen())
	{
		Rebuild();
	}
}

void UApexLeaderboardWidget::UpdateButtons()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (TrackButton)
	{
		FString Name = TEXT("No circuit");
		FApexTrackCatalogRow Row;
		if (Flow && TrackIds.IsValidIndex(TrackIndex) && Flow->GetTrackCatalogRow(TrackIds[TrackIndex], Row))
		{
			Name = Row.DisplayName;
		}
		TrackButton->SetLabel(Name);
	}
	if (CarButton)
	{
		FString Name = TEXT("Every car");
		FApexCarCatalogRow Row;
		if (bOneCar && Flow && Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Row))
		{
			Name = Row.DisplayName;
		}
		CarButton->SetLabel(Name);
	}
}

void UApexLeaderboardWidget::Rebuild()
{
	using namespace ApexUI;
	UpdateButtons();
	if (!List)
	{
		return;
	}
	List->ClearChildren();
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow || !TrackIds.IsValidIndex(TrackIndex))
	{
		StatusText->SetText(FText::FromString(Net && Net->IsAuthenticated() ? TEXT("NO CIRCUITS YOU CAN SEE") : TEXT("NOT CONNECTED TO A SERVER")));
		return;
	}
	const FString CarId = bOneCar ? Flow->GetPendingCarId() : FString();
	const TArray<FApexLeaderboardEntry>& Entries = Net->GetLeaderboard(TrackIds[TrackIndex], CarId);
	const TArray<FRow> Rows = MakeRows(Entries, Flow->PlayerName, [Flow](const FString& Id)
	{
		FApexCarCatalogRow Row;
		return Flow->GetCarCatalogRow(Id, Row) && !Row.DisplayName.IsEmpty() ? Row.DisplayName : FString(TEXT("-"));
	});
	StatusText->SetText(FText::FromString(!Net->HasLeaderboard(TrackIds[TrackIndex], CarId)
		? FString(TEXT("ASKING THE SERVER..."))
		: Rows.Num() == 0
		? FString(TEXT("NO LAPS YET ON THIS SERVER. THE FASTEST LEGAL LAP OF EVERY DRIVER AND CAR IS KEPT HERE"))
		: FString::Printf(TEXT("%d LAP%s  ·  FASTEST LEGAL LAP OF EACH DRIVER IN EACH CAR"), Rows.Num(), Rows.Num() == 1 ? TEXT("") : TEXT("S"))));

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FRow& Row = Rows[Index];
		const FLinearColor RowInk = Row.bMine ? Palette::Accent : Palette::TextPrimary;
		const FLinearColor Quiet = Row.bMine ? Palette::Accent : Palette::TextMuted;
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Line, MakeSized(*WidgetTree, MakeText(*WidgetTree, FString::FromInt(Row.Rank), Font::Mono(14.0f), Quiet), 44.0f, -1.0f), FMargin(), VAlign_Center);
		AddH(Line, MakeText(*WidgetTree, Row.Driver.ToUpper(), Font::Body(16.0f), RowInk), FMargin(), VAlign_Center, 1.0f);
		AddH(Line, MakeSized(*WidgetTree, MakeText(*WidgetTree, Row.Car, Font::Body(14.0f), Quiet), 230.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		UTextBlock* Time = MakeText(*WidgetTree, Row.Time, Font::Mono(15.0f), RowInk);
		Time->SetJustification(ETextJustify::Right);
		AddH(Line, MakeSized(*WidgetTree, Time, 100.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		UTextBlock* Gap = MakeText(*WidgetTree, Row.Gap, Font::Mono(13.0f), Quiet);
		Gap->SetJustification(ETextJustify::Right);
		AddH(Line, MakeSized(*WidgetTree, Gap, 84.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		AddH(Line, MakeSized(*WidgetTree, MakeText(*WidgetTree, Row.When, Font::Mono(12.0f), Palette::TextMuted), 92.0f, -1.0f), FMargin(12.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);

		UWidget* Panel = MakePanel(*WidgetTree, Line, FMargin(12.0f, 8.0f),
			MakeBrush(Row.bMine ? FLinearColor(Palette::Accent.R, Palette::Accent.G, Palette::Accent.B, 0.12f) : Palette::Background));
		List->AddChild(Panel);
		if (UScrollBoxSlot* RowSlot = Cast<UScrollBoxSlot>(Panel->Slot))
		{
			RowSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
		}
	}
}

void UApexLeaderboardWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (Button == BackButton)
	{
		GoBack();
	}
	else if (Button == TrackButton)
	{
		StepTrack(1);
	}
	else if (Button == CarButton)
	{
		ToggleCar();
	}
}
