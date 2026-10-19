#include "Cars/ApexBlockCompress.h"

#include "Async/ParallelFor.h"

// Nested in ApexBc so a unity build cannot fold these into a neighbour's helpers.
namespace ApexBc
{
namespace
{
	/** Blocks in a level below which spreading it over the task graph costs more than it saves. */
	constexpr int32 kParallelBlocks = 4096;

	/** A block's sixteen pixels, row by row, as red, green, blue, alpha. */
	using FBlockPixels = int32[16][4];

	int32 Expand5(int32 V)
	{
		return (V << 3) | (V >> 2);
	}

	int32 Expand6(int32 V)
	{
		return (V << 2) | (V >> 4);
	}

	/** The nearest 5:6:5 colour, rounded rather than truncated. */
	uint16 Pack565(int32 R, int32 G, int32 B)
	{
		const int32 R5 = (FMath::Clamp(R, 0, 255) * 31 + 127) / 255;
		const int32 G6 = (FMath::Clamp(G, 0, 255) * 63 + 127) / 255;
		const int32 B5 = (FMath::Clamp(B, 0, 255) * 31 + 127) / 255;
		return uint16((R5 << 11) | (G6 << 5) | B5);
	}

	void Unpack565(uint16 C, int32 Out[3])
	{
		Out[0] = Expand5((C >> 11) & 31);
		Out[1] = Expand6((C >> 5) & 63);
		Out[2] = Expand5(C & 31);
	}

	/** The four colours of a four-colour block. */
	void Palette(uint16 C0, uint16 C1, int32 Out[4][3])
	{
		Unpack565(C0, Out[0]);
		Unpack565(C1, Out[1]);
		for (int32 c = 0; c < 3; ++c)
		{
			Out[2][c] = (2 * Out[0][c] + Out[1][c] + 1) / 3;
			Out[3][c] = (Out[0][c] + 2 * Out[1][c] + 1) / 3;
		}
	}

	/** Each pixel's nearest palette entry, two bits a pixel; the squared error in `OutError`. */
	uint32 MatchColours(const FBlockPixels& Px, uint16 C0, uint16 C1, int32& OutError)
	{
		int32 Pal[4][3];
		Palette(C0, C1, Pal);
		uint32 Indices = 0;
		OutError = 0;
		for (int32 i = 0; i < 16; ++i)
		{
			int32 Best = 0;
			int32 BestError = MAX_int32;
			for (int32 k = 0; k < 4; ++k)
			{
				const int32 Dr = Px[i][0] - Pal[k][0];
				const int32 Dg = Px[i][1] - Pal[k][1];
				const int32 Db = Px[i][2] - Pal[k][2];
				const int32 Error = Dr * Dr + Dg * Dg + Db * Db;
				if (Error < BestError)
				{
					BestError = Error;
					Best = k;
				}
			}
			Indices |= uint32(Best) << (2 * i);
			OutError += BestError;
		}
		return Indices;
	}

	/**
	 * For every 8-bit value, the pair of quantised endpoints whose
	 * two-thirds colour comes nearest it: a block of one colour is drawn
	 * from that entry, which is far closer than the colour's own 5:6:5.
	 * The spread between the two is charged a little, so a decoder that
	 * rounds the thirds differently still lands near.
	 */
	struct FSingleColourTables
	{
		uint8 Five[256][2];
		uint8 Six[256][2];

		FSingleColourTables()
		{
			Fill(Five, 31, Expand5);
			Fill(Six, 63, Expand6);
		}

		static void Fill(uint8 (&Table)[256][2], int32 Max, int32 (*Expand)(int32))
		{
			for (int32 Value = 0; Value < 256; ++Value)
			{
				int32 BestScore = MAX_int32;
				for (int32 A = 0; A <= Max; ++A)
				{
					for (int32 B = 0; B <= Max; ++B)
					{
						const int32 Ea = Expand(A);
						const int32 Eb = Expand(B);
						const int32 Score = FMath::Abs((2 * Ea + Eb + 1) / 3 - Value) * 100 + FMath::Abs(Ea - Eb) * 3;
						if (Score < BestScore)
						{
							BestScore = Score;
							Table[Value][0] = uint8(A);
							Table[Value][1] = uint8(B);
						}
					}
				}
			}
		}
	};

	const FSingleColourTables& SingleColour()
	{
		static const FSingleColourTables Tables;
		return Tables;
	}

	/**
	 * The endpoints that fit the pixels best for the given selectors, by
	 * least squares; false when every pixel picked the same end.
	 */
	bool RefineEndpoints(const FBlockPixels& Px, uint32 Indices, uint16& OutC0, uint16& OutC1)
	{
		// Thirds of each end, per palette entry.
		static constexpr int32 W0[4] = {3, 0, 2, 1};
		static constexpr int32 W1[4] = {0, 3, 1, 2};
		int32 Aa = 0, Bb = 0, Ab = 0;
		int32 Ax[3] = {0, 0, 0};
		int32 Bx[3] = {0, 0, 0};
		for (int32 i = 0; i < 16; ++i)
		{
			const int32 k = (Indices >> (2 * i)) & 3;
			Aa += W0[k] * W0[k];
			Bb += W1[k] * W1[k];
			Ab += W0[k] * W1[k];
			for (int32 c = 0; c < 3; ++c)
			{
				Ax[c] += W0[k] * Px[i][c];
				Bx[c] += W1[k] * Px[i][c];
			}
		}
		const int64 Det = int64(Aa) * Bb - int64(Ab) * Ab;
		if (Det == 0)
		{
			return false;
		}
		int32 E0[3];
		int32 E1[3];
		for (int32 c = 0; c < 3; ++c)
		{
			// The weights are in thirds, so the solve gives a third of each end.
			const double U = double(int64(Ax[c]) * Bb - int64(Bx[c]) * Ab) / double(Det);
			const double V = double(int64(Bx[c]) * Aa - int64(Ax[c]) * Ab) / double(Det);
			E0[c] = FMath::RoundToInt32(3.0 * U);
			E1[c] = FMath::RoundToInt32(3.0 * V);
		}
		OutC0 = Pack565(E0[0], E0[1], E0[2]);
		OutC1 = Pack565(E1[0], E1[1], E1[2]);
		return true;
	}

	void WriteColourBlock(uint16 C0, uint16 C1, uint32 Indices, uint8* Out)
	{
		// The first end must be the greater, or the block decodes in the
		// three-colour mode whose fourth entry is transparent black.
		if (C0 < C1)
		{
			Swap(C0, C1);
			Indices ^= 0x55555555u;
		}
		else if (C0 == C1)
		{
			Indices = 0;
		}
		Out[0] = uint8(C0 & 0xFF);
		Out[1] = uint8(C0 >> 8);
		Out[2] = uint8(C1 & 0xFF);
		Out[3] = uint8(C1 >> 8);
		Out[4] = uint8(Indices & 0xFF);
		Out[5] = uint8((Indices >> 8) & 0xFF);
		Out[6] = uint8((Indices >> 16) & 0xFF);
		Out[7] = uint8(Indices >> 24);
	}

	void EncodeColourBlock(const FBlockPixels& Px, uint8* Out)
	{
		bool bSolid = true;
		for (int32 i = 1; i < 16 && bSolid; ++i)
		{
			bSolid = Px[i][0] == Px[0][0] && Px[i][1] == Px[0][1] && Px[i][2] == Px[0][2];
		}
		if (bSolid)
		{
			const FSingleColourTables& T = SingleColour();
			const uint16 C0 = uint16((T.Five[Px[0][0]][0] << 11) | (T.Six[Px[0][1]][0] << 5) | T.Five[Px[0][2]][0]);
			const uint16 C1 = uint16((T.Five[Px[0][0]][1] << 11) | (T.Six[Px[0][1]][1] << 5) | T.Five[Px[0][2]][1]);
			WriteColourBlock(C0, C1, 0xAAAAAAAAu, Out);
			return;
		}

		// The principal axis of the colours, by power iteration on their covariance.
		double Mean[3] = {0, 0, 0};
		int32 Min[3] = {255, 255, 255};
		int32 Max[3] = {0, 0, 0};
		for (int32 i = 0; i < 16; ++i)
		{
			for (int32 c = 0; c < 3; ++c)
			{
				Mean[c] += Px[i][c];
				Min[c] = FMath::Min(Min[c], Px[i][c]);
				Max[c] = FMath::Max(Max[c], Px[i][c]);
			}
		}
		for (double& M : Mean)
		{
			M /= 16.0;
		}
		double Cov[6] = {0, 0, 0, 0, 0, 0};
		for (int32 i = 0; i < 16; ++i)
		{
			const double R = Px[i][0] - Mean[0];
			const double G = Px[i][1] - Mean[1];
			const double B = Px[i][2] - Mean[2];
			Cov[0] += R * R;
			Cov[1] += R * G;
			Cov[2] += R * B;
			Cov[3] += G * G;
			Cov[4] += G * B;
			Cov[5] += B * B;
		}
		double Axis[3] = {double(Max[0] - Min[0]), double(Max[1] - Min[1]), double(Max[2] - Min[2])};
		for (int32 Iteration = 0; Iteration < 4; ++Iteration)
		{
			const double X = Axis[0] * Cov[0] + Axis[1] * Cov[1] + Axis[2] * Cov[2];
			const double Y = Axis[0] * Cov[1] + Axis[1] * Cov[3] + Axis[2] * Cov[4];
			const double Z = Axis[0] * Cov[2] + Axis[1] * Cov[4] + Axis[2] * Cov[5];
			const double Largest = FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z));
			if (Largest < 1e-9)
			{
				break;
			}
			Axis[0] = X / Largest;
			Axis[1] = Y / Largest;
			Axis[2] = Z / Largest;
		}
		if (FMath::Abs(Axis[0]) + FMath::Abs(Axis[1]) + FMath::Abs(Axis[2]) < 1e-9)
		{
			// Colours that differ along no axis but brightness.
			Axis[0] = 0.299;
			Axis[1] = 0.587;
			Axis[2] = 0.114;
		}
		int32 Lo = 0;
		int32 Hi = 0;
		double LoDot = MAX_dbl;
		double HiDot = -MAX_dbl;
		for (int32 i = 0; i < 16; ++i)
		{
			const double Dot = Px[i][0] * Axis[0] + Px[i][1] * Axis[1] + Px[i][2] * Axis[2];
			if (Dot < LoDot)
			{
				LoDot = Dot;
				Lo = i;
			}
			if (Dot > HiDot)
			{
				HiDot = Dot;
				Hi = i;
			}
		}

		uint16 C0 = Pack565(Px[Hi][0], Px[Hi][1], Px[Hi][2]);
		uint16 C1 = Pack565(Px[Lo][0], Px[Lo][1], Px[Lo][2]);
		int32 Error = 0;
		uint32 Indices = MatchColours(Px, C0, C1, Error);
		for (int32 Pass = 0; Pass < 2 && Error > 0; ++Pass)
		{
			uint16 R0 = 0;
			uint16 R1 = 0;
			if (!RefineEndpoints(Px, Indices, R0, R1) || (R0 == C0 && R1 == C1))
			{
				break;
			}
			int32 RefinedError = 0;
			const uint32 RefinedIndices = MatchColours(Px, R0, R1, RefinedError);
			if (RefinedError >= Error)
			{
				break;
			}
			C0 = R0;
			C1 = R1;
			Indices = RefinedIndices;
			Error = RefinedError;
		}
		WriteColourBlock(C0, C1, Indices, Out);
	}

	/** An eight-entry alpha palette: interpolated between the ends, or (A0 <= A1) five steps plus 0 and 255. */
	void AlphaPalette(int32 A0, int32 A1, int32 Out[8])
	{
		Out[0] = A0;
		Out[1] = A1;
		if (A0 > A1)
		{
			for (int32 i = 1; i <= 6; ++i)
			{
				Out[1 + i] = ((7 - i) * A0 + i * A1 + 3) / 7;
			}
		}
		else
		{
			for (int32 i = 1; i <= 4; ++i)
			{
				Out[1 + i] = ((5 - i) * A0 + i * A1 + 2) / 5;
			}
			Out[6] = 0;
			Out[7] = 255;
		}
	}

	uint64 MatchAlpha(const FBlockPixels& Px, int32 A0, int32 A1, int32& OutError)
	{
		int32 Pal[8];
		AlphaPalette(A0, A1, Pal);
		uint64 Indices = 0;
		OutError = 0;
		for (int32 i = 0; i < 16; ++i)
		{
			int32 Best = 0;
			int32 BestError = MAX_int32;
			for (int32 k = 0; k < 8; ++k)
			{
				const int32 D = Px[i][3] - Pal[k];
				if (D * D < BestError)
				{
					BestError = D * D;
					Best = k;
				}
			}
			Indices |= uint64(Best) << (3 * i);
			OutError += BestError;
		}
		return Indices;
	}

	void EncodeAlphaBlock(const FBlockPixels& Px, uint8* Out)
	{
		int32 Min = 255, Max = 0;
		// The ends of what lies strictly between 0 and 255, for the mode that has those two for free.
		int32 InnerMin = 255, InnerMax = 0;
		for (int32 i = 0; i < 16; ++i)
		{
			const int32 A = Px[i][3];
			Min = FMath::Min(Min, A);
			Max = FMath::Max(Max, A);
			if (A > 0 && A < 255)
			{
				InnerMin = FMath::Min(InnerMin, A);
				InnerMax = FMath::Max(InnerMax, A);
			}
		}
		if (InnerMin > InnerMax)
		{
			InnerMin = InnerMax = 0;
		}
		int32 A0 = InnerMin;
		int32 A1 = InnerMax;
		int32 Error = 0;
		uint64 Indices = MatchAlpha(Px, A0, A1, Error);
		if (Max > Min && Error > 0)
		{
			int32 Interpolated = 0;
			const uint64 InterpolatedIndices = MatchAlpha(Px, Max, Min, Interpolated);
			if (Interpolated < Error)
			{
				A0 = Max;
				A1 = Min;
				Indices = InterpolatedIndices;
			}
		}
		Out[0] = uint8(A0);
		Out[1] = uint8(A1);
		for (int32 b = 0; b < 6; ++b)
		{
			Out[2 + b] = uint8((Indices >> (8 * b)) & 0xFF);
		}
	}

	void FetchBlock(const uint8* Bgra, int32 Width, int32 Height, int32 Bx, int32 By, FBlockPixels& Px)
	{
		for (int32 y = 0; y < 4; ++y)
		{
			const int32 Sy = FMath::Min(By * 4 + y, Height - 1);
			for (int32 x = 0; x < 4; ++x)
			{
				const int32 Sx = FMath::Min(Bx * 4 + x, Width - 1);
				const uint8* P = Bgra + (int64(Sy) * Width + Sx) * 4;
				int32* Dst = Px[y * 4 + x];
				Dst[0] = P[2];
				Dst[1] = P[1];
				Dst[2] = P[0];
				Dst[3] = P[3];
			}
		}
	}
}	 // namespace
}	 // namespace ApexBc

bool ApexBc::IsOpaque(TConstArrayView<uint8> Bgra)
{
	for (int32 i = 3; i < Bgra.Num(); i += 4)
	{
		if (Bgra[i] != 255)
		{
			return false;
		}
	}
	return true;
}

void ApexBc::EncodeLevel(const uint8* Bgra, int32 Width, int32 Height, bool bAlpha, TArray<uint8>& Out)
{
	const int32 BlocksX = (FMath::Max(1, Width) + 3) / 4;
	const int32 BlocksY = (FMath::Max(1, Height) + 3) / 4;
	const int32 BlockBytes = bAlpha ? 16 : 8;
	Out.SetNumUninitialized(BlocksX * BlocksY * BlockBytes);
	uint8* Dst = Out.GetData();
	auto EncodeRow = [&](int32 By) {
		FBlockPixels Px;
		for (int32 Bx = 0; Bx < BlocksX; ++Bx)
		{
			FetchBlock(Bgra, Width, Height, Bx, By, Px);
			uint8* Block = Dst + (int64(By) * BlocksX + Bx) * BlockBytes;
			if (bAlpha)
			{
				EncodeAlphaBlock(Px, Block);
				EncodeColourBlock(Px, Block + 8);
			}
			else
			{
				EncodeColourBlock(Px, Block);
			}
		}
	};
	// Built here, before any worker can race to build it.
	SingleColour();
	if (BlocksX * BlocksY >= kParallelBlocks)
	{
		ParallelFor(BlocksY, EncodeRow);
	}
	else
	{
		for (int32 By = 0; By < BlocksY; ++By)
		{
			EncodeRow(By);
		}
	}
}

EPixelFormat ApexBc::CompressChain(int32 Width, int32 Height, TArray<TArray<uint8>>& Mips)
{
	if (Width <= 0 || Height <= 0 || Width % 4 != 0 || Height % 4 != 0 || Mips.Num() == 0
		|| Mips[0].Num() != Width * Height * 4)
	{
		return PF_B8G8R8A8;
	}
	const bool bAlpha = !IsOpaque(Mips[0]);
	for (int32 Level = 0; Level < Mips.Num(); ++Level)
	{
		const int32 W = FMath::Max(1, Width >> Level);
		const int32 H = FMath::Max(1, Height >> Level);
		if (Mips[Level].Num() != W * H * 4)
		{
			// Not a chain this can read; what is above it stands.
			Mips.SetNum(Level);
			break;
		}
		TArray<uint8> Blocks;
		EncodeLevel(Mips[Level].GetData(), W, H, bAlpha, Blocks);
		Mips[Level] = MoveTemp(Blocks);
	}
	return bAlpha ? PF_DXT5 : PF_DXT1;
}

void ApexBc::DecodeBlock(const uint8* Block, bool bAlpha, uint8* Out)
{
	const uint8* Colour = bAlpha ? Block + 8 : Block;
	const uint16 C0 = uint16(Colour[0] | (Colour[1] << 8));
	const uint16 C1 = uint16(Colour[2] | (Colour[3] << 8));
	int32 Pal[4][3];
	Palette(C0, C1, Pal);
	bool bTransparentEntry = false;
	if (!bAlpha && C0 <= C1)
	{
		for (int32 c = 0; c < 3; ++c)
		{
			Pal[2][c] = (Pal[0][c] + Pal[1][c]) / 2;
			Pal[3][c] = 0;
		}
		bTransparentEntry = true;
	}
	const uint32 Indices = uint32(Colour[4]) | (uint32(Colour[5]) << 8) | (uint32(Colour[6]) << 16) | (uint32(Colour[7]) << 24);
	int32 Alphas[8];
	uint64 AlphaIndices = 0;
	if (bAlpha)
	{
		AlphaPalette(Block[0], Block[1], Alphas);
		for (int32 b = 0; b < 6; ++b)
		{
			AlphaIndices |= uint64(Block[2 + b]) << (8 * b);
		}
	}
	for (int32 i = 0; i < 16; ++i)
	{
		const int32 k = (Indices >> (2 * i)) & 3;
		Out[i * 4 + 0] = uint8(Pal[k][2]);
		Out[i * 4 + 1] = uint8(Pal[k][1]);
		Out[i * 4 + 2] = uint8(Pal[k][0]);
		Out[i * 4 + 3] = bAlpha ? uint8(Alphas[(AlphaIndices >> (3 * i)) & 7])
								: (bTransparentEntry && k == 3 ? 0 : 255);
	}
}
