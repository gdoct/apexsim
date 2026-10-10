#include "ApexTrackMaterialGraphs.h"

#include "ApexTrackEditorModule.h"
#include "Cars/ApexCarMaterials.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "MaterialShared.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDDX.h"
#include "Materials/MaterialExpressionDDY.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSign.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "Race/ApexCarDamage.h"
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
	void ApplyRoadStateGraph(UMaterial* Parent, UMaterialExpression* TexCoord, UMaterialExpression* WorldPos,
		UMaterialExpression*& InOutAlbedo, UMaterialExpression*& InOutRoughness, UMaterialExpression*& InOutNormal);

	/**
	 * `bRoadState` builds `M_ApexTrackRoad`: the same surface, with the
	 * session's road state (rubber, marbles, a dry line, water) drawn over
	 * it from the textures the race director fills (ApplyRoadStateGraph).
	 * Only the road samples them, so the other surfaces do not pay for it.
	 */
	void BuildTrackBase(UMaterial* Parent, bool& bOutGroundTextures, bool bRoadState = false)
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

		// Stripe mask: fmod(floor(|u| / period), 2) alternates 0/1 along `u`,
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
		// |u|, not u: at a negative `u` floor(u / 1e6) is -1, the stripe
		// index of a surface with stripes off came out odd, and the paint
		// was the secondary colour (or, with a plain fmod, which keeps the
		// sign, 2 x base - secondary: black). An imported circuit's
		// world-metre UVs are negative on half the map (AC Spa's asphalt
		// from the start line back round Eau Rouge drew black), and so is
		// the generated terrain's far ground. Stripes mirror about u = 0,
		// where no generated strip reaches.
		UMaterialExpressionAbs* AbsU = AddExpr<UMaterialExpressionAbs>(Parent);
		AbsU->Input.Expression = MaskU;
		UMaterialExpressionDivide* StripeU = AddExpr<UMaterialExpressionDivide>(Parent);
		StripeU->A.Expression = AbsU;
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
			// `_rough` single-channel BC4, on every set.
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

		if (bRoadState)
		{
			ApplyRoadStateGraph(Parent, TexCoord, WorldPos, FinalAlbedo, FinalRoughness, FinalNormal);
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

	/** What the car parents' `BaseColorTexture` shows until an instance sets one: white, so the factor alone decides. */
	const TCHAR* kWhiteTexture = TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");

	/** Which of the four car parents a graph is. */
	enum class ECarParent : uint8
	{
		Opaque,
		ClearCoat,
		Masked,
		Translucent,
	};

	UMaterialExpressionComponentMask* CarMask(UMaterial* Material, UMaterialExpression* Input, bool R, bool G, bool B, bool A)
	{
		UMaterialExpressionComponentMask* Result = AddExpr<UMaterialExpressionComponentMask>(Material);
		// A vector parameter's first pin is RGB only: masking its alpha
		// from there fails to compile (the translucent parent fell back to
		// the engine's default material). Pin 5 is RGBA.
		if (Cast<UMaterialExpressionVectorParameter>(Input))
		{
			Result->Input.Connect(5, Input);
		}
		else
		{
			Result->Input.Expression = Input;
		}
		Result->R = R;
		Result->G = G;
		Result->B = B;
		Result->A = A;
		return Result;
	}

	UMaterialExpressionScalarParameter* CarScalar(UMaterial* Material, const TCHAR* Name, float Default)
	{
		UMaterialExpressionScalarParameter* Result = AddExpr<UMaterialExpressionScalarParameter>(Material);
		Result->ParameterName = Name;
		Result->DefaultValue = Default;
		return Result;
	}

	/** Terse graph building for the damage and smoke graphs: each call adds one expression. */
	struct FGraph
	{
		UMaterial* M;

		UMaterialExpression* C(float Value) const
		{
			UMaterialExpressionConstant* E = AddExpr<UMaterialExpressionConstant>(M);
			E->R = Value;
			return E;
		}
		UMaterialExpression* C3(float X, float Y, float Z) const
		{
			UMaterialExpressionConstant3Vector* E = AddExpr<UMaterialExpressionConstant3Vector>(M);
			E->Constant = FLinearColor(X, Y, Z);
			return E;
		}
		template <typename T>
		UMaterialExpression* Binary(UMaterialExpression* A, UMaterialExpression* B) const
		{
			T* E = AddExpr<T>(M);
			E->A.Expression = A;
			E->B.Expression = B;
			return E;
		}
		UMaterialExpression* Mul(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionMultiply>(A, B); }
		UMaterialExpression* Add(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionAdd>(A, B); }
		UMaterialExpression* Sub(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionSubtract>(A, B); }
		UMaterialExpression* Div(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionDivide>(A, B); }
		UMaterialExpression* Max(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionMax>(A, B); }
		UMaterialExpression* Append(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionAppendVector>(A, B); }
		UMaterialExpression* Cross(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionCrossProduct>(A, B); }
		UMaterialExpression* Dot(UMaterialExpression* A, UMaterialExpression* B) const { return Binary<UMaterialExpressionDotProduct>(A, B); }
		UMaterialExpression* Lerp(UMaterialExpression* A, UMaterialExpression* B, UMaterialExpression* Alpha) const
		{
			UMaterialExpressionLinearInterpolate* E = AddExpr<UMaterialExpressionLinearInterpolate>(M);
			E->A.Expression = A;
			E->B.Expression = B;
			E->Alpha.Expression = Alpha;
			return E;
		}
		UMaterialExpression* Sat(UMaterialExpression* Input) const
		{
			UMaterialExpressionSaturate* E = AddExpr<UMaterialExpressionSaturate>(M);
			E->Input.Expression = Input;
			return E;
		}
		UMaterialExpression* Sign(UMaterialExpression* Input) const
		{
			UMaterialExpressionSign* E = AddExpr<UMaterialExpressionSign>(M);
			E->Input.Expression = Input;
			return E;
		}
		UMaterialExpression* Normalize(UMaterialExpression* Input) const
		{
			UMaterialExpressionNormalize* E = AddExpr<UMaterialExpressionNormalize>(M);
			E->VectorInput.Expression = Input;
			return E;
		}
		UMaterialExpression* Pow(UMaterialExpression* Base, float Exponent) const
		{
			UMaterialExpressionPower* E = AddExpr<UMaterialExpressionPower>(M);
			E->Base.Expression = Base;
			E->ConstExponent = Exponent;
			return E;
		}
		UMaterialExpression* Mask(UMaterialExpression* Input, bool R, bool G, bool B) const
		{
			UMaterialExpressionComponentMask* E = AddExpr<UMaterialExpressionComponentMask>(M);
			E->Input.Expression = Input;
			E->R = R;
			E->G = G;
			E->B = B;
			E->A = false;
			return E;
		}
		UMaterialExpression* Transform(UMaterialExpression* Input, EMaterialVectorCoordTransformSource From, EMaterialVectorCoordTransform To) const
		{
			UMaterialExpressionTransform* E = AddExpr<UMaterialExpressionTransform>(M);
			E->Input.Expression = Input;
			E->TransformSourceType = From;
			E->TransformType = To;
			return E;
		}
		/** Gradient noise in 0..1, computed (it runs in the vertex shader too); `Scale` is cycles per cm. */
		UMaterialExpression* Noise(UMaterialExpression* Position, float Scale, int32 Levels) const
		{
			UMaterialExpressionNoise* E = AddExpr<UMaterialExpressionNoise>(M);
			E->Position.Expression = Position;
			E->Scale = Scale;
			E->Levels = Levels;
			E->Quality = 1;
			E->NoiseFunction = NOISEFUNCTION_GradientALU;
			E->bTurbulence = false;
			E->OutputMin = 0.0f;
			E->OutputMax = 1.0f;
			E->LevelScale = 2.0f;
			return E;
		}
		/** x² (3 - 2x): a smooth step over an already saturated 0..1. */
		UMaterialExpression* Smooth(UMaterialExpression* X) const
		{
			return Mul(Mul(X, X), Sub(C(3.0f), Mul(C(2.0f), X)));
		}
		/** A scalar parameter read from the primitive's custom data at `Index`. */
		UMaterialExpression* PrimitiveScalar(const TCHAR* Name, int32 Index) const
		{
			UMaterialExpressionScalarParameter* E = AddExpr<UMaterialExpressionScalarParameter>(M);
			E->ParameterName = Name;
			E->DefaultValue = 0.0f;
			E->bUseCustomPrimitiveData = true;
			E->PrimitiveDataIndex = static_cast<uint8>(Index);
			return E;
		}
		/** A vector parameter's RGB read from the primitive's custom data at `Index`.. `Index + 3`. */
		UMaterialExpression* PrimitiveVector(const TCHAR* Name, int32 Index, const FLinearColor& Default) const
		{
			UMaterialExpressionVectorParameter* E = AddExpr<UMaterialExpressionVectorParameter>(M);
			E->ParameterName = Name;
			E->DefaultValue = Default;
			E->bUseCustomPrimitiveData = true;
			E->PrimitiveDataIndex = static_cast<uint8>(Index);
			return MaskRgb(E);
		}
		UMaterialExpression* MaskRgb(UMaterialExpression* Input) const { return Mask(Input, true, true, true); }
	};

	/** What the damage graph hands the car parent's outputs. */
	struct FCarDamage
	{
		/** World position offset: the dents and the crumple. */
		UMaterialExpression* Offset = nullptr;
		/** Tangent-space normal: faceted where dented, (0, 0, 1) elsewhere. */
		UMaterialExpression* Normal = nullptr;
		/** How scuffed the paint is here, 0..1, and what shows through. */
		UMaterialExpression* Scuff = nullptr;
		UMaterialExpression* ScuffColor = nullptr;
	};

	/**
	 * The damage every car parent draws (ApexCarDamage.h): the body's custom
	 * primitive data holds each zone's visual amount (0 front, 1 rear, 2
	 * left, 3 right), the body's centre (4) and half extents (8) in its mesh
	 * frame, and a seed (12). A zone's panels within reach of its face (15%
	 * of the half extent, 45% at full damage) are pushed in by up to
	 * `DamageDentCm` (the sides 60% of it, the tail 80%), unevenly by a 33 cm
	 * noise, crumpled by `DamageCrumpleCm` of an 8 cm one, and dropped a
	 * little; the same reach scuffs the paint in patches down to dark carbon
	 * with streaks of bare metal along the car, and the dented panels are
	 * shaded flat per triangle (the derivatives of the moved position), which
	 * reads as crushed metal. With every zone at zero nothing moves, the
	 * normal is (0, 0, 1) and nothing is scuffed: an undamaged car, a wheel,
	 * a flap and an imported track's scenery draw as before.
	 */
	FCarDamage BuildCarDamage(UMaterial* Parent)
	{
		const FGraph G{Parent};
		UMaterialExpression* Front = G.PrimitiveScalar(TEXT("DamageFront"), ApexDamage::CpdFront);
		UMaterialExpression* Rear = G.PrimitiveScalar(TEXT("DamageRear"), ApexDamage::CpdRear);
		UMaterialExpression* Left = G.PrimitiveScalar(TEXT("DamageLeft"), ApexDamage::CpdLeft);
		UMaterialExpression* Right = G.PrimitiveScalar(TEXT("DamageRight"), ApexDamage::CpdRight);
		UMaterialExpression* Centre = G.PrimitiveVector(TEXT("DamageCentre"), ApexDamage::CpdCentre, FLinearColor(0.0f, 0.0f, 60.0f));
		UMaterialExpression* Extent = G.PrimitiveVector(TEXT("DamageExtent"), ApexDamage::CpdExtent, FLinearColor(100.0f, 250.0f, 60.0f));
		UMaterialExpression* Seed = G.PrimitiveScalar(TEXT("DamageSeed"), ApexDamage::CpdSeed);
		UMaterialExpressionScalarParameter* Dent = AddExpr<UMaterialExpressionScalarParameter>(Parent);
		Dent->ParameterName = ApexCarMaterials::DamageDentCm;
		Dent->DefaultValue = 15.0f;
		UMaterialExpressionScalarParameter* Crumple = AddExpr<UMaterialExpressionScalarParameter>(Parent);
		Crumple->ParameterName = ApexCarMaterials::DamageCrumpleCm;
		Crumple->DefaultValue = 3.0f;

		// The undeformed position in the mesh's frame (nose +Y, left +X):
		// the dents and scuffs stay put on the panel whatever moves it.
		UMaterialExpressionLocalPosition* Local = AddExpr<UMaterialExpressionLocalPosition>(Parent);
		Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
		Local->LocalOrigin = ELocalPositionOrigin::Primitive;
		// -1..1 across the body's box; the extent never under a centimetre.
		UMaterialExpression* P = G.Div(G.Sub(Local, Centre), G.Max(Extent, G.C(1.0f)));
		UMaterialExpression* Px = G.Mask(P, true, false, false);
		UMaterialExpression* Py = G.Mask(P, false, true, false);
		// The seed moves the noise kilometres along: another car, another crumple.
		UMaterialExpression* NoisePos = G.Add(Local, G.Mul(Seed, G.C3(91700.0f, 53300.0f, 37100.0f)));

		// A zone's weight here: its amount, faded in over its reach of its face.
		auto Zone = [&G](UMaterialExpression* Toward, UMaterialExpression* Amount) {
			UMaterialExpression* Reach = G.Add(G.C(0.15f), G.Mul(Amount, G.C(0.3f)));
			UMaterialExpression* Into = G.Sat(G.Div(G.Sub(Toward, G.Sub(G.C(1.0f), Reach)), Reach));
			return G.Mul(G.Smooth(Into), Amount);
		};
		UMaterialExpression* WFront = Zone(Py, Front);
		UMaterialExpression* WRear = Zone(G.Mul(Py, G.C(-1.0f)), Rear);
		UMaterialExpression* WLeft = Zone(Px, Left);
		UMaterialExpression* WRight = Zone(G.Mul(Px, G.C(-1.0f)), Right);
		UMaterialExpression* Weight = G.Add(G.Add(WFront, WRear), G.Add(WLeft, WRight));

		// Pushed in toward the middle, unevenly.
		UMaterialExpression* Uneven = G.Add(G.C(0.35f), G.Mul(G.C(0.65f), G.Noise(NoisePos, 0.03f, 2)));
		UMaterialExpression* Depth = G.Mul(Dent, Uneven);
		UMaterialExpression* Ox = G.Mul(G.Sub(WRight, WLeft), G.Mul(Depth, G.C(0.6f)));
		UMaterialExpression* Oy = G.Sub(G.Mul(WRear, G.Mul(Depth, G.C(0.8f))), G.Mul(WFront, Depth));
		UMaterialExpression* Oz = G.Mul(Weight, G.Mul(Depth, G.C(-0.15f)));
		UMaterialExpression* Crush = G.Append(G.Append(Ox, Oy), Oz);
		UMaterialExpression* Wrinkle = G.Mul(G.Mul(G.Sub(G.Noise(NoisePos, 0.12f, 1), G.C(0.5f)), G.C(2.0f)), G.Mul(Crumple, Weight));
		UMaterialExpression* OffsetLocal = G.Add(Crush, G.Mul(Wrinkle, G.C3(0.7f, 0.7f, 1.0f)));

		FCarDamage Out;
		Out.Offset = G.Transform(OffsetLocal, TRANSFORMSOURCE_Local, TRANSFORM_World);

		// The moved surface's own facet normal, in tangent space and on the
		// face's side; blended in by how dented it is.
		UMaterialExpressionWorldPosition* World = AddExpr<UMaterialExpressionWorldPosition>(Parent);
		UMaterialExpressionDDX* Ddx = AddExpr<UMaterialExpressionDDX>(Parent);
		Ddx->Value.Expression = World;
		UMaterialExpressionDDY* Ddy = AddExpr<UMaterialExpressionDDY>(Parent);
		Ddy->Value.Expression = World;
		UMaterialExpression* Facet = G.Normalize(G.Add(G.Cross(Ddy, Ddx), G.C3(0.0f, 0.0f, 1.0e-6f)));
		UMaterialExpression* FacetTangent = G.Transform(Facet, TRANSFORMSOURCE_World, TRANSFORM_Tangent);
		UMaterialExpression* Outward = G.Mul(FacetTangent, G.Sign(G.Mask(FacetTangent, false, false, true)));
		UMaterialExpression* Flatness = G.Mul(G.Sat(G.Mul(Weight, G.C(2.0f))), G.C(0.8f));
		Out.Normal = G.Normalize(G.Lerp(G.C3(0.0f, 0.0f, 1.0f), Outward, Flatness));

		// Scuffed in patches where the weight is; streaks of bare metal along the car.
		UMaterialExpression* Patches = G.Noise(NoisePos, 0.05f, 2);
		// Never all of it: some paint survives even a write-off.
		Out.Scuff = G.Mul(G.Sat(G.Mul(G.Sub(G.Add(G.Mul(Weight, G.C(1.2f)), Patches), G.C(1.0f)), G.C(3.0f))), G.C(0.8f));
		UMaterialExpression* Streaks = G.Noise(G.Mul(NoisePos, G.C3(1.0f, 0.12f, 1.0f)), 0.35f, 1);
		Out.ScuffColor = G.Lerp(G.C3(0.03f, 0.03f, 0.032f), G.C3(0.3f, 0.3f, 0.29f), G.Sat(G.Mul(G.Sub(Streaks, G.C(0.7f)), G.C(6.0f))));
		return Out;
	}

	/**
	 * `M_ApexCarSmoke` (ApexCarMaterials::SmokeName): the smoke, steam and
	 * sparks AApexCarEffectsActor draws on instanced spheres and cubes. Lit
	 * translucent, so the sun and the night light it; per instance 0 the
	 * opacity, 1 the shade (near black to white), 2 the glow (emissive, a
	 * spark), 3 the edge softness: a puff fades toward its silhouette, so a
	 * sphere reads as a ball of smoke, and where it meets the ground.
	 */
	void BuildSmoke(UMaterial* Parent)
	{
		const FGraph G{Parent};
		auto Instance = [Parent](uint32 Index) {
			UMaterialExpressionPerInstanceCustomData* E = AddExpr<UMaterialExpressionPerInstanceCustomData>(Parent);
			E->DataIndex = Index;
			E->ConstDefaultValue = 0.0f;
			return E;
		};
		UMaterialExpression* Opacity = Instance(0);
		UMaterialExpression* Shade = Instance(1);
		UMaterialExpression* Glow = Instance(2);
		UMaterialExpression* Softness = Instance(3);

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = G.Lerp(G.C3(0.02f, 0.02f, 0.022f), G.C3(0.8f, 0.8f, 0.82f), Shade);
		EditorOnly->EmissiveColor.Expression = G.Mul(Glow, G.C3(1.0f, 0.45f, 0.12f));
		EditorOnly->Roughness.Expression = G.C(1.0f);
		EditorOnly->Specular.Expression = G.C(0.0f);
		EditorOnly->Metallic.Expression = G.C(0.0f);

		UMaterialExpressionVertexNormalWS* Normal = AddExpr<UMaterialExpressionVertexNormalWS>(Parent);
		UMaterialExpressionCameraVectorWS* Camera = AddExpr<UMaterialExpressionCameraVectorWS>(Parent);
		UMaterialExpression* Facing = G.Pow(G.Sat(G.Dot(Normal, Camera)), 1.6f);
		UMaterialExpression* Edge = G.Lerp(G.C(1.0f), Facing, Softness);
		UMaterialExpressionDepthFade* Fade = AddExpr<UMaterialExpressionDepthFade>(Parent);
		Fade->InOpacity.Expression = G.Mul(Opacity, Edge);
		Fade->FadeDistanceDefault = 60.0f;
		EditorOnly->Opacity.Expression = Fade;

		Parent->BlendMode = BLEND_Translucent;
		Parent->TranslucencyLightingMode = TLM_VolumetricNonDirectional;
		Parent->bUsedWithInstancedStaticMeshes = true;
		Parent->PostEditChange();
	}

	/**
	 * A car parent (`ApexCarMaterials`): glTF's metallic-roughness model
	 * under the names Interchange's glTF parents gave it, which is what the
	 * livery, ghost and brake-light code sets on the runtime instances.
	 * Base colour is the factor times the texture; alpha is the same pair's
	 * alpha. A masked parent clips at `AlphaCutoff` (a parameter, since a
	 * dynamic instance cannot move the clip value): the mask is
	 * `alpha - cutoff + 0.5` against a fixed clip of 0.5. Every one is
	 * two-sided, as every car GLB's material is.
	 */
	void BuildCar(UMaterial* Parent, ECarParent Kind, UTexture* White)
	{
		UMaterialExpressionVectorParameter* Factor = AddExpr<UMaterialExpressionVectorParameter>(Parent);
		Factor->ParameterName = ApexCarMaterials::BaseColorFactor;
		Factor->DefaultValue = FLinearColor::White;
		UMaterialExpressionTextureSampleParameter2D* Sample = AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
		Sample->ParameterName = ApexCarMaterials::BaseColorTexture;
		Sample->Texture = White;
		Sample->SamplerType = SAMPLERTYPE_Color;

		UMaterialExpressionMultiply* BaseColor = AddExpr<UMaterialExpressionMultiply>(Parent);
		BaseColor->A.Expression = CarMask(Parent, Factor, true, true, true, false);
		// Output 0 of a texture sample is its RGB, output 4 its alpha.
		BaseColor->B.Connect(0, Sample);
		UMaterialExpressionMultiply* Alpha = AddExpr<UMaterialExpressionMultiply>(Parent);
		Alpha->A.Expression = CarMask(Parent, Factor, false, false, false, true);
		Alpha->B.Connect(4, Sample);

		UMaterialExpressionVectorParameter* Emissive = AddExpr<UMaterialExpressionVectorParameter>(Parent);
		Emissive->ParameterName = ApexCarMaterials::EmissiveFactor;
		Emissive->DefaultValue = FLinearColor::Black;

		// Dents everywhere (glass and logos move with the panel under them),
		// scuffs on everything but the glass.
		const FGraph G{Parent};
		const FCarDamage Damage = BuildCarDamage(Parent);
		const bool bScuffs = Kind != ECarParent::Translucent;
		auto Scuffed = [&](UMaterialExpression* Clean, UMaterialExpression* Worn) {
			return bScuffs ? G.Lerp(Clean, Worn, Damage.Scuff) : Clean;
		};

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = Scuffed(BaseColor, Damage.ScuffColor);
		EditorOnly->Metallic.Expression = Scuffed(CarScalar(Parent, ApexCarMaterials::MetallicFactor, 1.0f), G.C(0.3f));
		EditorOnly->Roughness.Expression = Scuffed(CarScalar(Parent, ApexCarMaterials::RoughnessFactor, 1.0f), G.C(0.55f));
		EditorOnly->EmissiveColor.Expression = CarMask(Parent, Emissive, true, true, true, false);
		EditorOnly->WorldPositionOffset.Expression = Damage.Offset;
		EditorOnly->Normal.Expression = Damage.Normal;
		Parent->TwoSided = true;

		switch (Kind)
		{
		case ECarParent::ClearCoat:
			Parent->SetShadingModel(MSM_ClearCoat);
			EditorOnly->ClearCoat.Expression = Scuffed(CarScalar(Parent, ApexCarMaterials::ClearCoatFactor, 1.0f), G.C(0.0f));
			EditorOnly->ClearCoatRoughness.Expression = CarScalar(Parent, ApexCarMaterials::ClearCoatRoughnessFactor, 0.03f);
			break;
		case ECarParent::Masked:
		{
			UMaterialExpressionSubtract* Below = AddExpr<UMaterialExpressionSubtract>(Parent);
			Below->A.Expression = Alpha;
			Below->B.Expression = CarScalar(Parent, ApexCarMaterials::AlphaCutoff, 0.5f);
			UMaterialExpressionConstant* Half = AddExpr<UMaterialExpressionConstant>(Parent);
			Half->R = 0.5f;
			UMaterialExpressionAdd* Shifted = AddExpr<UMaterialExpressionAdd>(Parent);
			Shifted->A.Expression = Below;
			Shifted->B.Expression = Half;
			EditorOnly->OpacityMask.Expression = Shifted;
			Parent->BlendMode = BLEND_Masked;
			Parent->OpacityMaskClipValue = 0.5f;
			break;
		}
		case ECarParent::Translucent:
			EditorOnly->Opacity.Expression = Alpha;
			Parent->BlendMode = BLEND_Translucent;
			// Glass wants its reflections lit per pixel, not the volume's blur.
			Parent->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
			break;
		default:
			break;
		}
		Parent->PostEditChange();
	}

	/**
	 * The HLSL the tyre parent's two pixel nodes share: where on the tyre a
	 * pixel is and the tread's groove pattern there. Inputs (all custom
	 * node pins): `P` the local position, `Rm` / `Hm` the tyre's radius and
	 * half width in the mesh's units, `Rcm` / `Hcm` the same in world cm,
	 * `Tread` 0 / 1 / 2, `Chev` +-1, `Wear`, `Flat`, `FlatA`, `Depth` the
	 * pixel's depth, cm. Defines `onTread` (the running surface, 0..1),
	 * `u` (0..1 round the tyre), `av` (|across|, 0 centre to 1 shoulder),
	 * `fade` (how far below a pixel the grooves are), `keep` (what wear
	 * leaves of the grooves), `spot` (the flat spot) and `g` (groove, 0..1).
	 */
	const TCHAR* kTyrePrelude = TEXT(R"HLSL(
struct ApxTread
{
	float Line(float x, float w, float a) { return 1.0 - smoothstep(w - a, w + a, x); }
	// The groove at `u` round the tyre and `av` across it. Each pattern's
	// pitch is a whole number round the tyre (`sTot` is the circumference in
	// tread widths), so no seam where `theta` wraps.
	float G(float u, float av, float tread, float chev, float aa, float sTot)
	{
		if (tread > 1.5)
		{
			// Wet: four deep channels and swept blocks, pointing forward on top.
			float circ = max(Line(abs(av - 0.24), 0.035, aa), Line(abs(av - 0.62), 0.03, aa));
			float ph = frac(u * max(round(sTot * 3.2), 1.0) * chev + av * 1.1);
			float lat = Line(abs(ph - 0.5), 0.07, aa * 3.2) * smoothstep(0.22, 0.27, av);
			return max(circ, lat);
		}
		if (tread > 0.5)
		{
			// Intermediate: two thin channels and many shallow sipes.
			float circ = Line(abs(av - 0.33), 0.022, aa);
			float ph = frac(u * max(round(sTot * 5.0), 1.0) * chev + av * 1.6);
			float lat = 0.75 * Line(abs(ph - 0.5), 0.05, aa * 5.0)
				* smoothstep(0.10, 0.14, av) * (1.0 - smoothstep(0.86, 0.92, av));
			return max(circ, lat);
		}
		return 0.0;
	}
	// What a pattern averages to once its grooves are smaller than a pixel.
	float Cover(float tread) { return tread > 1.5 ? 0.24 : (tread > 0.5 ? 0.14 : 0.0); }
};
ApxTread Tr;
float Rl = max(Rm, 1e-3);
float Hl = max(Hm, 1e-3);
float RW = max(Rcm, 1e-3);
float HW = max(Hcm, 1e-3);
float rr = length(P.yz) / Rl;
float th = atan2(P.z, P.y);
float u = th * 0.15915494 + 0.5;
float v = P.x / Hl;
float av = abs(v);
float sTot = 3.14159265 * RW / HW;
float onTread = Rm > 0.0 ? smoothstep(0.93, 0.975, rr) * (1.0 - smoothstep(0.86, 0.97, av)) : 0.0;
float aa = max(0.006, Depth * 0.0011 / (2.0 * HW));
float fade = saturate((aa - 0.02) / 0.04);
float keep = 1.0 - 0.85 * saturate(Wear);
float dth = abs(atan2(sin(th - FlatA), cos(th - FlatA)));
float flatLen = 0.10 + 0.22 * saturate(Flat);
float spot = (Flat > 0.01 ? 1.0 : 0.0) * (1.0 - smoothstep(flatLen * 0.7, flatLen, dth * RW / (2.0 * HW)))
	* onTread * (1.0 - smoothstep(0.7, 0.9, av));
float g = lerp(Tr.G(u, av, Tread, Chev, aa, sTot), Tr.Cover(Tread), fade) * onTread * keep * (1.0 - 0.7 * spot);
)HLSL");

	/** Base colour (rgb) and roughness (a) from the prelude, `Base` and `Rough`. */
	const TCHAR* kTyreSurface = TEXT(R"HLSL(
float3 c = Base * lerp(1.0, 0.3, g);
float r = lerp(Rough, 0.95, g);
// The compound's ring round both sidewalls (green inter, blue wet; none
// on a slick). A wheel model with its own `wheel_band` ring there hides it,
// and the wheel set paints that ring the same colour.
float ring = (Rm > 0.0 && dot(Band, float3(1.0, 1.0, 1.0)) > 0.0 ? 1.0 : 0.0)
	* smoothstep(0.93, 0.97, av)
	* (1.0 - smoothstep(BandW * 0.5, BandW * 0.5 + 0.004, abs(rr - 0.87)));
c = lerp(c, Band, ring);
r = lerp(r, 0.6, ring);
// Worn: lighter, matte streaks round the tread.
float h1 = frac(sin(floor(v * 40.0) * 91.345) * 43758.5453);
float scuff = saturate(Wear * 1.4) * (0.55 + 0.45 * h1) * onTread * (1.0 - 0.5 * fade);
c = lerp(c, c * 1.7 + 0.010, scuff * 0.7);
r = lerp(r, 0.95, scuff * 0.5);
// Past mid life: grained, little torn ridges across the tread.
float2 cell = floor(float2(u * max(round(sTot * 70.0), 1.0), v * 18.0));
float h2 = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);
float grain = smoothstep(0.45, 0.9, Wear) * step(0.72, h2) * onTread * (1.0 - fade);
c = lerp(c, c * 2.2 + 0.015, grain * 0.6);
// A flat spot: a glazed, lighter patch where the locked tyre slid.
float glaze = spot * (0.45 + 0.55 * saturate(Flat));
c = lerp(c, float3(0.075, 0.077, 0.085), glaze);
r = lerp(r, 0.32, glaze);
// Wet: darker and glossy, the sidewalls a little less; water stands in the grooves.
float w = saturate(Wet) * lerp(0.75, 1.0, onTread);
c *= lerp(1.0, 0.55, w);
r = lerp(r, 0.10, w * (0.8 + 0.2 * g));
return float4(c, saturate(r));
)HLSL");

	/**
	 * The world-space normal: the vertex's, tilted by the grooves' walls
	 * (the pattern's slope by finite differences, so no UVs, tangents or
	 * screen derivatives are needed). `Nv` the vertex normal, `Xw` / `Yw` /
	 * `Zw` the mesh's axes in world space.
	 */
	const TCHAR* kTyreNormal = TEXT(R"HLSL(
float3 n = normalize(Nv);
if (Tread < 0.5 || onTread <= 0.0 || fade >= 1.0)
{
	return n;
}
float e = 0.004;
float g0 = Tr.G(u, av, Tread, Chev, aa, sTot);
float gu = Tr.G(u + e / sTot, av, Tread, Chev, aa, sTot);
float gv = Tr.G(u, av + e, Tread, Chev, aa, sTot);
float3 ts = normalize(-sin(th) * Yw + cos(th) * Zw);
float3 tv = normalize(Xw) * (v < 0.0 ? -1.0 : 1.0);
// Groove depth, cm: a wet's channels deeper than an inter's sipes.
float depthCm = (Tread > 1.5 ? 0.6 : 0.35) * keep * onTread * (1.0 - fade);
// d(groove)/d(cm) round the tyre and across it: `u + e / sTot` is e tread
// widths (2 x HW cm) on, `av + e` is e half widths (HW cm).
float3 grad = ((gu - g0) / (e * 2.0 * HW)) * ts + ((gv - g0) / (e * HW)) * tv;
return normalize(n + depthCm * grad);
)HLSL");

	/**
	 * A deflated tyre, world position offset: below `Rcm - Sag` under the
	 * hub it is flattened onto the road, and the sidewalls there bulge out
	 * along the axle. `Rel` the vertex from the hub, world cm; `Axle` the
	 * axle's world direction. Nothing moves at `Sag` 0.
	 */
	const TCHAR* kTyreSag = TEXT(R"HLSL(
if (Sag <= 0.01 || Rcm <= 0.0)
{
	return float3(0.0, 0.0, 0.0);
}
float d = -Rel.z;
float lim = Rcm - Sag;
float3 o = float3(0.0, 0.0, max(d - lim, 0.0));
float lat = dot(Rel, Axle);
float band = smoothstep(lim - 2.5 * Sag, Rcm, d);
o += Axle * (lat >= 0.0 ? 1.0 : -1.0) * band * Sag * 0.6 * saturate(abs(lat) / max(Hcm, 1.0));
return o;
)HLSL");

	/** One input pin of a custom node. */
	struct FCustomPin
	{
		const TCHAR* Name;
		UMaterialExpression* Expression;
	};

	UMaterialExpressionCustom* AddCustom(UMaterial* Material, const TCHAR* Description, const FString& Code,
		ECustomMaterialOutputType Type, const TArray<FCustomPin>& Inputs)
	{
		UMaterialExpressionCustom* E = AddExpr<UMaterialExpressionCustom>(Material);
		E->Description = Description;
		E->Code = Code;
		E->OutputType = Type;
		E->Inputs.Reset();
		for (const FCustomPin& Pin : Inputs)
		{
			FCustomInput& In = E->Inputs.AddDefaulted_GetRef();
			In.InputName = Pin.Name;
			In.Input.Expression = Pin.Expression;
		}
		return E;
	}

	/**
	 * `M_ApexCarTyre`, the `wheel_tyre` slot's parent: the opaque car
	 * parent's inputs (`BaseColorFactor` x `BaseColorTexture`, `MetallicFactor`,
	 * `RoughnessFactor`, `EmissiveFactor`) and, from the wheel component's
	 * custom primitive data (ApexCarMaterials::TyreCpd), the tyre's state
	 * laid over them without UVs: the tread of an intermediate or a wet
	 * (grooves fading to their average under a pixel and with wear), wear's
	 * scuffing and graining, a flat spot's glazed patch, water, and a
	 * deflated tyre's squat (WPO). With the data at zero (no wheel set
	 * wrote it) it draws as the opaque parent would. The normal is world
	 * space: the class tyres' tangents are meaningless without UVs.
	 */
	void BuildCarTyre(UMaterial* Parent, UTexture* White)
	{
		const FGraph G{Parent};
		UMaterialExpressionVectorParameter* Factor = AddExpr<UMaterialExpressionVectorParameter>(Parent);
		Factor->ParameterName = ApexCarMaterials::BaseColorFactor;
		Factor->DefaultValue = FLinearColor::White;
		UMaterialExpressionTextureSampleParameter2D* Sample = AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
		Sample->ParameterName = ApexCarMaterials::BaseColorTexture;
		Sample->Texture = White;
		Sample->SamplerType = SAMPLERTYPE_Color;
		UMaterialExpressionMultiply* Base = AddExpr<UMaterialExpressionMultiply>(Parent);
		Base->A.Expression = CarMask(Parent, Factor, true, true, true, false);
		Base->B.Connect(0, Sample);
		UMaterialExpressionVectorParameter* Emissive = AddExpr<UMaterialExpressionVectorParameter>(Parent);
		Emissive->ParameterName = ApexCarMaterials::EmissiveFactor;
		Emissive->DefaultValue = FLinearColor::Black;
		UMaterialExpression* Rough = CarScalar(Parent, ApexCarMaterials::RoughnessFactor, 0.8f);

		namespace Cpd = ApexCarMaterials::TyreCpd;
		UMaterialExpression* Rm = G.PrimitiveScalar(TEXT("TyreRadiusLocal"), Cpd::RadiusLocal);
		UMaterialExpression* Hm = G.PrimitiveScalar(TEXT("TyreHalfWidthLocal"), Cpd::HalfWidthLocal);
		UMaterialExpression* Rcm = G.PrimitiveScalar(TEXT("TyreRadiusCm"), Cpd::RadiusCm);
		UMaterialExpression* Hcm = G.PrimitiveScalar(TEXT("TyreHalfWidthCm"), Cpd::HalfWidthCm);
		UMaterialExpression* Tread = G.PrimitiveScalar(TEXT("TyreTread"), Cpd::Tread);
		UMaterialExpression* Chev = G.PrimitiveScalar(TEXT("TyreChevron"), Cpd::Chevron);
		UMaterialExpression* Wear = G.PrimitiveScalar(TEXT("TyreWear"), Cpd::Wear);
		UMaterialExpression* Wet = G.PrimitiveScalar(TEXT("TyreWetness"), Cpd::Wetness);
		UMaterialExpression* Flat = G.PrimitiveScalar(TEXT("TyreFlatSpot"), Cpd::FlatSpot);
		UMaterialExpression* FlatA = G.PrimitiveScalar(TEXT("TyreFlatSpotAngle"), Cpd::FlatSpotAngle);
		UMaterialExpression* Sag = G.PrimitiveScalar(TEXT("TyreSagCm"), Cpd::SagCm);

		// The undeformed position in the wheel mesh's frame: the tread turns with the wheel.
		UMaterialExpressionLocalPosition* Local = AddExpr<UMaterialExpressionLocalPosition>(Parent);
		Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
		Local->LocalOrigin = ELocalPositionOrigin::Primitive;
		UMaterialExpressionPixelDepth* Depth = AddExpr<UMaterialExpressionPixelDepth>(Parent);
		auto Axis = [&](float X, float Y, float Z) {
			return G.Transform(G.C3(X, Y, Z), TRANSFORMSOURCE_Local, TRANSFORM_World);
		};

		const FString Prelude(kTyrePrelude);
		UMaterialExpressionCustom* Surface = AddCustom(Parent, TEXT("ApexTyreSurface"), Prelude + kTyreSurface, CMOT_Float4,
			{{TEXT("P"), Local}, {TEXT("Rm"), Rm}, {TEXT("Hm"), Hm}, {TEXT("Rcm"), Rcm}, {TEXT("Hcm"), Hcm},
				{TEXT("Tread"), Tread}, {TEXT("Chev"), Chev}, {TEXT("Wear"), Wear}, {TEXT("Flat"), Flat},
				{TEXT("FlatA"), FlatA}, {TEXT("Depth"), Depth}, {TEXT("Base"), Base}, {TEXT("Rough"), Rough},
				{TEXT("Wet"), Wet}, {TEXT("Band"), G.PrimitiveVector(TEXT("TyreBandColour"), Cpd::BandColour, FLinearColor::Black)},
				{TEXT("BandW"), CarScalar(Parent, ApexCarMaterials::TyreBandWidth, 0.035f)}});
		UMaterialExpressionCustom* Normal = AddCustom(Parent, TEXT("ApexTyreNormal"), Prelude + kTyreNormal, CMOT_Float3,
			{{TEXT("P"), Local}, {TEXT("Rm"), Rm}, {TEXT("Hm"), Hm}, {TEXT("Rcm"), Rcm}, {TEXT("Hcm"), Hcm},
				{TEXT("Tread"), Tread}, {TEXT("Chev"), Chev}, {TEXT("Wear"), Wear}, {TEXT("Flat"), Flat},
				{TEXT("FlatA"), FlatA}, {TEXT("Depth"), Depth}, {TEXT("Nv"), AddExpr<UMaterialExpressionVertexNormalWS>(Parent)},
				{TEXT("Xw"), Axis(1.0f, 0.0f, 0.0f)}, {TEXT("Yw"), Axis(0.0f, 1.0f, 0.0f)}, {TEXT("Zw"), Axis(0.0f, 0.0f, 1.0f)}});
		UMaterialExpressionCustom* Squat = AddCustom(Parent, TEXT("ApexTyreSag"), kTyreSag, CMOT_Float3,
			{{TEXT("Rel"), G.Transform(Local, TRANSFORMSOURCE_Local, TRANSFORM_World)},
				{TEXT("Axle"), G.Normalize(Axis(1.0f, 0.0f, 0.0f))}, {TEXT("Sag"), Sag}, {TEXT("Rcm"), Rcm},
				{TEXT("Hcm"), Hcm}});

		UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
		EditorOnly->BaseColor.Expression = G.MaskRgb(Surface);
		UMaterialExpressionComponentMask* SurfaceRough = AddExpr<UMaterialExpressionComponentMask>(Parent);
		SurfaceRough->Input.Expression = Surface;
		SurfaceRough->R = SurfaceRough->G = SurfaceRough->B = 0;
		SurfaceRough->A = 1;
		EditorOnly->Roughness.Expression = SurfaceRough;
		EditorOnly->Metallic.Expression = CarScalar(Parent, ApexCarMaterials::MetallicFactor, 0.0f);
		EditorOnly->EmissiveColor.Expression = CarMask(Parent, Emissive, true, true, true, false);
		EditorOnly->Normal.Expression = Normal;
		EditorOnly->WorldPositionOffset.Expression = Squat;
		Parent->bTangentSpaceNormal = false;
		Parent->TwoSided = true;
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

	/** Generate one parent with `Build` as `PackageName`, and save it. */
	bool BakeAs(const FString& PackageName, TFunctionRef<void(UMaterial*)> Build, FString& OutError)
	{
		const FString Name = FPackageName::GetShortName(PackageName);
		UPackage* Package = MakeMaterialPackage(PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
			return false;
		}
		UMaterial* Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone);
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

	/** A track parent, under `/Game/Materials/Track`. */
	bool BakeOne(const TCHAR* Name, TFunctionRef<void(UMaterial*)> Build, FString& OutError)
	{
		return BakeAs(ApexTrackMaterials::PackageName(Name), Build, OutError);
	}

	bool ParentExists(const TCHAR* Name)
	{
		return FPackageName::DoesPackageExist(ApexTrackMaterials::PackageName(Name));
	}

	/**
	 * A ground map the base samples as another class than the map now is:
	 * the roughness maps went from G8 (`TC_Grayscale`) to BC4 (`TC_Alpha`),
	 * and a base baked before cannot compile against the re-imported ones.
	 */
	FString StaleGroundSampler(const UMaterialInterface* Base)
	{
		const UMaterial* Material = Base ? Base->GetMaterial() : nullptr;
		if (!Material)
		{
			return FString();
		}
		const FString Root = FString(ApexGround::TexturesRoot) + TEXT("/");
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			const UMaterialExpressionTextureBase* Sample = Cast<UMaterialExpressionTextureBase>(Expression.Get());
			if (Sample && Sample->Texture && Sample->Texture->GetPathName().StartsWith(Root)
				&& Sample->SamplerType != MaterialExpressionUtils::GetSamplerTypeForTexture(Sample->Texture))
			{
				return Sample->Texture->GetName();
			}
		}
		return FString();
	}
	/**
	 * The road state over the road's surface (`M_ApexTrackRoad` only).
	 *
	 * The race director fills two textures from the server's `RoadState`
	 * (`FApexRoadStateMap`): `RoadState`, a column per 10 m cell of the lap
	 * and a row per 1 m bin across the road (R rubber, G marbles, B dry, A
	 * water depth, 255 = 2.55 of heavy rain), and `RoadGeometry`, one texel
	 * per cell holding where the server measures that cell's bins from (the
	 * centerline point in world cm, then the unit vector to its left in
	 * world axes). The road mesh's UV0 `u` is metres of station, which picks
	 * the cell; the pixel's world position against that cell's point gives
	 * its lateral exactly as `RoadState::lateral_of` does, which picks the
	 * bin. `RoadStateAmount` 0 (the default, and what an instance has until
	 * a road state arrives) leaves the surface as built.
	 *
	 * What it draws: rubber darkens the asphalt and makes it a little
	 * glossier (0.5 is the calibrated look, unchanged); marbles show as dark
	 * crumbs where they lie; water darkens and glosses the bins the wheels
	 * have not wiped, so a dry line appears between wet edges; standing
	 * water over the rain's depth goes mirror-smooth and flat.
	 */
	void ApplyRoadStateGraph(UMaterial* Parent, UMaterialExpression* TexCoord, UMaterialExpression* WorldPos,
		UMaterialExpression*& InOutAlbedo, UMaterialExpression*& InOutRoughness, UMaterialExpression*& InOutNormal)
	{
		const FGraph G{Parent};

		// A neutral 1x1 default, a subobject of the parent (a rebake moves it
		// out with the old parent): rubber at
		// the calibrated half, no marbles, no water. Linear, so the samplers
		// take the director's linear textures at runtime.
		UTexture2D* Neutral = NewObject<UTexture2D>(Parent, TEXT("T_RoadStateNeutral"));
		const uint8 NeutralTexel[4] = {0, 0, 128, 0};	 // B G R A
		Neutral->Source.Init(1, 1, 1, 1, TSF_BGRA8, NeutralTexel);
		Neutral->SRGB = false;
		Neutral->CompressionSettings = TC_VectorDisplacementmap;
		Neutral->MipGenSettings = TMGS_NoMipmaps;
		Neutral->Filter = TF_Nearest;
		Neutral->PostEditChange();

		auto Param = [Parent](const TCHAR* Name, float Default) {
			UMaterialExpressionScalarParameter* P = AddExpr<UMaterialExpressionScalarParameter>(Parent);
			P->ParameterName = Name;
			P->DefaultValue = Default;
			return P;
		};
		auto Sample = [Parent, Neutral](const TCHAR* Name, UMaterialExpression* Coords) {
			UMaterialExpressionTextureSampleParameter2D* S = AddExpr<UMaterialExpressionTextureSampleParameter2D>(Parent);
			S->ParameterName = Name;
			S->Texture = Neutral;
			S->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Neutral);
			S->Coordinates.Expression = Coords;
			return S;
		};
		// One output of a sample (1 R, 2 G, 3 B, 4 A) as a node of its own.
		auto Channel = [Parent](UMaterialExpression* Source, int32 Output) {
			UMaterialExpressionMultiply* M = AddExpr<UMaterialExpressionMultiply>(Parent);
			M->A.Expression = Source;
			M->A.OutputIndex = Output;
			M->ConstB = 1.0f;
			return M;
		};

		UMaterialExpression* Amount = Param(TEXT("RoadStateAmount"), 0.0f);
		// 1 / (cell length x cells in the texture): station metres to `u`.
		UMaterialExpression* PerMetre = Param(TEXT("RoadStateU"), 0.0f);
		UMaterialExpression* HalfSpan = Param(TEXT("RoadStateHalfSpanM"), 16.0f);

		// The cell: `u` from the station.
		UMaterialExpression* Station = G.Mask(TexCoord, true, false, false);
		UMaterialExpression* U = G.Mul(Station, PerMetre);
		UMaterialExpression* Geo = Sample(TEXT("RoadGeometry"), G.Append(U, G.C(0.5f)));
		UMaterialExpression* CentreCm = G.Append(Channel(Geo, 1), Channel(Geo, 2));
		UMaterialExpression* LeftDir = G.Append(Channel(Geo, 3), Channel(Geo, 4));

		// The bin: the lateral, positive right, as the server measures it.
		UMaterialExpression* Here = G.Mask(WorldPos, true, true, false);
		UMaterialExpression* LeftCm = G.Dot(G.Sub(Here, CentreCm), LeftDir);
		UMaterialExpression* RightM = G.Mul(LeftCm, G.C(-0.01f));
		UMaterialExpression* V = G.Div(G.Add(RightM, HalfSpan), G.Mul(HalfSpan, G.C(2.0f)));
		UMaterialExpression* State = Sample(TEXT("RoadState"), G.Append(U, V));
		UMaterialExpression* Rubber = Channel(State, 1);
		UMaterialExpression* Marbles = Channel(State, 2);
		UMaterialExpression* Dry = Channel(State, 3);
		UMaterialExpression* Depth = G.Mul(Channel(State, 4), G.C(2.55f));

		// Water the wheels have not wiped: the road's own wet threshold (0.5
		// of heavy rain is fully wet, `road_state::step`), less what is dry.
		UMaterialExpression* Wet = G.Mul(G.Sat(G.Mul(Depth, G.C(2.0f))), G.Sub(G.C(1.0f), Dry));
		// Standing water deeper than the rain leaves.
		UMaterialExpression* Puddle = G.Sat(G.Mul(G.Sub(Depth, G.C(1.0f)), G.C(2.0f)));
		// Marbles as crumbs: 10 cm grain, shown where there are marbles.
		UMaterialExpression* Grain = G.Noise(WorldPos, 0.1f, 1);
		UMaterialExpression* Crumbs = G.Sat(G.Mul(G.Sub(Grain, G.Sub(G.C(1.0f), Marbles)), G.C(6.0f)));

		UMaterialExpression* RubberOver = G.Sub(Rubber, G.C(0.5f));
		UMaterialExpression* Shade = G.Mul(
			G.Mul(G.Sub(G.C(1.0f), G.Mul(RubberOver, G.C(0.35f))), G.Lerp(G.C(1.0f), G.C(0.55f), Crumbs)),
			G.Sub(G.C(1.0f), G.Mul(Wet, G.C(0.3f))));
		UMaterialExpression* Albedo = G.Mul(InOutAlbedo, Shade);

		UMaterialExpression* Rough = G.Sub(InOutRoughness, G.Mul(RubberOver, G.C(0.15f)));
		Rough = G.Add(Rough, G.Mul(Crumbs, G.C(0.1f)));
		// Wet is ApexSky::WetRoadRoughness (0.3); a puddle is a mirror.
		Rough = G.Lerp(Rough, G.C(0.3f), Wet);
		Rough = G.Lerp(Rough, G.C(0.05f), Puddle);

		InOutAlbedo = G.Lerp(InOutAlbedo, Albedo, Amount);
		InOutRoughness = G.Lerp(InOutRoughness, Rough, Amount);
		if (InOutNormal)
		{
			UMaterialExpression* Flat = G.C3(0.0f, 0.0f, 1.0f);
			UMaterialExpression* Calm = G.Lerp(InOutNormal, Flat, G.Mul(Puddle, Amount));
			InOutNormal = G.Normalize(Calm);
		}
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
		else if (const FString Stale = StaleGroundSampler(Current.Base); !Stale.IsEmpty())
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    %s samples %s as another kind of texture than it now is; baking it again"),
				ApexTrackMaterials::BaseName, *Stale);
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
	// The road's own parent: the base's graph plus the road state. Baked
	// with the base, so the two always carry the same surface.
	if (bRebakeBase || !ParentExists(ApexTrackMaterials::RoadName))
	{
		bool bGroundTextures = false;
		if (!BakeOne(ApexTrackMaterials::RoadName,
				[&bGroundTextures](UMaterial* M) { BuildTrackBase(M, bGroundTextures, /*bRoadState*/ true); }, OutError))
		{
			return false;
		}
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

	// The cars' parents, under /Game/Materials/Car.
	UTexture* White = LoadObject<UTexture>(nullptr, kWhiteTexture);
	if (!White)
	{
		UE_LOG(LogApexTrackImport, Warning, TEXT("    the engine's WhiteSquareTexture is missing; the car parents sample the default texture until an instance sets one"));
		White = Placeholder;
	}
	const TPair<const TCHAR*, ECarParent> CarParents[] = {
		{ApexCarMaterials::OpaqueName, ECarParent::Opaque},
		{ApexCarMaterials::ClearCoatName, ECarParent::ClearCoat},
		{ApexCarMaterials::MaskedName, ECarParent::Masked},
		{ApexCarMaterials::TranslucentName, ECarParent::Translucent},
	};
	int32 CarsBaked = 0;
	for (const TPair<const TCHAR*, ECarParent>& Car : CarParents)
	{
		const FString PackageName = ApexCarMaterials::PackageName(Car.Key);
		if (!bForce && FPackageName::DoesPackageExist(PackageName))
		{
			// One baked before the damage graph draws no dents: bake it again.
			const UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, *ApexCarMaterials::ObjectPath(Car.Key));
			float Dent = 0.0f;
			if (!Existing
				|| Existing->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(ApexCarMaterials::DamageDentCm), Dent))
			{
				continue;
			}
			UE_LOG(LogApexTrackImport, Display, TEXT("    %s has no damage graph; baking it again"), Car.Key);
		}
		const ECarParent Kind = Car.Value;
		if (!BakeAs(PackageName, [Kind, White](UMaterial* M) { BuildCar(M, Kind, White); }, OutError))
		{
			return false;
		}
		++CarsBaked;
	}
	const FString TyrePackage = ApexCarMaterials::PackageName(ApexCarMaterials::TyreName);
	bool bBakeTyre = bForce || !FPackageName::DoesPackageExist(TyrePackage);
	if (!bBakeTyre)
	{
		// One baked before the compound ring draws none: bake it again.
		const UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, *ApexCarMaterials::ObjectPath(ApexCarMaterials::TyreName));
		float Width = 0.0f;
		bBakeTyre = Existing
			&& !Existing->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(ApexCarMaterials::TyreBandWidth), Width);
		if (bBakeTyre)
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    %s has no compound ring; baking it again"), ApexCarMaterials::TyreName);
		}
	}
	if (bBakeTyre)
	{
		if (!BakeAs(TyrePackage, [White](UMaterial* M) { BuildCarTyre(M, White); }, OutError))
		{
			return false;
		}
		++CarsBaked;
	}
	const FString SmokePackage = ApexCarMaterials::PackageName(ApexCarMaterials::SmokeName);
	if (bForce || !FPackageName::DoesPackageExist(SmokePackage))
	{
		if (!BakeAs(SmokePackage, [](UMaterial* M) { BuildSmoke(M); }, OutError))
		{
			return false;
		}
		++CarsBaked;
	}
	if (CarsBaked == 0)
	{
		UE_LOG(LogApexTrackImport, Display, TEXT("    the car parent materials under %s are up to date"),
			ApexCarMaterials::Folder);
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
