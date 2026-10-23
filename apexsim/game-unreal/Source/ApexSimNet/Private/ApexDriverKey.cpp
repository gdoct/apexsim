#include "ApexDriverKey.h"

#include "ApexSimNetModule.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
// OpenSSL's UI typedef clashes with Unreal's UI namespace (as in
// ApexTlsSession.cpp).
#define UI UI_ST
#include <wincrypt.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#else
THIRD_PARTY_INCLUDES_START
#define UI UI_ST
#include <openssl/evp.h>
#include <openssl/rand.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#endif

namespace ApexDriverKey
{
	namespace
	{
		FString ToHex(const uint8* Bytes, int32 Count)
		{
			return BytesToHex(Bytes, Count).ToLower();
		}

		/** The seed as an OpenSSL private key; null for a seed that is not one. Free with EVP_PKEY_free. */
		EVP_PKEY* KeyOf(const TArray<uint8>& Seed)
		{
			return Seed.Num() == KeyBytes
				? EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, Seed.GetData(), Seed.Num())
				: nullptr;
		}
	}

	bool Generate(TArray<uint8>& OutSeed)
	{
		OutSeed.SetNumZeroed(KeyBytes);
		if (RAND_bytes(OutSeed.GetData(), KeyBytes) != 1)
		{
			OutSeed.Reset();
			return false;
		}
		return true;
	}

	FString PublicKeyHex(const TArray<uint8>& Seed)
	{
		EVP_PKEY* Key = KeyOf(Seed);
		if (!Key)
		{
			return FString();
		}
		uint8 Public[KeyBytes];
		size_t Length = sizeof(Public);
		const bool bOk = EVP_PKEY_get_raw_public_key(Key, Public, &Length) == 1 && Length == KeyBytes;
		EVP_PKEY_free(Key);
		return bOk ? ToHex(Public, KeyBytes) : FString();
	}

	TArray<uint8> ProofMessage(const FString& Challenge, const FString& DriverId)
	{
		const FTCHARToUTF8 Text(*FString::Printf(TEXT("apexsim driver key v1:%s:%s"), *Challenge, *DriverId));
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
	}

	FString SignHex(const TArray<uint8>& Seed, const TArray<uint8>& Message)
	{
		EVP_PKEY* Key = KeyOf(Seed);
		if (!Key)
		{
			return FString();
		}
		uint8 Signature[64];
		size_t Length = sizeof(Signature);
		EVP_MD_CTX* Context = EVP_MD_CTX_new();
		const bool bOk = Context
			&& EVP_DigestSignInit(Context, nullptr, nullptr, nullptr, Key) == 1
			&& EVP_DigestSign(Context, Signature, &Length, Message.GetData(), Message.Num()) == 1
			&& Length == sizeof(Signature);
		EVP_MD_CTX_free(Context);
		EVP_PKEY_free(Key);
		if (!bOk)
		{
			UE_LOG(LogApexSimNet, Warning, TEXT("Driver key: could not sign the challenge"));
		}
		return bOk ? ToHex(Signature, sizeof(Signature)) : FString();
	}
}
