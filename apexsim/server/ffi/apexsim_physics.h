/*
 * apexsim_physics.h - the server's simulation behind a C API: the car step
 * for the client's prediction of its own car
 * (docs/proposals/client-prediction.md), a seeded race nobody drives, and a
 * showcase rendered from showcase.yml (docs/game/spectator.md).
 *
 * Built from server/ffi (crate apexsim-physics) as a static library:
 *   cd server/ffi && cargo build --release
 *   -> <target>/release/apexsim_physics.lib (Windows) / libapexsim_physics.a
 * See docs/building.md for the system libraries it links against.
 *
 * Hand-written: it mirrors server/ffi/src/lib.rs, whose tests pin the struct
 * sizes asserted below. Change both together and bump APX_ABI_VERSION.
 *
 * Every function returns an ApxStatus (APX_OK on success) and never lets a
 * panic out; apx_last_error() names the last failure on the calling thread.
 * A track handle is changed by each step (the tick's wind): a track and the
 * cars stepped on it belong to one thread at a time.
 */
#ifndef APEXSIM_PHYSICS_H
#define APEXSIM_PHYSICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APX_ABI_VERSION 5u

typedef int32_t ApxStatus;
#define APX_OK 0
#define APX_ERR_NULL 1       /* a required pointer was null */
#define APX_ERR_UTF8 2       /* a string was not UTF-8 */
#define APX_ERR_TRACK 3      /* the track YAML or a sidecar did not load */
#define APX_ERR_CAR 4        /* the car.toml did not parse or validate */
#define APX_ERR_CONDITIONS 5 /* the session conditions did not decode */
#define APX_ERR_INVALID 6    /* an argument out of range */
#define APX_ERR_STATE 7      /* a state blob of another version or car */
#define APX_ERR_BUFFER 8     /* buffer too small; the size needed was written */
#define APX_ERR_PANIC 9      /* the library panicked; the handle may be bad */
#define APX_ERR_CANCELLED 10 /* the caller's cancel flag was raised */
#define APX_ERR_IO 11        /* a file could not be read or written */

typedef struct ApxTrack ApxTrack;
typedef struct ApxCar ApxCar;

/* The driver's controls for one tick (ClientMessage::PlayerInput). */
typedef struct ApxInput {
    float throttle;     /* 0..1 */
    float brake;        /* 0..1 */
    float steering;     /* -1 right .. 1 left */
    float clutch;       /* 0..1, read when has_clutch */
    int8_t gear;        /* -1 reverse, 0 neutral, 1.. forward; read when has_gear */
    uint8_t has_gear;
    uint8_t has_clutch;
    uint8_t drs;
    int8_t headlights;  /* -1 left to the conditions, 0 off, 1 on */
    uint8_t flash;
    int8_t ers_mode;    /* -1 keeps the car's mode, else hybrid::ErsMode */
    uint8_t ers_boost;
    uint8_t handbrake;
} ApxInput;

#define APX_POSE_ON_TRACK 1u
#define APX_POSE_AIRBORNE 2u
#define APX_POSE_COLLIDING 4u
#define APX_POSE_DRS_OPEN 8u

/* The car as a renderer reads it, in the server frame (metres, +X along the
 * track at the line, +Y left, +Z up, yaw counter-clockwise). */
typedef struct ApxCarPose {
    float pos_x, pos_y, pos_z;
    float yaw_rad, pitch_rad, roll_rad;
    float vel_x, vel_y, vel_z;
    float speed_mps;
    float angular_vel_yaw, angular_vel_pitch, angular_vel_roll;
    float steering_input, throttle_input, brake_input;
    float engine_rpm;
    float track_progress_m;
    float lateral_offset_m;
    float wheel_angular_vel[4]; /* FL FR RL RR, rad/s */
    uint16_t current_lap;
    int8_t gear;
    uint8_t flags;              /* APX_POSE_* */
} ApxCarPose;

#define APX_SNAP_ON_TRACK 1u
#define APX_SNAP_AIRBORNE 2u
#define APX_SNAP_COLLIDING 4u
#define APX_SNAP_DRS_OPEN 8u
#define APX_SNAP_WHEELS_OFF_TRACK 16u
#define APX_NO_INDEX 0xFFFFFFFFu

/* What the car step carries from one tick to the next: the server's
 * DriverFeedback.own_car (sim/src/own_car.rs OwnCarSnapshot) without its
 * tick and input number. Server frame and units; per-wheel FL FR RL RR. */
typedef struct ApxCarSnapshot {
    float pos[3];
    float attitude[3];          /* yaw, pitch, roll, rad */
    float vel[3];
    float speed_mps;
    float angular_vel[3];       /* yaw, pitch, roll rates, rad/s */
    float wheel_angular_vel[4];
    float wheel_load_n[4];
    float suspension_travel_m[4];
    float suspension_velocity_mps[4];
    float g_forces[3];          /* lateral, longitudinal, vertical */
    float vertical_accel_mps2;
    float ground_slope[2];
    float porpoise[2];          /* phase, amplitude */
    float controls[4];          /* throttle, brake, steering, clutch as applied */
    float engine_rpm;
    float turbo_spool;
    float track_progress;
    float lateral_offset_m;
    uint32_t centerline_idx;    /* APX_NO_INDEX for none */
    uint16_t auto_shift_hold_ticks;
    uint16_t auto_reverse_ticks;
    uint16_t current_lap;
    int8_t gear;
    uint8_t flags;              /* APX_SNAP_* */
} ApxCarSnapshot;

#define APX_FEEDBACK_SAMPLES 32

/* What the driver should feel over the steps since the last
 * apx_car_feedback_take: the server's DriverFeedback (sim/src/feedback.rs)
 * without its tick and own-car fields, from the same collector, in the
 * wire's units. Per-wheel FL FR RL RR; the server's steering sign (positive
 * left). */
typedef struct ApxFeedback {
    float steer_torque[APX_FEEDBACK_SAMPLES]; /* one per step, oldest first */
    uint32_t steer_samples;
    float slip_ratio[4];        /* multiples of the peak, largest magnitude */
    float slip_angle[4];
    float suspension_mps[4];
    float impact_mps;
    float steer_kick;
    float steer_input;          /* the input the newest torque was worked out at */
    float steer_stiffness;
    float front_load;
    uint8_t surface[4];         /* 0 road, 1 curb, 2 off, the roughest */
    uint8_t flat_spot[4];       /* percent of the worst */
    uint8_t abs_active;
    uint8_t tc_active;
    uint8_t reserved[2];
} ApxFeedback;

#ifdef __cplusplus
static_assert(sizeof(ApxInput) == 28, "ApxInput layout");
static_assert(sizeof(ApxCarPose) == 96, "ApxCarPose layout");
static_assert(sizeof(ApxCarSnapshot) == 192, "ApxCarSnapshot layout");
static_assert(sizeof(ApxFeedback) == 212, "ApxFeedback layout");
#else /* C89 and MSVC's C have no _Static_assert by default */
typedef char apx_input_layout[(sizeof(ApxInput) == 28) ? 1 : -1];
typedef char apx_car_pose_layout[(sizeof(ApxCarPose) == 96) ? 1 : -1];
typedef char apx_car_snapshot_layout[(sizeof(ApxCarSnapshot) == 192) ? 1 : -1];
typedef char apx_feedback_layout[(sizeof(ApxFeedback) == 212) ? 1 : -1];
#endif

/* APX_ABI_VERSION of the linked library: check it before any other call. */
uint32_t apx_abi_version(void);

/* The version of the blobs apx_car_state_save writes. */
uint32_t apx_state_blob_version(void);

/* The last failure's message on this thread (UTF-8, empty after a success),
 * valid until the next call on the thread. */
const char *apx_last_error(void);

/* Load a track from its YAML and the sidecars beside it, with the session's
 * conditions baked in as the server bakes a session's own copy.
 * road_contact: 0 the centerline, 1 the road mesh (the server's
 * [physics] road_contact). conditions: SessionConditions as MessagePack
 * with named fields (as SessionJoined carries them, resolved), or NULL with
 * conditions_len 0 for the defaults. */
ApxStatus apx_track_load(const char *yaml_path, uint8_t road_contact,
                         const uint8_t *conditions, size_t conditions_len,
                         ApxTrack **out);
void apx_track_free(ApxTrack *track);

/* Load a track from its physics bundle (sim/src/track_bundle.rs: the YAML
 * and the sidecars, as the server sends it in TrackBundleChunks), loaded as
 * apx_track_load loads them from disk, the conditions baked in. Refused
 * (APX_ERR_TRACK, nothing loaded) unless the bundle's checksum is
 * expected_crc (SessionJoined.Physics.bundle_crc). */
ApxStatus apx_track_load_bundle(const uint8_t *bundle, size_t bundle_len,
                                uint32_t expected_crc, uint8_t road_contact,
                                const uint8_t *conditions, size_t conditions_len,
                                ApxTrack **out);

/* The bundle checksum of the YAML at yaml_path and the sidecars beside it
 * (the road mesh only for road_contact 1): what the server's bundle of the
 * same files carries, so a client whose copy matches needs no bundle. */
ApxStatus apx_track_files_crc(const char *yaml_path, uint8_t road_contact,
                              uint32_t *out);

/* The physics bundle of the YAML at yaml_path and the sidecars beside it,
 * as the server builds it (for tests and tools). *written gets its size and
 * *crc its checksum; with a NULL buf or a short capacity nothing is copied
 * and APX_ERR_BUFFER is returned. */
ApxStatus apx_track_bundle_build(const char *yaml_path, uint8_t road_contact,
                                 uint8_t *buf, size_t capacity, size_t *written,
                                 uint32_t *crc);

/* A car from its car.toml text and up to KNOB_COUNT (31) setup clicks in
 * CarSetup order (NULL/0 for stock), clamped as the server clamps them,
 * standing on grid slot grid_position (1-based; 0 the first slot). */
ApxStatus apx_car_create(const ApxTrack *track, const uint8_t *car_toml,
                         size_t car_toml_len, const int8_t *setup_clicks,
                         size_t n_clicks, uint8_t grid_position, ApxCar **out);
void apx_car_free(ApxCar *car);

/* Step the car one tick as the server steps a car alone on the track: the
 * car model, its place on the lap, the walls, with the wind of `tick`.
 * tick: the session tick being simulated; tick_rate_hz: the session's rate
 * (dt = 1 / tick_rate_hz, worked out as the server does). */
ApxStatus apx_car_step(ApxCar *car, ApxTrack *track, const ApxInput *input,
                       uint32_t tick, uint16_t tick_rate_hz);

/* The car's pose, velocities and dash figures. */
ApxStatus apx_car_pose(const ApxCar *car, ApxCarPose *out);

/* The car's full state as a versioned blob: loading it and stepping replays
 * bit for bit. *written gets its size; with a NULL buf or a short capacity
 * nothing is copied and APX_ERR_BUFFER is returned. The size moves a little
 * from tick to tick (MessagePack sizes a number by its value): keep
 * headroom in a ring of saved states, or ask again on APX_ERR_BUFFER. */
ApxStatus apx_car_state_save(const ApxCar *car, uint8_t *buf, size_t capacity,
                             size_t *written);

/* Replace the car's state with a blob of this version and this car. */
ApxStatus apx_car_state_load(ApxCar *car, const uint8_t *buf, size_t len);

/* The car's snapshot, as the server sends its own in DriverFeedback.own_car. */
ApxStatus apx_car_snapshot(const ApxCar *car, ApxCarSnapshot *out);

/* Lay a snapshot over the car's state: every field it carries set, the rest
 * (tyres, fuel, hybrid, brakes, damage, lap timing) kept. The rewind to a
 * DriverFeedback.own_car between two full blobs. A non-finite number is
 * refused (APX_ERR_INVALID) and the car left as it was. */
ApxStatus apx_car_snapshot_apply(ApxCar *car, const ApxCarSnapshot *snapshot);

/* What the driver should feel over the steps since the last call, and a
 * fresh interval: the server's own force-feedback collector, drained as the
 * game loop drains it into each DriverFeedback. Taken after every step it is
 * that tick's (one torque sample). Loading a state blob empties it; laying a
 * snapshot leaves it. */
ApxStatus apx_car_feedback_take(ApxCar *car, ApxFeedback *out);

/* Hold the track's road state to the server's from a RoadState message's
 * bytes: rows (whole road_state::ROW_BYTES cells from first_cell, wrapping
 * the lap), snow_rows (empty for a road with no snow: the snow stops
 * counting) and lane_snow (the whole pit lane; empty leaves it). At the
 * wire's byte resolution. APX_ERR_INVALID, the track unchanged, for a track
 * with no road state or bytes that are not whole cells. */
ApxStatus apx_track_road_rows(ApxTrack *track, uint32_t first_cell,
                              const uint8_t *rows, size_t rows_len,
                              const uint8_t *snow_rows, size_t snow_rows_len,
                              const uint8_t *lane_snow, size_t lane_snow_len);

/* ---- A seeded race nobody drives (ABI 4) ------------------------------- */

typedef struct ApxRace ApxRace;

/* One car of a race, in grid order: the server's frame, metres. */
typedef struct ApxRaceCar {
    uint32_t grid_position;
    float x, y, z;
    float yaw_rad;
    float speed_mps;
    uint32_t lap;            /* laps begun (current_lap) */
    float track_progress_m;  /* the place on this lap */
    uint8_t finished;        /* 1 once it has finished */
} ApxRaceCar;

#ifdef __cplusplus
static_assert(sizeof(ApxRaceCar) == 36, "ApxRaceCar layout");
#else
typedef char apx_race_car_layout[(sizeof(ApxRaceCar) == 36) ? 1 : -1];
#endif

/* Start a seeded race as `apexsim-replay render` starts one
 * (seeded_race::start_race): the track, conditions as apx_track_load takes
 * them (null for the defaults) resolved for the seed, ai_count cars of
 * host_car's class (a folder, id or name under cars_dir), laps to the flag,
 * countdown_s of countdown, at tick_rate. The same arguments give the same
 * race, tick for tick, as the server's render of that seed. */
ApxStatus apx_race_start(const char *track_yaml, const char *cars_dir,
                         const char *host_car, uint8_t ai_count, uint8_t laps,
                         uint16_t countdown_s, uint16_t tick_rate, uint64_t seed,
                         const uint8_t *conditions, size_t conditions_len,
                         ApxRace **out);

/* Tick it `ticks` times, every car driven by its AI (seeded_race::tick_field). */
ApxStatus apx_race_tick(ApxRace *race, uint32_t ticks);

/* The session's tick count. */
ApxStatus apx_race_current_tick(const ApxRace *race, uint32_t *out);

/* Every car in grid order into out (room for capacity); written is the
 * field's size. APX_ERR_BUFFER, nothing but written set, when it does not fit. */
ApxStatus apx_race_cars(const ApxRace *race, ApxRaceCar *out, size_t capacity,
                        size_t *written);

/* A digest of every car's full state and the tick (FNV-1a over their
 * MessagePack): two races that agree on it agree bit for bit. */
ApxStatus apx_race_digest(const ApxRace *race, uint64_t *out);

/* Free a race (null is ignored). */
void apx_race_free(ApxRace *race);

/* ---- Showcases and track guides (ABI 5) --------------------------------- */

/* Every showcase showcase.yml at list_path names (<track>.<class>.<variant>,
 * in the list's order), one per line, UTF-8, no terminator. *written gets
 * the size; with a NULL buf or a short capacity nothing is copied and
 * APX_ERR_BUFFER is returned. */
ApxStatus apx_showcase_names(const char *list_path, uint8_t *buf, size_t capacity,
                             size_t *written);

/* Render showcase `name` of the list into the spectator stream file
 * out_path: what `apexsim-replay showcase --name <name>` writes from the same
 * list, tracks_dir (default/<stem>/<stem>.yaml) and cars_dir, byte for
 * byte. Written beside out_path and moved over it when whole. Takes seconds
 * (a whole race): call it off the game thread. *cancel (cancel may be
 * NULL) is read once a simulated second; non-zero stops the render with
 * APX_ERR_CANCELLED and nothing written. */
ApxStatus apx_showcase_render(const char *list_path, const char *tracks_dir,
                              const char *cars_dir, const char *name,
                              const char *out_path, const volatile uint8_t *cancel);

/* Make the track guide of `class` (car.toml class: GT3, F1, ...) on the track
 * YAML with the class's cars under cars_dir, into out_dir as
 * <Stem>.<class>.guide.json and its .guide.apxs: what `apexsim-replay guide
 * <yaml> --class <class>` writes with its defaults, byte for byte. Both are
 * written aside and moved in, the stream first. Seconds of work: call it
 * off the game thread. *cancel as for apx_showcase_render, read before each
 * car's runs. */
ApxStatus apx_guide_build(const char *track_yaml, const char *cars_dir,
                          const char *class_name, const char *out_dir,
                          const volatile uint8_t *cancel);

#ifdef __cplusplus
}
#endif

#endif /* APEXSIM_PHYSICS_H */
