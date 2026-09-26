#include "ApexTrackMaterialGraphs.h"

#include "ApexTrackEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "Track/ApexGroundMaterials.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/**
	 * Tiling grayscale noise, the surface grain of last resort. An engine
	 * asset, so every project has it; used only when the baked ground set
	 * has not been imported, which is the state of a fresh clone.
	 */
	const TCHAR* kDetailTexture =
		TEXT("/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq.Good64x64TilingNoiseHighFreq");

	/** A placeholder for the texture parameters the instances fill in. */
	const TCHAR* kDefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");

	/**
	 * What a surface band fades toward at its edges (the default of the
	 * parent's `EdgeColor`; instances set their own, see the scene builder).
	 */
	const FLinearColor kEdgeDustColor(0.055f, 0.050f, 0.043f);

	/** The three baked maps of one ground set, once loaded. */
	struct FGroundMapSet
	{
		UTexture* Albedo = nullptr;
		UTexture* Normal = nullptr;
		UTexture* Roughness = nullptr;

		bool IsComplete() const
		{
			return Albedo != nullptr && Normal != nullptr && Roughness != nullptr;
		}
	};

	/**
	 * A baked ground set from `/Game/Ground`, or an empty one when it has
	 * not been imported (`-run=ApexGroundTexImport`). The package is tested
	 * before it is loaded: a missing one is the normal case here.
	 */
	FGroundMapSet LoadGroundSet(const FString& Set)
	{
		auto Load = [](const FString& PackageName) -> UTexture* {
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}
			return LoadObject<UTexture>(nullptr, *(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName)));
		};
		FGroundMapSet Maps;
		Maps.Albedo = Load(ApexGround::TexturePath(Set, TEXT("col")));
		Maps.Normal = Load(ApexGround::TexturePath(Set, TEXT("nrm")));
		Maps.Roughness = Load(ApexGround::TexturePath(Set, TEXT("rough")));
		return Maps;
	}

	/** Create a material expression and register it with the material. */
	template <typename T>
	T* AddExpr(UMaterial* Material)
	{
		T* Expression = NewObject<T>(Material);
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/**
	 * `M_ApexTrackBase`, the parent of every track material: an optional
	 * stripe over the `u` coordinate (curbs, tire walls), world-space noise
	 * breaking up albedo and roughness, the seam fringe on run-off bands,
	 * and the surface itself — the baked ground set when `/Game/Ground` has
	 * been imported, the engine's noise tile otherwise. Every instance sets
	 * its own values through the same parameter names (the scene builder's
	 * family table), cooked or at runtime.
	 */
	void BuildTrackBase(UMaterial* Parent, bool& bOutGroundTextures)
	{
		UMaterialExpressionVectorParameter* ColorParam =
			AddExpr<UMaterialExpressionVectorParameter>(Parent);
		ColorParam->ParameterName = TEXT("BaseColor");
		ColorParam->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

		UMaterialExpressionVectorParameter* SecondaryParam =
			AddExpr<UMaterialExpressionVectorParameter>(Parent);
		SecondaryParam->ParameterName = TEXT("SecondaryColor");
		SecondaryParam->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

		UMaterialExpressionScalarParameter* RoughnessParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		RoughnessParam->ParameterName = TEXT("Roughness");
		RoughnessParam->DefaultValue = 0.85f;

		// A period `u` never reaches, so stripes are off unless an instance
		// asks for them (0 would divide by zero in the graph below).
		UMaterialExpressionScalarParameter* StripeParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		StripeParam->ParameterName = TEXT("StripePeriod");
		StripeParam->DefaultValue = 1.0e6f;

		UMaterialExpressionScalarParameter* NoiseAmountParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		NoiseAmountParam->ParameterName = TEXT("NoiseAmount");
		NoiseAmountParam->DefaultValue = 0.0f;

		// Cycles per centimeter of world space.
		UMaterialExpressionScalarParameter* NoiseScaleParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		NoiseScaleParam->ParameterName = TEXT("NoiseScale");
		NoiseScaleParam->DefaultValue = 0.002f;

		UMaterialExpressionScalarParameter* RoughnessNoiseParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		RoughnessNoiseParam->ParameterName = TEXT("RoughnessNoise");
		RoughnessNoiseParam->DefaultValue = 0.0f;

		// Stripe mask: fmod(floor(u / period), 2) alternates 0/1 along `u`,
		// which the bake emits as meters of station for track strips and which
		// is plain 0..1 face UVs on the placeholder prop cubes.
		UMaterialExpressionTextureCoordinate* TexCoord =
			AddExpr<UMaterialExpressionTextureCoordinate>(Parent);
		UMaterialExpressionComponentMask* MaskU = AddExpr<UMaterialExpressionComponentMask>(Parent);
		MaskU->Input.Expression = TexCoord;
		MaskU->R = 1;
		MaskU->G = 0;
		MaskU->B = 0;
		MaskU->A = 0;
		UMaterialExpressionDivide* StripeU = AddExpr<UMaterialExpressionDivide>(Parent);
		StripeU->A.Expression = MaskU;
		StripeU->B.Expression = StripeParam;
		UMaterialExpressionFloor* StripeIndex = AddExpr<UMaterialExpressionFloor>(Parent);
		StripeIndex->Input.Expression = StripeU;
		UMaterialExpressionConstant* Two = AddExpr<UMaterialExpressionConstant>(Parent);
		Two->R = 2.0f;
		UMaterialExpressionFmod* Stripe = AddExpr<UMaterialExpressionFmod>(Parent);
		Stripe->A.Expression = StripeIndex;
		Stripe->B.Expression = Two;
		UMaterialExpressionLinearInterpolate* Paint =
			AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
		Paint->A.Expression = ColorParam;
		Paint->B.Expression = SecondaryParam;
		Paint->Alpha.Expression = Stripe;

		// World-space turbulence, recentred to ±0.5 so instances can scale it.
		UMaterialExpressionWorldPosition* WorldPos = AddExpr<UMaterialExpressionWorldPosition>(Parent);
		UMaterialExpressionMultiply* NoisePos = AddExpr<UMaterialExpressionMultiply>(Parent);
		NoisePos->A.Expression = WorldPos;
		NoisePos->B.Expression = NoiseScaleParam;
		UMaterialExpressionNoise* NoiseExpr = AddExpr<UMaterialExpressionNoise>(Parent);
		NoiseExpr->Position.Expression = NoisePos;
		NoiseExpr->Scale = 1.0f;
		NoiseExpr->bTurbulence = true;
		NoiseExpr->Levels = 3;
		NoiseExpr->OutputMin = 0.0f;
		NoiseExpr->OutputMax = 1.0f;
		UMaterialExpressionAdd* NoiseCentered = AddExpr<UMaterialExpressionAdd>(Parent);
		NoiseCentered->A.Expression = NoiseExpr;
		NoiseCentered->ConstB = -0.5f;

		// Albedo: the striped paint, brightened or darkened by the noise.
		UMaterialExpressionMultiply* Mottle = AddExpr<UMaterialExpressionMultiply>(Parent);
		Mottle->A.Expression = NoiseCentered;
		Mottle->B.Expression = NoiseAmountParam;
		UMaterialExpressionAdd* Brightness = AddExpr<UMaterialExpressionAdd>(Parent);
		Brightness->A.Expression = Mottle;
		Brightness->ConstB = 1.0f;
		UMaterialExpressionMultiply* Albedo = AddExpr<UMaterialExpressionMultiply>(Parent);
		Albedo->A.Expression = Paint;
		Albedo->B.Expression = Brightness;

		// Roughness: the base value, wobbled by the same noise so sheen varies
		// with the mottling instead of against it.
		UMaterialExpressionMultiply* RoughWobble = AddExpr<UMaterialExpressionMultiply>(Parent);
		RoughWobble->A.Expression = NoiseCentered;
		RoughWobble->B.Expression = RoughnessNoiseParam;
		UMaterialExpressionAdd* RoughSum = AddExpr<UMaterialExpressionAdd>(Parent);
		RoughSum->A.Expression = RoughnessParam;
		RoughSum->B.Expression = RoughWobble;

		// Per-instance colour swing for instanced foliage: `1 + data0 * Tint`,
		// where the tint is zero (no effect) unless an instance sets it and the
		// custom data reads as its default 0 on anything that is not instanced.
		UMaterialExpressionVectorParameter* TintParam =
			AddExpr<UMaterialExpressionVectorParameter>(Parent);
		TintParam->ParameterName = TEXT("InstanceTint");
		TintParam->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		UMaterialExpressionPerInstanceCustomData* InstanceData =
			AddExpr<UMaterialExpressionPerInstanceCustomData>(Parent);
		InstanceData->DataIndex = 0;
		InstanceData->ConstDefaultValue = 0.0f;
		UMaterialExpressionMultiply* TintSwing = AddExpr<UMaterialExpressionMultiply>(Parent);
		TintSwing->A.Expression = TintParam;
		TintSwing->B.Expression = InstanceData;
		UMaterialExpressionAdd* TintFactor = AddExpr<UMaterialExpressionAdd>(Parent);
		TintFactor->A.Expression = TintSwing;
		TintFactor->ConstB = 1.0f;
		UMaterialExpressionMultiply* Tinted = AddExpr<UMaterialExpressionMultiply>(Parent);
		Tinted->A.Expression = Albedo;
		Tinted->B.Expression = TintFactor;

		/** Output 1 of a texture sample or a vertex colour is its red channel. */
		constexpr int32 kRedOutput = 1;

		// The seam fringe.
		//
		// A run-off band is its own mesh at its own height beside the road, so
		// grass meets asphalt as a geometric line between two flat colours —
		// the single most "painted on" thing about a circuit after the surfaces
		// themselves. Nothing in the material knows where that line is, so the
		// builder measures each vertex's distance from the band's edge
		// (`ApexGround::EdgeFactors`) and puts the ramp in the red vertex
		// channel: 0 at the edge, 1 a couple of metres in. Here it is broken
		// with the macro noise and the outer stretch taken toward dust, so the
		// boundary wanders and frays instead of ruling a line.
		//
		// It is a fringe, not a real blend: the two surfaces still meet where
		// they met. Blending properly would mean the bands and the ground
		// sharing a mesh, which is the exporter's business, not this one's.
		//
		// The ×2 −0.5 is what keeps the interior clean. At a vertex colour of 1
		// the mask is saturate(−0.5 + noise × EdgeNoise), and the recentred
		// noise never exceeds +0.5, so for any EdgeNoise up to 1 the middle of
		// the band is untouched. Meshes that carry no colours read as white,
		// which is that same interior case, so this costs them nothing.
		UMaterialExpressionVertexColor* VertexColor = AddExpr<UMaterialExpressionVertexColor>(Parent);
		UMaterialExpressionScalarParameter* EdgeBlendParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		EdgeBlendParam->ParameterName = TEXT("EdgeBlend");
		EdgeBlendParam->DefaultValue = 0.0f;
		UMaterialExpressionScalarParameter* EdgeNoiseParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		EdgeNoiseParam->ParameterName = TEXT("EdgeNoise");
		EdgeNoiseParam->DefaultValue = 0.0f;
		UMaterialExpressionVectorParameter* EdgeColorParam =
			AddExpr<UMaterialExpressionVectorParameter>(Parent);
		EdgeColorParam->ParameterName = TEXT("EdgeColor");
		EdgeColorParam->DefaultValue = kEdgeDustColor;

		UMaterialExpressionSubtract* FromEdge = AddExpr<UMaterialExpressionSubtract>(Parent);
		FromEdge->ConstA = 1.0f;
		FromEdge->B.Expression = VertexColor;
		FromEdge->B.OutputIndex = kRedOutput;
		UMaterialExpressionMultiply* EdgeRamp = AddExpr<UMaterialExpressionMultiply>(Parent);
		EdgeRamp->A.Expression = FromEdge;
		EdgeRamp->ConstB = 2.0f;
		UMaterialExpressionSubtract* EdgeBias = AddExpr<UMaterialExpressionSubtract>(Parent);
		EdgeBias->A.Expression = EdgeRamp;
		EdgeBias->ConstB = 0.5f;
		UMaterialExpressionMultiply* EdgeWobble = AddExpr<UMaterialExpressionMultiply>(Parent);
		EdgeWobble->A.Expression = NoiseCentered;
		EdgeWobble->B.Expression = EdgeNoiseParam;
		UMaterialExpressionAdd* EdgeRagged = AddExpr<UMaterialExpressionAdd>(Parent);
		EdgeRagged->A.Expression = EdgeBias;
		EdgeRagged->B.Expression = EdgeWobble;
		UMaterialExpressionClamp* EdgeMask = AddExpr<UMaterialExpressionClamp>(Parent);
		EdgeMask->Input.Expression = EdgeRagged;
		EdgeMask->MinDefault = 0.0f;
		EdgeMask->MaxDefault = 1.0f;
		UMaterialExpressionMultiply* Fringe = AddExpr<UMaterialExpressionMultiply>(Parent);
		Fringe->A.Expression = EdgeMask;
		Fringe->B.Expression = EdgeBlendParam;
		UMaterialExpressionLinearInterpolate* Edged =
			AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
		Edged->A.Expression = Tinted;
		Edged->B.Expression = EdgeColorParam;
		Edged->Alpha.Expression = Fringe;

		// The surface itself. Track strips carry metres in their UVs (u along
		// the station, v across; ground tiles use world x/y), so every tiling
		// parameter below is repeats per metre.
		//
		// Two graphs, chosen once per track at bake time rather than branched at
		// runtime, because this material is generated per track anyway and a
		// texture sample multiplied by zero still costs a texture sample:
		//
		//  - the baked ground set from `/Game/Ground`, when it has been imported
		//    (`-run=ApexGroundTexImport` after `scripts/bake_ground_textures.py`);
		//  - otherwise the engine's 64-texel noise tile as plain grain, which is
		//    all this material had before the set existed and is what a fresh
		//    clone still gets.
		UMaterialExpression* FinalAlbedo = Edged;
		UMaterialExpression* FinalRoughness = RoughSum;
		UMaterialExpression* FinalNormal = nullptr;
		// Asphalt is the parent's default set. Every instance overrides the
		// three maps with its own, so which set supplies the defaults only
		// matters for a material that asks for none.
		const FGroundMapSet DefaultGround = LoadGroundSet(TEXT("asphalt"));
		const bool bGroundTextures = DefaultGround.IsComplete();
		bOutGroundTextures = bGroundTextures;
		if (bGroundTextures)
		{
			// Every map is sampled twice — at the instance's own tiling and at a
			// deliberately non-integer multiple of it — and mixed by a
			// world-space noise. One scale alone shows its repeat by the third
			// row of quads; two mixed at twenty metres do not, for one extra
			// sample per map. The same mix is then pushed all the way to the
			// coarse sample with distance, so the far field is low-frequency
			// texture rather than a metre of grain per pixel, which is what
			// shimmers, and the normal is flattened over the same range because
			// a tangent-space normal at a grazing angle is pure aliasing.
			UMaterialExpressionScalarParameter* TilingParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			TilingParam->ParameterName = TEXT("TextureTiling");
			TilingParam->DefaultValue = 1.0f / ApexGround::TextureTileM;
			// Off by default, all three: the prop stand-ins and anything else
			// that shares this parent without asking for a surface would
			// otherwise come out in asphalt grain and asphalt bumps. Only an
			// instance with a ground look turns them on.
			UMaterialExpressionScalarParameter* TextureAmountParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			TextureAmountParam->ParameterName = TEXT("TextureAmount");
			TextureAmountParam->DefaultValue = 0.0f;
			UMaterialExpressionScalarParameter* NormalStrengthParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			NormalStrengthParam->ParameterName = TEXT("NormalStrength");
			NormalStrengthParam->DefaultValue = 0.0f;
			UMaterialExpressionScalarParameter* RoughMapParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			RoughMapParam->ParameterName = TEXT("RoughnessMapAmount");
			RoughMapParam->DefaultValue = 0.0f;
			// Cycles per centimetre of world space, as `NoiseScale`: 0.0005 is
			// a twenty-metre patch, large enough that the eye reads it as the
			// ground varying rather than as the texture changing scale.
			UMaterialExpressionScalarParameter* MacroScaleParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			MacroScaleParam->ParameterName = TEXT("MacroBlendScale");
			MacroScaleParam->DefaultValue = 0.0005f;
			// Centimetres of pixel depth: fully coarse by 200 m out.
			UMaterialExpressionScalarParameter* FarStartParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			FarStartParam->ParameterName = TEXT("FarFadeStart");
			FarStartParam->DefaultValue = 4000.0f;
			UMaterialExpressionScalarParameter* FarRangeParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			FarRangeParam->ParameterName = TEXT("FarFadeRange");
			FarRangeParam->DefaultValue = 16000.0f;

			UMaterialExpressionMultiply* NearUV = AddExpr<UMaterialExpressionMultiply>(Parent);
			NearUV->A.Expression = TexCoord;
			NearUV->B.Expression = TilingParam;
			UMaterialExpressionMultiply* CoarseUV = AddExpr<UMaterialExpressionMultiply>(Parent);
			CoarseUV->A.Expression = NearUV;
			// Nothing near a ratio of small integers, or the two scales line up
			// again every few tiles and the repeat comes straight back.
			CoarseUV->ConstB = 0.371f;

			UMaterialExpressionMultiply* MacroPos = AddExpr<UMaterialExpressionMultiply>(Parent);
			MacroPos->A.Expression = WorldPos;
			MacroPos->B.Expression = MacroScaleParam;
			UMaterialExpressionNoise* MacroNoise = AddExpr<UMaterialExpressionNoise>(Parent);
			MacroNoise->Position.Expression = MacroPos;
			MacroNoise->Scale = 1.0f;
			MacroNoise->Levels = 2;
			MacroNoise->OutputMin = 0.0f;
			MacroNoise->OutputMax = 1.0f;

			UMaterialExpressionPixelDepth* Depth = AddExpr<UMaterialExpressionPixelDepth>(Parent);
			UMaterialExpressionSubtract* PastStart = AddExpr<UMaterialExpressionSubtract>(Parent);
			PastStart->A.Expression = Depth;
			PastStart->B.Expression = FarStartParam;
			UMaterialExpressionDivide* FarRaw = AddExpr<UMaterialExpressionDivide>(Parent);
			FarRaw->A.Expression = PastStart;
			FarRaw->B.Expression = FarRangeParam;
			UMaterialExpressionClamp* Far = AddExpr<UMaterialExpressionClamp>(Parent);
			Far->Input.Expression = FarRaw;
			Far->MinDefault = 0.0f;
			Far->MaxDefault = 1.0f;
			UMaterialExpressionLinearInterpolate* Mix =
				AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
			Mix->A.Expression = MacroNoise;
			Mix->ConstB = 1.0f;
			Mix->Alpha.Expression = Far;

			// The sampler type is baked into the node, not the override, so an
			// instance may only swap a map for one of the same class. That is
			// what the importer's fixed per-suffix settings are for: every
			// `_col` is sRGB colour, every `_nrm` a normal map and every
			// `_rough` grayscale, on every set.
			auto SampleMap = [&](const TCHAR* Name, UTexture* Texture, UMaterialExpression* Coords) {
				UMaterialExpressionTextureSampleParameter2D* Sample =
					AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
				Sample->ParameterName = Name;
				Sample->Texture = Texture;
				Sample->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
				Sample->Coordinates.Expression = Coords;
				return Sample;
			};
			auto BlendMap = [&](const TCHAR* Name, UTexture* Texture, int32 Output) {
				UMaterialExpressionLinearInterpolate* Blend =
					AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
				Blend->A.Expression = SampleMap(Name, Texture, NearUV);
				Blend->A.OutputIndex = Output;
				Blend->B.Expression = SampleMap(Name, Texture, CoarseUV);
				Blend->B.OutputIndex = Output;
				Blend->Alpha.Expression = Mix;
				return Blend;
			};
			UMaterialExpressionLinearInterpolate* AlbedoMap =
				BlendMap(TEXT("AlbedoMap"), DefaultGround.Albedo, 0);
			UMaterialExpressionLinearInterpolate* NormalMap =
				BlendMap(TEXT("NormalMap"), DefaultGround.Normal, 0);
			UMaterialExpressionLinearInterpolate* RoughMap =
				BlendMap(TEXT("RoughnessMap"), DefaultGround.Roughness, kRedOutput);

			// The maps are baked to a per-channel mean of 0.5 so the exporter's
			// own per-key colour still decides what a surface is — a red kerb
			// from a yellow one, this circuit's asphalt from its pit lane (see
			// the note in apex_tex.py) — and doubling brings the level back.
			UMaterialExpressionMultiply* MapDoubled = AddExpr<UMaterialExpressionMultiply>(Parent);
			MapDoubled->A.Expression = AlbedoMap;
			MapDoubled->ConstB = 2.0f;
			UMaterialExpressionLinearInterpolate* MapGain =
				AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
			MapGain->ConstA = 1.0f;
			MapGain->B.Expression = MapDoubled;
			MapGain->Alpha.Expression = TextureAmountParam;
			UMaterialExpressionMultiply* TexturedAlbedo = AddExpr<UMaterialExpressionMultiply>(Parent);
			TexturedAlbedo->A.Expression = Edged;
			TexturedAlbedo->B.Expression = MapGain;
			FinalAlbedo = TexturedAlbedo;

			UMaterialExpressionAdd* RoughCentered = AddExpr<UMaterialExpressionAdd>(Parent);
			RoughCentered->A.Expression = RoughMap;
			RoughCentered->ConstB = -0.5f;
			UMaterialExpressionMultiply* RoughSwing = AddExpr<UMaterialExpressionMultiply>(Parent);
			RoughSwing->A.Expression = RoughCentered;
			RoughSwing->B.Expression = RoughMapParam;
			UMaterialExpressionAdd* TexturedRoughness = AddExpr<UMaterialExpressionAdd>(Parent);
			TexturedRoughness->A.Expression = RoughSum;
			TexturedRoughness->B.Expression = RoughSwing;
			FinalRoughness = TexturedRoughness;

			UMaterialExpressionSubtract* NearFactor = AddExpr<UMaterialExpressionSubtract>(Parent);
			NearFactor->ConstA = 1.0f;
			NearFactor->B.Expression = Far;
			UMaterialExpressionMultiply* SlopeScale = AddExpr<UMaterialExpressionMultiply>(Parent);
			SlopeScale->A.Expression = NormalStrengthParam;
			SlopeScale->B.Expression = NearFactor;
			UMaterialExpressionComponentMask* SlopeXY =
				AddExpr<UMaterialExpressionComponentMask>(Parent);
			SlopeXY->Input.Expression = NormalMap;
			SlopeXY->R = 1;
			SlopeXY->G = 1;
			SlopeXY->B = 0;
			SlopeXY->A = 0;
			UMaterialExpressionComponentMask* SlopeZ =
				AddExpr<UMaterialExpressionComponentMask>(Parent);
			SlopeZ->Input.Expression = NormalMap;
			SlopeZ->R = 0;
			SlopeZ->G = 0;
			SlopeZ->B = 1;
			SlopeZ->A = 0;
			UMaterialExpressionMultiply* ScaledXY = AddExpr<UMaterialExpressionMultiply>(Parent);
			ScaledXY->A.Expression = SlopeXY;
			ScaledXY->B.Expression = SlopeScale;
			UMaterialExpressionAppendVector* NormalXYZ =
				AddExpr<UMaterialExpressionAppendVector>(Parent);
			NormalXYZ->A.Expression = ScaledXY;
			NormalXYZ->B.Expression = SlopeZ;
			UMaterialExpressionNormalize* NormalOut = AddExpr<UMaterialExpressionNormalize>(Parent);
			NormalOut->VectorInput.Expression = NormalXYZ;
			FinalNormal = NormalOut;
		}
		else if (UTexture* DetailTexture = LoadObject<UTexture>(nullptr, kDetailTexture))
		{
			// The fallback: two samples of the noise tile at coprime scales to
			// hide its 64-texel repeat, and a finite difference of the first
			// turned into a bump so the grain catches light rather than just
			// tinting it. All off (0) by default; the family branches below
			// switch it on.
			UMaterialExpressionScalarParameter* DetailTilingParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			DetailTilingParam->ParameterName = TEXT("DetailTiling");
			DetailTilingParam->DefaultValue = 0.0f;
			UMaterialExpressionScalarParameter* DetailAmountParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			DetailAmountParam->ParameterName = TEXT("DetailAmount");
			DetailAmountParam->DefaultValue = 0.0f;
			UMaterialExpressionScalarParameter* DetailRoughnessParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			DetailRoughnessParam->ParameterName = TEXT("DetailRoughness");
			DetailRoughnessParam->DefaultValue = 0.0f;
			UMaterialExpressionScalarParameter* DetailNormalParam =
				AddExpr<UMaterialExpressionScalarParameter>(Parent);
			DetailNormalParam->ParameterName = TEXT("DetailNormal");
			DetailNormalParam->DefaultValue = 0.0f;

			UMaterialExpressionMultiply* DetailUV = AddExpr<UMaterialExpressionMultiply>(Parent);
			DetailUV->A.Expression = TexCoord;
			DetailUV->B.Expression = DetailTilingParam;

			const EMaterialSamplerType SamplerType =
				MaterialExpressionUtils::GetSamplerTypeForTexture(DetailTexture);
			auto SampleDetail = [&](UMaterialExpression* Coordinates) {
				UMaterialExpressionTextureSampleParameter2D* Sample =
					AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
				Sample->ParameterName = TEXT("DetailTexture");
				Sample->Texture = DetailTexture;
				Sample->SamplerType = SamplerType;
				Sample->Coordinates.Expression = Coordinates;
				return Sample;
			};
			auto OffsetUV = [&](float DU, float DV) {
				UMaterialExpressionConstant2Vector* Delta =
					AddExpr<UMaterialExpressionConstant2Vector>(Parent);
				Delta->R = DU;
				Delta->G = DV;
				UMaterialExpressionAdd* Shifted = AddExpr<UMaterialExpressionAdd>(Parent);
				Shifted->A.Expression = DetailUV;
				Shifted->B.Expression = Delta;
				return Shifted;
			};
			UMaterialExpressionTextureSampleParameter2D* Fine = SampleDetail(DetailUV);
			UMaterialExpressionMultiply* CoarseUV = AddExpr<UMaterialExpressionMultiply>(Parent);
			CoarseUV->A.Expression = DetailUV;
			CoarseUV->ConstB = 0.137f;
			UMaterialExpressionTextureSampleParameter2D* Coarse = SampleDetail(CoarseUV);
			UMaterialExpressionAdd* DetailSum = AddExpr<UMaterialExpressionAdd>(Parent);
			DetailSum->A.Expression = Fine;
			DetailSum->A.OutputIndex = kRedOutput;
			DetailSum->B.Expression = Coarse;
			DetailSum->B.OutputIndex = kRedOutput;
			// Average of the two, recentred to ±0.5 like the macro noise.
			UMaterialExpressionMultiply* DetailMean = AddExpr<UMaterialExpressionMultiply>(Parent);
			DetailMean->A.Expression = DetailSum;
			DetailMean->ConstB = 0.5f;
			UMaterialExpressionAdd* DetailCentered = AddExpr<UMaterialExpressionAdd>(Parent);
			DetailCentered->A.Expression = DetailMean;
			DetailCentered->ConstB = -0.5f;

			UMaterialExpressionMultiply* DetailMottle = AddExpr<UMaterialExpressionMultiply>(Parent);
			DetailMottle->A.Expression = DetailCentered;
			DetailMottle->B.Expression = DetailAmountParam;
			UMaterialExpressionAdd* DetailBrightness = AddExpr<UMaterialExpressionAdd>(Parent);
			DetailBrightness->A.Expression = DetailMottle;
			DetailBrightness->ConstB = 1.0f;
			UMaterialExpressionMultiply* DetailedAlbedo = AddExpr<UMaterialExpressionMultiply>(Parent);
			DetailedAlbedo->A.Expression = Edged;
			DetailedAlbedo->B.Expression = DetailBrightness;
			FinalAlbedo = DetailedAlbedo;

			UMaterialExpressionMultiply* DetailRough = AddExpr<UMaterialExpressionMultiply>(Parent);
			DetailRough->A.Expression = DetailCentered;
			DetailRough->B.Expression = DetailRoughnessParam;
			UMaterialExpressionAdd* DetailedRoughness = AddExpr<UMaterialExpressionAdd>(Parent);
			DetailedRoughness->A.Expression = RoughSum;
			DetailedRoughness->B.Expression = DetailRough;
			FinalRoughness = DetailedRoughness;

			// Bump from the fine sample: height falling along +u tilts the
			// normal toward +u, so slope = h(uv) - h(uv + d). One texel of the
			// 64 × 64 source per step; `DetailNormal` sets the strength.
			const float Texel = 1.0f / 64.0f;
			UMaterialExpressionTextureSampleParameter2D* FineU = SampleDetail(OffsetUV(Texel, 0.0f));
			UMaterialExpressionTextureSampleParameter2D* FineV = SampleDetail(OffsetUV(0.0f, Texel));
			UMaterialExpressionSubtract* SlopeU = AddExpr<UMaterialExpressionSubtract>(Parent);
			SlopeU->A.Expression = Fine;
			SlopeU->A.OutputIndex = kRedOutput;
			SlopeU->B.Expression = FineU;
			SlopeU->B.OutputIndex = kRedOutput;
			UMaterialExpressionSubtract* SlopeV = AddExpr<UMaterialExpressionSubtract>(Parent);
			SlopeV->A.Expression = Fine;
			SlopeV->A.OutputIndex = kRedOutput;
			SlopeV->B.Expression = FineV;
			SlopeV->B.OutputIndex = kRedOutput;
			UMaterialExpressionMultiply* BumpU = AddExpr<UMaterialExpressionMultiply>(Parent);
			BumpU->A.Expression = SlopeU;
			BumpU->B.Expression = DetailNormalParam;
			UMaterialExpressionMultiply* BumpV = AddExpr<UMaterialExpressionMultiply>(Parent);
			BumpV->A.Expression = SlopeV;
			BumpV->B.Expression = DetailNormalParam;
			UMaterialExpressionAppendVector* BumpUV = AddExpr<UMaterialExpressionAppendVector>(Parent);
			BumpUV->A.Expression = BumpU;
			BumpUV->B.Expression = BumpV;
			UMaterialExpressionConstant* One = AddExpr<UMaterialExpressionConstant>(Parent);
			One->R = 1.0f;
			UMaterialExpressionAppendVector* BumpXYZ = AddExpr<UMaterialExpressionAppendVector>(Parent);
			BumpXYZ->A.Expression = BumpUV;
			BumpXYZ->B.Expression = One;
			UMaterialExpressionNormalize* Bump = AddExpr<UMaterialExpressionNormalize>(Parent);
			Bump->VectorInput.Expression = BumpXYZ;
			FinalNormal = Bump;
		}
		else
		{
			UE_LOG(LogApexTrackImport, Warning,
				TEXT("    no ground textures and %s is missing — surfaces have no grain at all"),
				kDetailTexture);
		}

		UMaterialExpressionClamp* RoughOut = AddExpr<UMaterialExpressionClamp>(Parent);
		RoughOut->Input.Expression = FinalRoughness;
		RoughOut->MinDefault = 0.05f;
		RoughOut->MaxDefault = 1.0f;

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = FinalAlbedo;
		EditorOnly->Roughness.Expression = RoughOut;
		if (FinalNormal)
		{
			EditorOnly->Normal.Expression = FinalNormal;
		}
		// The prop stand-ins go through instanced components; without the flag
		// a cooked build draws them with the default material.
		Parent->bUsedWithInstancedStaticMeshes = true;
		Parent->PostEditChange();
	}

	/**
	 * `M_ApexEmissive`: the start lights, lamps and screens. The track
	 * material has no emissive input and the start lights are driven at
	 * runtime through `EmissiveStrength` on dynamic instances, so they want
	 * a graph of their own rather than another branch in the big one.
	 */
	void BuildEmissive(UMaterial* Parent)
	{
		UMaterialExpressionVectorParameter* ColorParam =
			AddExpr<UMaterialExpressionVectorParameter>(Parent);
		ColorParam->ParameterName = TEXT("BaseColor");
		ColorParam->DefaultValue = FLinearColor(0.05f, 0.01f, 0.01f, 1.0f);
		UMaterialExpressionScalarParameter* RoughnessParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		RoughnessParam->ParameterName = TEXT("Roughness");
		RoughnessParam->DefaultValue = 0.4f;
		UMaterialExpressionVectorParameter* EmissiveColor =
			AddExpr<UMaterialExpressionVectorParameter>(Parent);
		EmissiveColor->ParameterName = TEXT("EmissiveColor");
		EmissiveColor->DefaultValue = FLinearColor(1.0f, 0.02f, 0.02f, 1.0f);
		UMaterialExpressionScalarParameter* EmissiveStrength =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		EmissiveStrength->ParameterName = TEXT("EmissiveStrength");
		EmissiveStrength->DefaultValue = 0.0f;
		UMaterialExpressionMultiply* Emissive = AddExpr<UMaterialExpressionMultiply>(Parent);
		Emissive->A.Expression = EmissiveColor;
		Emissive->B.Expression = EmissiveStrength;

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = ColorParam;
		EditorOnly->Roughness.Expression = RoughnessParam;
		EditorOnly->EmissiveColor.Expression = Emissive;
		// Its instances land on the kit's instanced boards and Nanite screens.
		Parent->bUsedWithInstancedStaticMeshes = true;
		Parent->bUsedWithNanite = true;
		Parent->PostEditChange();
	}

	/**
	 * `M_ApexBrand`: nothing but a texture on a matte surface. The authored
	 * boards carry a default brand baked into their material; a prop's
	 * `text` swaps that slot for an instance of this.
	 */
	void BuildBrand(UMaterial* Parent, UTexture* Placeholder)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample =
			AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
		Sample->ParameterName = TEXT("BrandTexture");
		Sample->Texture = Placeholder;
		Sample->SamplerType = SAMPLERTYPE_Color;
		UMaterialExpressionScalarParameter* RoughnessParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		RoughnessParam->ParameterName = TEXT("Roughness");
		RoughnessParam->DefaultValue = 0.45f;

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = Sample;
		EditorOnly->Roughness.Expression = RoughnessParam;
		// Brands go on instanced boards and on Nanite bridges and garages.
		Parent->bUsedWithInstancedStaticMeshes = true;
		Parent->bUsedWithNanite = true;
		Parent->PostEditChange();
	}

	/**
	 * `M_ApexDecal`: paint on the road — the texture's colour at a paint's
	 * sheen, its alpha as the mask, so the asphalt shows through wherever
	 * nothing is painted.
	 */
	void BuildDecal(UMaterial* Parent, UTexture* Placeholder)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample =
			AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
		Sample->ParameterName = TEXT("DecalTexture");
		Sample->Texture = Placeholder;
		Sample->SamplerType = SAMPLERTYPE_Color;
		// Road paint is a matte coat; the tint dims the texture's white to what
		// a sprayed white reflects under the race's sun.
		UMaterialExpressionVectorParameter* TintParam = AddExpr<UMaterialExpressionVectorParameter>(Parent);
		TintParam->ParameterName = TEXT("Tint");
		TintParam->DefaultValue = FLinearColor(0.72f, 0.72f, 0.70f, 1.0f);
		UMaterialExpressionMultiply* Tinted = AddExpr<UMaterialExpressionMultiply>(Parent);
		Tinted->A.Expression = Sample;
		Tinted->B.Expression = TintParam;
		UMaterialExpressionScalarParameter* RoughnessParam =
			AddExpr<UMaterialExpressionScalarParameter>(Parent);
		RoughnessParam->ParameterName = TEXT("Roughness");
		RoughnessParam->DefaultValue = 0.6f;

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = Tinted;
		EditorOnly->Roughness.Expression = RoughnessParam;
		// Output 4 of a texture sample is its alpha.
		EditorOnly->OpacityMask.Connect(4, Sample);
		Parent->BlendMode = BLEND_Masked;
		Parent->OpacityMaskClipValue = 0.4f;
		Parent->PostEditChange();
	}

	/**
	 * A package ready to receive a freshly generated asset: an existing one
	 * of the same name is renamed out of the way first, since re-baking is
	 * the normal case.
	 */
	UPackage* MakeMaterialPackage(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}
		Package->FullyLoad();
		const FString ObjectName = FPackageName::GetShortName(PackageName);
		if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *ObjectName))
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Existing->MarkAsGarbage();
		}
		return Package;
	}

	/** Generate one parent with `Build`, and save it. */
	bool BakeOne(const TCHAR* Name, TFunctionRef<void(UMaterial*)> Build, FString& OutError)
	{
		const FString PackageName = ApexTrackMaterials::PackageName(Name);
		UPackage* Package = MakeMaterialPackage(PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
			return false;
		}
		UMaterial* Material = NewObject<UMaterial>(Package, Name, RF_Public | RF_Standalone);
		Build(Material);
		FAssetRegistryModule::AssetCreated(Material);
		Package->MarkPackageDirty();

		const FString FileName =
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Material, *FileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("failed to save %s"), *FileName);
			return false;
		}
		UE_LOG(LogApexTrackImport, Display, TEXT("    baked %s"), *PackageName);
		return true;
	}

	bool ParentExists(const TCHAR* Name)
	{
		return FPackageName::DoesPackageExist(ApexTrackMaterials::PackageName(Name));
	}
}	 // namespace

bool ApexTrackMaterialGraphs::GroundSetImported()
{
	return LoadGroundSet(TEXT("asphalt")).IsComplete();
}

bool ApexTrackMaterialGraphs::Bake(bool bForce, FString& OutError)
{
	UTexture* Placeholder = LoadObject<UTexture>(nullptr, kDefaultTexture);
	int32 Baked = 0;

	// The base parent carries one of two surface graphs, chosen by whether
	// `/Game/Ground` holds the baked sets; one baked before they were
	// imported (or since removed) is baked again.
	bool bRebakeBase = bForce || !ParentExists(ApexTrackMaterials::BaseName);
	if (!bRebakeBase)
	{
		const FApexTrackParents Current = ApexTrackMaterials::LoadParents();
		if (ApexTrackMaterials::HasGroundTextures(Current.Base) != GroundSetImported())
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    %s was baked %s the ground textures; baking it again"),
				ApexTrackMaterials::BaseName,
				GroundSetImported() ? TEXT("without") : TEXT("with"));
			bRebakeBase = true;
		}
	}
	if (bRebakeBase)
	{
		bool bGroundTextures = false;
		if (!BakeOne(ApexTrackMaterials::BaseName, [&bGroundTextures](UMaterial* M) { BuildTrackBase(M, bGroundTextures); }, OutError))
		{
			return false;
		}
		UE_LOG(LogApexTrackImport, Display, TEXT("    %s samples %s"), ApexTrackMaterials::BaseName,
			bGroundTextures ? TEXT("the baked ground sets") : TEXT("the engine noise tile (no /Game/Ground yet)"));
		++Baked;
	}
	if (bForce || !ParentExists(ApexTrackMaterials::EmissiveName))
	{
		if (!BakeOne(ApexTrackMaterials::EmissiveName, [](UMaterial* M) { BuildEmissive(M); }, OutError))
		{
			return false;
		}
		++Baked;
	}
	if (!Placeholder)
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    the engine's DefaultTexture is missing; no brand or decal parent (boards keep their "
				 "imported brands, road decals are left out)"));
	}
	else
	{
		if (bForce || !ParentExists(ApexTrackMaterials::BrandName))
		{
			if (!BakeOne(ApexTrackMaterials::BrandName, [Placeholder](UMaterial* M) { BuildBrand(M, Placeholder); }, OutError))
			{
				return false;
			}
			++Baked;
		}
		if (bForce || !ParentExists(ApexTrackMaterials::DecalName))
		{
			if (!BakeOne(ApexTrackMaterials::DecalName, [Placeholder](UMaterial* M) { BuildDecal(M, Placeholder); }, OutError))
			{
				return false;
			}
			++Baked;
		}
	}
	if (Baked == 0)
	{
		UE_LOG(LogApexTrackImport, Display, TEXT("    the track parent materials under %s are up to date"),
			ApexTrackMaterials::Folder);
	}
	return true;
}

bool ApexTrackMaterialGraphs::EnsureParents(FApexTrackParents& Out, FString& OutError)
{
	if (!Bake(/*bForce*/ false, OutError))
	{
		return false;
	}
	Out = ApexTrackMaterials::LoadParents();
	if (!Out.Base)
	{
		OutError = FString::Printf(TEXT("%s did not load after baking"),
			*ApexTrackMaterials::ObjectPath(ApexTrackMaterials::BaseName));
		return false;
	}
	return true;
}
