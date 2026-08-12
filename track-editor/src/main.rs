use bevy::prelude::*;
use bevy_egui::{egui, EguiContexts, EguiPlugin, EguiPrimaryContextPass};

use track_editor::mcp::McpPlugin;
use track_editor::scene::ScenePlugin;
use track_editor::state::OpenTrack;
use track_editor::track_io;

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
        .add_systems(EguiPrimaryContextPass, ui_root)
        .run();
}

fn ui_root(mut contexts: EguiContexts, mut open_track: ResMut<OpenTrack>) -> Result {
    let ctx = contexts.ctx_mut()?;
    let mut viewport_ui = egui::Ui::new(
        ctx.clone(),
        "viewport".into(),
        egui::UiBuilder::new()
            .layer_id(egui::LayerId::background())
            .max_rect(ctx.viewport_rect()),
    );

    egui::Panel::top("top_panel").show(&mut viewport_ui, |ui| {
        egui::MenuBar::new().ui(ui, |ui| {
            egui::containers::menu::MenuButton::new("File").ui(ui, |ui| {
                if ui.button("Open track.yaml…").clicked() {
                    ui.close();
                    if let Some(path) = rfd::FileDialog::new()
                        .add_filter("Track files", &["yaml", "yml", "json"])
                        .pick_file()
                    {
                        match track_io::load_track_file(&path) {
                            Ok(track) => {
                                open_track.status = format!(
                                    "Loaded \"{}\" ({} nodes)",
                                    track.name,
                                    track.nodes.len()
                                );
                                open_track.path = Some(path);
                                open_track.track = Some(track);
                            }
                            Err(e) => {
                                open_track.status = format!("Failed to load: {e}");
                            }
                        }
                    }
                }

                let can_save = open_track.track.is_some();
                if ui
                    .add_enabled(can_save, egui::Button::new("Save"))
                    .clicked()
                {
                    ui.close();
                    if let (Some(path), Some(track)) = (&open_track.path, &open_track.track) {
                        match track_io::save_track_file(path, track) {
                            Ok(()) => open_track.status = "Saved.".to_string(),
                            Err(e) => open_track.status = format!("Failed to save: {e}"),
                        }
                    }
                }
            });
        });
    });

    egui::Panel::left("side_panel")
        .default_size(280.0)
        .show(&mut viewport_ui, |ui| match &open_track.track {
            Some(track) => {
                ui.heading(&track.name);
                ui.label(format!("Nodes: {}", track.nodes.len()));
                ui.label(format!("Closed loop: {}", track.closed_loop));
                ui.label(format!("Default width: {:.2} m", track.default_width));
                ui.label(format!("Checkpoints: {}", track.checkpoints.len()));
                ui.label(format!("Spawn points: {}", track.spawn_points.len()));
                if let Some(country) = track.metadata.as_ref().and_then(|m| m.country.as_ref()) {
                    ui.label(format!("Country: {country}"));
                }
                ui.separator();
                ui.label("Drag a node handle to move it.");
                ui.label("Right-drag orbits, middle-drag pans, scroll zooms.");
                ui.label("Ctrl+Z / Ctrl+Shift+Z undo/redo.");
                ui.separator();
                ui.label(&open_track.status);
            }
            None => {
                ui.label("No track open. Use File \u{2192} Open track.yaml\u{2026}");
                ui.separator();
                ui.label(&open_track.status);
            }
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

    // Deliberately no CentralPanel: leaving the middle of the screen free of
    // egui content lets the 3D viewport (rendered by the same camera, behind
    // the egui overlay) show through untouched.

    Ok(())
}
