#pragma once

#include "CoreMinimal.h"

/**
 * A player's own livery as it travels between clients
 * (server/protocol/src/livery.rs `LiveryPackage`): one `[[livery]]` table of
 * a livery.toml with its images inlined, one MessagePack map with named
 * keys. The client packs it from the player's own livery folder, uploads it
 * in chunks (UploadLivery) named by its SHA-256, and unpacks the ones other
 * players wear (fetched with RequestLivery).
 */
struct APEXSIMNET_API FApexLiveryPackage
{
	/** The car it is painted for: its catalog id (the server's CarConfigId). */
	FString Car;
	FString Name;
	/** Linear RGB; zero alpha when the livery names none. */
	FLinearColor Paint = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	FLinearColor Accent = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	/** Negative when the livery names none. */
	float Metallic = -1.0f;
	/** The `car_logo` image (PNG or JPEG bytes); empty for none. */
	TArray<uint8> Logo;
	/** Every `car_skin*` slot's image; empty for none. */
	TArray<uint8> Skin;
	/** Named slots' images, (slot, bytes). */
	TArray<TPair<FString, TArray<uint8>>> Textures;
};

namespace ApexLiveryPackage
{
	/** A package's bytes at most: 1 MiB (livery.rs MAX_LIVERY_BYTES). */
	inline constexpr int32 MaxBytes = 1 << 20;
	/** One UploadLivery / LiveryChunk's bytes at most: 256 KiB. */
	inline constexpr int32 ChunkBytes = 256 * 1024;
	/** An image's side at most, pixels. */
	inline constexpr int32 MaxImageSide = 4096;
	inline constexpr int32 MaxTextures = 8;
	inline constexpr int32 MaxNameChars = 40;

	/** The package's bytes, as livery.rs writes it (keys in its order, absent fields left off). */
	APEXSIMNET_API TArray<uint8> Encode(const FApexLiveryPackage& Package);

	/** The package in Bytes, checked as the server checks it (Check); false with a reason otherwise. */
	APEXSIMNET_API bool Decode(TArrayView<const uint8> Bytes, FApexLiveryPackage& Out, FString& OutError);

	/** The server's checks: size, car id, name, colours within 0..1, PNG/JPEG images within the side, slot names. */
	APEXSIMNET_API bool Check(const FApexLiveryPackage& Package, int32 EncodedBytes, FString& OutError);

	/** A PNG's or JPEG's width and height from its header; false for anything else or a side over MaxImageSide. */
	APEXSIMNET_API bool ImageSize(TArrayView<const uint8> Bytes, int32& OutWidth, int32& OutHeight);

	/** True for a JPEG's bytes (FF D8 FF), false for a PNG's or anything else. */
	APEXSIMNET_API bool IsJpeg(TArrayView<const uint8> Bytes);

	/** The SHA-256 of Bytes as lowercase hex: the livery's name on the wire. */
	APEXSIMNET_API FString Hash(TArrayView<const uint8> Bytes);

	/** How many chunks a package of Bytes comes in (at least one). */
	inline int32 ChunkCount(int32 Bytes) { return FMath::Max(1, (Bytes + ChunkBytes - 1) / ChunkBytes); }
}
