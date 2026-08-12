//! Shared editor state.
//!
//! Two data layers with very different rules:
//! - [`OpenTrack`] — the source `track.yaml`. **Read-only.** Loaded once per
//!   open, never mutated, never written back.
//! - [`OpenScene`] — the `.ats` scene sitting next to the YAML. This is the
//!   only thing the editor edits and the only file it saves.
//!
//! Every scene mutation that should trigger a viewport rebuild goes through
//! the [`OpenScene`] resource so `scene.rs` can drive rebuilds off Bevy's
//! change detection. The status line lives in its own resource so writing a
//! status message doesn't spuriously retrigger scene rebuilds.

use std::path::PathBuf;

use bevy::prelude::Resource;

use crate::ats::AtsScene;
use crate::track_data::TrackFile;

/// The read-only source track currently open, if any.
#[derive(Resource, Default)]
pub struct OpenTrack {
    pub path: Option<PathBuf>,
    pub track: Option<TrackFile>,
}

/// The editable `.ats` scene for the open track.
#[derive(Resource, Default)]
pub struct OpenScene {
    pub path: Option<PathBuf>,
    pub scene: Option<AtsScene>,
    /// Unsaved changes since the last save (or since creation).
    pub dirty: bool,
}

/// One-line status shown in the side panel.
#[derive(Resource, Default)]
pub struct StatusLine(pub String);

/// What's selected in the viewport / layer lists.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SelectedElement {
    Curb(u64),
    Marking(u64),
    Prop(u64),
    PitLane,
}

#[derive(Resource, Default)]
pub struct Selection(pub Option<SelectedElement>);

/// True while a viewport drag (prop or pit-lane node) is in flight; the
/// scene rebuild is deferred until the drag ends so the entity under the
/// pointer isn't despawned mid-drag.
#[derive(Resource, Default)]
pub struct DragActive(pub bool);

/// Whole-snapshot undo/redo over the scene. Coarse (clones the full
/// [`AtsScene`] per entry) but simple and correct at the element counts real
/// scenes carry.
#[derive(Resource, Default)]
pub struct UndoStack {
    past: Vec<AtsScene>,
    future: Vec<AtsScene>,
}

impl UndoStack {
    /// Record `snapshot` (the state *before* an edit) and drop the redo
    /// history — a fresh edit invalidates any previously undone future.
    pub fn push(&mut self, snapshot: AtsScene) {
        self.past.push(snapshot);
        self.future.clear();
    }

    /// Pop the most recent snapshot, pushing `current` onto the redo stack.
    pub fn undo(&mut self, current: AtsScene) -> Option<AtsScene> {
        let previous = self.past.pop()?;
        self.future.push(current);
        Some(previous)
    }

    /// Pop the most recently undone snapshot, pushing `current` back onto
    /// the undo stack.
    pub fn redo(&mut self, current: AtsScene) -> Option<AtsScene> {
        let next = self.future.pop()?;
        self.past.push(current);
        Some(next)
    }

    /// Forget all history (e.g. when a different track is opened).
    pub fn clear(&mut self) {
        self.past.clear();
        self.future.clear();
    }
}
