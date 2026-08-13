use bevy::prelude::*;
use bevy_egui::{egui, EguiContexts, EguiPlugin, EguiPrimaryContextPass};

use track_editor::ats::{
    AtsScene, Curb, Marking, MarkingKind, PitLane, Prop, PropKind, Side, Surface, SurfaceKind,
};
use track_editor::ats_io;
use track_editor::coords;
use track_editor::mcp::McpPlugin;
use track_editor::project;
use track_editor::scene::{OrbitCamera, ScenePlugin, TrackPathRes};
use track_editor::state::{
    OpenScene, OpenTrack, SelectedElement, Selection, StatusLine, UndoStack,
};

const CURB_STYLES: [&str; 4] = ["red_white", "yellow_black", "green_white", "blue_white"];

fn main() {
    App::new()
        .add_plugins(DefaultPlugins.set(WindowPlugin {
            primary_window: Some(Window {
                title: "ApexSim Track Editor".to_string(),
                ..default()
            }),
            ..default()
        }))
        .add_plugins(MeshPickingPlugin)
        .add_plugins(EguiPlugin::default())
        .add_plugins(ScenePlugin)
        .add_plugins(McpPlugin)
        .init_resource::<OpenTrack>()
        .init_resource::<OpenScene>()
        .add_systems(EguiPrimaryContextPass, ui_root)
        .run();
}

/// A discrete edit requested from the UI this frame; applied after the
/// panels are drawn so list iteration and mutation don't fight over the
/// scene borrow.
enum UiAction {
    AddSurface(SurfaceKind),
    AddCurb,
    AddMarking,
    AddProp(PropKind),
    CreatePitLane,
    DeleteSelected,
}

#[allow(clippy::too_many_arguments)]
fn ui_root(
    mut contexts: EguiContexts,
    mut open_track: ResMut<OpenTrack>,
    mut open_scene: ResMut<OpenScene>,
    mut selection: ResMut<Selection>,
    mut undo_stack: ResMut<UndoStack>,
    mut status: ResMut<StatusLine>,
    track_path: Res<TrackPathRes>,
    orbit: Res<OrbitCamera>,
    mut last_frame_edited: Local<bool>,
    mut new_surface_kind: Local<Option<SurfaceKind>>,
    mut new_prop_kind: Local<Option<PropKind>>,
) -> Result {
    let ctx = contexts.ctx_mut()?;
    let mut viewport_ui = egui::Ui::new(
        ctx.clone(),
        "viewport".into(),
        egui::UiBuilder::new()
            .layer_id(egui::LayerId::background())
            .max_rect(ctx.viewport_rect()),
    );

    let total_len = track_path.0.as_ref().map(|p| p.total_length_m());
    let snapshot_before = open_scene.scene.clone();
    let mut action: Option<UiAction> = None;
    let mut inspector_edited = false;
    let mut save_requested = ctx.input(|i| i.modifiers.command && i.key_pressed(egui::Key::S));

    egui::Panel::top("top_panel").show(&mut viewport_ui, |ui| {
        egui::MenuBar::new().ui(ui, |ui| {
            egui::containers::menu::MenuButton::new("File").ui(ui, |ui| {
                if ui.button("Open track.yaml… (read-only)").clicked() {
                    ui.close();
                    if let Some(path) = rfd::FileDialog::new()
                        .add_filter("Track files", &["yaml", "yml", "json"])
                        .pick_file()
                    {
                        open_track_and_scene(
                            &path,
                            &mut open_track,
                            &mut open_scene,
                            &mut undo_stack,
                            &mut selection,
                            &mut status,
                        );
                    }
                }

                let can_save = open_scene.scene.is_some();
                if ui
                    .add_enabled(can_save, egui::Button::new("Save scene (.ats)"))
                    .clicked()
                {
                    ui.close();
                    save_requested = true;
                }
            });
        });
    });

    egui::Panel::left("side_panel")
        .default_size(320.0)
        .show(&mut viewport_ui, |ui| {
            egui::ScrollArea::vertical().show(ui, |ui| {
                match &open_track.track {
                    Some(track) => {
                        ui.heading(&track.name);
                        ui.label(format!("Nodes: {} (read-only)", track.nodes.len()));
                        if let Some(len) = total_len {
                            ui.label(format!("Length: {:.0} m", len));
                        }
                        ui.weak("The YAML is source data; edits go to the .ats scene.");
                    }
                    None => {
                        ui.label("No track open. Use File → Open track.yaml…");
                    }
                }
                ui.separator();

                // Panels below borrow the scene mutably (inspector) and
                // immutably (lists); bypass change detection here and mark
                // the resource changed once at the end iff something was
                // actually edited, so redrawing the UI doesn't rebuild the
                // 3D scene every frame.
                let raw = open_scene.bypass_change_detection();
                if let Some(scene) = &raw.scene {
                    scene_layers_ui(
                        ui,
                        scene,
                        &mut selection,
                        &mut action,
                        &mut new_surface_kind,
                        &mut new_prop_kind,
                    );
                    ui.separator();
                }
                if let Some(scene) = raw.scene.as_mut() {
                    inspector_edited =
                        inspector_ui(ui, scene, &mut selection, total_len, &mut action);
                    ui.separator();
                }

                if raw.dirty {
                    ui.label("● unsaved scene changes");
                }
                ui.label(&status.0);
                ui.separator();
                ui.label("Left-drag props / pit nodes to move them.");
                ui.label("Right-drag orbits, middle-drag pans, scroll zooms.");
                ui.label("Ctrl+Z / Ctrl+Shift+Z undo/redo, Ctrl+S saves.");
            });
        });

    egui::Panel::bottom("mcp_status_panel")
        .resizable(false)
        .show_separator_line(false)
        .show(&mut viewport_ui, |ui| {
            ui.horizontal(|ui| {
                ui.weak("MCP:");
                ui.weak(track_editor::mcp::endpoint_url());
                ui.weak("— claude mcp add --transport http track-editor <url>");
            });
        });

    // ------------------------------------------------------------------
    // Apply this frame's edits, with undo bookkeeping.
    // ------------------------------------------------------------------

    if let Some(action) = action {
        if let Some(before) = snapshot_before.clone() {
            undo_stack.push(before);
        }
        apply_ui_action(
            action,
            &mut open_scene,
            &mut selection,
            &mut status,
            &track_path,
            &orbit,
        );
        *last_frame_edited = false;
    } else if inspector_edited {
        // Coalesce a held slider drag into one undo entry: only push a
        // snapshot on the first edited frame of a run.
        if !*last_frame_edited {
            if let Some(before) = snapshot_before {
                undo_stack.push(before);
            }
        }
        let scene = open_scene.as_mut(); // marks the resource changed
        scene.dirty = true;
        *last_frame_edited = true;
    } else {
        *last_frame_edited = false;
    }

    if save_requested {
        save_scene(&mut open_scene, &mut status);
    }

    Ok(())
}

fn open_track_and_scene(
    path: &std::path::Path,
    open_track: &mut OpenTrack,
    open_scene: &mut OpenScene,
    undo_stack: &mut UndoStack,
    selection: &mut Selection,
    status: &mut StatusLine,
) {
    match project::open_project(path) {
        Ok(project) => {
            status.0 = project.message;
            open_track.path = Some(project.track_path);
            open_track.track = Some(project.track);
            open_scene.path = Some(project.ats_path);
            open_scene.scene = project.scene;
            open_scene.dirty = project.dirty;
            undo_stack.clear();
            selection.0 = None;
        }
        Err(e) => status.0 = e,
    }
}

fn save_scene(open_scene: &mut OpenScene, status: &mut StatusLine) {
    let OpenScene { path, scene, dirty } = &mut *open_scene;
    match (path.as_ref(), scene.as_ref()) {
        (Some(path), Some(scene)) => match ats_io::save_ats(path, scene) {
            Ok(()) => {
                *dirty = false;
                status.0 = format!("Saved {}", path.display());
            }
            Err(e) => status.0 = format!("Failed to save: {e}"),
        },
        _ => status.0 = "Nothing to save.".to_string(),
    }
}

// ---------------------------------------------------------------------
// Layer lists
// ---------------------------------------------------------------------

fn scene_layers_ui(
    ui: &mut egui::Ui,
    scene: &AtsScene,
    selection: &mut Selection,
    action: &mut Option<UiAction>,
    new_surface_kind: &mut Option<SurfaceKind>,
    new_prop_kind: &mut Option<PropKind>,
) {
    ui.heading("Scene");

    egui::CollapsingHeader::new(format!("Surfaces ({})", scene.surfaces.len()))
        .default_open(true)
        .show(ui, |ui| {
            for surface in &scene.surfaces {
                let is_selected = selection.0 == Some(SelectedElement::Surface(surface.id));
                let label = format!(
                    "#{} {} {} {:.0}–{:.0} m",
                    surface.id,
                    surface.kind.label(),
                    surface.side.label(),
                    surface.start_m,
                    surface.end_m
                );
                if ui.selectable_label(is_selected, label).clicked() {
                    selection.0 = Some(SelectedElement::Surface(surface.id));
                }
            }
            ui.horizontal(|ui| {
                let kind = new_surface_kind.get_or_insert(SurfaceKind::Gravel);
                egui::ComboBox::from_id_salt("new_surface_kind")
                    .selected_text(kind.label())
                    .show_ui(ui, |ui| {
                        for candidate in SurfaceKind::ALL {
                            ui.selectable_value(kind, candidate, candidate.label());
                        }
                    });
                if ui.button("+ Add surface").clicked() {
                    *action = Some(UiAction::AddSurface(*kind));
                }
            });
        });

    egui::CollapsingHeader::new(format!("Curbs ({})", scene.curbs.len()))
        .default_open(true)
        .show(ui, |ui| {
            for curb in &scene.curbs {
                let is_selected = selection.0 == Some(SelectedElement::Curb(curb.id));
                let label = format!(
                    "#{} {} {:.0}–{:.0} m",
                    curb.id,
                    curb.side.label(),
                    curb.start_m,
                    curb.end_m
                );
                if ui.selectable_label(is_selected, label).clicked() {
                    selection.0 = Some(SelectedElement::Curb(curb.id));
                }
            }
            if ui.button("+ Add curb").clicked() {
                *action = Some(UiAction::AddCurb);
            }
        });

    egui::CollapsingHeader::new(format!("Markings ({})", scene.markings.len()))
        .default_open(true)
        .show(ui, |ui| {
            for marking in &scene.markings {
                let is_selected = selection.0 == Some(SelectedElement::Marking(marking.id));
                let label = format!(
                    "#{} {} {:.0}–{:.0} m",
                    marking.id,
                    marking.kind.label(),
                    marking.start_m,
                    marking.end_m
                );
                if ui.selectable_label(is_selected, label).clicked() {
                    selection.0 = Some(SelectedElement::Marking(marking.id));
                }
            }
            if ui.button("+ Add marking").clicked() {
                *action = Some(UiAction::AddMarking);
            }
        });

    egui::CollapsingHeader::new("Pit lane")
        .default_open(true)
        .show(ui, |ui| match &scene.pit_lane {
            Some(pit) => {
                let is_selected = selection.0 == Some(SelectedElement::PitLane);
                let label = format!("{} nodes, {} boxes", pit.nodes.len(), pit.box_count);
                if ui.selectable_label(is_selected, label).clicked() {
                    selection.0 = Some(SelectedElement::PitLane);
                }
            }
            None => {
                if ui.button("+ Create pit lane").clicked() {
                    *action = Some(UiAction::CreatePitLane);
                }
            }
        });

    egui::CollapsingHeader::new(format!("Props ({})", scene.props.len()))
        .default_open(true)
        .show(ui, |ui| {
            for prop in &scene.props {
                let is_selected = selection.0 == Some(SelectedElement::Prop(prop.id));
                let label = format!("#{} {} ({})", prop.id, prop.kind.label(), prop.asset);
                if ui.selectable_label(is_selected, label).clicked() {
                    selection.0 = Some(SelectedElement::Prop(prop.id));
                }
            }
            ui.horizontal(|ui| {
                let kind = new_prop_kind.get_or_insert(PropKind::Tree);
                egui::ComboBox::from_id_salt("new_prop_kind")
                    .selected_text(kind.label())
                    .show_ui(ui, |ui| {
                        for candidate in PropKind::ALL {
                            ui.selectable_value(kind, candidate, candidate.label());
                        }
                    });
                if ui.button("+ Add prop").clicked() {
                    *action = Some(UiAction::AddProp(*kind));
                }
            });
        });
}

// ---------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------

/// Draws the property editor for the current selection directly against the
/// scene data. Returns true if any widget reported a change this frame.
fn inspector_ui(
    ui: &mut egui::Ui,
    scene: &mut AtsScene,
    selection: &mut Selection,
    total_len: Option<f32>,
    action: &mut Option<UiAction>,
) -> bool {
    let Some(selected) = selection.0 else {
        ui.weak("Nothing selected. Click an element in the viewport or the lists above.");
        return false;
    };
    ui.heading("Inspector");
    let max_station = total_len.unwrap_or(100_000.0);
    let mut changed = false;

    match selected {
        SelectedElement::Surface(id) => match scene.surface_mut(id) {
            Some(surface) => {
                ui.label(format!("Surface #{id}"));
                egui::ComboBox::from_label("kind")
                    .selected_text(surface.kind.label())
                    .show_ui(ui, |ui| {
                        for kind in SurfaceKind::ALL {
                            changed |= ui
                                .selectable_value(&mut surface.kind, kind, kind.label())
                                .changed();
                        }
                    });
                egui::ComboBox::from_label("side")
                    .selected_text(surface.side.label())
                    .show_ui(ui, |ui| {
                        for side in Side::ALL {
                            changed |= ui
                                .selectable_value(&mut surface.side, side, side.label())
                                .changed();
                        }
                    });
                changed |= station_row(ui, "start (m)", &mut surface.start_m, max_station);
                changed |= station_row(ui, "end (m)", &mut surface.end_m, max_station);
                changed |= drag_row(ui, "inner gap (m)", &mut surface.inner_m, 0.1, 0.0..=60.0);
                changed |= drag_row(ui, "width (m)", &mut surface.width_m, 0.5, 0.5..=400.0);
                ui.horizontal(|ui| {
                    let mut tapered = surface.end_width_m.is_some();
                    if ui.checkbox(&mut tapered, "taper").changed() {
                        surface.end_width_m = tapered.then_some(surface.width_m);
                        changed = true;
                    }
                });
                if let Some(end_width) = surface.end_width_m.as_mut() {
                    changed |= drag_row(ui, "end width (m)", end_width, 0.5, 0.5..=400.0);
                }
            }
            None => selection.0 = None,
        },
        SelectedElement::Curb(id) => match scene.curb_mut(id) {
            Some(curb) => {
                ui.label(format!("Curb #{id}"));
                egui::ComboBox::from_label("side")
                    .selected_text(curb.side.label())
                    .show_ui(ui, |ui| {
                        for side in Side::ALL {
                            changed |= ui
                                .selectable_value(&mut curb.side, side, side.label())
                                .changed();
                        }
                    });
                changed |= station_row(ui, "start (m)", &mut curb.start_m, max_station);
                changed |= station_row(ui, "end (m)", &mut curb.end_m, max_station);
                changed |= drag_row(ui, "width (m)", &mut curb.width_m, 0.05, 0.1..=6.0);
                egui::ComboBox::from_label("style")
                    .selected_text(curb.style.clone())
                    .show_ui(ui, |ui| {
                        for style in CURB_STYLES {
                            changed |= ui
                                .selectable_value(&mut curb.style, style.to_string(), style)
                                .changed();
                        }
                    });
            }
            None => selection.0 = None,
        },
        SelectedElement::Marking(id) => match scene.marking_mut(id) {
            Some(marking) => {
                ui.label(format!("Marking #{id}"));
                egui::ComboBox::from_label("kind")
                    .selected_text(marking.kind.label())
                    .show_ui(ui, |ui| {
                        for kind in MarkingKind::ALL {
                            changed |= ui
                                .selectable_value(&mut marking.kind, kind, kind.label())
                                .changed();
                        }
                    });
                changed |= station_row(ui, "start (m)", &mut marking.start_m, max_station);
                changed |= station_row(ui, "end (m)", &mut marking.end_m, max_station);
                changed |= drag_row(
                    ui,
                    "lat from (m)",
                    &mut marking.lat_from_m,
                    0.05,
                    -80.0..=80.0,
                );
                changed |= drag_row(ui, "lat to (m)", &mut marking.lat_to_m, 0.05, -80.0..=80.0);
                ui.horizontal(|ui| {
                    ui.label("color");
                    changed |= ui
                        .color_edit_button_rgba_unmultiplied(&mut marking.color)
                        .changed();
                });
            }
            None => selection.0 = None,
        },
        SelectedElement::Prop(id) => match scene.prop_mut(id) {
            Some(prop) => {
                ui.label(format!("Prop #{id}"));
                egui::ComboBox::from_label("kind")
                    .selected_text(prop.kind.label())
                    .show_ui(ui, |ui| {
                        for kind in PropKind::ALL {
                            changed |= ui
                                .selectable_value(&mut prop.kind, kind, kind.label())
                                .changed();
                        }
                    });
                ui.horizontal(|ui| {
                    ui.label("asset");
                    changed |= ui.text_edit_singleline(&mut prop.asset).changed();
                });
                changed |= drag_row(ui, "x (m)", &mut prop.x, 0.5, -100_000.0..=100_000.0);
                changed |= drag_row(ui, "y (m)", &mut prop.y, 0.5, -100_000.0..=100_000.0);
                changed |= drag_row(ui, "z (m)", &mut prop.z, 0.2, -1000.0..=1000.0);
                changed |= drag_row(
                    ui,
                    "yaw (rad)",
                    &mut prop.yaw_rad,
                    0.05,
                    -std::f32::consts::TAU..=std::f32::consts::TAU,
                );
                changed |= drag_row(ui, "scale", &mut prop.scale, 0.05, 0.1..=25.0);
                ui.horizontal(|ui| {
                    ui.label("text");
                    let mut text = prop.text.clone().unwrap_or_default();
                    if ui.text_edit_singleline(&mut text).changed() {
                        prop.text = if text.is_empty() { None } else { Some(text) };
                        changed = true;
                    }
                });
            }
            None => selection.0 = None,
        },
        SelectedElement::PitLane => match scene.pit_lane.as_mut() {
            Some(pit) => {
                ui.label(format!("Pit lane — {} nodes", pit.nodes.len()));
                ui.weak("Drag the blue spheres in the viewport to shape it.");
                changed |= drag_row(ui, "width (m)", &mut pit.width_m, 0.1, 3.0..=20.0);
                ui.horizontal(|ui| {
                    ui.label("pit boxes");
                    changed |= ui
                        .add(egui::DragValue::new(&mut pit.box_count).range(0..=60))
                        .changed();
                });
                changed |= drag_row(
                    ui,
                    "speed limit (km/h)",
                    &mut pit.speed_limit_kmh,
                    1.0,
                    30.0..=120.0,
                );
                ui.horizontal(|ui| {
                    if ui.button("+ node").clicked() {
                        let last = *pit.nodes.last().expect("pit lane has nodes");
                        let dir = if pit.nodes.len() >= 2 {
                            let prev = pit.nodes[pit.nodes.len() - 2];
                            let dx = last[0] - prev[0];
                            let dy = last[1] - prev[1];
                            let len = dx.hypot(dy).max(1e-3);
                            (dx / len, dy / len)
                        } else {
                            (1.0, 0.0)
                        };
                        pit.nodes
                            .push([last[0] + dir.0 * 25.0, last[1] + dir.1 * 25.0, last[2]]);
                        changed = true;
                    }
                    if pit.nodes.len() > 2 && ui.button("- node").clicked() {
                        pit.nodes.pop();
                        changed = true;
                    }
                });
            }
            None => selection.0 = None,
        },
    }

    if ui.button("🗑 Delete selected").clicked() {
        *action = Some(UiAction::DeleteSelected);
    }
    changed
}

fn drag_row(
    ui: &mut egui::Ui,
    label: &str,
    value: &mut f32,
    speed: f32,
    range: std::ops::RangeInclusive<f32>,
) -> bool {
    let mut changed = false;
    ui.horizontal(|ui| {
        ui.label(label);
        changed = ui
            .add(egui::DragValue::new(value).speed(speed).range(range))
            .changed();
    });
    changed
}

fn station_row(ui: &mut egui::Ui, label: &str, value: &mut f32, max_station: f32) -> bool {
    drag_row(ui, label, value, 1.0, 0.0..=max_station)
}

// ---------------------------------------------------------------------
// Discrete actions
// ---------------------------------------------------------------------

fn apply_ui_action(
    action: UiAction,
    open_scene: &mut OpenScene,
    selection: &mut Selection,
    status: &mut StatusLine,
    track_path: &TrackPathRes,
    orbit: &OrbitCamera,
) {
    let Some(scene) = open_scene.scene.as_mut() else {
        return;
    };
    let total_len = track_path.0.as_ref().map(|p| p.total_length_m());

    match action {
        UiAction::AddSurface(kind) => {
            let id = scene.alloc_id();
            scene.surfaces.push(Surface {
                id,
                kind,
                side: Side::Right,
                start_m: 0.0,
                end_m: total_len.map_or(200.0, |l| (l * 0.05).clamp(40.0, 300.0)),
                inner_m: 1.5,
                width_m: 25.0,
                end_width_m: None,
            });
            selection.0 = Some(SelectedElement::Surface(id));
            status.0 = format!("Added {} surface #{id}.", kind.label());
        }
        UiAction::AddCurb => {
            let id = scene.alloc_id();
            scene.curbs.push(Curb {
                id,
                side: Side::Left,
                start_m: 0.0,
                end_m: total_len.map_or(100.0, |l| (l * 0.05).clamp(20.0, 150.0)),
                width_m: 1.2,
                style: "red_white".to_string(),
            });
            selection.0 = Some(SelectedElement::Curb(id));
            status.0 = format!("Added curb #{id}.");
        }
        UiAction::AddMarking => {
            let id = scene.alloc_id();
            scene.markings.push(Marking {
                id,
                kind: MarkingKind::EdgeLine,
                start_m: 0.0,
                end_m: total_len.map_or(50.0, |l| (l * 0.02).clamp(10.0, 80.0)),
                lat_from_m: 4.0,
                lat_to_m: 4.15,
                color: [1.0, 1.0, 1.0, 1.0],
            });
            selection.0 = Some(SelectedElement::Marking(id));
            status.0 = format!("Added marking #{id}.");
        }
        UiAction::AddProp(kind) => {
            let (x, y, z) = coords::from_bevy(orbit.focus);
            let id = scene.alloc_id();
            scene.props.push(Prop {
                id,
                kind,
                asset: format!("{}_generic", kind.label()),
                x,
                y,
                z: z.max(0.0),
                yaw_rad: 0.0,
                scale: 1.0,
                text: None,
            });
            selection.0 = Some(SelectedElement::Prop(id));
            status.0 = format!("Added {} #{id} at the camera focus.", kind.label());
        }
        UiAction::CreatePitLane => {
            let nodes = default_pit_lane_nodes(track_path);
            scene.pit_lane = Some(PitLane {
                nodes,
                width_m: 8.0,
                box_count: 10,
                speed_limit_kmh: 80.0,
            });
            selection.0 = Some(SelectedElement::PitLane);
            status.0 = "Created pit lane — drag its nodes into place.".to_string();
        }
        UiAction::DeleteSelected => {
            match selection.0 {
                Some(SelectedElement::PitLane) => {
                    scene.pit_lane = None;
                    status.0 = "Deleted pit lane.".to_string();
                }
                Some(
                    SelectedElement::Surface(id)
                    | SelectedElement::Curb(id)
                    | SelectedElement::Marking(id)
                    | SelectedElement::Prop(id),
                ) => {
                    scene.remove_element(id);
                    status.0 = format!("Deleted element #{id}.");
                }
                None => {}
            }
            selection.0 = None;
        }
    }
    open_scene.dirty = true;
}

/// A starter pit lane: a short lane parallel to the track, offset to the
/// right of the first few hundred meters after the start/finish line.
fn default_pit_lane_nodes(track_path: &TrackPathRes) -> Vec<[f32; 3]> {
    use track_editor::track_path::offset_point;

    let Some(path) = &track_path.0 else {
        return vec![[0.0, -15.0, 0.0], [100.0, -15.0, 0.0]];
    };
    let span = (path.total_length_m() * 0.25).min(300.0);
    let count = 6;
    (0..count)
        .map(|i| {
            let s = span * (i as f32 / (count - 1) as f32);
            let sample = path.sample_at(s);
            let p = offset_point(&sample, -(sample.width_right_m + 8.0));
            [p.0, p.1, p.2]
        })
        .collect()
}
