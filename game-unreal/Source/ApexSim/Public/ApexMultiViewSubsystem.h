#pragma once

#include "ApexMultiView.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexMultiViewSubsystem.generated.h"

class ULocalPlayer;
class UUserWidget;

/**
 * Triple-monitor support: the window, the views and the menus.
 *
 * With Settings > Graphics > Screens on TRIPLE (settings.yml `screens: 3`,
 * or `-ApexScreens=3` for one run) this
 *
 *  - stretches the game window, borderless, across the three monitors
 *    standing in a row (ApexMultiView::FindTripleRow), and keeps it there:
 *    the engine sizes a borderless window to one monitor on every
 *    resolution change, so the window is checked each frame and put back;
 *  - adds two local players, the *side viewers* (AApexSideViewController),
 *    whose only job is to own an engine view each. The engine's split
 *    screen then renders three views into the one window, our
 *    UApexGameViewportClient lays them out centre-left-right, and each
 *    viewer's AApexSideViewCameraManager follows the real camera with the
 *    yaw and off-axis frustum ApexMultiView::SideView works out. Every view
 *    is a full render — Lumen, TSR, the post-process stack — because that is
 *    what the engine does for split screen; nothing is composited by hand;
 *  - pins the menu shell to the centre third of the window, so the HUD and
 *    the menus stay on the monitor in front of the driver while the race
 *    runs on across all three.
 *
 * The geometry (panel width, bezel, eye distance, side angle) is on the
 * settings slot and edited live on the Graphics page; Screens itself is in
 * settings.yml as well, because a window stretched across the wrong
 * monitors is something a player has to be able to undo from outside the
 * game. On a machine without a three-monitor row the mode still works —
 * the three views share whatever window there is — which is how it is
 * checked on a single monitor.
 */
UCLASS()
class APEXSIM_API UApexMultiViewSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** True while the game runs as a triple: three views, the shell in the middle. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Display")
	bool IsTriple() const { return bTriple; }

	/**
	 * Reads the setting (and the command line) again and brings the window,
	 * the viewers and the shell into line with it. The settings subsystem
	 * calls it with the rest of the graphics group; it is safe to call at
	 * any time, and does nothing until the game has a window and a world.
	 */
	void Apply();

	/** The rig as set up, with the aspect of the centre view filled in from the window. */
	ApexMultiView::FTripleGeometry Geometry() const;

	/** The horizontal field of view the centre monitor asks for, degrees. */
	float CentreFovDeg() const;

	/**
	 * The frustum a side viewer draws this frame, given the field of view the
	 * main camera is drawing the centre with (a broadcast lens narrows it).
	 */
	ApexMultiView::FSideView SideView(bool bLeft, float PrimaryFovDeg) const;

	/** Which panel a local player draws: -1 the left, +1 the right, 0 the centre (or not a viewer). */
	int32 SideOf(const ULocalPlayer* Player) const;

	/**
	 * The widget that has to stay on the centre monitor — the menu shell.
	 * Registered once by the game mode after it is added to the viewport;
	 * re-anchored on every Apply.
	 */
	void SetCentreWidget(UUserWidget* Widget);

	/** True while a side viewer's player controller is being spawned: the game mode picks its class by it. */
	bool IsSpawningSideViewer() const { return bSpawningSideViewer; }

	/** The desktop rectangle the window spans as a triple, or the window's own when there is no row. */
	FIntRect GetSpan() const { return Span; }

private:
	bool Tick(float DeltaSeconds);

	/** What the setting and the command line ask for: 1 or 3. */
	int32 RequestedScreens() const;

	/** Finds the monitor row, or settles for the window; logs the choice once per Apply. */
	void MeasureSpan();
	/** Puts the window over the span, borderless. Idempotent; called every frame while a triple. */
	void SpanWindow();
	/** Gives the window back to the display settings after a triple. */
	void UnspanWindow();

	void EnsureViewers();
	void RemoveViewers();
	void PlaceCentreWidget();
	void SetViewportLayout(bool bEnable);

	bool bTriple = false;
	/** -ApexScreens= on the command line, for this run only; 0 when absent. */
	int32 ForcedScreens = 0;

	FIntRect Span;
	/** No three-monitor row: the span is the window itself and nothing is moved. */
	bool bSpanIsWindow = true;
	bool bSpanLogged = false;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ULocalPlayer>> Viewers;

	TWeakObjectPtr<UUserWidget> CentreWidget;
	bool bSpawningSideViewer = false;
	bool bPendingApply = false;

	FTSTicker::FDelegateHandle TickHandle;
};
