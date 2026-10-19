#pragma once

#include "CoreMinimal.h"
#include "Hud/ApexHudValue.h"

struct FApexHudData;

/**
 * What an expression can see while it is evaluated: the game's data points,
 * and inside a `repeat` the row being drawn (`item.<field>`) and its number
 * (`index`, from 0).
 */
struct FApexHudScope
{
	const FApexHudData* Data = nullptr;
	const FApexHudRecord* Item = nullptr;
	int32 Index = -1;
};

/**
 * The HUD's expression language: a component's dynamic attributes, compiled
 * once when the component is loaded and evaluated every frame.
 *
 * Small on purpose, and side-effect free, so a component can only ever read
 * the game, never change it, and a broken one cannot take the frame down:
 *
 * - literals `12`, `1.5`, `'text'` / `"text"`, `true`, `false`, `null`
 * - data points by name, `car.speed_kph`; in a repeat, `item.name` and `index`
 * - `+ - * / %` (`+` joins text when either side is text), `== != < <= > >=`,
 *   `&& || !` (or `and or not`), `cond ? a : b`
 * - functions: see docs/game/hud-modding.md, or `FunctionNames()`
 *
 * A name nobody fills evaluates to `null`, which reads as false, 0 and "".
 */
class APEXSIM_API FApexHudExpr
{
public:
	/** An expression: `car.rpm / car.rpm_max`. Null with OutError set when it does not parse. */
	static TSharedPtr<const FApexHudExpr> Compile(const FString& Source, FString& OutError);

	/** Text with `{expression}` holes: `LAP {lap.display}`; `{{` and `}}` are literal braces. */
	static TSharedPtr<const FApexHudExpr> CompileTemplate(const FString& Source, FString& OutError);

	/** An expression that is always `Value`. */
	static TSharedPtr<const FApexHudExpr> Constant(const FApexHudValue& Value);

	FApexHudValue Evaluate(const FApexHudScope& Scope) const;

	/** Whether it reads nothing at all, so it can be applied once and forgotten. */
	bool IsConstant() const;

	/** Every data point it reads (global names only; `item.x` comes back as `item.x`). */
	void CollectIdentifiers(TArray<FString>& Out) const;

	/** Every function name the language knows. */
	static TArray<FString> FunctionNames();

	const FString& GetSource() const { return Source; }

	/** Opaque node storage; public so the parser and evaluator in the .cpp can share it. */
	struct FNode;

private:
	FString Source;
	TArray<FNode> Nodes;
	int32 Root = INDEX_NONE;

	FApexHudValue Eval(int32 NodeIndex, const FApexHudScope& Scope) const;

	friend struct FApexHudExprParser;
};

struct FApexHudExpr::FNode
{
	enum class EKind : uint8
	{
		Literal,
		/** A data point, `Name`. */
		Global,
		/** `item.<Name>` in a repeat over a list. */
		ItemField,
		/** `index` in a repeat. */
		Index,
		Unary,
		Binary,
		/** `Args[0] ? Args[1] : Args[2]`. */
		Ternary,
		Call,
		/** Template pieces joined as text. */
		Concat,
	};

	EKind Kind = EKind::Literal;
	FApexHudValue Literal;
	FName Name;
	/** The operator of a Unary / Binary node (an EOp in the .cpp). */
	uint8 Op = 0;
	/** The function of a Call node (an EFunction in the .cpp). */
	int32 Function = INDEX_NONE;
	TArray<int32> Args;
};
