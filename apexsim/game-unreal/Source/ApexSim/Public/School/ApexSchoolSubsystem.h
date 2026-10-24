#pragma once

#include "CoreMinimal.h"
#include "ApexSimJob.h"
#include "Async/Future.h"
#include "School/ApexSchool.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "ApexSchoolSubsystem.generated.h"

class AApexSchoolMarkerActor;
struct FApexTelemetryFrame;

/** Where a circuit's tests are (UApexSchoolSubsystem::Prepare). */
UENUM()
enum class EApexSchoolPrep : uint8
{
	None,
	/** Making the circuit's track guide of the car's class (the game's own simulation). */
	Making,
	/** Timing the guide car through the corner-time test's corner, from the guide's recording. */
	Measuring,
	Ready,
	Failed,
};

/** Where a test in its session is. */
UENUM()
enum class EApexSchoolStage : uint8
{
	/** No school session. */
	Off,
	/** In the garage, the test card up. */
	Briefing,
	/** Out on track, the try being judged. */
	Driving,
	/** Judged: the result card (up once the car is back in the garage). */
	Result,
};

/**
 * The driving school's licence tests (School/ApexSchool.h): makes a circuit's
 * tests from its track guide of the pending car's class (making the guide
 * first when it is missing or stale, as the guide screen does), runs a test
 * in a hotlap session the client creates for itself (the car goes out with
 * HotlapRelocate Track, back with Garage), judges it on the local car's
 * telemetry and files the medal in the driver profile.
 *
 * The test and result cards are UApexSchoolCardWidget, shown by the root in
 * place of the hotlap garage; the menu is UApexSchoolWidget. The marks on the
 * road (board, stop line, corner gates) are AApexSchoolMarkerActor.
 *
 * Console: `apexsim.school.Run <Stem> <test id> [car name]` (prepare, then
 * start once ready), `.Go` (START / RETRY on the card), `.Next`, `.Leave`.
 */
UCLASS()
class APEXSIM_API UApexSchoolSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickableWhenPaused() const override { return true; }

	// --- A circuit's tests -----------------------------------------------------

	/** Make the tests of a circuit for the pending car (its class's guide). Again only when the circuit, car or guide changed. */
	void Prepare(const FString& TrackId);
	EApexSchoolPrep GetPrep() const { return Prep; }
	/** What the screen says while preparing, or why it failed. */
	const FString& GetPrepMessage() const { return PrepMessage; }
	const FString& GetTrackId() const { return TrackId; }
	const FString& GetCarId() const { return CarId; }
	const TArray<ApexSchool::FTest>& GetTests() const { return Tests; }
	/** The profile's best at one of the prepared tests, or null. */
	const FApexSchoolMedal* GetBest(const FString& TestId) const;

	/** Create the session for test `Index` of the prepared circuit. False with why. */
	bool StartTest(int32 Index, FString& OutError);
	/** Prepare `TrackId` and start the test `TestId` as soon as it is ready (the unattended path). */
	void RequestRun(const FString& InTrackId, const FString& TestId);

	// --- In the session ---------------------------------------------------------

	/** The session the player is in is the school's. */
	bool IsSchoolSession() const;
	EApexSchoolStage GetStage() const { return Stage; }
	const ApexSchool::FTest* GetActiveTest() const { return Tests.IsValidIndex(ActiveIndex) ? &Tests[ActiveIndex] : nullptr; }
	const ApexSchool::FResult& GetResult() const { return Judge.GetResult(); }
	bool IsNewBest() const { return bNewBest; }
	/** A try given up ("Back in the garage"), shown on the next test card; empty otherwise. */
	const FString& GetNotice() const { return Notice; }

	/** START / RETRY: out onto the run-up, the try judged from there. */
	void Go();
	/** The circuit's next test, its card. */
	void NextTest();
	/** Leave the session for the School screen. */
	void LeaveToSchool();
	/** The session was left to go back to the School screen: true once. */
	bool ConsumeReturnToSchool();

	/** Anything a card or the screen shows changed. */
	FSimpleMulticastDelegate OnChanged;

private:
	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);
	UFUNCTION()
	void HandleSessionLeft();

	/** Find the guide of the class and go on from it: make it, measure it, or fail. */
	void ContinuePrepare();
	void FailPrepare(const FString& Why);
	/** Whether this machine can make a guide of a circuit (the physics library and the track's YAML). */
	bool CanMakeGuide(const FString& Stem) const;
	/** The lap length the session reports, else the guide's. */
	float LapLengthM() const;
	/** The marks for the active test on the road, once the track is up. */
	void UpdateMarker();
	void DestroyMarker();
	void Changed() { OnChanged.Broadcast(); }

	EApexSchoolPrep Prep = EApexSchoolPrep::None;
	FString PrepMessage;
	FString TrackId;
	FString Stem;
	FString CarId;
	FString CarClass;
	float GuideLapM = 0.0f;
	TArray<ApexSchool::FTest> Tests;
	/** Made once per Prepare: a guide that comes out unusable is not made again. */
	bool bMadeGuide = false;

	FApexSimJob MakeJob;
	FString SimTracksDir;
	FString SimCarsDir;
	/** The guide car's seconds through the corner-time test's corner; 0 when it could not be timed. */
	TFuture<float> Measuring;

	/** RequestRun's test, started when the tests are ready. */
	FString PendingTestId;

	bool bSession = false;
	bool bReturnToSchool = false;
	EApexSchoolStage Stage = EApexSchoolStage::Off;
	int32 ActiveIndex = INDEX_NONE;
	ApexSchool::FJudge Judge;
	bool bNewBest = false;
	FString Notice;
	/** Seconds until the car is sent back to the garage after a try; negative for none. */
	float ParkIn = -1.0f;
	/** The last local car's garage flag. */
	bool bCarInGarage = true;

	UPROPERTY(Transient)
	TObjectPtr<AApexSchoolMarkerActor> Marker;
	int32 MarkerTest = INDEX_NONE;
};
