#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"

/**
 * A texture as a runtime track carries it: the mips of a DDS file, ready to
 * be copied into a transient `UTexture2D` block for block, with no decode.
 * That is what lets an imported circuit's few hundred megabytes of
 * textures cost a few hundred megabytes rather than four times that as
 * BGRA8 (`docs/AC_TRACK_IMPORT.md`, "Compressed runtime textures").
 */
struct FApexTrackTexture
{
	/** The path the manifest named, relative to its own directory. */
	FString Path;
	int32 Width = 0;
	int32 Height = 0;
	/** PF_DXT1, PF_DXT3, PF_DXT5, PF_BC5, PF_BC7 or PF_B8G8R8A8. */
	EPixelFormat Format = PF_Unknown;
	/** Colour data is sRGB; a normal or mask map would not be. */
	bool bSRGB = true;
	/** Largest first, each exactly the bytes Unreal expects for its level. */
	TArray<TArray<uint8>> Mips;

	int64 NumBytes() const
	{
		int64 Total = 0;
		for (const TArray<uint8>& Mip : Mips)
		{
			Total += Mip.Num();
		}
		return Total;
	}
};

namespace ApexDds
{
	/**
	 * Parse a DDS file (the classic 124-byte header, or with the DX10
	 * extension) into its mips. Block-compressed formats are copied as they
	 * are; 24- and 32-bit uncompressed ones become BGRA8. Fails on anything
	 * else, on a compressed texture whose top level is not a multiple of
	 * four (which a transient texture cannot hold) and on a truncated top
	 * level; a truncated lower mip only ends the chain there.
	 */
	APEXSIM_API bool Parse(TConstArrayView<uint8> Bytes, FApexTrackTexture& Out, FString& OutError);

	/** Bytes one mip level of `Format` takes at `Width` x `Height`. */
	APEXSIM_API int64 MipBytes(EPixelFormat Format, int32 Width, int32 Height);
}	 // namespace ApexDds
