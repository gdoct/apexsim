#pragma once

#include "CoreMinimal.h"

/**
 * A driver profile's Ed25519 key (server/server/src/driver_keys.rs): the
 * 32-byte private seed stays in the profile's save on this machine; the
 * public half goes out in `Authenticate.driver_key`, and the seed signs the
 * server's `AuthSuccess.key_challenge` in `ProveDriverKey`, which makes the
 * profile's id a proof rather than a claim. OpenSSL's, as the TLS is.
 */
namespace ApexDriverKey
{
	/** The length of a seed and of a public key, bytes. */
	constexpr int32 KeyBytes = 32;

	/** A fresh seed from the system's randomness; false (and empty) when there was none. */
	APEXSIMNET_API bool Generate(TArray<uint8>& OutSeed);

	/** The seed's public key as lower-case hex (64 characters); empty for a seed that is not one. */
	APEXSIMNET_API FString PublicKeyHex(const TArray<uint8>& Seed);

	/** What the proof signs: `apexsim driver key v1:<challenge>:<driver id>` (driver_keys::proof_message). */
	APEXSIMNET_API TArray<uint8> ProofMessage(const FString& Challenge, const FString& DriverId);

	/** The seed's signature over Message as lower-case hex (128 characters); empty on failure. */
	APEXSIMNET_API FString SignHex(const TArray<uint8>& Seed, const TArray<uint8>& Message);
}
