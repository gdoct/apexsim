#include "UI/ApexMirrorWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UI/ApexUIStyle.h"

using namespace ApexUI;

UApexMirrorWidget::UApexMirrorWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UApexMirrorWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Glass = WidgetTree->ConstructWidget<UImage>();
	// Mirror image: left becomes right. A negative scale about the centre is
	// the whole of it; the capture itself is an ordinary rear-facing camera.
	Glass->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
	Glass->SetRenderScale(FVector2D(-1.0f, 1.0f));

	FSlateBrush Blank = MakeBrush(FLinearColor(0.02f, 0.02f, 0.025f));
	Glass->SetBrush(Blank);

	// The bezel is the face's own dark border; it is what stops the glass
	// reading as a hole in the bodywork.
	UBorder* Bezel = MakePanel(*WidgetTree, Glass, FMargin(6.0f), MakeBrush(FLinearColor(0.01f, 0.01f, 0.012f)));
	Frame = MakeSized(*WidgetTree, Bezel, FaceSize.X, FaceSize.Y);
	WidgetTree->RootWidget = Frame;
}

void UApexMirrorWidget::SetFaceSize(const FVector2D& Size)
{
	FaceSize = Size;
	if (Frame)
	{
		Frame->SetWidthOverride(Size.X);
		Frame->SetHeightOverride(Size.Y);
	}
}

void UApexMirrorWidget::SetTexture(UTextureRenderTarget2D* Texture)
{
	if (Shown == Texture || !Glass)
	{
		return;
	}
	Shown = Texture;

	FSlateBrush Brush = MakeBrush(FLinearColor::White);
	if (Texture)
	{
		Brush.SetResourceObject(Texture);
		Brush.ImageSize = FVector2D(Texture->SizeX, Texture->SizeY);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.TintColor = FSlateColor(FLinearColor::White);
	}
	else
	{
		Brush = MakeBrush(FLinearColor(0.02f, 0.02f, 0.025f));
	}
	Glass->SetBrush(Brush);
}
