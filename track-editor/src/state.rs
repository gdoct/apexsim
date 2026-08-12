//! Shared editor state: the currently open track and its undo history.

use std::path::PathBuf;

use bevy::prelude::Resource;

use crate::track_data::TrackFile;

/// The track currently open in the editor, if any, plus the path it was
/// loaded from/should save to. `None` means no track is open yet.
///
/// Every mutation that should trigger a viewport rebuild (load, node drag,
/// undo/redo) goes through this resource so `scene.rs` can drive the rebuild
/// purely off Bevy's change detection (`resource_changed::<OpenTrack>`)
/// instead of a hand-rolled dirty flag.
#[derive(Resource, Default)]
pub struct OpenTrack {
    pub path: Option<PathBuf>,
    pub track: Option<TrackFile>,
    pub status: String,
}

/// Whole-snapshot undo/redo. Coarse (clones the full `TrackFile` per entry)
/// but simple and correct; fine at the node-count scale real tracks operate
/// at. Revisit with targeted diffs if snapshot cost ever shows up during
/// interactive terrain/overlay editing later.
#[derive(Resource, Default)]
pub struct UndoStack {
    past: Vec<TrackFile>,
    future: Vec<TrackFile>,
}

impl UndoStack {
    /// Record `snapshot` (the state *before* an edit) and drop the redo
    /// history — a fresh edit invalidates any previously undone future.
    pub fn push(&mut self, snapshot: TrackFile) {
        self.past.push(snapshot);
        self.future.clear();
    }

    /// Pop the most recent snapshot, pushing `current` onto the redo stack.
    pub fn undo(&mut self, current: TrackFile) -> Option<TrackFile> {
        let previous = self.past.pop()?;
        self.future.push(current);
        Some(previous)
    }

    /// Pop the most recently undone snapshot, pushing `current` back onto
    /// the undo stack.
    pub fn redo(&mut self, current: TrackFile) -> Option<TrackFile> {
        let next = self.future.pop()?;
        self.past.push(current);
        Some(next)
    }
}
