#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"

/**
 * A BC1 / BC3 encoder for textures the game makes at runtime (a car's GLB
 * images, its livery PNGs, an uncompressed DDS beside a track), which the
 * cook never sees and so would otherwise sit in video memory as BGRA8: a
 * shipped car's three 2048 maps are 50 MB that way, 6-13 MB compressed.
 * The engine's own encoders are editor-only.
 *
 * Principal axis endpoints refined by least squares, single-colour blocks
 * from exhaustive tables, the alpha block tried in both of its modes: 41-61
 * dB PSNR on the repo's car maps. A 2048 map takes 30-50 ms on one core and
 * a few ms spread over the task graph, which a large level is. Pure data:
 * runs on any thread.
 */
namespace ApexBc
{
	/** Whether every pixel of a BGRA8 image is opaque. */
	APEXSIM_API bool IsOpaque(TConstArrayView<uint8> Bgra);

	/**
	 * One BGRA8 level as BC1 (8 bytes a block, alpha ignored) or BC3 (16).
	 * Any size: the blocks past the edge repeat its last row and column.
	 */
	APEXSIM_API void EncodeLevel(const uint8* Bgra, int32 Width, int32 Height, bool bAlpha, TArray<uint8>& Out);

	/**
	 * A BGRA8 chain (largest first, each level half the last, at least 1)
	 * compressed in place: PF_DXT1 when the top level is opaque, else
	 * PF_DXT5. A top level that is not a multiple of four, which a transient
	 * texture cannot hold compressed, is left as it is (PF_B8G8R8A8).
	 */
	APEXSIM_API EPixelFormat CompressChain(int32 Width, int32 Height, TArray<TArray<uint8>>& Mips);

	/** BGRA8 of one BC1 or BC3 block (`Out` 64 bytes); for the tests. */
	APEXSIM_API void DecodeBlock(const uint8* Block, bool bAlpha, uint8* Out);
}	 // namespace ApexBc
