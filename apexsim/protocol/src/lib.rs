//! What the server and the people watching it exchange, apart from the
//! sockets: the wire messages (`network`, `session_wire`), the spectator
//! stream and its file (`spectator`, `session_stream`), the lap records and
//! ghosts (`records`), the setup sheet (`setup_sheet`), and the render of a
//! seeded race into a stream (`render`, what `apexsim-replay render` and
//! the physics library's showcases run).
//!
//! It depends only on the simulation, so the client's physics library can
//! write the same stream bytes as the server. The server re-exports every
//! module under the same path (`apexsim_server::network`, ...).

pub mod network;
pub mod records;
pub mod render;
pub mod session_stream;
pub mod session_wire;
pub mod setup_sheet;
pub mod spectator;

// The simulation's modules under their own paths, so `crate::data` and
// `crate::game_session` resolve here as they do in the server.
pub use apexsim_sim::aero;
pub use apexsim_sim::ai_driver;
pub use apexsim_sim::balance;
pub use apexsim_sim::brakes;
pub use apexsim_sim::car_loader;
pub use apexsim_sim::car_setup;
pub use apexsim_sim::car_step;
pub use apexsim_sim::classification;
pub use apexsim_sim::conditions;
pub use apexsim_sim::content_crc;
pub use apexsim_sim::curbs;
pub use apexsim_sim::damage;
pub use apexsim_sim::data;
pub use apexsim_sim::debug_hooks;
pub use apexsim_sim::drs;
pub use apexsim_sim::engine_heat;
pub use apexsim_sim::feedback;
pub use apexsim_sim::game_session;
pub use apexsim_sim::geometry;
pub use apexsim_sim::ghost;
pub use apexsim_sim::grid_order;
pub use apexsim_sim::ground;
pub use apexsim_sim::headlights;
pub use apexsim_sim::hybrid;
pub use apexsim_sim::laps;
pub use apexsim_sim::own_car;
pub use apexsim_sim::physics;
pub use apexsim_sim::pit;
pub use apexsim_sim::race_control;
pub use apexsim_sim::race_start;
pub use apexsim_sim::racecraft;
pub use apexsim_sim::racing_line;
pub use apexsim_sim::recovery;
pub use apexsim_sim::road_mesh;
pub use apexsim_sim::road_state;
pub use apexsim_sim::safety_car;
pub use apexsim_sim::seeded_race;
pub use apexsim_sim::slipstream;
pub use apexsim_sim::track_bundle;
pub use apexsim_sim::track_loader;
pub use apexsim_sim::tyre_thermal;
pub use apexsim_sim::walls;
pub use apexsim_sim::weekend;
pub use apexsim_sim::wind;
