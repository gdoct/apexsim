#pragma once

#include "CoreMinimal.h"
#include "apexsim_physics.h"

struct FApexPlayerInput;
struct FApexOwnCarSnapshot;
struct FApexDriverFeedback;

/**
 * The server's car step, linked into the client as a static library
 * (server/ffi, the ApexPhysics third-party module): a thin owner of its C
 * handles for the prediction of the local car
 * (docs/proposals/client-prediction.md, docs/game/client.md).
 *
 * A client built without the library (WITH_APEX_PHYSICS 0, the .lib not
 * built by scripts/build_physics_lib.ps1) compiles the same calls: every one
 * fails with a reason, and IsAvailable() says why.
 *
 * Everything is in the SERVER frame and units. A track and the cars stepped
 * on it belong to one thread at a time (each step changes the track's wind).
 */
namespace ApexPhysics
{
	/** The library is linked and speaks this header's ABI; OutWhy says why not. */
	APEXSIM_API bool IsAvailable(FString* OutWhy = nullptr);

	class APEXSIM_API FTrack : public FNoncopyable
	{
	public:
		~FTrack();

		/**
		 * The track YAML and the sidecars beside it, with the session's
		 * conditions (SessionJoined's bytes) baked in as the server bakes its
		 * session copy. RoadContact: 0 the centerline, 1 the road mesh.
		 */
		static TUniquePtr<FTrack> Load(const FString& YamlPath, uint8 RoadContact, TArrayView<const uint8> Conditions,
			FString& OutError);

		/** Holds the road state to the server's from a RoadState message's bytes (apx_track_road_rows). */
		bool SetRoadRows(uint32 FirstCell, TArrayView<const uint8> Rows, TArrayView<const uint8> SnowRows,
			TArrayView<const uint8> LaneSnow, FString& OutError);

		ApxTrack* Handle() const { return Track; }

	private:
		FTrack() = default;
		ApxTrack* Track = nullptr;
	};

	class APEXSIM_API FCar : public FNoncopyable
	{
	public:
		~FCar();

		/** A car from its car.toml text and setup clicks (CarSetup order), on a grid slot (1-based). */
		static TUniquePtr<FCar> Create(const FTrack& Track, TArrayView<const uint8> CarToml, TArrayView<const int8> Clicks,
			int32 GridSlot, FString& OutError);

		/** One tick of `Tick` at the session's rate, as the server steps a car alone. */
		bool Step(FTrack& Track, const ApxInput& Input, uint32 Tick, int32 TickRateHz);
		bool Pose(ApxCarPose& Out) const;
		bool Snapshot(ApxCarSnapshot& Out) const;
		/** Lays a snapshot over the state; the slow state is kept. */
		bool ApplySnapshot(const ApxCarSnapshot& Snapshot);
		/** The full state blob ("APXC"), as the server sends it. */
		bool Save(TArray<uint8>& Out) const;
		bool Load(TArrayView<const uint8> Blob);
		/**
		 * What the driver should feel over the steps since the last take: the
		 * server's own force-feedback collector (apx_car_feedback_take). Taken
		 * after every step, it is that tick's.
		 */
		bool TakeFeedback(ApxFeedback& Out);

	private:
		FCar() = default;
		ApxCar* Car = nullptr;
	};

	/** The library's last failure on this thread. */
	APEXSIM_API FString LastError();

	/** A player input as the server reads it (sanitised by the step itself). */
	APEXSIM_API ApxInput ToInput(const FApexPlayerInput& Input);

	/** The wire's snapshot as the library lays it. */
	APEXSIM_API ApxCarSnapshot ToSnapshot(const FApexOwnCarSnapshot& Snapshot);

	/**
	 * The library's feedback as the wire's DriverFeedback decodes (the codec's
	 * units: flat spots 0..1, surfaces clamped), at `Tick`: what the force
	 * feedback, the road sound and the pad read.
	 */
	APEXSIM_API FApexDriverFeedback ToFeedback(const ApxFeedback& Feedback, int64 Tick);
}
