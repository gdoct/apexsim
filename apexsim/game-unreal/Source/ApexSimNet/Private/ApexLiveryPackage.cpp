#include "ApexLiveryPackage.h"

#include "MsgPack/MsgPackReader.h"
#include "MsgPack/MsgPackWriter.h"
#include "Misc/Guid.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
// OpenSSL's UI typedef clashes with Unreal's UI namespace (as in
// ApexTlsSession.cpp).
#define UI UI_ST
#include <wincrypt.h>
#include <openssl/evp.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#else
THIRD_PARTY_INCLUDES_START
#define UI UI_ST
#include <openssl/evp.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#endif

namespace ApexLiveryPackagePrivate
{
	void WriteColour(FMsgPackWriter& Writer, const FLinearColor& Colour)
	{
		Writer.WriteArrayHeader(3);
		Writer.WriteFloat(Colour.R);
		Writer.WriteFloat(Colour.G);
		Writer.WriteFloat(Colour.B);
	}

	bool ReadColour(FMsgPackReader& Reader, FLinearColor& Out)
	{
		int32 Count = 0;
		if (!Reader.ReadArrayHeader(Count) || Count != 3)
		{
			return false;
		}
		float Rgb[3] = {0.0f, 0.0f, 0.0f};
		for (float& Channel : Rgb)
		{
			if (!Reader.ReadFloat(Channel))
			{
				return false;
			}
		}
		Out = FLinearColor(Rgb[0], Rgb[1], Rgb[2], 1.0f);
		return true;
	}

	bool ReadBytes(FMsgPackReader& Reader, TArray<uint8>& Out)
	{
		if (Reader.TryReadNil())
		{
			Out.Reset();
			return true;
		}
		TArrayView<const uint8> View;
		if (!Reader.ReadBinary(View))
		{
			return false;
		}
		Out = TArray<uint8>(View);
		return true;
	}

	bool InUnit(float Value) { return FMath::IsFinite(Value) && Value >= 0.0f && Value <= 1.0f; }
}

namespace ApexLiveryPackage
{
	using namespace ApexLiveryPackagePrivate;

	TArray<uint8> Encode(const FApexLiveryPackage& Package)
	{
		const bool bPaint = Package.Paint.A > 0.0f;
		const bool bAccent = Package.Accent.A > 0.0f;
		const bool bMetallic = Package.Metallic >= 0.0f;
		const bool bLogo = Package.Logo.Num() > 0;
		const bool bSkin = Package.Skin.Num() > 0;
		const bool bTextures = Package.Textures.Num() > 0;
		int32 Reserve = 256 + Package.Logo.Num() + Package.Skin.Num();
		for (const TPair<FString, TArray<uint8>>& Texture : Package.Textures)
		{
			Reserve += Texture.Value.Num() + 80;
		}
		FMsgPackWriter Writer(Reserve);
		Writer.WriteMapHeader(2 + bPaint + bAccent + bMetallic + bLogo + bSkin + bTextures);
		Writer.WriteString("car");
		Writer.WriteString(Package.Car);
		Writer.WriteString("name");
		Writer.WriteString(Package.Name);
		if (bPaint)
		{
			Writer.WriteString("paint");
			WriteColour(Writer, Package.Paint);
		}
		if (bAccent)
		{
			Writer.WriteString("accent");
			WriteColour(Writer, Package.Accent);
		}
		if (bMetallic)
		{
			Writer.WriteString("metallic");
			Writer.WriteFloat(Package.Metallic);
		}
		if (bLogo)
		{
			Writer.WriteString("logo");
			Writer.WriteBinary(Package.Logo);
		}
		if (bSkin)
		{
			Writer.WriteString("skin");
			Writer.WriteBinary(Package.Skin);
		}
		if (bTextures)
		{
			Writer.WriteString("textures");
			Writer.WriteArrayHeader(Package.Textures.Num());
			for (const TPair<FString, TArray<uint8>>& Texture : Package.Textures)
			{
				Writer.WriteMapHeader(2);
				Writer.WriteString("slot");
				Writer.WriteString(Texture.Key);
				Writer.WriteString("image");
				Writer.WriteBinary(Texture.Value);
			}
		}
		return MoveTemp(Writer.GetBuffer());
	}

	bool Decode(TArrayView<const uint8> Bytes, FApexLiveryPackage& Out, FString& OutError)
	{
		Out = FApexLiveryPackage();
		if (Bytes.Num() > MaxBytes)
		{
			OutError = FString::Printf(TEXT("the livery is %d bytes, over the %d allowed"), Bytes.Num(), MaxBytes);
			return false;
		}
		FMsgPackReader Reader(Bytes);
		int32 Fields = 0;
		bool bOk = Reader.ReadMapHeader(Fields);
		bool bCar = false;
		bool bName = false;
		for (int32 i = 0; bOk && i < Fields; ++i)
		{
			FString Key;
			bOk = Reader.ReadString(Key);
			if (!bOk)
			{
				break;
			}
			if (Key == TEXT("car"))           { bOk = Reader.ReadString(Out.Car); bCar = true; }
			else if (Key == TEXT("name"))     { bOk = Reader.ReadString(Out.Name); bName = true; }
			else if (Key == TEXT("paint"))    { bOk = Reader.TryReadNil() || ReadColour(Reader, Out.Paint); }
			else if (Key == TEXT("accent"))   { bOk = Reader.TryReadNil() || ReadColour(Reader, Out.Accent); }
			else if (Key == TEXT("metallic")) { bOk = Reader.TryReadNil() || Reader.ReadFloat(Out.Metallic); }
			else if (Key == TEXT("logo"))     { bOk = ReadBytes(Reader, Out.Logo); }
			else if (Key == TEXT("skin"))     { bOk = ReadBytes(Reader, Out.Skin); }
			else if (Key == TEXT("textures"))
			{
				int32 Count = 0;
				bOk = Reader.ReadArrayHeader(Count) && Count <= MaxTextures;
				for (int32 t = 0; bOk && t < Count; ++t)
				{
					TPair<FString, TArray<uint8>>& Texture = Out.Textures.AddDefaulted_GetRef();
					int32 TextureFields = 0;
					bOk = Reader.ReadMapHeader(TextureFields);
					for (int32 f = 0; bOk && f < TextureFields; ++f)
					{
						FString TextureKey;
						bOk = Reader.ReadString(TextureKey);
						if (bOk && TextureKey == TEXT("slot"))       { bOk = Reader.ReadString(Texture.Key); }
						else if (bOk && TextureKey == TEXT("image")) { bOk = ReadBytes(Reader, Texture.Value); }
						else if (bOk)                                { bOk = Reader.SkipValue(); }
					}
				}
			}
			else { bOk = Reader.SkipValue(); }
		}
		if (!bOk || !bCar || !bName)
		{
			OutError = TEXT("the livery does not read");
			return false;
		}
		return Check(Out, Bytes.Num(), OutError);
	}

	bool Check(const FApexLiveryPackage& Package, int32 EncodedBytes, FString& OutError)
	{
		if (EncodedBytes > MaxBytes)
		{
			OutError = FString::Printf(TEXT("the livery is %d bytes, over the %d allowed"), EncodedBytes, MaxBytes);
			return false;
		}
		FGuid Car;
		if (!FGuid::Parse(Package.Car, Car))
		{
			OutError = TEXT("the livery's car is not a car id");
			return false;
		}
		const FString Name = Package.Name.TrimStartAndEnd();
		bool bControl = false;
		for (const TCHAR Char : Name)
		{
			bControl |= Char < 0x20 || Char == 0x7F;
		}
		if (Name.IsEmpty() || Name.Len() > MaxNameChars || bControl)
		{
			OutError = FString::Printf(TEXT("the livery needs a name of 1 to %d characters"), MaxNameChars);
			return false;
		}
		for (const FLinearColor* Colour : {&Package.Paint, &Package.Accent})
		{
			if (Colour->A > 0.0f && !(InUnit(Colour->R) && InUnit(Colour->G) && InUnit(Colour->B)))
			{
				OutError = TEXT("the livery's colours must be within 0 to 1");
				return false;
			}
		}
		if (Package.Metallic >= 0.0f && !InUnit(Package.Metallic))
		{
			OutError = TEXT("the livery's colours must be within 0 to 1");
			return false;
		}
		if (Package.Paint.A <= 0.0f && Package.Skin.IsEmpty() && Package.Textures.IsEmpty())
		{
			OutError = TEXT("the livery needs a paint, a skin or textures");
			return false;
		}
		if (Package.Textures.Num() > MaxTextures)
		{
			OutError = TEXT("the livery has too many textures");
			return false;
		}
		TSet<FString> Slots;
		for (const TPair<FString, TArray<uint8>>& Texture : Package.Textures)
		{
			bool bName = !Texture.Key.IsEmpty() && Texture.Key.Len() <= 64;
			for (const TCHAR Char : Texture.Key)
			{
				bName &= (Char < 128 && FChar::IsAlnum(Char)) || Char == TEXT('_');
			}
			bool bAlready = false;
			Slots.Add(Texture.Key.ToLower(), &bAlready);
			if (!bName || bAlready)
			{
				OutError = TEXT("the livery's texture slots must be distinct names of letters, digits and _");
				return false;
			}
		}
		int32 Width = 0;
		int32 Height = 0;
		if ((Package.Logo.Num() > 0 && !ImageSize(Package.Logo, Width, Height))
			|| (Package.Skin.Num() > 0 && !ImageSize(Package.Skin, Width, Height)))
		{
			OutError = TEXT("the livery's images must be PNG or JPEG");
			return false;
		}
		for (const TPair<FString, TArray<uint8>>& Texture : Package.Textures)
		{
			if (!ImageSize(Texture.Value, Width, Height))
			{
				OutError = TEXT("the livery's images must be PNG or JPEG");
				return false;
			}
		}
		return true;
	}

	bool ImageSize(TArrayView<const uint8> Bytes, int32& OutWidth, int32& OutHeight)
	{
		auto Within = [&OutWidth, &OutHeight](uint32 W, uint32 H)
		{
			OutWidth = static_cast<int32>(FMath::Min<uint32>(W, MAX_int32));
			OutHeight = static_cast<int32>(FMath::Min<uint32>(H, MAX_int32));
			return W > 0 && H > 0 && W <= static_cast<uint32>(MaxImageSide) && H <= static_cast<uint32>(MaxImageSide);
		};
		auto Be32 = [&Bytes](int32 At)
		{
			return (uint32(Bytes[At]) << 24) | (uint32(Bytes[At + 1]) << 16) | (uint32(Bytes[At + 2]) << 8) | uint32(Bytes[At + 3]);
		};
		static const uint8 Png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		if (Bytes.Num() >= 24 && FMemory::Memcmp(Bytes.GetData(), Png, 8) == 0
			&& FMemory::Memcmp(Bytes.GetData() + 12, "IHDR", 4) == 0)
		{
			return Within(Be32(16), Be32(20));
		}
		if (IsJpeg(Bytes))
		{
			// The markers up to the first SOFn (C0-CF but C4, C8, CC).
			int32 At = 2;
			while (At + 4 <= Bytes.Num())
			{
				if (Bytes[At] != 0xFF)
				{
					return false;
				}
				const uint8 Marker = Bytes[At + 1];
				if (Marker == 0xFF)
				{
					++At;
					continue;
				}
				const int32 Length = (int32(Bytes[At + 2]) << 8) | int32(Bytes[At + 3]);
				if (Length < 2)
				{
					return false;
				}
				if (Marker >= 0xC0 && Marker <= 0xCF && Marker != 0xC4 && Marker != 0xC8 && Marker != 0xCC)
				{
					if (At + 9 > Bytes.Num())
					{
						return false;
					}
					const uint32 H = (uint32(Bytes[At + 5]) << 8) | uint32(Bytes[At + 6]);
					const uint32 W = (uint32(Bytes[At + 7]) << 8) | uint32(Bytes[At + 8]);
					return Within(W, H);
				}
				At += 2 + Length;
			}
		}
		return false;
	}

	bool IsJpeg(TArrayView<const uint8> Bytes)
	{
		return Bytes.Num() >= 4 && Bytes[0] == 0xFF && Bytes[1] == 0xD8 && Bytes[2] == 0xFF;
	}

	FString Hash(TArrayView<const uint8> Bytes)
	{
		uint8 Digest[EVP_MAX_MD_SIZE];
		unsigned int Length = 0;
		if (EVP_Digest(Bytes.GetData(), Bytes.Num(), Digest, &Length, EVP_sha256(), nullptr) != 1)
		{
			return FString();
		}
		return BytesToHex(Digest, static_cast<int32>(Length)).ToLower();
	}
}
