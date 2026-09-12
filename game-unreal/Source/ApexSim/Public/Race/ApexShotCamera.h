#pragma once

#include "CoreMinimal.h"
#include "Misc/DefaultValueHelper.h"
#include "Race/ApexRaceCoordinate.h"

/**
 * Pure helpers behind the race director's free screenshot camera: turning the
 * numbers typed into `apexsim.cam.*` or passed as `-ApexCamera=` /
 * `-ApexCameraLookAt=` into an Unreal pose. Everything the player types is in
 * the SERVER frame (metres, +Y left, yaw counter-clockwise), so a shot can be
 * worked out straight from a track YAML's centerline.
 */
namespace ApexShotCamera
{
	/** Where the shot camera sits and looks, in Unreal world space. */
	struct FPose
	{
		FVector LocationCm = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};

	/**
	 * "1, 2.5,-3" or "1 2.5 -3" -> {1, 2.5, -3}. Commas and whitespace both
	 * separate, so the console's space-separated arguments and the command
	 * line's comma list go through the same parser. False (and `Out` empty)
	 * on an empty list or any piece that is not a number.
	 */
	inline bool ParseNumberList(const FString& Text, TArray<double>& Out)
	{
		Out.Reset();
		FString Normalised = Text.Replace(TEXT(","), TEXT(" "));
		TArray<FString> Pieces;
		Normalised.ParseIntoArrayWS(Pieces);
		for (const FString& Piece : Pieces)
		{
			double Value = 0.0;
			if (!FDefaultValueHelper::ParseDouble(Piece, Value) || !FMath::IsFinite(Value))
			{
				Out.Reset();
				return false;
			}
			Out.Add(Value);
		}
		return Out.Num() > 0;
	}

	/** X Y Z [Yaw [Pitch]]: a position in metres and a view in degrees, pitch up. */
	inline bool PoseFromGoto(TConstArrayView<double> Values, FPose& Out)
	{
		if (Values.Num() < 3 || Values.Num() > 5)
		{
			return false;
		}
		const double Yaw = Values.Num() > 3 ? Values[3] : 0.0;
		const double Pitch = Values.Num() > 4 ? Values[4] : 0.0;
		Out.LocationCm = ApexRace::ServerToUnrealPosition(FVector(Values[0], Values[1], Values[2]));
		Out.Rotation = ApexRace::ServerViewToUnrealRotation(Yaw, FMath::Clamp(Pitch, -89.9, 89.9));
		return true;
	}

	/** X Y Z TX TY TZ: a position and a point to look at, both in metres. */
	inline bool PoseFromLookAt(TConstArrayView<double> Values, FPose& Out)
	{
		if (Values.Num() != 6)
		{
			return false;
		}
		const FVector From(Values[0], Values[1], Values[2]);
		const FVector Target(Values[3], Values[4], Values[5]);
		Out.LocationCm = ApexRace::ServerToUnrealPosition(From);
		Out.Rotation = ApexRace::ServerLookAtUnrealRotation(From, Target);
		return true;
	}
}
