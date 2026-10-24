#include "Race/ApexLiveryExchange.h"

#include "ApexLiveryPackage.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Race/ApexCarLivery.h"

namespace ApexLiveryExchangePrivate
{
	/** One message every this many seconds at most: under the server's livery bucket (4/s). */
	constexpr double SendIntervalSeconds = 0.3;
	/** A chunk not answered in this long is asked for again, three times at most. */
	constexpr double ChunkTimeoutSeconds = 10.0;
	constexpr int32 MaxTries = 3;
	/** The cache's size at most. */
	constexpr int64 MaxCacheBytes = 64ll << 20;

	FString PackagePath(const FString& Hash)
	{
		return FPaths::Combine(UApexLiveryExchange::CacheDir(), Hash + TEXT(".apxlivery"));
	}

	bool ReadImage(const FString& Path, TArray<uint8>& Out, const TCHAR* What, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			return true;
		}
		int32 Width = 0;
		int32 Height = 0;
		if (!FFileHelper::LoadFileToArray(Out, *Path))
		{
			OutError = FString::Printf(TEXT("its %s (%s) does not read"), What, *Path);
			return false;
		}
		if (!ApexLiveryPackage::ImageSize(Out, Width, Height))
		{
			OutError = FString::Printf(TEXT("its %s (%s) is not a PNG or JPEG within %d pixels a side"), What, *Path,
				ApexLiveryPackage::MaxImageSide);
			return false;
		}
		return true;
	}

	FString ImageFile(const FString& Folder, const FString& Stem, TArrayView<const uint8> Bytes)
	{
		return FPaths::Combine(Folder, Stem + (ApexLiveryPackage::IsJpeg(Bytes) ? TEXT(".jpg") : TEXT(".png")));
	}
}

namespace ApexLiveryExchange
{
	using namespace ApexLiveryExchangePrivate;

	bool Pack(const FApexCarLivery& Livery, const FString& CarId, FApexLiveryPackage& Out, FString& OutError)
	{
		Out = FApexLiveryPackage();
		Out.Car = CarId;
		Out.Name = Livery.Name.TrimStartAndEnd().Left(ApexLiveryPackage::MaxNameChars);
		if (Livery.HasPaint())
		{
			Out.Paint = FLinearColor(Livery.Paint.R, Livery.Paint.G, Livery.Paint.B, 1.0f);
		}
		if (Livery.Accent.A > 0.0f)
		{
			Out.Accent = FLinearColor(Livery.Accent.R, Livery.Accent.G, Livery.Accent.B, 1.0f);
		}
		Out.Metallic = Livery.PaintMetallic >= 0.0f ? FMath::Min(Livery.PaintMetallic, 1.0f) : -1.0f;
		if (!ReadImage(Livery.RuntimeLogo, Out.Logo, TEXT("logo"), OutError)
			|| !ReadImage(Livery.RuntimeSkin, Out.Skin, TEXT("skin"), OutError))
		{
			return false;
		}
		for (const FApexLiveryTexture& Texture : Livery.RuntimeTextures)
		{
			TPair<FString, TArray<uint8>>& Entry = Out.Textures.AddDefaulted_GetRef();
			Entry.Key = Texture.Slot.ToString();
			if (!ReadImage(Texture.RuntimeTexture, Entry.Value, *Entry.Key, OutError))
			{
				return false;
			}
		}
		const int32 Bytes = ApexLiveryPackage::Encode(Out).Num();
		return ApexLiveryPackage::Check(Out, Bytes, OutError);
	}

	bool Unpack(const FApexLiveryPackage& Package, const FString& Folder, FApexCarLivery& Out)
	{
		Out = FApexCarLivery();
		Out.Name = Package.Name;
		Out.Paint = Package.Paint;
		Out.Accent = Package.Accent;
		Out.PaintMetallic = Package.Metallic;
		IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
		auto Write = [&Folder](const FString& Stem, const TArray<uint8>& Bytes, FString& OutPath)
		{
			if (Bytes.IsEmpty())
			{
				return true;
			}
			OutPath = ImageFile(Folder, Stem, Bytes);
			return FFileHelper::SaveArrayToFile(Bytes, *OutPath);
		};
		bool bOk = Write(TEXT("logo"), Package.Logo, Out.RuntimeLogo) && Write(TEXT("skin"), Package.Skin, Out.RuntimeSkin);
		for (int32 i = 0; bOk && i < Package.Textures.Num(); ++i)
		{
			FApexLiveryTexture& Texture = Out.RuntimeTextures.AddDefaulted_GetRef();
			Texture.Slot = FName(*Package.Textures[i].Key);
			bOk = Write(FString::Printf(TEXT("texture_%d"), i), Package.Textures[i].Value, Texture.RuntimeTexture);
		}
		return bOk;
	}

	bool IsHash(const FString& Hash)
	{
		if (Hash.Len() != 64)
		{
			return false;
		}
		for (const TCHAR Char : Hash)
		{
			if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	TArray<FString> ToPrune(TArray<TPair<FString, int64>> FilesOldestFirst, int64 MaxBytes)
	{
		int64 Total = 0;
		for (const TPair<FString, int64>& File : FilesOldestFirst)
		{
			Total += File.Value;
		}
		TArray<FString> Prune;
		for (const TPair<FString, int64>& File : FilesOldestFirst)
		{
			if (Total <= MaxBytes)
			{
				break;
			}
			Prune.Add(File.Key);
			Total -= File.Value;
		}
		return Prune;
	}
}

FString UApexLiveryExchange::CacheDir()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ApexSim"), TEXT("Liveries"));
}

void UApexLiveryExchange::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		ChunkHandle = Net->OnLiveryChunk.AddUObject(this, &UApexLiveryExchange::HandleChunk);
		Net->OnSessionJoined.AddDynamic(this, &UApexLiveryExchange::HandleSessionJoined);
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UApexLiveryExchange::Tick), 0.0f);

	// The cache under its size, the oldest packages (and their images) first.
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(CacheDir(), TEXT("*.apxlivery")), true, false);
	TArray<TTuple<FDateTime, FString, int64>> Dated;
	for (const FString& File : Files)
	{
		const FString Path = FPaths::Combine(CacheDir(), File);
		Dated.Emplace(IFileManager::Get().GetTimeStamp(*Path), Path, IFileManager::Get().FileSize(*Path));
	}
	Dated.Sort([](const auto& A, const auto& B) { return A.template Get<0>() < B.template Get<0>(); });
	TArray<TPair<FString, int64>> OldestFirst;
	for (const auto& Entry : Dated)
	{
		OldestFirst.Emplace(Entry.template Get<1>(), Entry.template Get<2>());
	}
	for (const FString& Path : ApexLiveryExchange::ToPrune(MoveTemp(OldestFirst), ApexLiveryExchangePrivate::MaxCacheBytes))
	{
		IFileManager::Get().Delete(*Path);
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(CacheDir(), FPaths::GetBaseFilename(Path)), false, true);
	}
}

void UApexLiveryExchange::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnLiveryChunk.Remove(ChunkHandle);
		Net->OnSessionJoined.RemoveDynamic(this, &UApexLiveryExchange::HandleSessionJoined);
	}
	Super::Deinitialize();
}

const FApexCarLivery* UApexLiveryExchange::FindWorn(const FString& Hash, const FString& CarId) const
{
	const FReady* Found = Hash.IsEmpty() ? nullptr : Ready.Find(Hash);
	return Found && Found->Car.Equals(CarId, ESearchCase::IgnoreCase) ? &Found->Livery : nullptr;
}

void UApexLiveryExchange::HandleSessionJoined(const FString& SessionId, int32 GridPosition)
{
	// A join (a rejoin too) is a session the livery goes to again.
	UploadedKey.Reset();
}

void UApexLiveryExchange::StartUpload(const FString& CarId, int32 Livery)
{
	UploadChunks.Reset();
	UploadNext = 0;
	UploadHash.Reset();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	FApexCarCatalogRow Row;
	const FApexCarLivery* Own = Flow && Flow->GetCarCatalogRow(CarId, Row) ? ApexLivery::Find(Row, Livery) : nullptr;
	if (!Own || !Own->bOwn)
	{
		return;
	}
	FApexLiveryPackage Package;
	FString Why;
	if (!ApexLiveryExchange::Pack(*Own, CarId, Package, Why))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Own livery %s stays on this client: %s"), *Own->Name, *Why);
		if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
		{
			Net->OnServerNotice.Broadcast(FString::Printf(TEXT("Your livery %s is not sent to the others: %s"), *Own->Name, *Why));
		}
		return;
	}
	const TArray<uint8> Bytes = ApexLiveryPackage::Encode(Package);
	UploadHash = ApexLiveryPackage::Hash(Bytes);
	for (int32 Start = 0; Start < Bytes.Num(); Start += ApexLiveryPackage::ChunkBytes)
	{
		UploadChunks.Emplace(Bytes.GetData() + Start, FMath::Min(ApexLiveryPackage::ChunkBytes, Bytes.Num() - Start));
	}
	// The player's own car is painted from the folder; the package is
	// cached as well so a replay or a later session finds it.
	Accept(UploadHash, Bytes, /*bSave*/ true);
	UE_LOG(LogApexSim, Log, TEXT("Own livery %s goes to the server: %d bytes in %d chunk(s), %s"), *Package.Name, Bytes.Num(),
		UploadChunks.Num(), *UploadHash);
}

bool UApexLiveryExchange::Accept(const FString& Hash, TArrayView<const uint8> Bytes, bool bSave)
{
	FApexLiveryPackage Package;
	FString Why;
	if (ApexLiveryPackage::Hash(Bytes) != Hash || !ApexLiveryPackage::Decode(Bytes, Package, Why))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Livery %s refused: %s"), *Hash, Why.IsEmpty() ? TEXT("its bytes do not match its hash") : *Why);
		return false;
	}
	if (bSave)
	{
		FFileHelper::SaveArrayToFile(Bytes, *ApexLiveryExchangePrivate::PackagePath(Hash));
	}
	FReady& Entry = Ready.FindOrAdd(Hash);
	Entry.Car = Package.Car;
	if (!ApexLiveryExchange::Unpack(Package, FPaths::Combine(CacheDir(), Hash), Entry.Livery))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Livery %s could not be written under %s"), *Hash, *CacheDir());
		Ready.Remove(Hash);
		return false;
	}
	return true;
}

void UApexLiveryExchange::HandleChunk(const FApexLiveryChunk& Chunk)
{
	if (!bFetchInFlight || Chunk.Hash != FetchHash || Chunk.Chunk != FetchNext)
	{
		return;
	}
	bFetchInFlight = false;
	const int32 MaxChunks = ApexLiveryPackage::ChunkCount(ApexLiveryPackage::MaxBytes);
	if (Chunk.Chunks <= 0 || Chunk.Chunks > MaxChunks || (FetchChunks > 0 && Chunk.Chunks != FetchChunks)
		|| Chunk.Bytes.Num() > ApexLiveryPackage::ChunkBytes)
	{
		UE_LOG(LogApexSim, Log, TEXT("Livery %s is not on the server: the car keeps its works livery"), *FetchHash);
		Failed.Add(FetchHash);
		FetchHash.Reset();
		return;
	}
	FetchChunks = Chunk.Chunks;
	FetchBytes.Append(Chunk.Bytes);
	FetchTries = 0;
	if (++FetchNext < FetchChunks)
	{
		return;
	}
	const FString Hash = FetchHash;
	FetchHash.Reset();
	if (Accept(Hash, FetchBytes, /*bSave*/ true))
	{
		UE_LOG(LogApexSim, Log, TEXT("Livery %s fetched: %d bytes"), *Hash, FetchBytes.Num());
		OnLiveryReady.Broadcast();
	}
	else
	{
		Failed.Add(Hash);
	}
	FetchBytes.Reset();
}

bool UApexLiveryExchange::Tick(float DeltaSeconds)
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || !Net->IsInSession())
	{
		UploadedKey.Reset();
		UploadChunks.Reset();
		UploadHash.Reset();
		FetchHash.Reset();
		bFetchInFlight = false;
		return true;
	}
	const FApexSessionRoster& Roster = Net->GetSessionRoster();
	const int32 LocalCar = Net->GetLocalCarIndex();

	// The player's own pick, once per session, car and pick.
	for (const FApexRosterEntry& Entry : Roster.Entries)
	{
		if (Entry.CarIndex == LocalCar && !Entry.bIsAi && !Entry.CarConfigId.IsEmpty())
		{
			const FString Key = FString::Printf(TEXT("%s|%s|%d|%d"), *Net->GetCurrentSessionId(), *Entry.CarConfigId,
				Net->GetPendingLivery(), Net->IsPendingLiveryOwn() ? 1 : 0);
			if (Key != UploadedKey)
			{
				UploadedKey = Key;
				UploadChunks.Reset();
				UploadHash.Reset();
				if (Net->IsPendingLiveryOwn())
				{
					StartUpload(Entry.CarConfigId, Net->GetPendingLivery());
				}
			}
			break;
		}
	}

	// The others' own liveries: from the cache, else fetched one at a time.
	bool bReadyNow = false;
	for (const FApexRosterEntry& Entry : Roster.Entries)
	{
		const FString& Hash = Entry.OwnLivery;
		if (Entry.CarIndex == LocalCar || !ApexLiveryExchange::IsHash(Hash) || Ready.Contains(Hash) || Failed.Contains(Hash)
			|| Hash == FetchHash)
		{
			continue;
		}
		TArray<uint8> Cached;
		bool bTried = false;
		CacheTried.Add(Hash, &bTried);
		if (!bTried && FFileHelper::LoadFileToArray(Cached, *ApexLiveryExchangePrivate::PackagePath(Hash), FILEREAD_Silent))
		{
			if (Accept(Hash, Cached, /*bSave*/ false))
			{
				bReadyNow = true;
				continue;
			}
			IFileManager::Get().Delete(*ApexLiveryExchangePrivate::PackagePath(Hash));
		}
		if (FetchHash.IsEmpty())
		{
			FetchHash = Hash;
			FetchChunks = 0;
			FetchNext = 0;
			FetchTries = 0;
			FetchBytes.Reset();
			bFetchInFlight = false;
		}
	}
	if (bReadyNow)
	{
		OnLiveryReady.Broadcast();
	}

	// One message at a time, under the server's rate.
	const double Now = FPlatformTime::Seconds();
	if (Now - LastSendSeconds < ApexLiveryExchangePrivate::SendIntervalSeconds)
	{
		return true;
	}
	if (UploadNext < UploadChunks.Num())
	{
		Net->UploadLiveryChunk(UploadHash, UploadNext, UploadChunks.Num(), UploadChunks[UploadNext]);
		++UploadNext;
		LastSendSeconds = Now;
		return true;
	}
	if (!FetchHash.IsEmpty() && bFetchInFlight && Now - FetchAskedAt > ApexLiveryExchangePrivate::ChunkTimeoutSeconds)
	{
		bFetchInFlight = false;
		if (++FetchTries >= ApexLiveryExchangePrivate::MaxTries)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Livery %s did not come: the car keeps its works livery"), *FetchHash);
			Failed.Add(FetchHash);
			FetchHash.Reset();
		}
	}
	if (!FetchHash.IsEmpty() && !bFetchInFlight)
	{
		Net->RequestLiveryChunk(FetchHash, FetchNext);
		bFetchInFlight = true;
		FetchAskedAt = Now;
		LastSendSeconds = Now;
	}
	return true;
}
