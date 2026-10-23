//! The simulation's integration tests, one binary: each file is a module, so
//! `cargo test --test all fuel_test::` runs one file and a name filter runs
//! one test. One binary links once; a binary per file relinked them all on
//! every change.

mod aero_test;
mod ai_race_start_test;
mod air_test;
mod brake_heat_test;
mod car_physics_gaps_test;
mod car_stability_test;
mod damage_test;
mod debug_hooks_test;
mod determinism_test;
mod fuel_test;
mod geometry_test;
mod grip_probe_test;
mod imported_car_test;
mod imported_track_test;
mod lap_timing_test;
mod multi_class_test;
mod pad_driver_probe;
mod penalty_test;
mod race_start_test;
mod racecraft_test;
mod racing_line_test;
mod recovery_test;
mod road_mesh_test;
mod slipstream_test;
mod snow_tyre_test;
mod test_track_load;
mod track_file_compat_test;
mod tyre_temperature_test;
