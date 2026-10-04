#include "Guide/ApexGuidePlayer.h"

#include "Guide/ApexTrackGuide.h"
#include "InputCoreTypes.h"

const TCHAR* ApexGuide::PhaseName(EPhase Phase)
{
	switch (Phase)
	{
	case EPhase::Overview: return TEXT("overview");
	case EPhase::Travel: return TEXT("travel");
	case EPhase::Normal: return TEXT("normal");
	case EPhase::Slow: return TEXT("slow");
	}
	return TEXT("?");
}

void ApexGuide::FPlayer::Setup(double InOverviewS, const TArray<FLoop>& InLoops, double InDurationS)
{
	DurationS = FMath::Max(InDurationS, 0.0);
	OverviewS = Clamp(InOverviewS);
	Loops = InLoops;
	for (FLoop& Loop : Loops)
	{
		// A loop running past the recording would never reach its end.
		Loop.FromS = Clamp(FMath::Min(Loop.FromS, Loop.ToS));
		Loop.ToS = Clamp(FMath::Max(Loop.FromS, Loop.ToS));
		Loop.SlowFromS = FMath::Clamp(Loop.SlowFromS, Loop.FromS, Loop.ToS);
		Loop.SlowToS = FMath::Clamp(Loop.SlowToS, Loop.SlowFromS, Loop.ToS);
	}
	bPaused = false;
	ShowOverview();
}

void ApexGuide::FPlayer::Setup(const FApexTrackGuide& Guide, double InDurationS)
{
	TArray<FLoop> InLoops;
	InLoops.Reserve(Guide.Corners.Num());
	for (const FApexGuideCorner& Stop : Guide.Corners)
	{
		FLoop& Loop = InLoops.AddDefaulted_GetRef();
		Loop.FromS = Stop.FromS;
		Loop.ToS = Stop.ToS;
		Loop.SlowFromS = Stop.SlowFromS;
		Loop.SlowToS = Stop.SlowToS;
		Loop.SlowRate = Stop.SlowRate;
	}
	Setup(Guide.OverviewTimeS, InLoops, InDurationS);
}

void ApexGuide::FPlayer::CutTo(double Seconds)
{
	Time = Clamp(Seconds);
	bCut = true;
}

void ApexGuide::FPlayer::ShowOverview()
{
	Corner = INDEX_NONE;
	Phase = EPhase::Overview;
	LoopCount = 0;
	CutTo(OverviewS);
}

void ApexGuide::FPlayer::GoToCorner(int32 Index, bool bCutThere)
{
	if (!Loops.IsValidIndex(Index))
	{
		ShowOverview();
		return;
	}
	// Somewhere new is something to watch, not a frame to stare at.
	bPaused = false;
	Corner = Index;
	LoopCount = 0;
	const FLoop& Loop = Loops[Index];
	const double Ahead = Loop.FromS - Time;
	const double Rate = FMath::Max(Settings.TravelRate, 1.0f);
	if (bCutThere || Ahead <= Settings.MinTravelSeconds * Rate)
	{
		CutTo(Loop.FromS);
		Phase = EPhase::Normal;
		return;
	}
	const double Longest = Rate * FMath::Max(Settings.MaxTravelSeconds, Settings.MinTravelSeconds);
	if (Ahead > Longest)
	{
		// The far side of the circuit: jump most of the way, then run the rest.
		CutTo(Loop.FromS - Longest);
	}
	Phase = EPhase::Travel;
}

void ApexGuide::FPlayer::Next()
{
	if (Loops.Num() == 0)
	{
		ShowOverview();
		return;
	}
	if (Corner == INDEX_NONE)
	{
		GoToCorner(0);
	}
	else if (Corner + 1 < Loops.Num())
	{
		GoToCorner(Corner + 1);
	}
	else
	{
		bPaused = false;
		ShowOverview();
	}
}

bool ApexGuide::FPlayer::Previous()
{
	if (Corner == INDEX_NONE)
	{
		return false;
	}
	if (Corner == 0)
	{
		bPaused = false;
		ShowOverview();
	}
	else
	{
		GoToCorner(Corner - 1, /*bCut*/ true);
	}
	return true;
}

float ApexGuide::FPlayer::GetRate() const
{
	if (bPaused || Phase == EPhase::Overview || !Loops.IsValidIndex(Corner))
	{
		return 0.0f;
	}
	switch (Phase)
	{
	case EPhase::Travel: return FMath::Max(Settings.TravelRate, 1.0f);
	case EPhase::Slow: return Loops[Corner].SlowRate;
	default: return 1.0f;
	}
}

void ApexGuide::FPlayer::Tick(double DeltaSeconds)
{
	if (bPaused || Phase == EPhase::Overview || !Loops.IsValidIndex(Corner) || DeltaSeconds <= 0.0)
	{
		return;
	}
	const FLoop& Loop = Loops[Corner];
	switch (Phase)
	{
	case EPhase::Travel:
		Time += DeltaSeconds * FMath::Max(Settings.TravelRate, 1.0f);
		if (Time >= Loop.FromS)
		{
			// Arrived: the clock runs on unbroken into the loop.
			Time = Loop.FromS;
			Phase = EPhase::Normal;
		}
		break;

	case EPhase::Normal:
		if (Loop.ToS - Loop.FromS < 0.05)
		{
			// Nothing to loop: hold the moment.
			break;
		}
		Time += DeltaSeconds;
		if (Time >= Loop.ToS)
		{
			if (Loop.HasSlowPart())
			{
				CutTo(Loop.SlowFromS);
				Phase = EPhase::Slow;
			}
			else
			{
				CutTo(Loop.FromS);
				++LoopCount;
			}
		}
		break;

	case EPhase::Slow:
		Time += DeltaSeconds * Loop.SlowRate;
		if (Time >= Loop.SlowToS)
		{
			CutTo(Loop.FromS);
			Phase = EPhase::Normal;
			++LoopCount;
		}
		break;

	default:
		break;
	}
	Time = Clamp(Time);
}

ApexGuide::FCommand ApexGuide::CommandFor(const FKey& Key, bool bShift)
{
	FCommand Out;
	if (Key == EKeys::Left || Key == EKeys::PageUp || Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Gamepad_LeftShoulder)
	{
		Out.Action = EAction::Previous;
	}
	else if (Key == EKeys::Right || Key == EKeys::PageDown || Key == EKeys::Enter || Key == EKeys::Gamepad_DPad_Right
		|| Key == EKeys::Gamepad_RightShoulder)
	{
		Out.Action = EAction::Next;
	}
	else if (Key == EKeys::C)
	{
		Out.Action = bShift ? EAction::CameraBack : EAction::Camera;
	}
	else if (Key == EKeys::Gamepad_FaceButton_Top)
	{
		Out.Action = EAction::Camera;
	}
	else if (Key == EKeys::Gamepad_FaceButton_Left)
	{
		Out.Action = EAction::CameraBack;
	}
	else if (Key == EKeys::SpaceBar || Key == EKeys::Gamepad_FaceButton_Bottom || Key == EKeys::Gamepad_Special_Right)
	{
		Out.Action = EAction::Pause;
	}
	else if (Key == EKeys::Home || Key == EKeys::Gamepad_Special_Left)
	{
		Out.Action = EAction::Overview;
	}
	else if (Key == EKeys::Escape || Key == EKeys::BackSpace || Key == EKeys::Gamepad_FaceButton_Right)
	{
		Out.Action = EAction::Leave;
	}
	else
	{
		static const FKey Digits[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five,
			EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine, EKeys::Zero };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Digits); ++Index)
		{
			if (Key == Digits[Index])
			{
				Out.Action = EAction::Corner;
				Out.Corner = Index + 1;
				break;
			}
		}
	}
	return Out;
}
