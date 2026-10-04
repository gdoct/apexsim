#include "Hud/ApexHudExpression.h"

#include "Hud/ApexHudData.h"

namespace
{
	enum class EOp : uint8
	{
		Add,
		Sub,
		Mul,
		Div,
		Mod,
		Eq,
		Ne,
		Lt,
		Le,
		Gt,
		Ge,
		And,
		Or,
		Not,
		Neg,
	};

	enum class EFunction : int32
	{
		If,
		Switch,
		Has,
		Min,
		Max,
		Clamp,
		Abs,
		Floor,
		Ceil,
		Round,
		Fmt,
		FmtTime,
		FmtSplit,
		FmtGap,
		FmtDelta,
		FmtClock,
		Upper,
		Lower,
		Str,
		Num,
		Mix,
		Ramp,
		Alpha,
		Rgb,
	};

	struct FFunctionInfo
	{
		const TCHAR* Name;
		EFunction Id;
		int32 MinArgs;
		/** -1: any number from MinArgs up. */
		int32 MaxArgs;
	};

	const FFunctionInfo Functions[] = {
		{TEXT("if"), EFunction::If, 3, 3},
		{TEXT("switch"), EFunction::Switch, 3, -1},
		{TEXT("has"), EFunction::Has, 1, 1},
		{TEXT("min"), EFunction::Min, 1, -1},
		{TEXT("max"), EFunction::Max, 1, -1},
		{TEXT("clamp"), EFunction::Clamp, 3, 3},
		{TEXT("abs"), EFunction::Abs, 1, 1},
		{TEXT("floor"), EFunction::Floor, 1, 1},
		{TEXT("ceil"), EFunction::Ceil, 1, 1},
		{TEXT("round"), EFunction::Round, 1, 2},
		{TEXT("fmt"), EFunction::Fmt, 1, 2},
		{TEXT("fmt_time"), EFunction::FmtTime, 1, 1},
		{TEXT("fmt_split"), EFunction::FmtSplit, 1, 1},
		{TEXT("fmt_gap"), EFunction::FmtGap, 1, 2},
		{TEXT("fmt_delta"), EFunction::FmtDelta, 1, 1},
		{TEXT("fmt_clock"), EFunction::FmtClock, 1, 1},
		{TEXT("upper"), EFunction::Upper, 1, 1},
		{TEXT("lower"), EFunction::Lower, 1, 1},
		{TEXT("str"), EFunction::Str, 1, 1},
		{TEXT("num"), EFunction::Num, 1, 1},
		{TEXT("mix"), EFunction::Mix, 3, 3},
		{TEXT("ramp"), EFunction::Ramp, 3, -1},
		{TEXT("alpha"), EFunction::Alpha, 2, 2},
		{TEXT("rgb"), EFunction::Rgb, 3, 4},
	};

	const FFunctionInfo* FindFunction(const FString& Name)
	{
		for (const FFunctionInfo& Info : Functions)
		{
			if (Name.Equals(Info.Name, ESearchCase::CaseSensitive))
			{
				return &Info;
			}
		}
		return nullptr;
	}

	enum class ETok : uint8
	{
		End,
		Number,
		String,
		Ident,
		Op,
		LParen,
		RParen,
		Comma,
		Question,
		Colon,
	};

	struct FTok
	{
		ETok Kind = ETok::End;
		FString Text;
		double Number = 0.0;
		int32 Pos = 0;
	};

	bool IsIdentStart(TCHAR C) { return FChar::IsAlpha(C) || C == '_'; }
	bool IsIdentChar(TCHAR C) { return FChar::IsAlnum(C) || C == '_' || C == '.'; }

	bool Tokenize(const FString& Source, TArray<FTok>& Out, FString& OutError)
	{
		int32 I = 0;
		const int32 N = Source.Len();
		while (I < N)
		{
			const TCHAR C = Source[I];
			if (FChar::IsWhitespace(C))
			{
				++I;
				continue;
			}
			FTok Tok;
			Tok.Pos = I;
			if (FChar::IsDigit(C) || (C == '.' && I + 1 < N && FChar::IsDigit(Source[I + 1])))
			{
				int32 End = I;
				while (End < N && (FChar::IsDigit(Source[End]) || Source[End] == '.'))
				{
					++End;
				}
				Tok.Kind = ETok::Number;
				Tok.Text = Source.Mid(I, End - I);
				Tok.Number = FCString::Atod(*Tok.Text);
				I = End;
			}
			else if (C == '\'' || C == '"')
			{
				int32 End = I + 1;
				FString Text;
				while (End < N && Source[End] != C)
				{
					if (Source[End] == '\\' && End + 1 < N)
					{
						++End;
					}
					Text.AppendChar(Source[End]);
					++End;
				}
				if (End >= N)
				{
					OutError = FString::Printf(TEXT("unterminated text starting at column %d"), I + 1);
					return false;
				}
				Tok.Kind = ETok::String;
				Tok.Text = MoveTemp(Text);
				I = End + 1;
			}
			else if (IsIdentStart(C))
			{
				int32 End = I;
				while (End < N && IsIdentChar(Source[End]))
				{
					++End;
				}
				Tok.Kind = ETok::Ident;
				Tok.Text = Source.Mid(I, End - I);
				if (Tok.Text.EndsWith(TEXT(".")))
				{
					OutError = FString::Printf(TEXT("'%s' ends with a dot"), *Tok.Text);
					return false;
				}
				// The word operators are spelt out for anyone who prefers them.
				if (Tok.Text == TEXT("and"))
				{
					Tok.Kind = ETok::Op;
					Tok.Text = TEXT("&&");
				}
				else if (Tok.Text == TEXT("or"))
				{
					Tok.Kind = ETok::Op;
					Tok.Text = TEXT("||");
				}
				else if (Tok.Text == TEXT("not"))
				{
					Tok.Kind = ETok::Op;
					Tok.Text = TEXT("!");
				}
				I = End;
			}
			else
			{
				static const TCHAR* TwoChar[] = {TEXT("=="), TEXT("!="), TEXT("<="), TEXT(">="), TEXT("&&"), TEXT("||")};
				bool bMatched = false;
				for (const TCHAR* Op : TwoChar)
				{
					if (I + 1 < N && Source[I] == Op[0] && Source[I + 1] == Op[1])
					{
						Tok.Kind = ETok::Op;
						Tok.Text = Op;
						I += 2;
						bMatched = true;
						break;
					}
				}
				if (!bMatched)
				{
					switch (C)
					{
					case '+': case '-': case '*': case '/': case '%': case '<': case '>': case '!':
						Tok.Kind = ETok::Op;
						Tok.Text = FString::Chr(C);
						break;
					case '(': Tok.Kind = ETok::LParen; break;
					case ')': Tok.Kind = ETok::RParen; break;
					case ',': Tok.Kind = ETok::Comma; break;
					case '?': Tok.Kind = ETok::Question; break;
					case ':': Tok.Kind = ETok::Colon; break;
					default:
						OutError = FString::Printf(TEXT("unexpected '%c' at column %d"), C, I + 1);
						return false;
					}
					++I;
				}
			}
			Out.Add(MoveTemp(Tok));
		}
		FTok End;
		End.Pos = N;
		Out.Add(End);
		return true;
	}

	/** Binding power of a binary operator; -1 for anything else. */
	int32 Precedence(const FTok& Tok, EOp& OutOp)
	{
		if (Tok.Kind != ETok::Op)
		{
			return -1;
		}
		struct FEntry { const TCHAR* Text; EOp Op; int32 Prec; };
		static const FEntry Table[] = {
			{TEXT("||"), EOp::Or, 2},
			{TEXT("&&"), EOp::And, 3},
			{TEXT("=="), EOp::Eq, 4},
			{TEXT("!="), EOp::Ne, 4},
			{TEXT("<"), EOp::Lt, 5},
			{TEXT("<="), EOp::Le, 5},
			{TEXT(">"), EOp::Gt, 5},
			{TEXT(">="), EOp::Ge, 5},
			{TEXT("+"), EOp::Add, 6},
			{TEXT("-"), EOp::Sub, 6},
			{TEXT("*"), EOp::Mul, 7},
			{TEXT("/"), EOp::Div, 7},
			{TEXT("%"), EOp::Mod, 7},
		};
		for (const FEntry& Entry : Table)
		{
			if (Tok.Text == Entry.Text)
			{
				OutOp = Entry.Op;
				return Entry.Prec;
			}
		}
		return -1;
	}

	FString FixedDecimals(double Value, int32 Decimals)
	{
		Decimals = FMath::Clamp(Decimals, 0, 6);
		return FString::Printf(TEXT("%.*f"), Decimals, Value);
	}

	FLinearColor ColourOf(const FApexHudValue& Value)
	{
		FLinearColor Out = FLinearColor::Transparent;
		Value.AsColour(Out);
		return Out;
	}
}

/** Recursive descent with precedence climbing, appending into an expression's node array. */
struct FApexHudExprParser
{
	FApexHudExpr& Target;
	TArray<FTok> Tokens;
	int32 At = 0;
	FString Error;

	explicit FApexHudExprParser(FApexHudExpr& InTarget) : Target(InTarget) {}

	using FNode = FApexHudExpr::FNode;

	int32 Add(FNode&& Node)
	{
		return Target.Nodes.Add(MoveTemp(Node));
	}

	const FTok& Peek() const { return Tokens[At]; }

	bool Fail(const FString& Message)
	{
		if (Error.IsEmpty())
		{
			Error = Message;
		}
		return false;
	}

	FString Describe(const FTok& Tok) const
	{
		switch (Tok.Kind)
		{
		case ETok::End: return TEXT("the end");
		case ETok::String: return FString::Printf(TEXT("'%s'"), *Tok.Text);
		case ETok::LParen: return TEXT("'('");
		case ETok::RParen: return TEXT("')'");
		case ETok::Comma: return TEXT("','");
		case ETok::Question: return TEXT("'?'");
		case ETok::Colon: return TEXT("':'");
		default: return FString::Printf(TEXT("'%s'"), *Tok.Text);
		}
	}

	/** Parses the whole token stream; the root node index, or INDEX_NONE. */
	int32 ParseAll(const FString& Source)
	{
		if (!Tokenize(Source, Tokens, Error))
		{
			return INDEX_NONE;
		}
		if (Peek().Kind == ETok::End)
		{
			Fail(TEXT("empty expression"));
			return INDEX_NONE;
		}
		const int32 Root = ParseExpr(0);
		if (Root != INDEX_NONE && Peek().Kind != ETok::End)
		{
			Fail(FString::Printf(TEXT("unexpected %s at column %d"), *Describe(Peek()), Peek().Pos + 1));
			return INDEX_NONE;
		}
		return Error.IsEmpty() ? Root : INDEX_NONE;
	}

	int32 ParseExpr(int32 MinPrec)
	{
		int32 Left = ParseUnary();
		while (Left != INDEX_NONE)
		{
			const FTok& Tok = Peek();
			if (Tok.Kind == ETok::Question && MinPrec <= 1)
			{
				++At;
				const int32 Then = ParseExpr(1);
				if (Then == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				if (Peek().Kind != ETok::Colon)
				{
					Fail(FString::Printf(TEXT("expected ':' at column %d"), Peek().Pos + 1));
					return INDEX_NONE;
				}
				++At;
				const int32 Else = ParseExpr(1);
				if (Else == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				FNode Node;
				Node.Kind = FNode::EKind::Ternary;
				Node.Args = {Left, Then, Else};
				Left = Add(MoveTemp(Node));
				continue;
			}
			EOp Op;
			const int32 Prec = Precedence(Tok, Op);
			if (Prec < 0 || Prec < MinPrec)
			{
				break;
			}
			++At;
			const int32 Right = ParseExpr(Prec + 1);
			if (Right == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			FNode Node;
			Node.Kind = FNode::EKind::Binary;
			Node.Op = static_cast<uint8>(Op);
			Node.Args = {Left, Right};
			Left = Add(MoveTemp(Node));
		}
		return Left;
	}

	int32 ParseUnary()
	{
		const FTok& Tok = Peek();
		if (Tok.Kind == ETok::Op && (Tok.Text == TEXT("!") || Tok.Text == TEXT("-")))
		{
			const EOp Op = Tok.Text == TEXT("!") ? EOp::Not : EOp::Neg;
			++At;
			const int32 Operand = ParseUnary();
			if (Operand == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			FNode Node;
			Node.Kind = FNode::EKind::Unary;
			Node.Op = static_cast<uint8>(Op);
			Node.Args = {Operand};
			return Add(MoveTemp(Node));
		}
		return ParsePrimary();
	}

	int32 ParsePrimary()
	{
		const FTok Tok = Peek();
		switch (Tok.Kind)
		{
		case ETok::Number:
		{
			++At;
			FNode Node;
			Node.Literal = FApexHudValue::Of(Tok.Number);
			return Add(MoveTemp(Node));
		}
		case ETok::String:
		{
			++At;
			FNode Node;
			Node.Literal = FApexHudValue::Of(Tok.Text);
			return Add(MoveTemp(Node));
		}
		case ETok::LParen:
		{
			++At;
			const int32 Inner = ParseExpr(0);
			if (Inner == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			if (Peek().Kind != ETok::RParen)
			{
				Fail(FString::Printf(TEXT("expected ')' at column %d"), Peek().Pos + 1));
				return INDEX_NONE;
			}
			++At;
			return Inner;
		}
		case ETok::Ident:
			++At;
			if (Peek().Kind == ETok::LParen)
			{
				return ParseCall(Tok);
			}
			return MakeName(Tok);
		default:
			Fail(FString::Printf(TEXT("unexpected %s at column %d"), *Describe(Tok), Tok.Pos + 1));
			return INDEX_NONE;
		}
	}

	int32 MakeName(const FTok& Tok)
	{
		FNode Node;
		if (Tok.Text == TEXT("true") || Tok.Text == TEXT("false"))
		{
			Node.Literal = FApexHudValue::Of(Tok.Text == TEXT("true"));
		}
		else if (Tok.Text == TEXT("null"))
		{
			// The default FApexHudValue is none.
		}
		else if (Tok.Text == TEXT("index"))
		{
			Node.Kind = FNode::EKind::Index;
		}
		else if (Tok.Text.StartsWith(TEXT("item.")))
		{
			Node.Kind = FNode::EKind::ItemField;
			Node.Name = FName(*Tok.Text.Mid(5));
		}
		else if (Tok.Text == TEXT("item"))
		{
			Fail(TEXT("'item' needs a field: item.name"));
			return INDEX_NONE;
		}
		else
		{
			Node.Kind = FNode::EKind::Global;
			Node.Name = FName(*Tok.Text);
		}
		return Add(MoveTemp(Node));
	}

	int32 ParseCall(const FTok& NameTok)
	{
		const FFunctionInfo* Info = FindFunction(NameTok.Text);
		if (!Info)
		{
			Fail(FString::Printf(TEXT("unknown function '%s'"), *NameTok.Text));
			return INDEX_NONE;
		}
		++At; // (
		TArray<int32> Args;
		if (Peek().Kind != ETok::RParen)
		{
			while (true)
			{
				const int32 Arg = ParseExpr(0);
				if (Arg == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				Args.Add(Arg);
				if (Peek().Kind == ETok::Comma)
				{
					++At;
					continue;
				}
				break;
			}
		}
		if (Peek().Kind != ETok::RParen)
		{
			Fail(FString::Printf(TEXT("expected ')' or ',' at column %d"), Peek().Pos + 1));
			return INDEX_NONE;
		}
		++At;
		if (Args.Num() < Info->MinArgs || (Info->MaxArgs >= 0 && Args.Num() > Info->MaxArgs))
		{
			Fail(Info->MaxArgs == Info->MinArgs
				? FString::Printf(TEXT("%s() takes %d argument(s), not %d"), Info->Name, Info->MinArgs, Args.Num())
				: FString::Printf(TEXT("%s() takes at least %d argument(s), got %d"), Info->Name, Info->MinArgs, Args.Num()));
			return INDEX_NONE;
		}
		if (Info->Id == EFunction::Ramp && Args.Num() % 2 == 0)
		{
			Fail(TEXT("ramp() takes a value and then stop/colour pairs"));
			return INDEX_NONE;
		}
		FNode Node;
		Node.Kind = FNode::EKind::Call;
		Node.Function = static_cast<int32>(Info->Id);
		Node.Args = MoveTemp(Args);
		return Add(MoveTemp(Node));
	}
};

TSharedPtr<const FApexHudExpr> FApexHudExpr::Compile(const FString& InSource, FString& OutError)
{
	TSharedPtr<FApexHudExpr> Expr = MakeShared<FApexHudExpr>();
	Expr->Source = InSource;
	FApexHudExprParser Parser(*Expr);
	Expr->Root = Parser.ParseAll(InSource);
	if (Expr->Root == INDEX_NONE)
	{
		OutError = Parser.Error.IsEmpty() ? FString(TEXT("does not parse")) : Parser.Error;
		return nullptr;
	}
	return Expr;
}

TSharedPtr<const FApexHudExpr> FApexHudExpr::CompileTemplate(const FString& InSource, FString& OutError)
{
	TSharedPtr<FApexHudExpr> Expr = MakeShared<FApexHudExpr>();
	Expr->Source = InSource;

	FNode Concat;
	Concat.Kind = FNode::EKind::Concat;
	FString Literal;
	auto FlushLiteral = [&]()
	{
		if (!Literal.IsEmpty())
		{
			FNode Piece;
			Piece.Literal = FApexHudValue::Of(Literal);
			Concat.Args.Add(Expr->Nodes.Add(MoveTemp(Piece)));
			Literal.Reset();
		}
	};

	const int32 N = InSource.Len();
	for (int32 I = 0; I < N; ++I)
	{
		const TCHAR C = InSource[I];
		if (C == '{' && I + 1 < N && InSource[I + 1] == '{')
		{
			Literal.AppendChar('{');
			++I;
			continue;
		}
		if (C == '}' && I + 1 < N && InSource[I + 1] == '}')
		{
			Literal.AppendChar('}');
			++I;
			continue;
		}
		if (C == '}')
		{
			OutError = FString::Printf(TEXT("'}' at column %d closes nothing (write '}}' for a brace)"), I + 1);
			return nullptr;
		}
		if (C != '{')
		{
			Literal.AppendChar(C);
			continue;
		}
		// The hole runs to the matching brace, skipping any inside quotes.
		int32 End = I + 1;
		TCHAR Quote = 0;
		while (End < N && (Quote != 0 || InSource[End] != '}'))
		{
			const TCHAR D = InSource[End];
			if (Quote != 0 && D == Quote)
			{
				Quote = 0;
			}
			else if (Quote == 0 && (D == '\'' || D == '"'))
			{
				Quote = D;
			}
			++End;
		}
		if (End >= N)
		{
			OutError = FString::Printf(TEXT("'{' at column %d is never closed"), I + 1);
			return nullptr;
		}
		FlushLiteral();
		FApexHudExprParser Parser(*Expr);
		const int32 Hole = Parser.ParseAll(InSource.Mid(I + 1, End - I - 1));
		if (Hole == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("in {%s}: %s"), *InSource.Mid(I + 1, End - I - 1), *Parser.Error);
			return nullptr;
		}
		Concat.Args.Add(Hole);
		I = End;
	}
	FlushLiteral();

	if (Concat.Args.Num() == 1 && Expr->Nodes[Concat.Args[0]].Kind == FNode::EKind::Literal)
	{
		Expr->Root = Concat.Args[0];
	}
	else if (Concat.Args.Num() == 0)
	{
		FNode Empty;
		Empty.Literal = FApexHudValue::Of(FString());
		Expr->Root = Expr->Nodes.Add(MoveTemp(Empty));
	}
	else
	{
		Expr->Root = Expr->Nodes.Add(MoveTemp(Concat));
	}
	return Expr;
}

TSharedPtr<const FApexHudExpr> FApexHudExpr::Constant(const FApexHudValue& Value)
{
	TSharedPtr<FApexHudExpr> Expr = MakeShared<FApexHudExpr>();
	Expr->Source = Value.AsString();
	FNode Node;
	Node.Literal = Value;
	Expr->Root = Expr->Nodes.Add(MoveTemp(Node));
	return Expr;
}

bool FApexHudExpr::IsConstant() const
{
	for (const FNode& Node : Nodes)
	{
		if (Node.Kind == FNode::EKind::Global || Node.Kind == FNode::EKind::ItemField || Node.Kind == FNode::EKind::Index)
		{
			return false;
		}
	}
	return true;
}

void FApexHudExpr::CollectIdentifiers(TArray<FString>& Out) const
{
	for (const FNode& Node : Nodes)
	{
		if (Node.Kind == FNode::EKind::Global)
		{
			Out.AddUnique(Node.Name.ToString());
		}
		else if (Node.Kind == FNode::EKind::ItemField)
		{
			Out.AddUnique(TEXT("item.") + Node.Name.ToString());
		}
		else if (Node.Kind == FNode::EKind::Index)
		{
			Out.AddUnique(TEXT("index"));
		}
	}
}

TArray<FString> FApexHudExpr::FunctionNames()
{
	TArray<FString> Out;
	for (const FFunctionInfo& Info : Functions)
	{
		Out.Add(Info.Name);
	}
	return Out;
}

FApexHudValue FApexHudExpr::Evaluate(const FApexHudScope& Scope) const
{
	return Root == INDEX_NONE ? FApexHudValue() : Eval(Root, Scope);
}

FApexHudValue FApexHudExpr::Eval(int32 NodeIndex, const FApexHudScope& Scope) const
{
	const FNode& Node = Nodes[NodeIndex];
	switch (Node.Kind)
	{
	case FNode::EKind::Literal:
		return Node.Literal;

	case FNode::EKind::Global:
		if (Scope.Data)
		{
			if (const FApexHudValue* Value = Scope.Data->Values.Find(Node.Name))
			{
				return *Value;
			}
		}
		return FApexHudValue();

	case FNode::EKind::ItemField:
		if (Scope.Item)
		{
			if (const FApexHudValue* Value = Scope.Item->Find(Node.Name))
			{
				return *Value;
			}
		}
		return FApexHudValue();

	case FNode::EKind::Index:
		return Scope.Index >= 0 ? FApexHudValue::Of(Scope.Index) : FApexHudValue();

	case FNode::EKind::Concat:
	{
		FString Text;
		for (const int32 Arg : Node.Args)
		{
			Text += Eval(Arg, Scope).AsString();
		}
		return FApexHudValue::Of(Text);
	}

	case FNode::EKind::Ternary:
		return Eval(Node.Args[0], Scope).AsBool() ? Eval(Node.Args[1], Scope) : Eval(Node.Args[2], Scope);

	case FNode::EKind::Unary:
	{
		const FApexHudValue Operand = Eval(Node.Args[0], Scope);
		return static_cast<EOp>(Node.Op) == EOp::Not
			? FApexHudValue::Of(!Operand.AsBool())
			: FApexHudValue::Of(-Operand.AsNumber());
	}

	case FNode::EKind::Binary:
	{
		const EOp Op = static_cast<EOp>(Node.Op);
		// Both logic operators stop as soon as the answer is known.
		if (Op == EOp::And)
		{
			return FApexHudValue::Of(Eval(Node.Args[0], Scope).AsBool() && Eval(Node.Args[1], Scope).AsBool());
		}
		if (Op == EOp::Or)
		{
			return FApexHudValue::Of(Eval(Node.Args[0], Scope).AsBool() || Eval(Node.Args[1], Scope).AsBool());
		}
		const FApexHudValue A = Eval(Node.Args[0], Scope);
		const FApexHudValue B = Eval(Node.Args[1], Scope);
		const bool bText = A.Type == EApexHudValueType::String || B.Type == EApexHudValueType::String;
		switch (Op)
		{
		case EOp::Add:
			return bText ? FApexHudValue::Of(A.AsString() + B.AsString()) : FApexHudValue::Of(A.AsNumber() + B.AsNumber());
		case EOp::Sub:
			return FApexHudValue::Of(A.AsNumber() - B.AsNumber());
		case EOp::Mul:
			return FApexHudValue::Of(A.AsNumber() * B.AsNumber());
		case EOp::Div:
		{
			// Nothing on a HUD wants infinity: a division by zero is zero.
			const double Divisor = B.AsNumber();
			return FApexHudValue::Of(Divisor != 0.0 ? A.AsNumber() / Divisor : 0.0);
		}
		case EOp::Mod:
		{
			const double Divisor = B.AsNumber();
			return FApexHudValue::Of(Divisor != 0.0 ? FMath::Fmod(A.AsNumber(), Divisor) : 0.0);
		}
		case EOp::Eq:
			return FApexHudValue::Of(A.Equals(B));
		case EOp::Ne:
			return FApexHudValue::Of(!A.Equals(B));
		case EOp::Lt:
		case EOp::Le:
		case EOp::Gt:
		case EOp::Ge:
		{
			int32 Order;
			if (A.Type == EApexHudValueType::String && B.Type == EApexHudValueType::String)
			{
				Order = A.String.Compare(B.String, ESearchCase::CaseSensitive);
			}
			else
			{
				const double X = A.AsNumber();
				const double Y = B.AsNumber();
				Order = X < Y ? -1 : (X > Y ? 1 : 0);
			}
			const bool bResult = Op == EOp::Lt ? Order < 0 : Op == EOp::Le ? Order <= 0 : Op == EOp::Gt ? Order > 0 : Order >= 0;
			return FApexHudValue::Of(bResult);
		}
		default:
			return FApexHudValue();
		}
	}

	case FNode::EKind::Call:
	{
		const TArray<int32>& Args = Node.Args;
		auto Arg = [&](int32 I) { return Eval(Args[I], Scope); };
		auto Number = [&](int32 I) { return Eval(Args[I], Scope).AsNumber(); };
		switch (static_cast<EFunction>(Node.Function))
		{
		case EFunction::If:
			return Arg(0).AsBool() ? Arg(1) : Arg(2);
		case EFunction::Switch:
		{
			const FApexHudValue Key = Arg(0);
			int32 I = 1;
			for (; I + 1 < Args.Num(); I += 2)
			{
				if (Key.Equals(Arg(I)))
				{
					return Arg(I + 1);
				}
			}
			// An odd one out at the end is the default.
			return I < Args.Num() ? Arg(I) : FApexHudValue();
		}
		case EFunction::Has:
			return FApexHudValue::Of(!Arg(0).IsNone());
		case EFunction::Min:
		case EFunction::Max:
		{
			const bool bMin = static_cast<EFunction>(Node.Function) == EFunction::Min;
			double Best = Number(0);
			for (int32 I = 1; I < Args.Num(); ++I)
			{
				const double V = Number(I);
				Best = bMin ? FMath::Min(Best, V) : FMath::Max(Best, V);
			}
			return FApexHudValue::Of(Best);
		}
		case EFunction::Clamp:
			return FApexHudValue::Of(FMath::Clamp(Number(0), Number(1), Number(2)));
		case EFunction::Abs:
			return FApexHudValue::Of(FMath::Abs(Number(0)));
		case EFunction::Floor:
			return FApexHudValue::Of(FMath::FloorToDouble(Number(0)));
		case EFunction::Ceil:
			return FApexHudValue::Of(FMath::CeilToDouble(Number(0)));
		case EFunction::Round:
		{
			const double Scale = Args.Num() > 1 ? FMath::Pow(10.0, FMath::Clamp(Number(1), 0.0, 6.0)) : 1.0;
			return FApexHudValue::Of(FMath::RoundToDouble(Number(0) * Scale) / Scale);
		}
		case EFunction::Fmt:
		{
			// An unknown figure reads as a dash, the HUD's sign for "no data".
			const FApexHudValue Value = Arg(0);
			if (Value.IsNone())
			{
				return FApexHudValue::Of(TEXT("—"));
			}
			return FApexHudValue::Of(FixedDecimals(Value.AsNumber(), Args.Num() > 1 ? FMath::RoundToInt(Number(1)) : 0));
		}
		case EFunction::FmtTime:
		{
			const double Seconds = Number(0);
			if (Seconds <= 0.0)
			{
				return FApexHudValue::Of(TEXT("--:--.---"));
			}
			const int32 Minutes = FMath::FloorToInt(Seconds / 60.0);
			return FApexHudValue::Of(FString::Printf(TEXT("%d:%06.3f"), Minutes, Seconds - Minutes * 60.0));
		}
		case EFunction::FmtSplit:
		{
			const double Seconds = Number(0);
			return FApexHudValue::Of(Seconds > 0.0 ? FString::Printf(TEXT("%.3f"), Seconds) : FString(TEXT("--.---")));
		}
		case EFunction::FmtGap:
		{
			const double Seconds = Number(0);
			const bool bSigned = Args.Num() < 2 || Arg(1).AsBool();
			if (Seconds <= 0.0 || Seconds > 999.0)
			{
				return FApexHudValue::Of(TEXT("—"));
			}
			return FApexHudValue::Of(FString::Printf(TEXT("%s%.3f"), bSigned ? TEXT("+") : TEXT(""), Seconds));
		}
		case EFunction::FmtDelta:
		{
			const FApexHudValue Value = Arg(0);
			return FApexHudValue::Of(Value.IsNone() ? FString(TEXT("—")) : FString::Printf(TEXT("%+.3f"), Value.AsNumber()));
		}
		case EFunction::FmtClock:
		{
			// A countdown: whole seconds rounded up, so 0:00 means out of time.
			const FApexHudValue Value = Arg(0);
			if (Value.IsNone())
			{
				return FApexHudValue::Of(TEXT("--:--"));
			}
			const int64 Total = FMath::Max<int64>(0, static_cast<int64>(FMath::CeilToDouble(Value.AsNumber())));
			const int64 Hours = Total / 3600;
			const int64 Minutes = Total / 60 % 60;
			const int64 Secs = Total % 60;
			return FApexHudValue::Of(Hours > 0
				? FString::Printf(TEXT("%lld:%02lld:%02lld"), Hours, Minutes, Secs)
				: FString::Printf(TEXT("%lld:%02lld"), Minutes, Secs));
		}
		case EFunction::Upper:
			return FApexHudValue::Of(Arg(0).AsString().ToUpper());
		case EFunction::Lower:
			return FApexHudValue::Of(Arg(0).AsString().ToLower());
		case EFunction::Str:
			return FApexHudValue::Of(Arg(0).AsString());
		case EFunction::Num:
			return FApexHudValue::Of(Number(0));
		case EFunction::Mix:
			return FApexHudValue::OfColour(FMath::Lerp(ColourOf(Arg(0)), ColourOf(Arg(1)), static_cast<float>(FMath::Clamp(Number(2), 0.0, 1.0))));
		case EFunction::Ramp:
		{
			// ramp(x, x0, c0, x1, c1, ...): the colour at x along the stops, held past either end.
			const double X = Number(0);
			const int32 Stops = (Args.Num() - 1) / 2;
			double PrevX = Number(1);
			FLinearColor PrevC = ColourOf(Arg(2));
			if (X <= PrevX)
			{
				return FApexHudValue::OfColour(PrevC);
			}
			for (int32 Stop = 1; Stop < Stops; ++Stop)
			{
				const double StopX = Number(1 + Stop * 2);
				const FLinearColor StopC = ColourOf(Arg(2 + Stop * 2));
				if (X <= StopX)
				{
					const double Span = StopX - PrevX;
					const float Alpha = Span > 0.0 ? static_cast<float>((X - PrevX) / Span) : 1.0f;
					return FApexHudValue::OfColour(FMath::Lerp(PrevC, StopC, Alpha));
				}
				PrevX = StopX;
				PrevC = StopC;
			}
			return FApexHudValue::OfColour(PrevC);
		}
		case EFunction::Alpha:
		{
			FLinearColor Colour = ColourOf(Arg(0));
			Colour.A = static_cast<float>(FMath::Clamp(Number(1), 0.0, 1.0));
			return FApexHudValue::OfColour(Colour);
		}
		case EFunction::Rgb:
		{
			auto Byte = [&](int32 I) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Number(I)), 0, 255)); };
			FLinearColor Colour = FLinearColor::FromSRGBColor(FColor(Byte(0), Byte(1), Byte(2)));
			if (Args.Num() > 3)
			{
				Colour.A = static_cast<float>(FMath::Clamp(Number(3), 0.0, 1.0));
			}
			return FApexHudValue::OfColour(Colour);
		}
		default:
			return FApexHudValue();
		}
	}

	default:
		return FApexHudValue();
	}
}
