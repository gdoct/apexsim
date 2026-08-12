//! Conversion between track space and Bevy's render space.
//!
//! Track space (per `TRACK_EDITOR.md` \#2 and the server's coordinate
//! convention): right-handed, `+X` follows the course from start/finish,
//! `+Y` is left, `+Z` is up.
//!
//! Bevy space: right-handed, `+Y` is up. We map
//! `(x, y, z)_track -> (x, z, -y)_bevy`. That matrix has determinant +1 (a
//! pure rotation, not a mirror), so cross products, winding order, and
//! banking direction all carry over unchanged — track `+Z` (up) lands on
//! Bevy `+Y` (up), which is all that actually matters for rendering.

use bevy::prelude::Vec3;

pub fn to_bevy(x: f32, y: f32, z: f32) -> Vec3 {
    Vec3::new(x, z, -y)
}

pub fn from_bevy(v: Vec3) -> (f32, f32, f32) {
    (v.x, -v.z, v.y)
}
