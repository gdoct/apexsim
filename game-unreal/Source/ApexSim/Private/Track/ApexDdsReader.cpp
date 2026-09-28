#include "Track/ApexDdsReader.h"

namespace
{
	constexpr uint32 kFlagMipMapCount = 0x20000;
	constexpr uint32 kPfFourCC = 0x4;
	constexpr uint32 kPfRgb = 0x40;
	constexpr uint32 kPfAlphaPixels = 0x1;

	uint32 ReadU32(const uint8* P)
	{
		return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24);
	}

	uint32 FourCC(const char* Text)
	{
		return uint32(uint8(Text[0])) | (uint32(uint8(Text[1])) << 8) | (uint32(uint8(Text[2])) << 16)
			| (uint32(uint8(Text[3])) << 24);
	}

	/** DXGI_FORMAT values the DX10 header may carry. */
	enum : uint32
	{
		DXGI_R8G8B8A8_UNORM = 28,
		DXGI_R8G8B8A8_UNORM_SRGB = 29,
		DXGI_BC1_UNORM = 71,
		DXGI_BC1_UNORM_SRGB = 72,
		DXGI_BC2_UNORM = 74,
		DXGI_BC2_UNORM_SRGB = 75,
		DXGI_BC3_UNORM = 77,
		DXGI_BC3_UNORM_SRGB = 78,
		DXGI_BC5_UNORM = 83,
		DXGI_B8G8R8A8_UNORM = 87,
		DXGI_B8G8R8A8_UNORM_SRGB = 91,
		DXGI_BC7_UNORM = 98,
		DXGI_BC7_UNORM_SRGB = 99,
	};

	int32 BlockBytes(EPixelFormat Format)
	{
		switch (Format)
		{
		case PF_DXT1: return 8;
		case PF_DXT3:
		case PF_DXT5:
		case PF_BC5:
		case PF_BC7: return 16;
		default: return 0;
		}
	}
}	 // namespace

int64 ApexDds::MipBytes(EPixelFormat Format, int32 Width, int32 Height)
{
	if (const int32 Block = BlockBytes(Format))
	{
		return int64(FMath::Max(1, (Width + 3) / 4)) * FMath::Max(1, (Height + 3) / 4) * Block;
	}
	return int64(Width) * Height * 4;
}

bool ApexDds::Parse(TConstArrayView<uint8> Bytes, FApexTrackTexture& Out, FString& OutError)
{
	if (Bytes.Num() < 128 || Bytes[0] != 'D' || Bytes[1] != 'D' || Bytes[2] != 'S' || Bytes[3] != ' ')
	{
		OutError = TEXT("not a DDS file");
		return false;
	}
	const uint8* H = Bytes.GetData() + 4;
	const uint32 HeaderSize = ReadU32(H);
	const uint32 Flags = ReadU32(H + 4);
	const int32 Height = static_cast<int32>(ReadU32(H + 8));
	const int32 Width = static_cast<int32>(ReadU32(H + 12));
	const uint32 MipCount = (Flags & kFlagMipMapCount) ? FMath::Max(1u, ReadU32(H + 24)) : 1u;
	const uint32 PfFlags = ReadU32(H + 76);
	const uint32 PfFourCC = ReadU32(H + 80);
	const uint32 RgbBits = ReadU32(H + 84);
	const uint32 RMask = ReadU32(H + 88);
	const uint32 BMask = ReadU32(H + 96);
	if (HeaderSize != 124 || Width <= 0 || Height <= 0 || Width > 16384 || Height > 16384)
	{
		OutError = FString::Printf(TEXT("implausible DDS header (%u bytes, %d x %d)"), HeaderSize, Width, Height);
		return false;
	}

	int64 DataOffset = 128;
	EPixelFormat Format = PF_Unknown;
	// Source layout for uncompressed data: bytes per pixel and whether the
	// red channel comes first (RGBA) or last (BGRA, Unreal's order).
	int32 SourceBpp = 0;
	bool bRedFirst = false;
	if (PfFlags & kPfFourCC)
	{
		if (PfFourCC == FourCC("DXT1"))
		{
			Format = PF_DXT1;
		}
		else if (PfFourCC == FourCC("DXT3"))
		{
			Format = PF_DXT3;
		}
		else if (PfFourCC == FourCC("DXT5"))
		{
			Format = PF_DXT5;
		}
		else if (PfFourCC == FourCC("ATI2") || PfFourCC == FourCC("BC5U"))
		{
			Format = PF_BC5;
		}
		else if (PfFourCC == FourCC("DX10"))
		{
			if (Bytes.Num() < 148)
			{
				OutError = TEXT("DDS DX10 header is truncated");
				return false;
			}
			DataOffset = 148;
			const uint32 Dxgi = ReadU32(Bytes.GetData() + 128);
			switch (Dxgi)
			{
			case DXGI_BC1_UNORM:
			case DXGI_BC1_UNORM_SRGB: Format = PF_DXT1; break;
			case DXGI_BC2_UNORM:
			case DXGI_BC2_UNORM_SRGB: Format = PF_DXT3; break;
			case DXGI_BC3_UNORM:
			case DXGI_BC3_UNORM_SRGB: Format = PF_DXT5; break;
			case DXGI_BC5_UNORM: Format = PF_BC5; break;
			case DXGI_BC7_UNORM:
			case DXGI_BC7_UNORM_SRGB: Format = PF_BC7; break;
			case DXGI_B8G8R8A8_UNORM:
			case DXGI_B8G8R8A8_UNORM_SRGB:
				Format = PF_B8G8R8A8;
				SourceBpp = 4;
				break;
			case DXGI_R8G8B8A8_UNORM:
			case DXGI_R8G8B8A8_UNORM_SRGB:
				Format = PF_B8G8R8A8;
				SourceBpp = 4;
				bRedFirst = true;
				break;
			default:
				OutError = FString::Printf(TEXT("unsupported DXGI format %u"), Dxgi);
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("unsupported DDS FourCC %c%c%c%c"), char(PfFourCC & 0xFF), char((PfFourCC >> 8) & 0xFF),
				char((PfFourCC >> 16) & 0xFF), char((PfFourCC >> 24) & 0xFF));
			return false;
		}
	}
	else if ((PfFlags & kPfRgb) && (RgbBits == 32 || RgbBits == 24))
	{
		Format = PF_B8G8R8A8;
		SourceBpp = RgbBits / 8;
		bRedFirst = (RMask == 0x000000FFu) && (BMask == 0x00FF0000u);
		(void)kPfAlphaPixels;
	}
	else
	{
		OutError = FString::Printf(TEXT("unsupported DDS pixel format (flags %#x, %u bits)"), PfFlags, RgbBits);
		return false;
	}
	if (BlockBytes(Format) && (Width % 4 != 0 || Height % 4 != 0))
	{
		OutError = FString::Printf(TEXT("a block-compressed %d x %d texture is not a multiple of 4"), Width, Height);
		return false;
	}

	FApexTrackTexture Texture;
	Texture.Width = Width;
	Texture.Height = Height;
	Texture.Format = Format;
	int64 Offset = DataOffset;
	for (uint32 Level = 0; Level < MipCount; ++Level)
	{
		const int32 W = FMath::Max(1, Width >> Level);
		const int32 Hh = FMath::Max(1, Height >> Level);
		int64 SourceBytes = 0;
		if (SourceBpp)
		{
			SourceBytes = int64(W) * Hh * SourceBpp;
		}
		else
		{
			SourceBytes = MipBytes(Format, W, Hh);
		}
		if (Offset + SourceBytes > Bytes.Num())
		{
			if (Level == 0)
			{
				OutError = FString::Printf(TEXT("DDS data is truncated: level 0 needs %lld bytes, %lld left"), SourceBytes,
					int64(Bytes.Num()) - Offset);
				return false;
			}
			break;
		}
		TArray<uint8>& Mip = Texture.Mips.AddDefaulted_GetRef();
		const uint8* Src = Bytes.GetData() + Offset;
		if (!SourceBpp)
		{
			Mip.SetNumUninitialized(static_cast<int32>(SourceBytes));
			FMemory::Memcpy(Mip.GetData(), Src, SourceBytes);
		}
		else
		{
			// To BGRA8, in Unreal's channel order.
			Mip.SetNumUninitialized(W * Hh * 4);
			uint8* Dst = Mip.GetData();
			const int64 Pixels = int64(W) * Hh;
			for (int64 i = 0; i < Pixels; ++i)
			{
				const uint8* P = Src + i * SourceBpp;
				const uint8 A = SourceBpp == 4 ? P[3] : 255;
				if (bRedFirst)
				{
					Dst[i * 4 + 0] = P[2];
					Dst[i * 4 + 1] = P[1];
					Dst[i * 4 + 2] = P[0];
				}
				else
				{
					Dst[i * 4 + 0] = P[0];
					Dst[i * 4 + 1] = P[1];
					Dst[i * 4 + 2] = P[2];
				}
				Dst[i * 4 + 3] = A;
			}
		}
		Offset += SourceBytes;
	}
	Out = MoveTemp(Texture);
	return true;
}
