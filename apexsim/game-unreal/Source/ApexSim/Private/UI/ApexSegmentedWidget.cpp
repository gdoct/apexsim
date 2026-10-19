#include "UI/ApexSegmentedWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/SizeBox.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** Pills are shorter than a list row; the design's segmented controls are compact. */
	constexpr float PillHeight = 38.0f;
	constexpr float PillLabelSize = 13.0f;
	constexpr float PillGap = 4.0f;
}

UApexSegmentedWidget::UApexSegmentedWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UApexSegmentedWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	WidgetTree->RootWidget = Row;

	if (Options.Num() > 0)
	{
		// Set up before the tree existed; rebuild now that it does.
		Setup(Options, SelectedIndex, PillWidth);
	}
}

void UApexSegmentedWidget::Setup(const TArray<FString>& InOptions, int32 InSelectedIndex, float OptionWidth)
{
	Options = InOptions;
	SelectedIndex = FMath::Clamp(InSelectedIndex, 0, FMath::Max(0, Options.Num() - 1));
	PillWidth = OptionWidth;

	if (!Row)
	{
		return;
	}

	Row->ClearChildren();
	Pills.Reset();

	for (int32 Index = 0; Index < Options.Num(); ++Index)
	{
		UApexButtonWidget* Pill = WidgetTree->ConstructWidget<UApexButtonWidget>();

		FApexButtonSpec PillSpec;
		PillSpec.Label = Options[Index];
		PillSpec.Variant = EApexButtonVariant::Ghost;
		PillSpec.bCentreLabel = true;
		PillSpec.Height = PillHeight;
		PillSpec.LabelSize = PillLabelSize;
		// The index is the action id: the handler needs to know which pill was
		// hit, and the label is not unique across a screen full of OFF/ON rows.
		PillSpec.ActionId = FName(*FString::FromInt(Index));
		Pill->Setup(PillSpec);
		Pill->OnActivated.AddDynamic(this, &UApexSegmentedWidget::HandlePillActivated);

		ApexUI::AddH(
			Row,
			ApexUI::MakeSized(*WidgetTree, Pill, PillWidth, PillHeight),
			FMargin(Index == 0 ? 0.0f : PillGap, 0.0f, 0.0f, 0.0f));

		Pills.Add(Pill);
	}

	ApplySelection();
}

void UApexSegmentedWidget::SetSelectedIndex(int32 Index)
{
	const int32 Clamped = FMath::Clamp(Index, 0, FMath::Max(0, Options.Num() - 1));
	if (Clamped == SelectedIndex)
	{
		return;
	}
	SelectedIndex = Clamped;
	ApplySelection();
}

void UApexSegmentedWidget::ApplySelection()
{
	for (int32 Index = 0; Index < Pills.Num(); ++Index)
	{
		UApexButtonWidget* Pill = Pills[Index];
		if (!Pill)
		{
			continue;
		}

		// Variant, not the selected outline: the design fills the chosen pill.
		FApexButtonSpec PillSpec;
		PillSpec.Label = Options.IsValidIndex(Index) ? Options[Index] : FString();
		PillSpec.Variant = Index == SelectedIndex ? EApexButtonVariant::Primary : EApexButtonVariant::Ghost;
		PillSpec.bCentreLabel = true;
		PillSpec.Height = PillHeight;
		PillSpec.LabelSize = PillLabelSize;
		PillSpec.ActionId = FName(*FString::FromInt(Index));
		Pill->Setup(PillSpec);
	}
}

void UApexSegmentedWidget::HandlePillActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const int32 Index = FCString::Atoi(*Button->GetActionId().ToString());
	if (Index == SelectedIndex)
	{
		return;
	}

	SelectedIndex = Index;
	ApplySelection();
	OnChosen.Broadcast(this, SelectedIndex);
}
