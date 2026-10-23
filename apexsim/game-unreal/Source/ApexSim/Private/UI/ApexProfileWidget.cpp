#include "UI/ApexProfileWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexPortraitSubsystem.h"
#include "ApexProfileSave.h"
#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/Texture2D.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexContentCardWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionProfileRow(TEXT("Profile.Row"));
	const FName ActionProfileNew(TEXT("Profile.New"));
	const FName ActionProfileDone(TEXT("Profile.Done"));

	constexpr float ProfileCardWidth = 1240.0f;
	/** The portraits are 200 x 308. */
	constexpr float PortraitAspect = 200.0f / 308.0f;
	constexpr int32 PortraitsPerRow = 8;

	/** A texture drawn at its own aspect, letterboxed into a box of Width x Height; a blank panel without one. */
	UWidget* MakePicture(UWidgetTree& Tree, UImage*& OutImage, float Width, float Height)
	{
		OutImage = Tree.ConstructWidget<UImage>();
		UScaleBox* Fit = Tree.ConstructWidget<UScaleBox>();
		Fit->SetStretch(EStretch::ScaleToFit);
		Fit->AddChild(OutImage);
		UBorder* Matte = ApexUI::MakePanel(Tree, Fit, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
		Matte->SetHorizontalAlignment(HAlign_Fill);
		Matte->SetVerticalAlignment(VAlign_Fill);
		return ApexUI::MakeSized(Tree, Matte, Width, Height);
	}

	void ShowTexture(UImage* Image, UTexture2D* Texture)
	{
		if (!Image)
		{
			return;
		}
		if (Texture)
		{
			Image->SetBrushFromTexture(Texture, true);
			Image->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			Image->SetVisibility(ESlateVisibility::Hidden);
		}
	}
}

UApexProfileWidget::UApexProfileWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

TArray<TPair<FString, FString>> UApexProfileWidget::StatLines(const FApexDriverStats& Stats)
{
	TArray<TPair<FString, FString>> Lines;
	Lines.Emplace(TEXT("Laps"), FString::FromInt(Stats.Laps));
	Lines.Emplace(TEXT("Distance"), FString::Printf(TEXT("%.0f km"), Stats.DistanceM / 1000.0));
	Lines.Emplace(TEXT("Races"), FString::FromInt(Stats.Races));
	Lines.Emplace(TEXT("Wins"), FString::FromInt(Stats.Wins));
	Lines.Emplace(TEXT("Podiums"), FString::FromInt(Stats.Podiums));
	Lines.Emplace(TEXT("Incidents"), FString::FromInt(Stats.Incidents));
	return Lines;
}

TArray<UApexProfileWidget::FBestRow> UApexProfileWidget::BestRows(const FApexDriverStats& Stats)
{
	TArray<FBestRow> Rows;
	for (const TPair<FString, float>& Best : Stats.CarBests)
	{
		FString Track, Car;
		if (Best.Value > 0.0f && Best.Key.Split(TEXT("|"), &Track, &Car))
		{
			Rows.Add({Track, Car, Best.Value});
		}
	}
	Rows.Sort([](const FBestRow& A, const FBestRow& B)
	{
		return A.TrackId != B.TrackId ? A.TrackId < B.TrackId : A.CarId < B.CarId;
	});
	return Rows;
}

void UApexProfileWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexProfileWidget::BuildLayout()
{
	using namespace ApexUI;

	auto MakeButton = [this](const FString& Label, FName Action, EApexButtonVariant Variant, float Height)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = Variant;
		Spec.ActionId = Action;
		Spec.Height = Height;
		Spec.bCentreLabel = Variant != EApexButtonVariant::Panel;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexProfileWidget::HandleButtonActivated);
		return Button;
	};

	// Left: the drivers on this machine.
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Left, MakeLabel(*WidgetTree, TEXT("Drivers on this machine")), FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	ProfileList = WidgetTree->ConstructWidget<UVerticalBox>();
	UScrollBox* ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	ListScroll->AddChild(ProfileList);
	AddV(Left, ListScroll, FMargin(), HAlign_Fill, 1.0f);
	NewButton = MakeButton(TEXT("New driver"), ActionProfileNew, EApexButtonVariant::Ghost, 52.0f);
	AddV(Left, NewButton, FMargin(0.0f, 12.0f, 0.0f, 0.0f));
	UTextBlock* DeleteHint = MakeText(*WidgetTree, TEXT("DELETE (PAD Y) TWICE ON ANOTHER DRIVER REMOVES THEM"), Font::Mono(10.0f), Palette::TextMuted);
	DeleteHint->SetAutoWrapText(true);
	AddV(Left, DeleteHint, FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	// Right: the driver in use.
	UVerticalBox* Right = WidgetTree->ConstructWidget<UVerticalBox>();
	UHorizontalBox* Who = WidgetTree->ConstructWidget<UHorizontalBox>();
	UImage* Big = nullptr;
	AddH(Who, MakePicture(*WidgetTree, Big, 150.0f, 150.0f / PortraitAspect), FMargin(), VAlign_Top);
	PortraitImage = Big;
	UVerticalBox* NameColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(NameColumn, MakeLabel(*WidgetTree, TEXT("Driver name")));
	NameField = MakeSearchBox(*WidgetTree, TEXT("Your name on the timing screens"));
	FEditableTextBoxStyle NameStyle = NameField->GetWidgetStyle();
	NameStyle.SetFont(Font::Body(17.0f));
	NameField->SetWidgetStyle(NameStyle);
	NameField->OnTextCommitted.AddDynamic(this, &UApexProfileWidget::HandleNameCommitted);
	AddV(NameColumn, MakeSized(*WidgetTree, NameField, 360.0f, 48.0f), FMargin(0.0f, 8.0f, 0.0f, 18.0f), HAlign_Left);
	StatsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddV(NameColumn, StatsRow);
	AddH(Who, NameColumn, FMargin(24.0f, 0.0f, 0.0f, 0.0f), VAlign_Top, 1.0f);
	AddV(Right, Who);

	AddV(Right, MakeLabel(*WidgetTree, TEXT("Picture")), FMargin(0.0f, 22.0f, 0.0f, 10.0f));
	PortraitGrid = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Right, PortraitGrid);

	AddV(Right, MakeLabel(*WidgetTree, TEXT("Personal bests by circuit and car")), FMargin(0.0f, 22.0f, 0.0f, 10.0f));
	BestList = WidgetTree->ConstructWidget<UScrollBox>();
	AddV(Right, BestList, FMargin(), HAlign_Fill, 1.0f);

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	AddH(Columns, MakeSized(*WidgetTree, Left, 300.0f, -1.0f), FMargin(), VAlign_Fill);
	AddH(Columns, MakeDivider(*WidgetTree, true), FMargin(28.0f, 0.0f), VAlign_Fill);
	AddH(Columns, Right, FMargin(), VAlign_Fill, 1.0f);

	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Card, MakeLabel(*WidgetTree, TEXT("Driver profile"), Palette::Accent));
	HeadingText = MakeText(*WidgetTree, TEXT("Profile"), Font::Display(42.0f), Palette::TextPrimary);
	AddV(Card, HeadingText, FMargin(0.0f, 10.0f, 0.0f, 4.0f));
	StatusText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	AddV(Card, StatusText, FMargin(0.0f, 0.0f, 0.0f, 22.0f));
	AddV(Card, MakeSized(*WidgetTree, Columns, -1.0f, 660.0f));
	DoneButton = MakeButton(TEXT("Back"), ActionProfileDone, EApexButtonVariant::Primary, 56.0f);
	AddV(Card, MakeSized(*WidgetTree, DoneButton, 280.0f, -1.0f), FMargin(0.0f, 20.0f, 0.0f, 0.0f), HAlign_Right);

	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree,
		MakePanel(*WidgetTree, Card, FMargin(44.0f, 36.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f)),
		ProfileCardWidth, -1.0f), FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.2f);
	WidgetTree->RootWidget = MakePanel(*WidgetTree, Centre, FMargin(), MakeBrush(Palette::Background));
}

void UApexProfileWidget::OnScreenActivated()
{
	Super::OnScreenActivated();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (Flow && !bBound)
	{
		Flow->OnProfileChanged.AddDynamic(this, &UApexProfileWidget::HandleProfileChanged);
		bBound = true;
	}
	PendingDelete.Invalidate();
	RebuildPortraits();
	Rebuild();
	FocusDefault();
}

void UApexProfileWidget::OnScreenDeactivated()
{
	Super::OnScreenDeactivated();
	CommitName();
	if (UApexMenuFlowSubsystem* Flow = GetFlow(); Flow && bBound)
	{
		Flow->OnProfileChanged.RemoveDynamic(this, &UApexProfileWidget::HandleProfileChanged);
		bBound = false;
	}
	bPrompt = false;
}

void UApexProfileWidget::FocusDefault()
{
	// The driver in use, so Enter drives on as them.
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const int32 Current = Flow ? ProfileButtonIds.IndexOfByKey(Flow->GetProfileId()) : INDEX_NONE;
	if (!(ProfileButtons.IsValidIndex(Current) && ApexNav::Focus(ProfileButtons[Current])))
	{
		if (!(DoneButton && ApexNav::Focus(DoneButton)))
		{
			SetKeyboardFocus();
		}
	}
}

bool UApexProfileWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	// Columns, a grid of pictures and a field: by where things are drawn.
	return ApexNav::MoveToward(this, Direction, Source);
}

bool UApexProfileWidget::HandleBack()
{
	CommitName();
	if (bPrompt)
	{
		// Asked who drives: Back drives on as the one picked.
		DriveOn();
		return true;
	}
	return Super::HandleBack();
}

void UApexProfileWidget::DriveOn()
{
	// The launch's question answered: the menu, with nothing to go back to.
	bPrompt = false;
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ReplaceScreen(EApexScreen::MainMenu);
	}
}

void UApexProfileWidget::Rebuild()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}
	HeadingText->SetText(FText::FromString(bPrompt ? TEXT("Who is driving?") : Flow->PlayerName));
	DoneButton->SetLabel(bPrompt ? FString::Printf(TEXT("Drive as %s"), *Flow->PlayerName) : FString(TEXT("Back")));
	StatusText->SetText(FText::FromString(PendingDelete.IsValid()
		? FString(TEXT("PRESS DELETE AGAIN TO REMOVE THAT DRIVER AND EVERYTHING THEY SAVED"))
		: FString::Printf(TEXT("%d OF %d DRIVERS  ·  KEPT ON THIS MACHINE ONLY  ·  ID %s"),
			Flow->GetProfiles().Num(), ApexProfiles::MaxProfiles, *ApexProfiles::WireId(Flow->GetProfileId()).Left(8).ToUpper())));
	if (NameField && !NameField->HasKeyboardFocus())
	{
		NameField->SetText(FText::FromString(Flow->PlayerName));
	}
	UApexPortraitSubsystem* Portraits = UApexPortraitSubsystem::Get(this);
	ShowTexture(PortraitImage, Portraits ? Portraits->GetPortrait(Flow->GetPortrait()) : nullptr);
	for (int32 Index = 0; Index < PortraitCards.Num(); ++Index)
	{
		PortraitCards[Index]->SetSelected(Index + 1 == Flow->GetPortrait());
	}
	if (NewButton)
	{
		NewButton->SetVisibility(Flow->GetProfiles().Num() < ApexProfiles::MaxProfiles ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	RebuildList();
	RebuildStats();
}

void UApexProfileWidget::RebuildList()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !ProfileList)
	{
		return;
	}
	ProfileList->ClearChildren();
	ProfileButtons.Reset();
	ProfileButtonIds.Reset();
	for (const FApexProfileEntry& Entry : Flow->GetProfiles())
	{
		const bool bCurrent = Entry.Id == Flow->GetProfileId();
		FApexButtonSpec Spec;
		Spec.Label = Entry.Name;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.ActionId = ActionProfileRow;
		Spec.Height = 56.0f;
		Spec.Badge = bCurrent ? TEXT("DRIVING") : Entry.Id == PendingDelete ? TEXT("DELETE?") : FString();
		Spec.BadgeColour = Entry.Id == PendingDelete ? ApexUI::Palette::Error : ApexUI::Palette::Accent;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->SetSelected(bCurrent);
		Button->OnActivated.AddDynamic(this, &UApexProfileWidget::HandleButtonActivated);
		ApexUI::AddV(ProfileList, Button, FMargin(0.0f, ProfileButtons.Num() == 0 ? 0.0f : 6.0f, 0.0f, 0.0f));
		ProfileButtons.Add(Button);
		ProfileButtonIds.Add(Entry.Id);
	}
}

void UApexProfileWidget::RebuildPortraits()
{
	if (!PortraitGrid || PortraitCards.Num() > 0)
	{
		return;
	}
	UApexPortraitSubsystem* Portraits = UApexPortraitSubsystem::Get(this);
	const int32 Count = Portraits ? Portraits->Num() : 0;
	UHorizontalBox* Row = nullptr;
	for (int32 Index = 1; Index <= Count; ++Index)
	{
		if ((Index - 1) % PortraitsPerRow == 0)
		{
			Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			ApexUI::AddV(PortraitGrid, Row, FMargin(0.0f, Index == 1 ? 0.0f : 8.0f, 0.0f, 0.0f), HAlign_Left);
		}
		FApexCardSpec Spec;
		Spec.Id = FString::FromInt(Index);
		Spec.Preview = Portraits->GetPortrait(Index);
		Spec.PreviewHeight = 96.0f;
		UApexContentCardWidget* Card = WidgetTree->ConstructWidget<UApexContentCardWidget>();
		Card->Setup(Spec);
		Card->OnActivated.AddDynamic(this, &UApexProfileWidget::HandlePortraitActivated);
		ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Card, 84.0f, -1.0f), FMargin((Index - 1) % PortraitsPerRow == 0 ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f));
		PortraitCards.Add(Card);
	}
	if (Count == 0)
	{
		ApexUI::AddV(PortraitGrid, ApexUI::MakeText(*WidgetTree,
			FString::Printf(TEXT("NO PICTURES IN %s"), *UApexPortraitSubsystem::Folder().ToUpper()),
			ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted));
	}
}

void UApexProfileWidget::RebuildStats()
{
	using namespace ApexUI;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !StatsRow || !BestList)
	{
		return;
	}
	const FApexDriverStats& Stats = Flow->GetStats();
	StatsRow->ClearChildren();
	bool bFirst = true;
	for (const TPair<FString, FString>& Line : StatLines(Stats))
	{
		UTextBlock* Value = nullptr;
		AddH(StatsRow, MakeStat(*WidgetTree, Line.Key, Value, 21.0f), FMargin(bFirst ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		Value->SetText(FText::FromString(Line.Value));
		bFirst = false;
	}

	BestList->ClearChildren();
	const TArray<FBestRow> Rows = BestRows(Stats);
	if (Rows.Num() == 0)
	{
		BestList->AddChild(MakeText(*WidgetTree, TEXT("NO LAPS YET: EVERY SESSION YOU DRIVE ADDS TO THESE"), Font::Mono(11.0f), Palette::TextMuted));
	}
	for (const FBestRow& Row : Rows)
	{
		FApexTrackCatalogRow Track;
		FApexCarCatalogRow Car;
		const FString TrackName = Flow->GetTrackCatalogRow(Row.TrackId, Track) && !Track.DisplayName.IsEmpty() ? Track.DisplayName : FString(TEXT("Unknown circuit"));
		const FString CarName = Flow->GetCarCatalogRow(Row.CarId, Car) && !Car.DisplayName.IsEmpty() ? Car.DisplayName : FString(TEXT("Unknown car"));
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Line, MakeText(*WidgetTree, TrackName, Font::Body(15.0f), Palette::TextPrimary), FMargin(), VAlign_Center, 1.0f);
		AddH(Line, MakeSized(*WidgetTree, MakeText(*WidgetTree, CarName, Font::Body(14.0f), Palette::TextSecondary), 260.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		UTextBlock* Time = MakeText(*WidgetTree, UApexMenuFlowSubsystem::FormatLapTime(Row.Seconds), Font::Mono(15.0f), Palette::Accent);
		Time->SetJustification(ETextJustify::Right);
		AddH(Line, MakeSized(*WidgetTree, Time, 110.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
		UWidget* Panel = MakePanel(*WidgetTree, Line, FMargin(12.0f, 7.0f), MakeBrush(Palette::Background));
		BestList->AddChild(Panel);
		if (UScrollBoxSlot* RowSlot = Cast<UScrollBoxSlot>(Panel->Slot))
		{
			RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
		}
	}
}

void UApexProfileWidget::CommitName()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (Flow && NameField)
	{
		const FString Typed = NameField->GetText().ToString();
		if (!Typed.TrimStartAndEnd().IsEmpty() && ApexProfiles::CleanName(Typed) != Flow->PlayerName)
		{
			Flow->SetDriverName(Typed);
		}
	}
}

void UApexProfileWidget::HandleNameCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnCleared)
	{
		CommitName();
	}
}

void UApexProfileWidget::HandleProfileChanged()
{
	if (IsActiveScreen())
	{
		Rebuild();
	}
}

void UApexProfileWidget::HandlePortraitActivated(UApexContentCardWidget* Card)
{
	if (UApexMenuFlowSubsystem* Flow = GetFlow(); Flow && Card)
	{
		Flow->SetPortrait(FCString::Atoi(*Card->GetCardId()));
	}
}

FReply UApexProfileWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Delete || Key == EKeys::Gamepad_FaceButton_Top)
	{
		UApexMenuFlowSubsystem* Flow = GetFlow();
		const int32 Focused = ProfileButtons.IndexOfByPredicate([](const TObjectPtr<UApexButtonWidget>& Button)
		{
			return Button && (Button->HasAnyUserFocus() || Button->HasFocusedDescendants());
		});
		if (Flow && ProfileButtonIds.IsValidIndex(Focused) && ProfileButtonIds[Focused] != Flow->GetProfileId())
		{
			const FGuid Id = ProfileButtonIds[Focused];
			if (PendingDelete == Id)
			{
				PendingDelete.Invalidate();
				Flow->DeleteProfile(Id);
				ApexUiAudio::Play(this, EApexUiSound::Back);
				Rebuild();
				FocusDefault();
			}
			else
			{
				PendingDelete = Id;
				ApexUiAudio::Play(this, EApexUiSound::Adjust);
				Rebuild();
				if (ProfileButtons.IsValidIndex(Focused))
				{
					ApexNav::Focus(ProfileButtons[Focused]);
				}
			}
			return FReply::Handled();
		}
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UApexProfileWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow || !Button)
	{
		return;
	}
	PendingDelete.Invalidate();
	if (Button == DoneButton)
	{
		CommitName();
		if (bPrompt)
		{
			DriveOn();
		}
		else
		{
			GoBack();
		}
		return;
	}
	if (Button == NewButton)
	{
		CommitName();
		// The next picture nobody here has, and a name to type over.
		TSet<int32> Taken;
		for (const FApexProfileEntry& Entry : Flow->GetProfiles())
		{
			Taken.Add(Entry.Portrait);
		}
		const int32 Count = UApexPortraitSubsystem::Get(this) ? UApexPortraitSubsystem::Get(this)->Num() : 0;
		int32 Portrait = 0;
		for (int32 Index = 1; Index <= Count && Portrait == 0; ++Index)
		{
			Portrait = Taken.Contains(Index) ? 0 : Index;
		}
		if (Flow->CreateProfile(FString::Printf(TEXT("Driver %d"), Flow->GetProfiles().Num() + 1), Portrait).IsValid())
		{
			Rebuild();
			if (NameField)
			{
				NameField->SetKeyboardFocus();
			}
		}
		return;
	}
	const int32 Row = ProfileButtons.IndexOfByKey(Button);
	if (ProfileButtonIds.IsValidIndex(Row))
	{
		CommitName();
		if (ProfileButtonIds[Row] == Flow->GetProfileId())
		{
			if (bPrompt)
			{
				DriveOn();
			}
			return;
		}
		Flow->SwitchProfile(ProfileButtonIds[Row]);
		Rebuild();
		FocusDefault();
	}
}
