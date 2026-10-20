#pragma once

#include "CoreMinimal.h"
#include "apexsim_physics.h"

struct FApexPlayerInput;
struct FApexOwnCarSnapshot;
struct FApexDriverFeedback;

/**
 * The server's simulation, linked into the client as a static library
 * (server/ffi, the ApexPhysics third-party module): a thin owner of its C
 * handles for the prediction of the local car, and the showcase render
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

		/**
		 * The same from the track's physics bundle (the YAML and the sidecars as
		 * the server sends them, apx_track_load_bundle), refused unless its
		 * checksum is ExpectedCrc (SessionJoined.Physics.BundleCrc).
		 */
		static TUniquePtr<FTrack> LoadBundle(TArrayView<const uint8> Bundle, uint32 ExpectedCrc, uint8 RoadContact,
			TArrayView<const uint8> Conditions, FString& OutError);

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

	/**
	 * A watched hotlap simulated here (apx_watch_*): the session a server
	 * makes of the same CreateSession, ticked on the caller's thread, its
	 * spectator stream taken as framed records. Freed, it keeps its ladder
	 * level and ghost in its records folder.
	 */
	class APEXSIM_API FWatch : public FNoncopyable
	{
	public:
		~FWatch();

		/**
		 * From the codec's CreateSession bytes, on the track YAML in the car
		 * (folder, id or name under CarsDir); RecordsDir empty keeps nothing.
		 * TickRateHz 0 is the simulation's own rate (the stream's header names it).
		 */
		static TUniquePtr<FWatch> Start(TArrayView<const uint8> CreateSession, const FString& TrackYaml, const FString& CarsDir,
			const FString& Car, const FString& RecordsDir, uint8 RoadContact, int32 TickRateHz, uint64 Seed, FString& OutError);

		bool Tick(uint32 Ticks);
		/** The framed records since the last take, appended to Out. */
		bool Take(TArray<uint8>& Out);
		/** The TrackCorners message (MessagePack), empty for a track with none. */
		bool Corners(TArray<uint8>& Out) const;

	private:
		FWatch() = default;
		ApxWatch* Watch = nullptr;
	};

	/** The library's last failure on this thread. */
	APEXSIM_API FString LastError();

	/**
	 * The bundle checksum of the YAML and the sidecars beside it (the road
	 * mesh only for RoadContact 1): what the server's bundle of the same files
	 * carries (apx_track_files_crc).
	 */
	APEXSIM_API bool TrackFilesCrc(const FString& YamlPath, uint8 RoadContact, uint32& OutCrc, FString& OutError);

	/** The bundle the server would build of those files, and its checksum (apx_track_bundle_build; tests and tools). */
	APEXSIM_API bool BuildTrackBundle(const FString& YamlPath, uint8 RoadContact, TArray<uint8>& OutBundle, uint32& OutCrc,
		FString& OutError);

	/** Every showcase the showcase.yml at ListPath names (`<track>.<class>.<variant>`), in its order. */
	APEXSIM_API bool ShowcaseNames(const FString& ListPath, TArray<FString>& OutNames, FString& OutError);

	/** How a long piece of the library's work (a showcase, a guide) ended. */
	enum class EJobResult : uint8
	{
		Done,
		Cancelled,
		Failed,
	};

	/**
	 * Render showcase `Name` of the list into the stream file OutPath
	 * (apx_showcase_render): the file `apexsim-replay showcase` writes from
	 * the same list, tracks and cars, byte for byte. A whole race: seconds of
	 * work, so call it off the game thread. *Cancel non-zero stops it with
	 * nothing written.
	 */
	APEXSIM_API EJobResult RenderShowcase(const FString& ListPath, const FString& TracksDir, const FString& CarsDir,
		const FString& Name, const FString& OutPath, const volatile uint8* Cancel, FString& OutError);

	/**
	 * Make the track guide of `Class` for the track YAML into OutDir
	 * (apx_guide_build): `<Stem>.<Class>.guide.json` and its recording, as
	 * `apexsim-replay guide` writes them, byte for byte. Seconds of work:
	 * call it off the game thread.
	 */
	APEXSIM_API EJobResult BuildGuide(const FString& TrackYaml, const FString& CarsDir, const FString& Class,
		const FString& OutDir, const volatile uint8* Cancel, FString& OutError);

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
