#include "Race/ApexStreetLights.h"

namespace ApexLights
{
	TArray<FLightSpec> SpecsFor(const FString& MeshName)
	{
		TArray<FLightSpec> Out;
		if (MeshName.Contains(TEXT("lamp_arm_twin")))
		{
			// Two swan-neck arms; each head leans a little toward the road
			// (the other arm's side).
			for (const float Side : {1.0f, -1.0f})
			{
				FLightSpec Spec;
				Spec.LocalCm = FVector(0.0, Side * ArmHeadYCm, ArmHeadZCm);
				Spec.AimLocal = FRotator(ArmPitchDeg, -Side * 90.0f, 0.0f);
				Spec.Lumens = ArmLumens;
				Spec.RadiusCm = ArmRadiusCm;
				Spec.InnerConeDeg = ArmInnerDeg;
				Spec.OuterConeDeg = ArmOuterDeg;
				Spec.Color = FLinearColor::MakeFromColorTemperature(ArmColorTemperatureK);
				Out.Add(Spec);
			}
		}
		else if (MeshName.Contains(TEXT("lamp_globe_pole")))
		{
			FLightSpec Spec;
			Spec.LocalCm = FVector(0.0, 0.0, GlobeHeadZCm);
			Spec.bSpot = false;
			Spec.Lumens = GlobeLumens;
			Spec.RadiusCm = GlobeRadiusCm;
			Spec.Color = FLinearColor(1.0f, 0.88f, 0.70f);
			Out.Add(Spec);
		}
		else if (MeshName.Contains(TEXT("lamp_balloon_tether")))
		{
			FLightSpec Spec;
			Spec.LocalCm = FVector(0.0, 0.0, BalloonHeadZCm);
			Spec.bSpot = false;
			Spec.Lumens = BalloonLumens;
			Spec.RadiusCm = BalloonRadiusCm;
			Spec.Color = MeshName.EndsWith(TEXT("_orange")) ? FLinearColor(1.0f, 0.60f, 0.25f)
															 : FLinearColor(1.0f, 0.95f, 0.85f);
			Out.Add(Spec);
		}
		else if (MeshName.Contains(TEXT("pit_light_truss")))
		{
			// One downward spot per module over the working lane; the pivot
			// is the truss's bottom centre.
			FLightSpec Spec;
			Spec.LocalCm = FVector(0.0, 0.0, -10.0);
			Spec.AimLocal = FRotator(-90.0f, 0.0f, 0.0f);
			Spec.Lumens = TrussLumens;
			Spec.RadiusCm = TrussRadiusCm;
			Spec.InnerConeDeg = TrussInnerDeg;
			Spec.OuterConeDeg = TrussOuterDeg;
			Spec.Color = FLinearColor(1.0f, 0.95f, 0.85f);
			Out.Add(Spec);
		}
		else if (MeshName.Contains(TEXT("floodlight_tower")))
		{
			FLightSpec Spec;
			Spec.LocalCm = FVector(0.0, 230.0, 2800.0);
			Spec.AimLocal = FRotator(-52.0f, 90.0f, 0.0f);
			Spec.Lumens = 220000.0f;
			Spec.RadiusCm = 16000.0f;
			Spec.InnerConeDeg = 30.0f;
			Spec.OuterConeDeg = 52.0f;
			Spec.Color = FLinearColor(1.0f, 0.95f, 0.82f);
			Out.Add(Spec);
		}
		else if (MeshName.Contains(TEXT("lamp_post")))
		{
			FLightSpec Spec;
			Spec.LocalCm = FVector(0.0, 110.0, 750.0);
			Spec.AimLocal = FRotator(-70.0f, 90.0f, 0.0f);
			Spec.Lumens = 12000.0f;
			Spec.RadiusCm = 3500.0f;
			Spec.InnerConeDeg = 40.0f;
			Spec.OuterConeDeg = 60.0f;
			Spec.Color = FLinearColor(1.0f, 0.95f, 0.82f);
			Out.Add(Spec);
		}
		return Out;
	}

	int32 Select(const TArray<float>& Distances, TArray<uint8>& InOutOn, int32 MaxOn, float RangeCm)
	{
		const int32 Count = Distances.Num();
		if (InOutOn.Num() != Count)
		{
			InOutOn.Init(0, Count);
		}
		struct FRank
		{
			float Key;
			int32 Index;
		};
		TArray<FRank> Ranks;
		Ranks.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const bool bOn = InOutOn[i] != 0;
			const float Reach = bOn ? RangeCm * (1.0f + StickyShare) : RangeCm;
			if (Distances[i] <= Reach)
			{
				Ranks.Add({bOn ? Distances[i] * (1.0f - StickyShare) : Distances[i], i});
			}
		}
		Ranks.Sort([](const FRank& A, const FRank& B) { return A.Key < B.Key || (A.Key == B.Key && A.Index < B.Index); });
		for (uint8& Flag : InOutOn)
		{
			Flag = 0;
		}
		const int32 On = FMath::Min(FMath::Max(MaxOn, 0), Ranks.Num());
		for (int32 i = 0; i < On; ++i)
		{
			InOutOn[Ranks[i].Index] = 1;
		}
		return On;
	}
}	 // namespace ApexLights
