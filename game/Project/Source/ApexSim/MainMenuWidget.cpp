// Copyright ApexSim Team. All Rights Reserved.

#include "MainMenuWidget.h"
#include "Components/Button.h"
#include "Kismet/GameplayStatics.h"

UMainMenuWidget::UMainMenuWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UMainMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Automatically bind hover effects to all buttons
	BindButtonHoverEffects();
}

void UMainMenuWidget::BindButtonHoverEffects()
{
	// Bind Play Button
	if (PlayButton)
	{
		OriginalTransforms.Add(PlayButton, PlayButton->GetRenderTransform());
		OriginalColors.Add(PlayButton, PlayButton->GetColorAndOpacity());
		PlayButton->OnHovered.AddDynamic(this, &UMainMenuWidget::OnPlayButtonHovered);
		PlayButton->OnUnhovered.AddDynamic(this, &UMainMenuWidget::OnPlayButtonUnhovered);
	}

	// Bind Settings Button
	if (SettingsButton)
	{
		OriginalTransforms.Add(SettingsButton, SettingsButton->GetRenderTransform());
		OriginalColors.Add(SettingsButton, SettingsButton->GetColorAndOpacity());
		SettingsButton->OnHovered.AddDynamic(this, &UMainMenuWidget::OnSettingsButtonHovered);
		SettingsButton->OnUnhovered.AddDynamic(this, &UMainMenuWidget::OnSettingsButtonUnhovered);
	}

	// Bind Content Button
	if (ContentButton)
	{
		OriginalTransforms.Add(ContentButton, ContentButton->GetRenderTransform());
		OriginalColors.Add(ContentButton, ContentButton->GetColorAndOpacity());
		ContentButton->OnHovered.AddDynamic(this, &UMainMenuWidget::OnContentButtonHovered);
		ContentButton->OnUnhovered.AddDynamic(this, &UMainMenuWidget::OnContentButtonUnhovered);
	}

	// Bind Quit Button
	if (QuitButton)
	{
		OriginalTransforms.Add(QuitButton, QuitButton->GetRenderTransform());
		OriginalColors.Add(QuitButton, QuitButton->GetColorAndOpacity());
		QuitButton->OnHovered.AddDynamic(this, &UMainMenuWidget::OnQuitButtonHovered);
		QuitButton->OnUnhovered.AddDynamic(this, &UMainMenuWidget::OnQuitButtonUnhovered);
	}
}

// Play Button Hover Events
void UMainMenuWidget::OnPlayButtonHovered()
{
	if (bPlaySoundOnHover && HoverSound)
	{
		UGameplayStatics::PlaySound2D(this, HoverSound);
	}
	ApplyHoverEffect(PlayButton);
}

void UMainMenuWidget::OnPlayButtonUnhovered()
{
	RemoveHoverEffect(PlayButton);
}

// Settings Button Hover Events
void UMainMenuWidget::OnSettingsButtonHovered()
{
	if (bPlaySoundOnHover && HoverSound)
	{
		UGameplayStatics::PlaySound2D(this, HoverSound);
	}
	ApplyHoverEffect(SettingsButton);
}

void UMainMenuWidget::OnSettingsButtonUnhovered()
{
	RemoveHoverEffect(SettingsButton);
}

// Content Button Hover Events
void UMainMenuWidget::OnContentButtonHovered()
{
	if (bPlaySoundOnHover && HoverSound)
	{
		UGameplayStatics::PlaySound2D(this, HoverSound);
	}
	ApplyHoverEffect(ContentButton);
}

void UMainMenuWidget::OnContentButtonUnhovered()
{
	RemoveHoverEffect(ContentButton);
}

// Quit Button Hover Events
void UMainMenuWidget::OnQuitButtonHovered()
{
	if (bPlaySoundOnHover && HoverSound)
	{
		UGameplayStatics::PlaySound2D(this, HoverSound);
	}
	ApplyHoverEffect(QuitButton);
}

void UMainMenuWidget::OnQuitButtonUnhovered()
{
	RemoveHoverEffect(QuitButton);
}

void UMainMenuWidget::ApplyHoverEffect(UButton* Button)
{
	if (!Button)
	{
		return;
	}

	// Get original transform
	FWidgetTransform* OriginalTransform = OriginalTransforms.Find(Button);
	if (!OriginalTransform)
	{
		return;
	}

	// Create new transform with scale
	FWidgetTransform HoverTransform = *OriginalTransform;
	HoverTransform.Scale = FVector2D(HoverScaleMultiplier, HoverScaleMultiplier);

	// Apply transform
	Button->SetRenderTransform(HoverTransform);

	// Apply color tint
	FLinearColor* OriginalColor = OriginalColors.Find(Button);
	if (OriginalColor)
	{
		FLinearColor NewColor = *OriginalColor * HoverColorTint;
		Button->SetColorAndOpacity(NewColor);
	}
}

void UMainMenuWidget::RemoveHoverEffect(UButton* Button)
{
	if (!Button)
	{
		return;
	}

	// Restore original transform
	FWidgetTransform* OriginalTransform = OriginalTransforms.Find(Button);
	if (OriginalTransform)
	{
		Button->SetRenderTransform(*OriginalTransform);
	}

	// Restore original color
	FLinearColor* OriginalColor = OriginalColors.Find(Button);
	if (OriginalColor)
	{
		Button->SetColorAndOpacity(*OriginalColor);
	}
}
