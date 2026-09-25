//! The track pipeline: the `.ats` scene format, grooming, dressing,
//! smoothing, banking, terrain and the Unreal bake. No Bevy here, so the
//! `ats-*` tools and the tests build without the editor's renderer; the
//! editor (`track-editor`, one directory up) is a viewport on top of this.

pub mod ats;
pub mod ats_io;
pub mod barriers;
pub mod circuit_style;
pub mod dem;
pub mod dress;
pub mod groom;
pub mod layout;
pub mod pit;
pub mod project;
pub mod props;
pub mod strip_layout;
pub mod terrain;
pub mod track_bank;
pub mod track_data;
pub mod track_io;
pub mod track_path;
pub mod track_smooth;
pub mod ue_export;
pub mod ue_export_io;
