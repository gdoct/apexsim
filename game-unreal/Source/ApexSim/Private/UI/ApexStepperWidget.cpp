#include "UI/ApexStepperWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	/** The same pill as a segmented control's, so the two line up in a column. */
	constexpr float PillHeight = 38.0f;
	constexpr float PillWidth = 44.0f;
	constexpr float PillLabelSize = 15.0f;
	constexpr float PillGap = 4.0f;

	const FName ActionMinus = TEXT("Minus");
	const FName ActionPlus = TEXT("Plus");
}

UApexStepperWidget::UApexStepperWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UApexStepperWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	WidgetTree->RootWidget = Row;

	if (bBuilt)
	{
		// Set up before the tree existed; rebuild now that it does.
		Setup(Min, Max, Value, TextWidth);
	}
}

void UApexStepperWidget::Setup(int32 InMin, int32 InMax, int32 InValue, float ReadoutWidth)
{
	Min = FMath::Min(InMin, InMax);
	Max = FMath::Max(InMin, InMax);
	Value = FMath::Clamp(InValue, Min, Max);
	TextWidth = ReadoutWidth;
	bBuilt = true;

	if (!Row)
	{
		return;
	}

	Row->ClearChildren();

	auto MakePill = [this](const TCHAR* Label, FName ActionId) -> UApexButtonWidget*
	{
		UApexButtonWidget* Pill = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = EApexButtonVariant::Ghost;
		Spec.bCentreLabel = true;
		Spec.Height = PillHeight;
		Spec.LabelSize = PillLabelSize;
		Spec.ActionId = ActionId;
		Pill->Setup(Spec);
		Pill->OnActivated.AddDynamic(this, &UApexStepperWidget::HandlePillActivated);
		return Pill;
	};

	MinusPill = MakePill(TEXT("−"), ActionMinus);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, MinusPill, PillWidth, PillHeight));

	Readout = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(13.0f, 40), ApexUI::Palette::TextPrimary);
	Readout->SetJustification(ETextJustify::Center);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, Readout, TextWidth, -1.0f), FMargin(PillGap, 0.0f), VAlign_Center);

	PlusPill = MakePill(TEXT("+"), ActionPlus);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, PlusPill, PillWidth, PillHeight));

	ApplyValue();
}

void UApexStepperWidget::SetValue(int32 InValue)
{
	const int32 Clamped = FMath::Clamp(InValue, Min, Max);
	if (Clamped == Value)
	{
		return;
	}
	Value = Clamped;
	ApplyValue();
}

void UApexStepperWidget::ApplyValue()
{
	if (Readout)
	{
		const FString Text = Formatter ? Formatter(Value) : FString::FromInt(Value);
		Readout->SetText(FText::FromString(Text));
		// A knob off its stock value is what the player came here to see.
		Readout->SetColorAndOpacity(Value == 0 ? ApexUI::Palette::TextSecondary : ApexUI::Palette::TextPrimary);
	}
	// A pill at the end of its range is dimmed, not removed: focus must still
	// be able to land on it and cross to the other one.
	if (MinusPill)
	{
		MinusPill->SetRenderOpacity(Value > Min ? 1.0f : 0.35f);
	}
	if (PlusPill)
	{
		PlusPill->SetRenderOpacity(Value < Max ? 1.0f : 0.35f);
	}
}

void UApexStepperWidget::HandlePillActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const int32 Step = Button->GetActionId() == ActionPlus ? 1 : -1;
	const int32 Next = FMath::Clamp(Value + Step, Min, Max);
	if (Next == Value)
	{
		return;
	}

	Value = Next;
	ApplyValue();
	OnChanged.Broadcast(this, Value);
}
