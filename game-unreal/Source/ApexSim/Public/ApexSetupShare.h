#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * A car setup as one line of text, to paste into a chat or keep in a file
 * (the garage's Load / Save tab: COPY AS TEXT and PASTE):
 *
 *     APXS1;car=fugazzi-sf26;crc=1A2B3C4D;name=Monza low;c=0,0,-2,3,...
 *
 * `car` is the car.toml id the clicks were made on, `crc` that car.toml's
 * content checksum (ApexContentCrc; 0 when not known), `c` the clicks in
 * ApexCarSetup::EKnob order. Clicks are relative to one car's file, so a
 * setup is only taken for its own car; one from another version of that car
 * (another crc) is taken with its clicks pinned into range and a warning.
 * Version 1; a later one adds keys, and an unknown key is skipped.
 */
namespace ApexSetupShare
{
	inline const TCHAR* Prefix = TEXT("APXS1;");

	struct APEXSIM_API FShared
	{
		FString CarId;
		uint32 CarCrc = 0;
		FString Name;
		FApexCarSetup Setup;
	};

	/** The line, with a name cut to 40 characters and stripped of `;`, `=` and line breaks. */
	APEXSIM_API FString Encode(const FShared& Shared);

	/**
	 * Reads a line anywhere in Text (a chat message may wrap it in quotes or
	 * backticks). False with a reason a player can read when there is none or
	 * it lacks the car or the clicks. Missing knobs are 0, extra ones dropped,
	 * every knob pinned into its range.
	 */
	APEXSIM_API bool Decode(const FString& Text, FShared& Out, FString& OutError);

	/** A file name for an exported setup: `<car>-<name>.txt`, safe on any file system. */
	APEXSIM_API FString FileName(const FShared& Shared);
}
