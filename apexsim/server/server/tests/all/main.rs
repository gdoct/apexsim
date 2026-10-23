//! The server's integration tests, one binary: each file is a module, so
//! `cargo test --test all lobby_integration_tests::` runs one file and a name
//! filter runs one test. One binary links once; a binary per file relinked
//! them all on every change. The tests that need only the simulation live in
//! `apexsim/server/sim/tests/all`.

mod admin_test;
mod auth_test;
mod common;
mod demo_lap_tests;
mod driver_key_test;
mod event_test;
mod grid_order_test;
mod hotlap_test;
mod hotlap_watch_test;
mod integration_test;
mod leaderboard_test;
mod live_spectator_test;
mod live_stream_test;
mod lobby_integration_tests;
mod metrics_test;
mod physics_property_tests;
mod pit_stop_test;
mod protocol_test;
mod race_flow_test;
mod ready_grid_test;
mod reconnect_test;
mod recordings_test;
mod safety_car_test;
mod session_conditions_test;
mod session_state_test;
mod shootout_test;
mod showcase_test;
mod sim_wire_test;
mod stress_tests;
mod tls_requirement_test;
mod track_content_test;
mod track_evolution_test;
mod transport_backpressure_test;
mod weekend_test;
