#pragma once

#include "CoreMinimal.h"

/**
 * The content checksum the server sends and the client compares.
 *
 * The client renders a track from a level baked out of the track YAML and
 * shows a car from a mesh imported beside its car.toml; the server simulates
 * from those files themselves. Each file gets one number, computed the same
 * way on both sides, so a client whose bake is a version behind finds out
 * rather than racing on a road the server no longer has:
 *
 *  - server: `content_crc.rs`, sent as `ContentCrc` in the lobby summaries;
 *  - tracks: `scripts/build_track_catalog.py` writes `source_crc`, the sync
 *    commandlet puts it on the `DT_TrackCatalog` row as `SourceCrc`;
 *  - cars: `ApexCarImport` computes it here and puts it on the
 *    `DT_CarCatalog` row.
 *
 * It is the CRC-32 of zlib/PNG (reflected polynomial 0xEDB88320, initial and
 * final XOR 0xFFFFFFFF) over the file's bytes with every carriage return
 * removed first, so a checkout with `core.autocrlf` on and one without agree.
 * The check vector is "123456789" -> 0xCBF43926. Written out rather than
 * routed through FCrc so the contract with the other two implementations is
 * in one place and does not depend on which of the engine's CRCs is standard.
 */
namespace ApexContentCrc
{
	inline uint32 Compute(TArrayView<const uint8> Bytes)
	{
		static uint32 Table[256] = { 0 };
		static bool bTableReady = false;
		if (!bTableReady)
		{
			for (uint32 i = 0; i < 256; ++i)
			{
				uint32 C = i;
				for (int32 K = 0; K < 8; ++K)
				{
					C = (C & 1u) ? (0xEDB88320u ^ (C >> 1)) : (C >> 1);
				}
				Table[i] = C;
			}
			bTableReady = true;
		}

		uint32 Crc = 0xFFFFFFFFu;
		for (const uint8 B : Bytes)
		{
			if (B == '\r')
			{
				continue;
			}
			Crc = Table[(Crc ^ B) & 0xFFu] ^ (Crc >> 8);
		}
		return ~Crc;
	}
}

/** How a local content file compares with the server's copy. */
enum class EApexContentMatch : uint8
{
	/** Both checksums known and equal. */
	Match,
	/** Both known and different: the client's bake is not the server's file. */
	Mismatch,
	/** One side has no checksum (an old server, a row synced before the field, no row at all). */
	Unknown,
};

namespace ApexContent
{
	/** Local (catalog row) against server (lobby summary); 0 on either side means unknown. */
	inline EApexContentMatch Compare(int64 LocalCrc, int64 ServerCrc)
	{
		if (LocalCrc == 0 || ServerCrc == 0)
		{
			return EApexContentMatch::Unknown;
		}
		return (LocalCrc & 0xFFFFFFFF) == (ServerCrc & 0xFFFFFFFF) ? EApexContentMatch::Match : EApexContentMatch::Mismatch;
	}

	/** The line shown to the player and written to the log for a mismatch. */
	inline FString DescribeMismatch(const TCHAR* Kind, const FString& Name, int64 LocalCrc, int64 ServerCrc)
	{
		return FString::Printf(TEXT("%s \"%s\" differs from the server's: local %08X, server %08X. Re-import it or update the client."),
			Kind, *Name, static_cast<uint32>(LocalCrc & 0xFFFFFFFF), static_cast<uint32>(ServerCrc & 0xFFFFFFFF));
	}
}
