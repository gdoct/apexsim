#include "ApexTls.h"

namespace ApexTlsPrivate
{
	int32 HexValue(TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) { return C - TEXT('0'); }
		if (C >= TEXT('a') && C <= TEXT('f')) { return C - TEXT('a') + 10; }
		if (C >= TEXT('A') && C <= TEXT('F')) { return C - TEXT('A') + 10; }
		return -1;
	}
}

bool ApexTls::ParseMode(const FString& Text, EApexTlsMode& Out)
{
	const FString Lower = Text.TrimStartAndEnd().ToLower();
	if (Lower == TEXT("auto"))
	{
		Out = EApexTlsMode::Auto;
		return true;
	}
	if (Lower == TEXT("on") || Lower == TEXT("true") || Lower == TEXT("yes") || Lower == TEXT("required"))
	{
		Out = EApexTlsMode::On;
		return true;
	}
	if (Lower == TEXT("off") || Lower == TEXT("false") || Lower == TEXT("no"))
	{
		Out = EApexTlsMode::Off;
		return true;
	}
	return false;
}

const TCHAR* ApexTls::ModeName(EApexTlsMode Mode)
{
	switch (Mode)
	{
	case EApexTlsMode::On:  return TEXT("on");
	case EApexTlsMode::Off: return TEXT("off");
	default:                return TEXT("auto");
	}
}

bool ApexTls::NormaliseFingerprint(const FString& Text, FString& Out)
{
	FString Body = Text.TrimStartAndEnd();
	if (Body.StartsWith(TEXT("sha256:"), ESearchCase::IgnoreCase))
	{
		Body.RightChopInline(7);
	}
	else if (Body.StartsWith(TEXT("sha256 fingerprint="), ESearchCase::IgnoreCase))
	{
		// `openssl x509 -fingerprint -sha256` prints "SHA256 Fingerprint=AB:CD...".
		Body.RightChopInline(19);
	}

	FString Hex;
	Hex.Reserve(64);
	for (const TCHAR C : Body)
	{
		if (C == TEXT(':') || C == TEXT(' ') || C == TEXT('-') || C == TEXT('\t'))
		{
			continue;
		}
		if (ApexTlsPrivate::HexValue(C) < 0)
		{
			return false;
		}
		Hex.AppendChar(FChar::ToUpper(C));
	}
	if (Hex.Len() != 64)
	{
		return false;
	}
	Out = MoveTemp(Hex);
	return true;
}

bool ApexTls::CanonicalFingerprint(const FString& Text, FString& Out)
{
	FString Hex;
	if (!NormaliseFingerprint(Text, Hex))
	{
		return false;
	}
	FString Spelt;
	Spelt.Reserve(95);
	for (int32 Index = 0; Index < Hex.Len(); Index += 2)
	{
		if (Index > 0)
		{
			Spelt.AppendChar(TEXT(':'));
		}
		Spelt.AppendChar(Hex[Index]);
		Spelt.AppendChar(Hex[Index + 1]);
	}
	Out = MoveTemp(Spelt);
	return true;
}

FString ApexTls::FormatFingerprint(TArrayView<const uint8> Digest)
{
	FString Out;
	Out.Reserve(Digest.Num() * 3);
	for (int32 Index = 0; Index < Digest.Num(); ++Index)
	{
		if (Index > 0)
		{
			Out.AppendChar(TEXT(':'));
		}
		Out += FString::Printf(TEXT("%02X"), Digest[Index]);
	}
	return Out;
}

bool ApexTls::FingerprintMatches(const FString& Pinned, TArrayView<const uint8> Digest)
{
	FString Want;
	if (Digest.Num() != 32 || !NormaliseFingerprint(Pinned, Want))
	{
		return false;
	}
	FString Have;
	NormaliseFingerprint(FormatFingerprint(Digest), Have);
	return Want == Have;
}

bool ApexTls::LooksLikeTlsRecord(uint8 FirstByte)
{
	// change_cipher_spec 20, alert 21, handshake 22, application_data 23, heartbeat 24.
	return FirstByte >= 0x14 && FirstByte <= 0x18;
}

EApexTlsFailure ApexTls::ClassifyFailure(int64 BytesReceived, uint8 FirstByte, EApexTlsFailure Otherwise)
{
	if (BytesReceived <= 0)
	{
		return EApexTlsFailure::ClosedBeforeReply;
	}
	if (!LooksLikeTlsRecord(FirstByte))
	{
		return EApexTlsFailure::NotTls;
	}
	return Otherwise;
}

bool ApexTls::ShouldFallBackToPlaintext(const FApexTlsOptions& Options, EApexTlsFailure Failure)
{
	if (Options.Mode != EApexTlsMode::Auto || Options.IsPinned())
	{
		return false;
	}
	return Failure == EApexTlsFailure::ClosedBeforeReply || Failure == EApexTlsFailure::NotTls;
}

bool ApexTls::IsPermanent(EApexTlsFailure Failure)
{
	return Failure == EApexTlsFailure::Untrusted || Failure == EApexTlsFailure::Setup;
}
