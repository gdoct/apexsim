#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Race/ApexPhotoMode.h"

#include "ApexPhotoModeWidget.generated.h"

class UApexRootWidget;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * Photo mode's layer over a watched race (docs/game/spectator.md, "Photo
 * mode"): the lens card on the right (focal length, focus, aperture,
 * exposure, shot size; the picked row lit), a chip naming the camera and
 * whether the replay is held, and the keys along the bottom for the device
 * in use. It flies the race director's photo camera every frame
 * (ApexPhoto::Advance, AApexRaceDirector::SetPhotoCamera) from the keys,
 * sticks and mouse the shell's input processor hands it, so nothing here
 * takes focus, and it saves the high-resolution shot.
 *
 * The shell begins and ends it (UApexRootWidget::BeginPhotoMode), which
 * holds and resumes the replay and hides the HUD around it.
 */
UCLASS()
class APEXSIM_API UApexPhotoModeWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexPhotoModeWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** The shell that owns it: its toast, its pause, its exit. */
	void SetShell(UApexRootWidget* InShell) { Shell = InShell; }

	/** The director's photo camera on from wherever the view is. False with no race on screen. */
	bool Begin();
	/** The photo camera off and the layer hidden. */
	void End();
	bool IsActive() const { return bActive; }

	// --- Input, from the shell's input processor while active ---------------------------

	/** A press: a command (ApexPhoto::CommandFor) or a held key of the move. Always taken. */
	void HandleKeyDown(const FKeyEvent& InKeyEvent);
	void HandleKeyUp(const FKeyEvent& InKeyEvent);
	/** A stick or trigger moved: the move, the look, down and up. */
	void HandleAnalog(const FKey& Key, float Value);
	/** The mouse moved; it looks only while a button is held. */
	void HandleMouseMove(const FVector2D& DeltaPixels, bool bButtonHeld);
	/** The wheel: the focal length a step. */
	void HandleMouseWheel(float Delta);

	/** A command from the keys or the console. */
	void RunCommand(ApexPhoto::ECommand Command);

	// --- The console (apexsim.photo) ------------------------------------------------------

	/** Fly as if the keys were held: forward, right and up (-1..1) for `Seconds`. */
	void ScriptMove(const FVector& Move, float Seconds);
	/** Turn the camera (or the orbit) by degrees. */
	void ScriptLook(float YawDeg, float PitchDeg);
	/** One of the lens's values, exactly (focal length mm, focus m, f-number, EV, multiplier). */
	void SetLensValue(ApexPhoto::ESetting Setting, float Value);
	/** Save the shot now; the toast names the file. */
	void TakeShot();
	/** The camera's pose and the lens, for the log. */
	FString Describe() const;

private:
	UWidget* BuildCard();
	void RefreshPanel();
	/** The input of this frame from the held keys, the sticks and the mouse. */
	ApexPhoto::FInput GatherInput(float DeltaSeconds);

	UPROPERTY(Transient) TObjectPtr<UWidget> Layer;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ModeText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HintText;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> RowPanels;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowLabels;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowValues;

	TWeakObjectPtr<UApexRootWidget> Shell;

	bool bActive = false;
	bool bPanelHidden = false;
	ApexPhoto::FCamera Camera;
	/** Where photo mode began, for Reset. */
	ApexPhoto::FCamera StartCamera;
	ApexPhoto::FLens Lens;
	ApexPhoto::ESetting Row = ApexPhoto::ESetting::FocalLength;

	TSet<FKey> HeldKeys;
	/** The sticks and triggers, by axis key. */
	TMap<FKey, float> Axes;
	/** Mouse look waiting for the next frame, degrees. */
	FVector2D PendingLookDeg = FVector2D::ZeroVector;
	/** A console move under way. */
	FVector ScriptedMove = FVector::ZeroVector;
	float ScriptedSeconds = 0.0f;

	/** What the panel last showed, so the texts are set only on a change. */
	FString ShownState;
	int32 ShownRow = -1;
	bool bShownGamepad = false;
};
