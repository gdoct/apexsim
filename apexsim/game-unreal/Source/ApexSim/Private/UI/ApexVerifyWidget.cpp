#include "UI/ApexVerifyWidget.h"

#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexUIStyle.h"

namespace ApexVerifyUi
{
	constexpr float CardWidth = 1060.0f;
	constexpr float BriefWidth = 560.0f;
	/** How long the block's brief stays up once the clock starts. */
	constexpr float BriefS = 45.0f;
	/** What "more time" adds. */
	constexpr float MoreTimeS = 180.0f;
	/** The most answer words a check has (sittings.yml). */
	constexpr int32 MaxAnswers = 6;
	/** Lines of the brief's check list before "and N more". */
	constexpr int32 BriefCheckLines = 9;

	const TCHAR* const KeyRows[] = {
		TEXT("1234567890"),
		TEXT("qwertyuiop"),
		TEXT("asdfghjkl'"),
		TEXT("zxcvbnm,.?"),
	};

	FString Clock(float Seconds)
	{
		const int32 Whole = FMath::CeilToInt(FMath::Max(0.0f, Seconds));
		return FString::Printf(TEXT("%d:%02d"), Whole / 60, Whole % 60);
	}

	FString Now()
	{
		return FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S"));
	}
}

UApexVerifyWidget::UApexVerifyWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexVerifyWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	SetVisibility(ESlateVisibility::Collapsed);

	const FString Path = ApexVerifyCard::CommandLinePath();
	if (Path.IsEmpty())
	{
		return;
	}
	FString Error;
	if (!ApexVerifyCard::Load(Path, Card, Error))
	{
		UE_LOG(LogApexSim, Warning, TEXT("-ApexVerify: %s (%s); no verify card"), *Path, *Error);
		return;
	}
	bActive = true;
	Answers.SetNum(Card.Checks.Num());
	DueAtS = Card.Minutes * 60.0f;
	UE_LOG(LogApexSim, Log, TEXT("-ApexVerify: block %s, %d check(s), due after %.1f min of %s"), *Card.Block,
		Card.Checks.Num(), Card.Minutes, Card.bRace ? TEXT("the race view") : TEXT("the game"));

	BriefTitle->SetText(FText::FromString(Card.About));
	BriefSetup->SetText(FText::FromString(Card.Setup));
	FString List;
	for (int32 Index = 0; Index < Card.Checks.Num() && Index < ApexVerifyUi::BriefCheckLines; ++Index)
	{
		List += FString::Printf(TEXT("·  %s\n"), *Card.Checks[Index].Check);
	}
	if (Card.Checks.Num() > ApexVerifyUi::BriefCheckLines)
	{
		List += FString::Printf(TEXT("   and %d more\n"), Card.Checks.Num() - ApexVerifyUi::BriefCheckLines);
	}
	BriefChecks->SetText(FText::FromString(List.TrimEnd()));
	ClockKicker->SetText(FText::FromString(FString::Printf(TEXT("VERIFY  ·  %s"), *Card.Block.ToUpper())));
	RefreshVisibility();
}

void UApexVerifyWidget::BuildLayout()
{
	using namespace ApexUI;
	auto MakeButton = [this](const FString& Label, EApexButtonVariant Variant, float Height, float LabelSize)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = Variant;
		Spec.Height = Height;
		Spec.bCentreLabel = true;
		Spec.LabelSize = LabelSize;
		Spec.Sound = EApexUiSound::Accept;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexVerifyWidget::HandleButtonActivated);
		return Button;
	};
	FLinearColor Surface = Palette::Surface;
	Surface.A = 0.94f;

	// --- The clock, top right ----------------------------------------------------
	{
		UVerticalBox* Clock = WidgetTree->ConstructWidget<UVerticalBox>();
		ClockKicker = MakeLabel(*WidgetTree, FString(), Palette::Accent);
		AddV(Clock, ClockKicker, FMargin(), HAlign_Right);
		ClockText = MakeText(*WidgetTree, FString(), Font::Display(34.0f), Palette::TextPrimary);
		ClockText->SetJustification(ETextJustify::Right);
		AddV(Clock, ClockText, FMargin(0.0f, 2.0f, 0.0f, 0.0f), HAlign_Right);
		ClockNote = MakeText(*WidgetTree, FString(), Font::Mono(11.0f), Palette::TextMuted);
		ClockNote->SetJustification(ETextJustify::Right);
		AddV(Clock, ClockNote, FMargin(0.0f, 2.0f, 0.0f, 0.0f), HAlign_Right);
		ClockPanel = MakePanel(*WidgetTree, Clock, FMargin(18.0f, 10.0f), MakeBrush(Surface, Palette::Border, 1.0f));
	}

	// --- The brief, left ---------------------------------------------------------
	{
		UVerticalBox* Brief = WidgetTree->ConstructWidget<UVerticalBox>();
		AddV(Brief, MakeLabel(*WidgetTree, TEXT("Verify  ·  this block"), Palette::Accent));
		BriefTitle = MakeText(*WidgetTree, FString(), Font::Body(20.0f, true), Palette::TextPrimary);
		BriefTitle->SetAutoWrapText(true);
		AddV(Brief, BriefTitle, FMargin(0.0f, 8.0f, 0.0f, 6.0f));
		BriefSetup = MakeText(*WidgetTree, FString(), Font::Body(14.0f), Palette::TextSecondary);
		BriefSetup->SetAutoWrapText(true);
		AddV(Brief, BriefSetup, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
		AddV(Brief, MakeLabel(*WidgetTree, TEXT("Asked when the time is up")));
		BriefChecks = MakeText(*WidgetTree, FString(), Font::Body(13.0f), Palette::TextSecondary);
		BriefChecks->SetAutoWrapText(true);
		AddV(Brief, BriefChecks, FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		BriefPanel = MakeSized(*WidgetTree, MakePanel(*WidgetTree, Brief, FMargin(24.0f, 20.0f), MakeBrush(Surface, Palette::Border, 1.0f)),
			ApexVerifyUi::BriefWidth, -1.0f);
	}

	// --- The card ----------------------------------------------------------------
	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
	KickerText = MakeLabel(*WidgetTree, FString(), Palette::Accent);
	AddV(Content, KickerText);
	TitleText = MakeText(*WidgetTree, FString(), Font::Display(30.0f), Palette::TextPrimary);
	TitleText->SetAutoWrapText(true);
	AddV(Content, TitleText, FMargin(0.0f, 10.0f, 0.0f, 6.0f));
	DetailText = MakeText(*WidgetTree, FString(), Font::Body(16.0f), Palette::TextSecondary);
	DetailText->SetAutoWrapText(true);
	AddV(Content, DetailText);
	BeforeText = MakeText(*WidgetTree, FString(), Font::Mono(12.0f), Palette::TextMuted);
	AddV(Content, BeforeText, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	// One check: its answer words, its note, the way on.
	{
		UVerticalBox* Check = WidgetTree->ConstructWidget<UVerticalBox>();
		UHorizontalBox* Words = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (int32 Index = 0; Index < ApexVerifyUi::MaxAnswers; ++Index)
		{
			UApexButtonWidget* Button = MakeButton(FString(), EApexButtonVariant::Ghost, 72.0f, 20.0f);
			AnswerButtons.Add(Button);
			AddH(Words, Button, FMargin(Index > 0 ? 8.0f : 0.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		AddV(Check, Words, FMargin(0.0f, 22.0f, 0.0f, 0.0f));

		UHorizontalBox* NoteRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		NoteText = MakeText(*WidgetTree, FString(), Font::Mono(14.0f), Palette::TextSecondary);
		NoteText->SetAutoWrapText(true);
		AddH(NoteRow, NoteText, FMargin(0.0f, 0.0f, 12.0f, 0.0f), VAlign_Center, 1.0f);
		NoteButton = MakeButton(TEXT("Note"), EApexButtonVariant::Ghost, 52.0f, 15.0f);
		AddH(NoteRow, MakeSized(*WidgetTree, NoteButton, 150.0f, -1.0f));
		FlagButton = MakeButton(TEXT("Flag"), EApexButtonVariant::Ghost, 52.0f, 15.0f);
		AddH(NoteRow, MakeSized(*WidgetTree, FlagButton, 130.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
		AddV(Check, NoteRow, FMargin(0.0f, 16.0f, 0.0f, 0.0f));

		UVerticalBox* Keys = WidgetTree->ConstructWidget<UVerticalBox>();
		BuildKeyboard(Keys);
		KeyboardPanel = Keys;
		KeyboardPanel->SetVisibility(ESlateVisibility::Collapsed);
		AddV(Check, KeyboardPanel, FMargin(0.0f, 12.0f, 0.0f, 0.0f));

		UHorizontalBox* Way = WidgetTree->ConstructWidget<UHorizontalBox>();
		PrevButton = MakeButton(TEXT("Back"), EApexButtonVariant::Ghost, 52.0f, 15.0f);
		AddH(Way, MakeSized(*WidgetTree, PrevButton, 150.0f, -1.0f));
		SkipButton = MakeButton(TEXT("Skip"), EApexButtonVariant::Ghost, 52.0f, 15.0f);
		AddH(Way, MakeSized(*WidgetTree, SkipButton, 150.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
		AddH(Way, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);
		DriveButton = MakeButton(TEXT("Drive on"), EApexButtonVariant::Ghost, 52.0f, 15.0f);
		AddH(Way, MakeSized(*WidgetTree, DriveButton, 200.0f, -1.0f));
		AddV(Check, Way, FMargin(0.0f, 22.0f, 0.0f, 0.0f));
		CheckPanel = Check;
		AddV(Content, Check);
	}

	// The end: what was answered, and what comes next.
	{
		UVerticalBox* Summary = WidgetTree->ConstructWidget<UVerticalBox>();
		SummaryText = MakeText(*WidgetTree, FString(), Font::Mono(13.0f), Palette::TextSecondary);
		SummaryText->SetAutoWrapText(true);
		AddV(Summary, SummaryText, FMargin(0.0f, 16.0f, 0.0f, 0.0f));
		UHorizontalBox* Way = WidgetTree->ConstructWidget<UHorizontalBox>();
		BackToChecksButton = MakeButton(TEXT("Back"), EApexButtonVariant::Ghost, 60.0f, 15.0f);
		AddH(Way, MakeSized(*WidgetTree, BackToChecksButton, 140.0f, -1.0f));
		MoreTimeButton = MakeButton(TEXT("3 more minutes"), EApexButtonVariant::Ghost, 60.0f, 15.0f);
		AddH(Way, MakeSized(*WidgetTree, MoreTimeButton, 220.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
		AddH(Way, WidgetTree->ConstructWidget<USpacer>(), FMargin(), VAlign_Fill, 1.0f);
		StopButton = MakeButton(TEXT("Stop here"), EApexButtonVariant::Ghost, 60.0f, 15.0f);
		AddH(Way, MakeSized(*WidgetTree, StopButton, 170.0f, -1.0f));
		NextBlockButton = MakeButton(TEXT("Next block"), EApexButtonVariant::Primary, 60.0f, 17.0f);
		AddH(Way, MakeSized(*WidgetTree, NextBlockButton, 240.0f, -1.0f), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
		AddV(Summary, Way, FMargin(0.0f, 24.0f, 0.0f, 0.0f));
		SummaryPanel = Summary;
		SummaryPanel->SetVisibility(ESlateVisibility::Collapsed);
		AddV(Content, Summary);
	}

	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	AddV(Centre, MakeSized(*WidgetTree, MakeModalCard(*WidgetTree, Content, FMargin(40.0f, 30.0f)), ApexVerifyUi::CardWidth, -1.0f),
		FMargin(), HAlign_Center);
	AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	CardHost = MakeScrim(*WidgetTree, Centre, 6.0f);

	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* CardSlot = Root->AddChildToOverlay(CardHost);
	CardSlot->SetHorizontalAlignment(HAlign_Fill);
	CardSlot->SetVerticalAlignment(VAlign_Fill);
	UOverlaySlot* BriefSlot = Root->AddChildToOverlay(BriefPanel);
	BriefSlot->SetHorizontalAlignment(HAlign_Left);
	BriefSlot->SetVerticalAlignment(VAlign_Center);
	BriefSlot->SetPadding(FMargin(40.0f, 0.0f, 0.0f, 0.0f));
	UOverlaySlot* ClockSlot = Root->AddChildToOverlay(ClockPanel);
	ClockSlot->SetHorizontalAlignment(HAlign_Right);
	ClockSlot->SetVerticalAlignment(VAlign_Top);
	ClockSlot->SetPadding(FMargin(0.0f, 200.0f, 36.0f, 0.0f));
	WidgetTree->RootWidget = Root;
}

void UApexVerifyWidget::BuildKeyboard(UVerticalBox* Host)
{
	using namespace ApexUI;
	auto MakeKey = [this](const FString& Label, float Width)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = EApexButtonVariant::Ghost;
		Spec.Height = 46.0f;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 16.0f;
		Spec.Sound = EApexUiSound::Adjust;
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexVerifyWidget::HandleButtonActivated);
		return MakeSized(*WidgetTree, Button, Width, -1.0f);
	};
	constexpr float KeyWidth = 62.0f;
	constexpr float Gap = 6.0f;
	for (const TCHAR* Row : ApexVerifyUi::KeyRows)
	{
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (const TCHAR* Char = Row; *Char; ++Char)
		{
			const FString Text(1, Char);
			USizeBox* Key = MakeKey(Text, KeyWidth);
			KeyButtons.Add(Cast<UApexButtonWidget>(Key->GetContent()));
			KeyChars.Add(Text);
			AddH(Line, Key, FMargin(Line->GetChildrenCount() > 0 ? Gap : 0.0f, 0.0f, 0.0f, 0.0f));
		}
		AddV(Host, Line, FMargin(0.0f, Host->GetChildrenCount() > 0 ? Gap : 0.0f, 0.0f, 0.0f), HAlign_Center);
	}
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
	USizeBox* Shift = MakeKey(TEXT("Shift"), 120.0f);
	ShiftKey = Cast<UApexButtonWidget>(Shift->GetContent());
	AddH(Line, Shift);
	USizeBox* Space = MakeKey(TEXT("Space"), 300.0f);
	SpaceKey = Cast<UApexButtonWidget>(Space->GetContent());
	AddH(Line, Space, FMargin(Gap, 0.0f, 0.0f, 0.0f));
	USizeBox* Delete = MakeKey(TEXT("Delete"), 120.0f);
	DeleteKey = Cast<UApexButtonWidget>(Delete->GetContent());
	AddH(Line, Delete, FMargin(Gap, 0.0f, 0.0f, 0.0f));
	USizeBox* Clear = MakeKey(TEXT("Clear"), 100.0f);
	ClearKey = Cast<UApexButtonWidget>(Clear->GetContent());
	AddH(Line, Clear, FMargin(Gap, 0.0f, 0.0f, 0.0f));
	USizeBox* Done = MakeKey(TEXT("Done"), 110.0f);
	DoneKey = Cast<UApexButtonWidget>(Done->GetContent());
	AddH(Line, Done, FMargin(Gap, 0.0f, 0.0f, 0.0f));
	AddV(Host, Line, FMargin(0.0f, Gap, 0.0f, 0.0f), HAlign_Center);
}

void UApexVerifyWidget::SetDriving(bool bInDriving, bool bInInRaceView)
{
	bDriving = bInDriving;
	bInRaceView = bInInRaceView;
	bHasDriven |= bInDriving;
}

bool UApexVerifyWidget::WantsCard() const
{
	if (!bActive || bCardOpen)
	{
		return false;
	}
	// A race block whose race is over or was left: there is nothing more to drive.
	if (Card.bRace && bHasDriven && !bInRaceView)
	{
		return true;
	}
	return bDue && !bDriving;
}

void UApexVerifyWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!bActive)
	{
		return;
	}
	if (!bClockRunning && (!Card.bRace || bDriving))
	{
		bClockRunning = true;
		BriefLeftS = ApexVerifyUi::BriefS;
	}
	if (bClockRunning && !bCardOpen)
	{
		if (!Card.bRace || bInRaceView)
		{
			ElapsedS += InDeltaTime;
		}
		BriefLeftS = FMath::Max(0.0f, BriefLeftS - InDeltaTime);
		if (!bDue && ElapsedS >= DueAtS)
		{
			bDue = true;
			BriefLeftS = 0.0f;
			UE_LOG(LogApexSim, Log, TEXT("Verify: block %s is due (%.0f s driven)"), *Card.Block, ElapsedS);
			ApexUiAudio::Play(this, EApexUiSound::Notice);
		}
	}
	RefreshVisibility();
	RefreshClock();
}

void UApexVerifyWidget::RefreshClock()
{
	if (!ClockPanel || ClockPanel->GetVisibility() == ESlateVisibility::Collapsed)
	{
		return;
	}
	if (bDue)
	{
		ClockText->SetText(FText::FromString(TEXT("TIME'S UP")));
		ClockText->SetColorAndOpacity(ApexUI::Palette::Accent);
		ClockNote->SetText(FText::FromString(bDriving ? TEXT("PAUSE TO ANSWER") : TEXT("ANSWERING")));
		return;
	}
	ClockText->SetText(FText::FromString(ApexVerifyUi::Clock(DueAtS - ElapsedS)));
	ClockText->SetColorAndOpacity(ApexUI::Palette::TextPrimary);
	ClockNote->SetText(FText::FromString(FString::Printf(TEXT("%d CHECKS  ·  PAUSE > VERIFY TO ANSWER NOW"), Card.Checks.Num())));
}

void UApexVerifyWidget::RefreshVisibility()
{
	const bool bClock = bActive && bClockRunning && !bCardOpen;
	const bool bBrief = bActive && bClockRunning && !bCardOpen && BriefLeftS > 0.0f;
	if (ClockPanel)
	{
		ClockPanel->SetVisibility(bClock ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (BriefPanel)
	{
		BriefPanel->SetVisibility(bBrief ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (CardHost)
	{
		CardHost->SetVisibility(bCardOpen ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	// Never collapsed while there is a card: a collapsed widget does not tick,
	// and the tick is the clock.
	const ESlateVisibility Wanted = bCardOpen ? ESlateVisibility::SelfHitTestInvisible
		: (bActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (GetVisibility() != Wanted)
	{
		SetVisibility(Wanted);
	}
}

void UApexVerifyWidget::OpenCard()
{
	if (!bActive || bCardOpen)
	{
		return;
	}
	bCardOpen = true;
	RefreshVisibility();
	// Onto the first check not answered yet; the end when every one is.
	const int32 First = Answers.IndexOfByPredicate([](const FApexVerifyAnswer& Answer) { return Answer.Answer.IsEmpty(); });
	ShowPage(First == INDEX_NONE ? Card.Checks.Num() : First);
	ApexUiAudio::Play(this, EApexUiSound::Notice);
}

void UApexVerifyWidget::CloseCard()
{
	if (!bCardOpen)
	{
		return;
	}
	SetKeyboardOpen(false);
	bCardOpen = false;
	Save(EApexVerifyNext::Stop);
	RefreshVisibility();
}

FString UApexVerifyWidget::GetBadge() const
{
	if (!bActive)
	{
		return FString();
	}
	const FString Answered = FString::Printf(TEXT("%d of %d answered"), AnsweredCount(), Card.Checks.Num());
	return bDue ? Answered : FString::Printf(TEXT("%s left  ·  %s"), *ApexVerifyUi::Clock(DueAtS - ElapsedS), *Answered);
}

int32 UApexVerifyWidget::AnsweredCount() const
{
	return Answers.FilterByPredicate([](const FApexVerifyAnswer& Answer) { return !Answer.Answer.IsEmpty(); }).Num();
}

void UApexVerifyWidget::Save(EApexVerifyNext Next)
{
	ApexVerifyCard::SaveAnswers(Card, Answers, Next);
}

void UApexVerifyWidget::ShowPage(int32 InPage)
{
	Page = FMath::Clamp(InPage, 0, Card.Checks.Num());
	SetKeyboardOpen(false);
	Refresh();
	FocusDefault();
}

void UApexVerifyWidget::Refresh()
{
	if (!bActive || !TitleText)
	{
		return;
	}
	const int32 Count = Card.Checks.Num();
	CheckPanel->SetVisibility(IsSummary() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	SummaryPanel->SetVisibility(IsSummary() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	// Driving on only means something in a race still to drive; more time
	// also in a block with no race (the bench), where the card waits for it.
	DriveButton->SetVisibility(Card.bRace && bInRaceView ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	MoreTimeButton->SetVisibility(!Card.bRace || bInRaceView ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

	if (IsSummary())
	{
		KickerText->SetText(FText::FromString(FString::Printf(TEXT("VERIFY  ·  %s  ·  DONE"), *Card.Block.ToUpper())));
		TitleText->SetText(FText::FromString(FString::Printf(TEXT("%d of %d answered"), AnsweredCount(), Count)));
		DetailText->SetText(FText::FromString(TEXT("Saved to the verifier when the game closes. Next block starts the runner's next one; Stop here ends the sitting.")));
		BeforeText->SetText(FText::GetEmpty());
		FString Lines;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FApexVerifyAnswer& Answer = Answers[Index];
			FString Line = FString::Printf(TEXT("%-12s %s"), Answer.Answer.IsEmpty() ? TEXT("-") : *Answer.Answer, *Card.Checks[Index].Check);
			if (!Answer.Note.TrimStartAndEnd().IsEmpty())
			{
				Line += FString::Printf(TEXT("  (%s)"), *Answer.Note.TrimStartAndEnd());
			}
			if (Answer.bFlag)
			{
				Line += TEXT("  [flag]");
			}
			Lines += Line + TEXT("\n");
		}
		SummaryText->SetText(FText::FromString(Lines.TrimEnd()));
		return;
	}

	const FApexVerifyCheck& Check = Card.Checks[Page];
	const FApexVerifyAnswer& Answer = Answers[Page];
	KickerText->SetText(FText::FromString(FString::Printf(TEXT("VERIFY  ·  %s  ·  CHECK %d OF %d"), *Card.Block.ToUpper(), Page + 1, Count)));
	TitleText->SetText(FText::FromString(Check.Check));
	DetailText->SetText(FText::FromString(Check.Detail));
	DetailText->SetVisibility(Check.Detail.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	BeforeText->SetText(FText::FromString(Check.Before.IsEmpty() ? FString()
		: FString::Printf(TEXT("ANSWERED BEFORE: %s"), *Check.Before.ToUpper())));
	for (int32 Index = 0; Index < AnswerButtons.Num(); ++Index)
	{
		UApexButtonWidget* Button = AnswerButtons[Index];
		const bool bShown = Check.Answers.IsValidIndex(Index);
		Button->SetVisibility(bShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bShown)
		{
			Button->SetLabel(Check.Answers[Index].ToUpper());
			Button->SetSelected(Answer.Answer == Check.Answers[Index]);
		}
	}
	const FString Note = Answer.Note;
	NoteText->SetText(FText::FromString(Note.IsEmpty() && !bKeyboardOpen ? FString(TEXT("NO NOTE"))
		: FString::Printf(TEXT("NOTE  %s%s"), *Note, bKeyboardOpen ? TEXT("_") : TEXT(""))));
	NoteText->SetColorAndOpacity(Note.IsEmpty() && !bKeyboardOpen ? ApexUI::Palette::TextMuted : ApexUI::Palette::TextPrimary);
	FlagButton->SetSelected(Answer.bFlag);
	ShiftKey->SetSelected(bShift);
	for (int32 Index = 0; Index < KeyButtons.Num(); ++Index)
	{
		KeyButtons[Index]->SetLabel(bShift ? KeyChars[Index].ToUpper() : KeyChars[Index]);
	}
	PrevButton->SetVisibility(Page > 0 ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
	SkipButton->SetLabel(Answer.Answer.IsEmpty() ? TEXT("Skip") : TEXT("Next"));
}

void UApexVerifyWidget::SetKeyboardOpen(bool bOpen)
{
	if (bKeyboardOpen == bOpen)
	{
		return;
	}
	bKeyboardOpen = bOpen;
	bShift = false;
	if (KeyboardPanel)
	{
		KeyboardPanel->SetVisibility(bOpen ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	Refresh();
}

void UApexVerifyWidget::Type(const FString& Text)
{
	if (IsSummary())
	{
		return;
	}
	FApexVerifyAnswer& Answer = Answers[Page];
	Answer.Note += bShift ? Text.ToUpper() : Text;
	Answer.At = ApexVerifyUi::Now();
	bShift = false;
	Save(EApexVerifyNext::Stop);
	Refresh();
}

void UApexVerifyWidget::Finish(EApexVerifyNext Next)
{
	UE_LOG(LogApexSim, Log, TEXT("Verify: block %s done, %d of %d answered, %s"), *Card.Block, AnsweredCount(), Card.Checks.Num(),
		Next == EApexVerifyNext::Next ? TEXT("next block") : TEXT("stop"));
	Save(Next);
	OnQuit.Broadcast();
}

void UApexVerifyWidget::FocusDefault()
{
	UWidget* Target = nullptr;
	if (IsSummary())
	{
		Target = NextBlockButton;
	}
	else if (bKeyboardOpen)
	{
		Target = KeyButtons.IsValidIndex(10) ? KeyButtons[10].Get() : DoneKey.Get();
	}
	else
	{
		const FApexVerifyCheck& Check = Card.Checks[Page];
		const int32 Chosen = Check.Answers.IndexOfByKey(Answers[Page].Answer);
		Target = AnswerButtons.IsValidIndex(Chosen) ? AnswerButtons[Chosen].Get() : (AnswerButtons.Num() ? AnswerButtons[0].Get() : nullptr);
	}
	if (!(Target && ApexNav::Focus(Target)))
	{
		SetKeyboardFocus();
	}
}

bool UApexVerifyWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	return ApexNav::MoveToward(this, Direction, Source);
}

bool UApexVerifyWidget::HandleBack()
{
	if (bKeyboardOpen)
	{
		SetKeyboardOpen(false);
		ApexNav::Focus(NoteButton);
		return true;
	}
	if (Page > 0)
	{
		ShowPage(Page - 1);
		return true;
	}
	if (Card.bRace && bInRaceView)
	{
		OnCloseRequested.Broadcast();
	}
	return true;
}

FReply UApexVerifyWidget::NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// A real keyboard types the note too. Space and Backspace are the note's
	// while its keyboard is up; Space would otherwise press the focused key.
	if (bCardOpen && bKeyboardOpen)
	{
		if (InKeyEvent.GetKey() == EKeys::SpaceBar)
		{
			return FReply::Handled();
		}
		if (InKeyEvent.GetKey() == EKeys::BackSpace && !IsSummary())
		{
			Answers[Page].Note.LeftChopInline(1);
			Save(EApexVerifyNext::Stop);
			Refresh();
			return FReply::Handled();
		}
	}
	return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
}

FReply UApexVerifyWidget::NativeOnKeyChar(const FGeometry& InGeometry, const FCharacterEvent& InCharEvent)
{
	const TCHAR Char = InCharEvent.GetCharacter();
	if (!bCardOpen || IsSummary() || Char < 32 || Char == 127)
	{
		return Super::NativeOnKeyChar(InGeometry, InCharEvent);
	}
	// Typing on the card opens the note; a space opens nothing.
	if (!bKeyboardOpen)
	{
		if (Char == ' ')
		{
			return Super::NativeOnKeyChar(InGeometry, InCharEvent);
		}
		SetKeyboardOpen(true);
		FocusDefault();
	}
	const bool bWasShift = bShift;
	bShift = false;
	Type(FString(1, &Char));
	bShift = bWasShift;
	return FReply::Handled();
}

void UApexVerifyWidget::HandleCommand(const TArray<FString>& Args)
{
	const FString Verb = Args.Num() ? Args[0].ToLower() : FString();
	UApexButtonWidget* Press = nullptr;
	if (Verb == TEXT("answer") && Args.Num() > 1)
	{
		const int32 Index = FCString::Atoi(*Args[1]) - 1;
		Press = AnswerButtons.IsValidIndex(Index) && !IsSummary() && Card.Checks[Page].Answers.IsValidIndex(Index) ? AnswerButtons[Index].Get() : nullptr;
	}
	else if (Verb == TEXT("type") && Args.Num() > 1)
	{
		if (!bKeyboardOpen)
		{
			SetKeyboardOpen(true);
		}
		TArray<FString> Words(Args);
		Words.RemoveAt(0);
		Type(FString::Join(Words, TEXT(" ")));
		return;
	}
	else if (Verb == TEXT("note"))
	{
		Press = NoteButton;
	}
	else if (Verb == TEXT("flag"))
	{
		Press = FlagButton;
	}
	else if (Verb == TEXT("skip"))
	{
		Press = SkipButton;
	}
	else if (Verb == TEXT("back"))
	{
		HandleBack();
		return;
	}
	else if (Verb == TEXT("more"))
	{
		Press = MoreTimeButton;
	}
	else if (Verb == TEXT("next"))
	{
		Press = NextBlockButton;
	}
	else if (Verb == TEXT("stop"))
	{
		Press = StopButton;
	}
	if (Press)
	{
		HandleButtonActivated(Press);
	}
	else
	{
		UE_LOG(LogApexSim, Warning, TEXT("apexsim.verify: nothing to do for '%s'"), *FString::Join(Args, TEXT(" ")));
	}
}

void UApexVerifyWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button || !bActive)
	{
		return;
	}
	const int32 Word = ApexNav::IndexOf(AnswerButtons, Button);
	if (Word != INDEX_NONE && !IsSummary())
	{
		FApexVerifyAnswer& Answer = Answers[Page];
		Answer.Answer = Card.Checks[Page].Answers.IsValidIndex(Word) ? Card.Checks[Page].Answers[Word] : FString();
		Answer.At = ApexVerifyUi::Now();
		Save(EApexVerifyNext::Stop);
		ShowPage(Page + 1);
		return;
	}
	const int32 Key = ApexNav::IndexOf(KeyButtons, Button);
	if (Key != INDEX_NONE)
	{
		Type(KeyChars[Key]);
		return;
	}
	if (Button == ShiftKey)
	{
		bShift = !bShift;
		Refresh();
	}
	else if (Button == SpaceKey)
	{
		Type(TEXT(" "));
	}
	else if (Button == DeleteKey && !IsSummary())
	{
		Answers[Page].Note.LeftChopInline(1);
		Save(EApexVerifyNext::Stop);
		Refresh();
	}
	else if (Button == ClearKey && !IsSummary())
	{
		Answers[Page].Note.Reset();
		Save(EApexVerifyNext::Stop);
		Refresh();
	}
	else if (Button == DoneKey)
	{
		SetKeyboardOpen(false);
		ApexNav::Focus(NoteButton);
	}
	else if (Button == NoteButton)
	{
		SetKeyboardOpen(!bKeyboardOpen);
		if (bKeyboardOpen)
		{
			FocusDefault();
		}
	}
	else if (Button == FlagButton && !IsSummary())
	{
		Answers[Page].bFlag = !Answers[Page].bFlag;
		Answers[Page].At = ApexVerifyUi::Now();
		Save(EApexVerifyNext::Stop);
		Refresh();
	}
	else if (Button == PrevButton)
	{
		ShowPage(Page - 1);
	}
	else if (Button == SkipButton)
	{
		ShowPage(Page + 1);
	}
	else if (Button == BackToChecksButton)
	{
		ShowPage(Card.Checks.Num() - 1);
	}
	else if (Button == DriveButton)
	{
		OnCloseRequested.Broadcast();
	}
	else if (Button == MoreTimeButton)
	{
		DueAtS = FMath::Max(DueAtS, ElapsedS) + ApexVerifyUi::MoreTimeS;
		bDue = false;
		OnCloseRequested.Broadcast();
	}
	else if (Button == StopButton)
	{
		Finish(EApexVerifyNext::Stop);
	}
	else if (Button == NextBlockButton)
	{
		Finish(EApexVerifyNext::Next);
	}
}
