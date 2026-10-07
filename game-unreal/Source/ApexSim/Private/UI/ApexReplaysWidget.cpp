#include "UI/ApexReplaysWidget.h"

#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Misc/Paths.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionReplayRow(TEXT("Replays.Row"));
	const FName ActionReplaysBack(TEXT("Replays.Back"));

	constexpr float ReplaysCardWidth = 960.0f;

	const TCHAR* ReplayModeName(EApexGameMode Mode)
	{
		switch (Mode)
		{
		case EApexGameMode::Hotlap: return TEXT("Hotlap");
		case EApexGameMode::FreePractice: return TEXT("Practice");
		case EApexGameMode::Qualification: return TEXT("Qualifying");
		case EApexGameMode::Race: return TEXT("Race");
		default: return TEXT("Session");
		}
	}

	FString ReplayDuration(double Seconds)
	{
		const int32 Whole = FMath::Max(0, FMath::RoundToInt(Seconds));
		return Whole >= 3600 ? FString::Printf(TEXT("%d:%02d:%02d"), Whole / 3600, (Whole / 60) % 60, Whole % 60)
							 : FString::Printf(TEXT("%d:%02d"), Whole / 60, Whole % 60);
	}
}

UApexReplaysWidget::UApexReplaysWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexReplaysWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexReplaysWidget::BuildLayout()
{
	using namespace ApexUI;
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Card, MakeLabel(*WidgetTree, TEXT("Watch again"), Palette::Accent));
	AddV(Card, MakeText(*WidgetTree, TEXT("Replays"), Font::Display(42.0f), Palette::TextPrimary), FMargin(0.0f, 10.0f, 0.0f, 6.0f));
	CountText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	AddV(Card, CountText, FMargin(0.0f, 0.0f, 0.0f, 20.0f));

	EmptyText = MakeText(*WidgetTree,
		TEXT("No replays yet. Every session you drive or watch is kept here among the recent ones; SAVE REPLAY in the pause menu or the hotlap garage keeps one for good."),
		Font::Body(16.0f), Palette::TextSecondary);
	EmptyText->SetAutoWrapText(true);
	AddV(Card, EmptyText, FMargin(0.0f, 0.0f, 0.0f, 20.0f));

	List = WidgetTree->ConstructWidget<UScrollBox>();
	AddV(Card, MakeSized(*WidgetTree, List, -1.0f, 560.0f));

	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.Variant = EApexButtonVariant::Ghost;
	BackSpec.bCentreLabel = true;
	BackSpec.LabelSize = 17.0f;
	BackSpec.Height = 56.0f;
	BackSpec.ActionId = ActionReplaysBack;
	BackSpec.Sound = EApexUiSound::Back;
	BackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackButton->Setup(BackSpec);
	BackButton->OnActivated.AddDynamic(this, &UApexReplaysWidget::HandleButtonActivated);
	AddV(Card, MakeSized(*WidgetTree, BackButton, 180.0f, -1.0f), FMargin(0.0f, 18.0f, 0.0f, 0.0f), HAlign_Left);

	AddV(Card, MakeKeyHintBar(*WidgetTree,
		{ { TEXT("Enter"), TEXT("Watch") }, { TEXT("K"), TEXT("Keep") }, { TEXT("Del"), TEXT("Delete") } },
		{ { TEXT("Esc"), TEXT("Back") } }), FMargin(0.0f, 18.0f, 0.0f, 0.0f));

	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree,
		MakePanel(*WidgetTree, Card, FMargin(44.0f, 40.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f)),
		ReplaysCardWidth, -1.0f), FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.2f);
	WidgetTree->RootWidget = MakePanel(*WidgetTree, Centre, FMargin(), MakeBrush(Palette::Background));
}

void UApexReplaysWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	Refresh(LastFocused);
}

void UApexReplaysWidget::Refresh(int32 FocusIndex)
{
	Replays = UApexReplayRecorder::ListReplays();
	Rows.Reset();
	if (!List)
	{
		return;
	}
	List->ClearChildren();
	int32 Saved = 0;
	for (int32 Index = 0; Index < Replays.Num(); ++Index)
	{
		const FApexReplayInfo& Info = Replays[Index];
		Saved += Info.bSaved ? 1 : 0;
		FApexButtonSpec Spec;
		Spec.Label = FString::Printf(TEXT("%s  ·  %s"), *Info.Header.Track.DisplayName, ReplayModeName(Info.Header.GameMode));
		TArray<FString> Parts;
		Parts.Add(Info.When.ToString(TEXT("%Y-%m-%d %H:%M")));
		Parts.Add(ReplayDuration(Info.Header.DurationSeconds()));
		Parts.Add(Info.Cars == 1 ? FString(TEXT("1 car")) : FString::Printf(TEXT("%d cars"), Info.Cars));
		if (!Info.Driver.IsEmpty())
		{
			Parts.Add(Info.Driver);
		}
		// The sky in short (Describe's wind direction, clock and forecast ran
		// the line past the badge column).
		const FApexSessionConditions& Sky = Info.Header.Conditions;
		FString SkyText = FString::Printf(TEXT("%s · %s"), *FApexSessionConditions::WeatherLabel(Sky.Weather), *Sky.ClockText());
		if (Sky.HasAirTemp())
		{
			SkyText += FString::Printf(TEXT(" · %d°C"), Sky.AirTempC);
		}
		if (Sky.HasWind() && Sky.WindKph > 0)
		{
			SkyText += FString::Printf(TEXT(" · wind %d km/h"), Sky.WindKph);
		}
		Parts.Add(SkyText);
		Spec.SubLabel = FString::Join(Parts, TEXT("  ·  "));
		Spec.Badge = Info.bSaved ? TEXT("Saved") : TEXT("Recent");
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.LabelSize = 19.0f;
		Spec.Height = 72.0f;
		Spec.ActionId = ActionReplayRow;

		UApexButtonWidget* Row = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Row->Setup(Spec);
		Row->SetBadge(Spec.Badge, Info.bSaved ? ApexUI::Palette::Live : ApexUI::Palette::TextMuted);
		Row->OnActivated.AddDynamic(this, &UApexReplaysWidget::HandleButtonActivated);
		List->AddChild(Row);
		if (UScrollBoxSlot* RowSlot = Cast<UScrollBoxSlot>(Row->Slot))
		{
			RowSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : 2.0f, 0.0f, 0.0f));
		}
		Rows.Add(Row);
	}
	if (EmptyText)
	{
		EmptyText->SetVisibility(Replays.Num() == 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (CountText)
	{
		CountText->SetText(FText::FromString(FString::Printf(TEXT("%d SAVED  ·  %d RECENT  ·  %s"), Saved, Replays.Num() - Saved,
			*FPaths::ConvertRelativePathToFull(UApexReplayRecorder::ReplayDirectory()))));
	}
	LastFocused = Rows.Num() > 0 ? FMath::Clamp(FocusIndex, 0, Rows.Num() - 1) : 0;
	if (IsActiveScreen())
	{
		FocusDefault();
	}
}

void UApexReplaysWidget::FocusDefault()
{
	if (Rows.IsValidIndex(LastFocused) && ApexNav::Focus(Rows[LastFocused]))
	{
		List->ScrollWidgetIntoView(Rows[LastFocused], false);
		return;
	}
	if (!(BackButton && ApexNav::Focus(BackButton)))
	{
		SetKeyboardFocus();
	}
}

int32 UApexReplaysWidget::FocusedRow() const
{
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index] && (Rows[Index]->HasAnyUserFocus() || Rows[Index]->HasFocusedDescendants()))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool UApexReplaysWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	const int32 At = ApexNav::IndexOf(Rows, Source);
	if (At == INDEX_NONE)
	{
		if (Source == BackButton && Direction == EUINavigation::Up && Rows.Num() > 0)
		{
			LastFocused = Rows.Num() - 1;
			FocusDefault();
			return true;
		}
		return false;
	}
	if (Direction == EUINavigation::Up || Direction == EUINavigation::Down)
	{
		const int32 Next = At + (Direction == EUINavigation::Down ? 1 : -1);
		if (Rows.IsValidIndex(Next))
		{
			LastFocused = Next;
			FocusDefault();
		}
		else if (Next >= Rows.Num() && BackButton)
		{
			ApexNav::Focus(BackButton);
		}
		return true;
	}
	return true;
}

FReply UApexReplaysWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const int32 Row = FocusedRow();
	if (Row != INDEX_NONE && (Key == EKeys::Delete || Key == EKeys::Gamepad_FaceButton_Top))
	{
		DeleteRow(Row);
		return FReply::Handled();
	}
	if (Row != INDEX_NONE && (Key == EKeys::K || Key == EKeys::Gamepad_FaceButton_Left))
	{
		KeepRow(Row);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexReplaysWidget::KeepRow(int32 Index)
{
	if (!Replays.IsValidIndex(Index))
	{
		return;
	}
	if (Replays[Index].bSaved)
	{
		ShowToast(TEXT("Already saved"));
		return;
	}
	FString NewPath;
	if (UApexReplayRecorder::KeepReplay(Replays[Index].Path, NewPath))
	{
		ApexUiAudio::Play(this, EApexUiSound::Accept);
		ShowToast(FString::Printf(TEXT("Kept: %s"), *FPaths::GetBaseFilename(NewPath)));
	}
	else
	{
		ShowToast(TEXT("Could not keep this replay"), true);
	}
	Refresh(Index);
}

void UApexReplaysWidget::DeleteRow(int32 Index)
{
	if (!Replays.IsValidIndex(Index))
	{
		return;
	}
	if (UApexReplayRecorder::DeleteReplay(Replays[Index].Path))
	{
		ApexUiAudio::Play(this, EApexUiSound::Back);
		ShowToast(FString::Printf(TEXT("Deleted: %s"), *FPaths::GetBaseFilename(Replays[Index].Path)));
	}
	else
	{
		ShowToast(TEXT("Could not delete this replay"), true);
	}
	Refresh(Index);
}

void UApexReplaysWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (Button == BackButton)
	{
		GoBack();
		return;
	}
	const int32 Index = ApexNav::IndexOf(Rows, Button);
	if (!Replays.IsValidIndex(Index))
	{
		return;
	}
	LastFocused = Index;
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->WatchReplay(Replays[Index].Path);
	}
}
