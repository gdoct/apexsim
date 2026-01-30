mod track_data;
mod track_mesh;

use bevy::prelude::*;
use bevy::input::mouse::MouseWheel;
use bevy_egui::{egui, EguiContexts, EguiPlugin, EguiSettings};
use std::path::PathBuf;

use track_data::{TrackFileFormat, ProceduralWorldData, TerrainHeightmap, RacelinePoint, KerbSide, NodeKerbs, PlacedTree, TreeTypeDefinition, CanopyShape, TrackEditorOverlay};
use track_mesh::TrackEdges;

fn main() {
    App::new()
        .add_plugins(DefaultPlugins.set(WindowPlugin {
            primary_window: Some(Window {
                title: "ApexSim Track Editor".to_string(),
                resolution: (1600., 900.).into(),
                ..default()
            }),
            ..default()
        }))
        .add_plugins(EguiPlugin)
        .insert_resource(EguiSettings {
            scale_factor: 1.5,  // Scale up UI for high DPI displays
            ..default()
        })
        .init_state::<AppState>()
        .insert_resource(EditorState::default())
        .insert_resource(CameraState::default())
        .insert_resource(TrackEdgesResource::default())
        .add_systems(Update, splash_screen_system.run_if(in_state(AppState::Splash)))
        .add_systems(OnEnter(AppState::Browse), setup_browse_state)
        .add_systems(Update, browse_screen_system.run_if(in_state(AppState::Browse)))
        .insert_resource(TestModeState::default())
        .add_systems(OnEnter(AppState::Editor), setup_editor)
        .add_systems(Update, (
            editor_ui_system,
            camera_controller_system.run_if(|state: Res<EditorState>| state.editor_mode == EditorMode::Edit && state.active_tool == EditorTool::Camera),
            kerb_brush_system.run_if(|state: Res<EditorState>| state.editor_mode == EditorMode::Edit && state.active_tool == EditorTool::KerbBrush),
            terrain_level_brush_system.run_if(|state: Res<EditorState>| state.editor_mode == EditorMode::Edit && state.active_tool == EditorTool::TerrainLevel),
            tree_brush_system.run_if(|state: Res<EditorState>| state.editor_mode == EditorMode::Edit && state.active_tool == EditorTool::TreeBrush),
            test_mode_system.run_if(|state: Res<EditorState>| state.editor_mode == EditorMode::Test),
        ).run_if(in_state(AppState::Editor)))
        .add_systems(OnExit(AppState::Editor), cleanup_editor)
        .run();
}

#[derive(States, Debug, Clone, PartialEq, Eq, Hash, Default)]
enum AppState {
    #[default]
    Splash,
    Browse,
    Editor,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum EditorMode {
    #[default]
    Edit,
    Test,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum TestCameraType {
    #[default]
    Chase,
    Cockpit,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum EditorTool {
    #[default]
    Camera,
    KerbBrush,
    TerrainLevel,
    TreeBrush,
}

#[derive(Resource)]
struct TestModeState {
    progress: f32,
    speed: f32,
    playing: bool,
    camera_type: TestCameraType,
    raceline_length: f32,
}

impl Default for TestModeState {
    fn default() -> Self {
        Self {
            progress: 0.0,
            speed: 50.0,
            playing: false,
            camera_type: TestCameraType::Chase,
            raceline_length: 0.0,
        }
    }
}

#[derive(Resource)]
struct EditorState {
    tracks_folder: Option<PathBuf>,
    available_tracks: Vec<TrackEntry>,
    selected_track_index: Option<usize>,
    loaded_track: Option<LoadedTrack>,
    preview_track: Option<TrackFileFormat>,  // For 2D preview in browse screen
    splash_timer: f32,
    folder_input: String,
    editor_mode: EditorMode,
    // Tool state
    active_tool: EditorTool,
    kerb_brush: KerbBrushState,
    terrain_level_brush: TerrainLevelBrushState,
    tree_brush: TreeBrushState,
}

impl Default for EditorState {
    fn default() -> Self {
        Self {
            tracks_folder: None,
            available_tracks: Vec::new(),
            selected_track_index: None,
            loaded_track: None,
            preview_track: None,
            splash_timer: 0.0,
            folder_input: String::new(),
            editor_mode: EditorMode::default(),
            active_tool: EditorTool::default(),
            kerb_brush: KerbBrushState::default(),
            terrain_level_brush: TerrainLevelBrushState::default(),
            tree_brush: TreeBrushState::default(),
        }
    }
}

#[derive(Debug, Clone)]
struct KerbBrushState {
    /// Rate of height change per second while holding mouse
    height_rate: f32,
    /// Default width for new kerbs
    default_width: f32,
    /// Maximum kerb height
    max_height: f32,
}

impl Default for KerbBrushState {
    fn default() -> Self {
        Self {
            height_rate: 0.05,  // 5cm per second
            default_width: 0.8, // 80cm default width
            max_height: 0.15,   // 15cm max height
        }
    }
}

#[derive(Debug, Clone)]
struct TerrainLevelBrushState {
    /// Radius of the brush in meters
    radius: f32,
    /// Rate of terrain height change per second
    height_rate: f32,
    /// Falloff type: 0 = hard edge, 1 = linear falloff
    falloff: f32,
}

impl Default for TerrainLevelBrushState {
    fn default() -> Self {
        Self {
            radius: 20.0,      // 20m radius
            height_rate: 5.0,  // 5m per second
            falloff: 0.5,      // 50% falloff
        }
    }
}

#[derive(Debug, Clone)]
struct TreeBrushState {
    /// Brush radius in meters
    radius: f32,
    /// Trees placed per second while holding mouse
    density: f32,
    /// Selected tree type index
    selected_type: usize,
    /// Min random scale for placed trees
    scale_min: f32,
    /// Max random scale for placed trees
    scale_max: f32,
    /// Accumulator for fractional tree placement
    place_accumulator: f32,
}

impl Default for TreeBrushState {
    fn default() -> Self {
        Self {
            radius: 20.0,
            density: 8.0,
            selected_type: 0,
            scale_min: 0.7,
            scale_max: 1.3,
            place_accumulator: 0.0,
        }
    }
}

/// Returns the built-in tree type definitions
fn tree_type_definitions() -> Vec<TreeTypeDefinition> {
    vec![
        TreeTypeDefinition {
            name: "Pine",
            trunk_height: 8.0,
            trunk_radius: 0.3,
            canopy_shape: CanopyShape::Cone,
            canopy_radius: 2.5,
            canopy_height: 6.0,
            trunk_color: [0.35, 0.2, 0.1],
            canopy_color: [0.1, 0.35, 0.1],
        },
        TreeTypeDefinition {
            name: "Oak",
            trunk_height: 5.0,
            trunk_radius: 0.5,
            canopy_shape: CanopyShape::Sphere,
            canopy_radius: 4.0,
            canopy_height: 5.0,
            trunk_color: [0.3, 0.18, 0.08],
            canopy_color: [0.15, 0.4, 0.12],
        },
        TreeTypeDefinition {
            name: "Birch",
            trunk_height: 7.0,
            trunk_radius: 0.2,
            canopy_shape: CanopyShape::Sphere,
            canopy_radius: 2.5,
            canopy_height: 3.5,
            trunk_color: [0.85, 0.82, 0.75],
            canopy_color: [0.3, 0.55, 0.15],
        },
        TreeTypeDefinition {
            name: "Spruce",
            trunk_height: 10.0,
            trunk_radius: 0.35,
            canopy_shape: CanopyShape::MultiCone,
            canopy_radius: 3.0,
            canopy_height: 8.0,
            trunk_color: [0.3, 0.15, 0.05],
            canopy_color: [0.05, 0.25, 0.05],
        },
        TreeTypeDefinition {
            name: "Bush",
            trunk_height: 0.5,
            trunk_radius: 0.15,
            canopy_shape: CanopyShape::Sphere,
            canopy_radius: 1.5,
            canopy_height: 1.5,
            trunk_color: [0.3, 0.2, 0.1],
            canopy_color: [0.2, 0.45, 0.1],
        },
    ]
}

#[derive(Clone)]
struct TrackEntry {
    name: String,
    yaml_path: PathBuf,
    terrain_path: PathBuf,
    overlay_path: PathBuf,
}

struct LoadedTrack {
    name: String,
    track_data: TrackFileFormat,
    terrain_data: Option<ProceduralWorldData>,
    /// Path to the .apexedit overlay archive
    overlay_path: PathBuf,
    /// True when there are unsaved editor changes
    dirty: bool,
    /// Snapshot of terrain heights at load time, for computing sparse diffs on save
    original_terrain_heights: Option<Vec<f32>>,
}

#[derive(Resource)]
struct CameraState {
    yaw: f32,
    pitch: f32,
    distance: f32,
    focus: Vec3,
    mouse_sensitivity: f32,
    move_speed: f32,
    last_cursor_pos: Option<Vec2>,
}

impl Default for CameraState {
    fn default() -> Self {
        Self {
            yaw: 0.0,
            pitch: 0.5,  // Positive pitch = looking down from above
            distance: 500.0,
            focus: Vec3::ZERO,
            mouse_sensitivity: 0.005,  // Radians per pixel of cursor movement
            move_speed: 200.0,
            last_cursor_pos: None,
        }
    }
}

#[derive(Component)]
struct EditorCamera;

#[derive(Component)]
struct TrackMeshEntity;

#[derive(Component)]
struct TerrainMeshEntity;

#[derive(Component)]
struct RacelineMeshEntity;

#[derive(Component)]
struct KerbMeshEntity;

#[derive(Component)]
struct TerrainBrushEntity;

#[derive(Component)]
struct TreeMeshEntity;

#[derive(Component)]
struct TreeBrushEntity;

/// Precomputed track edge polylines, stored as a resource for use by kerb brush etc.
#[derive(Resource, Default)]
struct TrackEdgesResource {
    edges: Option<TrackEdges>,
}

// Splash screen system
fn splash_screen_system(
    mut contexts: EguiContexts,
    mut next_state: ResMut<NextState<AppState>>,
    mut editor_state: ResMut<EditorState>,
    time: Res<Time>,
) {
    editor_state.splash_timer += time.delta_seconds();

    egui::CentralPanel::default().show(contexts.ctx_mut(), |ui| {
        ui.vertical_centered(|ui| {
            ui.add_space(200.0);
            ui.heading(egui::RichText::new("ApexSim Track Editor").size(48.0));
            ui.add_space(20.0);
            ui.label(egui::RichText::new("Loading...").size(24.0));
            ui.add_space(40.0);
            ui.spinner();
        });
    });

    // Transition after 1 second
    if editor_state.splash_timer > 1.0 {
        next_state.set(AppState::Browse);
    }
}

fn setup_browse_state(mut editor_state: ResMut<EditorState>) {
    // Set default tracks folder - try multiple paths
    if editor_state.tracks_folder.is_none() {
        let possible_paths = [
            PathBuf::from("content/tracks/real"),
            PathBuf::from("../content/tracks/real"),
            std::env::current_dir()
                .ok()
                .map(|p| p.join("content/tracks/real"))
                .unwrap_or_default(),
        ];

        for path in possible_paths {
            if path.exists() && path.is_dir() {
                editor_state.tracks_folder = Some(path);
                scan_tracks_folder(&mut editor_state);
                break;
            }
        }
    }
}

fn scan_tracks_folder(editor_state: &mut EditorState) {
    editor_state.available_tracks.clear();

    if let Some(folder) = &editor_state.tracks_folder {
        if let Ok(entries) = std::fs::read_dir(folder) {
            let mut tracks: Vec<TrackEntry> = entries
                .filter_map(|e| e.ok())
                .filter_map(|entry| {
                    let path = entry.path();
                    if path.extension().map(|e| e == "yaml").unwrap_or(false) {
                        let stem = path.file_stem()?.to_string_lossy().to_string();
                        let terrain_path = folder.join(format!("{}.terrain.msgpack", stem));
                        let overlay_path = folder.join(format!("{}.apexedit", stem));

                        if terrain_path.exists() {
                            Some(TrackEntry {
                                name: stem,
                                yaml_path: path,
                                terrain_path,
                                overlay_path,
                            })
                        } else {
                            None
                        }
                    } else {
                        None
                    }
                })
                .collect();

            tracks.sort_by(|a, b| a.name.cmp(&b.name));
            editor_state.available_tracks = tracks;
        }
    }
}

fn browse_screen_system(
    mut contexts: EguiContexts,
    mut editor_state: ResMut<EditorState>,
    mut next_state: ResMut<NextState<AppState>>,
) {
    // Track selection changes to load preview
    let mut selection_changed = false;
    let mut new_selection_idx: Option<usize> = None;

    egui::CentralPanel::default().show(contexts.ctx_mut(), |ui| {
        let available_size = ui.available_size();
        let panel_height = available_size.y - 100.0;

        // Header
        ui.vertical_centered(|ui| {
            ui.add_space(15.0);
            ui.heading(egui::RichText::new("ApexSim Track Editor").size(36.0));
            ui.add_space(5.0);
            ui.label("Visualize and edit race track data");
            ui.add_space(15.0);
        });

        // Main content area - horizontal split
        ui.horizontal(|ui| {
            // Left panel - track list
            egui::Frame::none()
                .fill(ui.visuals().window_fill)
                .stroke(ui.visuals().window_stroke)
                .rounding(8.0)
                .inner_margin(12.0)
                .show(ui, |ui| {
                    ui.set_width(280.0);
                    ui.set_height(panel_height);

                    ui.vertical(|ui| {
                        // Folder selection
                        ui.label("Tracks folder:");
                        ui.add_space(4.0);

                        if editor_state.folder_input.is_empty() {
                            if let Some(folder) = &editor_state.tracks_folder {
                                editor_state.folder_input = folder.display().to_string();
                            }
                        }

                        ui.horizontal(|ui| {
                            let response = ui.add(
                                egui::TextEdit::singleline(&mut editor_state.folder_input)
                                    .desired_width(ui.available_width() - 70.0)
                                    .hint_text("Path...")
                            );

                            if response.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) {
                                let path = PathBuf::from(&editor_state.folder_input);
                                if path.exists() && path.is_dir() {
                                    editor_state.tracks_folder = Some(path);
                                    scan_tracks_folder(&mut editor_state);
                                }
                            }

                            if ui.button("Browse").clicked() {
                                if let Some(folder) = rfd::FileDialog::new()
                                    .set_title("Select tracks folder")
                                    .pick_folder()
                                {
                                    editor_state.folder_input = folder.display().to_string();
                                    editor_state.tracks_folder = Some(folder);
                                    scan_tracks_folder(&mut editor_state);
                                }
                            }
                        });

                        ui.add_space(10.0);
                        ui.separator();
                        ui.add_space(10.0);

                        if editor_state.available_tracks.is_empty() {
                            ui.label("No tracks found.");
                        } else {
                            ui.label(format!("{} tracks:", editor_state.available_tracks.len()));
                            ui.add_space(8.0);

                            // Scrollable track list
                            let scroll_height = ui.available_height() - 50.0;
                            let mut new_selection = editor_state.selected_track_index;

                            egui::ScrollArea::vertical()
                                .max_height(scroll_height.max(100.0))
                                .auto_shrink([false, false])
                                .show(ui, |ui| {
                                    ui.set_width(ui.available_width());
                                    for (idx, track) in editor_state.available_tracks.iter().enumerate() {
                                        let is_selected = editor_state.selected_track_index == Some(idx);

                                        if ui.selectable_label(is_selected, &track.name).clicked() {
                                            new_selection = Some(idx);
                                            if editor_state.selected_track_index != new_selection {
                                                selection_changed = true;
                                                new_selection_idx = new_selection;
                                            }
                                        }
                                    }
                                });
                            editor_state.selected_track_index = new_selection;

                            ui.add_space(8.0);

                            // Edit button at the bottom
                            let can_edit = editor_state.selected_track_index.is_some();
                            ui.add_enabled_ui(can_edit, |ui| {
                                if ui.button(egui::RichText::new("Edit Track").size(16.0)).clicked() {
                                    if let Some(idx) = editor_state.selected_track_index {
                                        let track_entry = editor_state.available_tracks[idx].clone();

                                        if let Ok(yaml_content) = std::fs::read_to_string(&track_entry.yaml_path) {
                                            if let Ok(track_data) = serde_yaml::from_str::<TrackFileFormat>(&yaml_content) {
                                                let mut terrain_data = std::fs::read(&track_entry.terrain_path)
                                                    .ok()
                                                    .and_then(|bytes| rmp_serde::from_slice::<ProceduralWorldData>(&bytes).ok());

                                                let mut track_data = track_data;

                                                // Load and apply overlay if it exists
                                                if let Ok(Some(overlay)) = TrackEditorOverlay::load(&track_entry.overlay_path) {
                                                    overlay.apply(&mut track_data, &mut terrain_data);
                                                }

                                                // Snapshot terrain heights for diffing on save
                                                let original_terrain_heights = terrain_data.as_ref()
                                                    .and_then(|t| t.heightmap.as_ref())
                                                    .map(|h| h.heights.clone());

                                                editor_state.loaded_track = Some(LoadedTrack {
                                                    name: track_entry.name.clone(),
                                                    track_data,
                                                    terrain_data,
                                                    overlay_path: track_entry.overlay_path.clone(),
                                                    dirty: false,
                                                    original_terrain_heights,
                                                });

                                                next_state.set(AppState::Editor);
                                            }
                                        }
                                    }
                                }
                            });
                        }
                    });
                });

            ui.add_space(10.0);

            // Right panel - track preview
            egui::Frame::none()
                .fill(ui.visuals().extreme_bg_color)
                .stroke(ui.visuals().window_stroke)
                .rounding(8.0)
                .inner_margin(12.0)
                .show(ui, |ui| {
                    ui.set_width(ui.available_width() - 20.0);
                    ui.set_height(panel_height);

                    if let Some(track_data) = &editor_state.preview_track {
                        // Track name header
                        ui.horizontal(|ui| {
                            ui.heading(&track_data.name);
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                ui.label(format!("{} nodes", track_data.nodes.len()));
                                if !track_data.raceline.is_empty() {
                                    ui.label(format!("| {} raceline pts", track_data.raceline.len()));
                                }
                            });
                        });
                        ui.add_space(8.0);

                        // Draw 2D track preview - use remaining height
                        let available_width = ui.available_width();
                        let available_height = ui.available_height();
                        let preview_size = egui::vec2(available_width, available_height.max(200.0));

                        // Allocate the space for the preview
                        let (response, painter) = ui.allocate_painter(preview_size, egui::Sense::hover());
                        let rect = response.rect;

                        // Calculate bounds of the track
                        if !track_data.nodes.is_empty() {
                            let mut min_x = f32::MAX;
                            let mut max_x = f32::MIN;
                            let mut min_y = f32::MAX;
                            let mut max_y = f32::MIN;

                            for node in &track_data.nodes {
                                min_x = min_x.min(node.x);
                                max_x = max_x.max(node.x);
                                min_y = min_y.min(node.y);
                                max_y = max_y.max(node.y);
                            }

                            // Add padding
                            let padding = 40.0;
                            let track_width = max_x - min_x;
                            let track_height = max_y - min_y;

                            // Calculate scale to fit in preview area
                            let scale_x = (rect.width() - padding * 2.0) / track_width.max(1.0);
                            let scale_y = (rect.height() - padding * 2.0) / track_height.max(1.0);
                            let scale = scale_x.min(scale_y);

                            // Center offset
                            let center_x = (min_x + max_x) / 2.0;
                            let center_y = (min_y + max_y) / 2.0;

                            let to_screen = |x: f32, y: f32| -> egui::Pos2 {
                                egui::Pos2::new(
                                    rect.center().x + (x - center_x) * scale,
                                    rect.center().y - (y - center_y) * scale, // Flip Y for screen coords
                                )
                            };

                            // Draw track width as a thick line (simplified)
                            let default_width = track_data.default_width.max(10.0);
                            let track_stroke_width = (default_width * scale).clamp(4.0, 50.0);

                            // Draw track surface (lighter gray for visibility)
                            let track_points: Vec<egui::Pos2> = track_data.nodes
                                .iter()
                                .map(|n| to_screen(n.x, n.y))
                                .collect();

                            if track_points.len() >= 2 {
                                // Draw thick track surface
                                for i in 0..track_points.len() - 1 {
                                    painter.line_segment(
                                        [track_points[i], track_points[i + 1]],
                                        egui::Stroke::new(track_stroke_width, egui::Color32::from_rgb(80, 80, 90)),
                                    );
                                }

                                // Close the loop if it's a closed track
                                if track_data.closed_loop && track_points.len() > 2 {
                                    painter.line_segment(
                                        [*track_points.last().unwrap(), track_points[0]],
                                        egui::Stroke::new(track_stroke_width, egui::Color32::from_rgb(80, 80, 90)),
                                    );
                                }

                                // Draw centerline (white/light for visibility)
                                for i in 0..track_points.len() - 1 {
                                    painter.line_segment(
                                        [track_points[i], track_points[i + 1]],
                                        egui::Stroke::new(2.0, egui::Color32::from_rgb(200, 200, 210)),
                                    );
                                }

                                if track_data.closed_loop && track_points.len() > 2 {
                                    painter.line_segment(
                                        [*track_points.last().unwrap(), track_points[0]],
                                        egui::Stroke::new(2.0, egui::Color32::from_rgb(200, 200, 210)),
                                    );
                                }
                            }

                            // Draw raceline if available (bright green)
                            if !track_data.raceline.is_empty() {
                                let raceline_points: Vec<egui::Pos2> = track_data.raceline
                                    .iter()
                                    .map(|p| to_screen(p.x, p.y))
                                    .collect();

                                for i in 0..raceline_points.len() - 1 {
                                    painter.line_segment(
                                        [raceline_points[i], raceline_points[i + 1]],
                                        egui::Stroke::new(2.5, egui::Color32::from_rgb(50, 255, 100)),
                                    );
                                }
                            }

                            // Draw start/finish marker
                            if !track_data.nodes.is_empty() {
                                let start = to_screen(track_data.nodes[0].x, track_data.nodes[0].y);
                                painter.circle_filled(start, 10.0, egui::Color32::from_rgb(255, 60, 60));
                                painter.circle_stroke(start, 10.0, egui::Stroke::new(2.0, egui::Color32::WHITE));
                            }
                        }
                    } else {
                        // No track selected
                        ui.centered_and_justified(|ui| {
                            ui.label(egui::RichText::new("Select a track to preview").size(18.0).color(egui::Color32::GRAY));
                        });
                    }
                });
        });
    });

    // Load preview data if selection changed
    if selection_changed {
        if let Some(idx) = new_selection_idx {
            if idx < editor_state.available_tracks.len() {
                let track_entry = &editor_state.available_tracks[idx];
                if let Ok(yaml_content) = std::fs::read_to_string(&track_entry.yaml_path) {
                    if let Ok(track_data) = serde_yaml::from_str::<TrackFileFormat>(&yaml_content) {
                        editor_state.preview_track = Some(track_data);
                    }
                }
            }
        }
    }
}

fn setup_editor(
    mut commands: Commands,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    editor_state: Res<EditorState>,
    mut camera_state: ResMut<CameraState>,
    mut test_mode_state: ResMut<TestModeState>,
    mut track_edges_res: ResMut<TrackEdgesResource>,
) {
    // Add light
    commands.spawn((
        DirectionalLightBundle {
            directional_light: DirectionalLight {
                illuminance: 15000.0,
                shadows_enabled: true,
                ..default()
            },
            transform: Transform::from_xyz(500.0, 500.0, 500.0).looking_at(Vec3::ZERO, Vec3::Z),
            ..default()
        },
        TrackMeshEntity,
    ));

    // Ambient light
    commands.insert_resource(AmbientLight {
        color: Color::WHITE,
        brightness: 500.0,
    });

    if let Some(loaded) = &editor_state.loaded_track {
        let nodes = &loaded.track_data.nodes;

        // Position camera at start/finish line (first node)
        if nodes.len() >= 2 {
            let start_node = &nodes[0];
            let next_node = &nodes[1];

            // Calculate heading from start to next node
            let dx = next_node.x - start_node.x;
            let dy = next_node.y - start_node.y;
            let heading = dy.atan2(dx);

            // Position camera behind the start line, looking down the track
            camera_state.focus = Vec3::new(start_node.x, start_node.y, start_node.z + 2.0);
            camera_state.distance = 100.0;
            camera_state.yaw = heading + std::f32::consts::PI; // Look in track direction
            camera_state.pitch = 0.3;  // Slight elevation angle
        } else if !nodes.is_empty() {
            camera_state.focus = Vec3::new(nodes[0].x, nodes[0].y, nodes[0].z);
            camera_state.distance = 100.0;
            camera_state.yaw = 0.0;
            camera_state.pitch = 0.3;
        }

        // Compute track edge polylines and store as resource
        let edges = track_mesh::compute_track_edges(nodes, loaded.track_data.closed_loop);
        track_edges_res.edges = Some(edges);

        // Generate track mesh from centerline nodes
        let track_mesh = track_mesh::generate_track_mesh(nodes, loaded.track_data.closed_loop);

        commands.spawn((
            PbrBundle {
                mesh: meshes.add(track_mesh),
                material: materials.add(StandardMaterial {
                    base_color: Color::srgb(0.3, 0.3, 0.35),
                    perceptual_roughness: 0.8,
                    cull_mode: None,  // Render both sides to avoid missing triangles
                    ..default()
                }),
                ..default()
            },
            TrackMeshEntity,
        ));

        // Generate raceline mesh if raceline data exists
        if !loaded.track_data.raceline.is_empty() {
            let (raceline_mesh, raceline_length) = generate_raceline_mesh(&loaded.track_data.raceline);
            test_mode_state.raceline_length = raceline_length;
            test_mode_state.progress = 0.0;
            test_mode_state.playing = false;

            commands.spawn((
                PbrBundle {
                    mesh: meshes.add(raceline_mesh),
                    material: materials.add(StandardMaterial {
                        base_color: Color::srgb(0.2, 0.9, 0.3),
                        emissive: bevy::color::LinearRgba::new(0.1, 0.5, 0.15, 1.0),
                        perceptual_roughness: 0.3,
                        ..default()
                    }),
                    ..default()
                },
                RacelineMeshEntity,
            ));
        }

        // Generate terrain mesh if available
        if let Some(terrain) = &loaded.terrain_data {
            if let Some(heightmap) = &terrain.heightmap {
                println!("Terrain heightmap: {}x{}, cell_size={}, origin=({}, {})",
                    heightmap.width, heightmap.height, heightmap.cell_size_m,
                    heightmap.origin_x, heightmap.origin_y);

                // Debug: compare track and terrain heights at multiple points
                if !nodes.is_empty() {
                    let mut max_diff = 0.0f32;
                    let mut max_diff_idx = 0;
                    for (i, node) in nodes.iter().enumerate() {
                        let terrain_z = heightmap.sample(node.x, node.y);
                        let diff = terrain_z - node.z;
                        if diff.abs() > max_diff.abs() {
                            max_diff = diff;
                            max_diff_idx = i;
                        }
                    }
                    let node = &nodes[max_diff_idx];
                    let terrain_z = heightmap.sample(node.x, node.y);
                    println!("Max height diff at node {} ({:.1}, {:.1}): track_z={:.2}, terrain_z={:.2}, diff={:.2}",
                        max_diff_idx, node.x, node.y, node.z, terrain_z, max_diff);

                    // Also show terrain height range
                    let min_h = heightmap.heights.iter().cloned().fold(f32::INFINITY, f32::min);
                    let max_h = heightmap.heights.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
                    println!("Terrain height range: {:.2} to {:.2}", min_h, max_h);

                    // Track height range
                    let track_min_z = nodes.iter().map(|n| n.z).fold(f32::INFINITY, f32::min);
                    let track_max_z = nodes.iter().map(|n| n.z).fold(f32::NEG_INFINITY, f32::max);
                    println!("Track height range: {:.2} to {:.2}", track_min_z, track_max_z);
                }

                let terrain_mesh = generate_terrain_mesh(heightmap, nodes);

                commands.spawn((
                    PbrBundle {
                        mesh: meshes.add(terrain_mesh),
                        material: materials.add(StandardMaterial {
                            // Use white base so vertex colors show through
                            base_color: Color::WHITE,
                            perceptual_roughness: 0.95,
                            cull_mode: None,  // Render both sides
                            ..default()
                        }),
                        ..default()
                    },
                    TerrainMeshEntity,
                ));

                // Add wireframe grid overlay for elevation visualization
                let grid_mesh = generate_terrain_grid(heightmap, nodes);
                commands.spawn((
                    PbrBundle {
                        mesh: meshes.add(grid_mesh),
                        material: materials.add(StandardMaterial {
                            base_color: Color::srgba(0.0, 0.0, 0.0, 0.5),
                            unlit: true,
                            alpha_mode: bevy::prelude::AlphaMode::Blend,
                            ..default()
                        }),
                        transform: Transform::from_xyz(0.0, 0.0, 0.1), // Slightly above terrain
                        ..default()
                    },
                    TerrainMeshEntity,
                ));
            } else {
                println!("Terrain data exists but no heightmap!");
            }
        } else {
            println!("No terrain data loaded!");
        }

        // Generate kerb meshes from per-node kerb data using precomputed edges
        if let Some(edges) = &track_edges_res.edges {
            let kerb_color = Color::srgb(0.9, 0.2, 0.1);
            for side in [KerbSide::Left, KerbSide::Right] {
                let kerb_meshes = generate_kerb_meshes_from_edges(
                    &loaded.track_data.nodes,
                    edges,
                    side,
                );
                for mesh in kerb_meshes {
                    commands.spawn((
                        PbrBundle {
                            mesh: meshes.add(mesh),
                            material: materials.add(StandardMaterial {
                                base_color: kerb_color,
                                perceptual_roughness: 0.7,
                                ..default()
                            }),
                            ..default()
                        },
                        KerbMeshEntity,
                    ));
                }
            }
        }

        // Spawn existing placed trees
        if let Some(terrain) = &loaded.terrain_data {
            if !terrain.placed_trees.is_empty() {
                if let Some(mesh) = generate_all_trees_mesh(&terrain.placed_trees) {
                    commands.spawn((
                        PbrBundle {
                            mesh: meshes.add(mesh),
                            material: materials.add(StandardMaterial {
                                base_color: Color::WHITE,
                                perceptual_roughness: 0.9,
                                cull_mode: None,
                                ..default()
                            }),
                            ..default()
                        },
                        TreeMeshEntity,
                    ));
                }
            }
        }

        // Calculate initial camera position
        let camera_pos = calculate_camera_position(&camera_state);

        // Spawn camera
        commands.spawn((
            Camera3dBundle {
                transform: Transform::from_translation(camera_pos)
                    .looking_at(camera_state.focus, Vec3::Z),
                ..default()
            },
            EditorCamera,
        ));
    }
}

fn calculate_camera_position(state: &CameraState) -> Vec3 {
    // Spherical coordinates: pitch is elevation angle (0 = horizontal, positive = up)
    // yaw is azimuth angle around Z axis
    let cos_pitch = state.pitch.cos();
    let sin_pitch = state.pitch.sin();
    let cos_yaw = state.yaw.cos();
    let sin_yaw = state.yaw.sin();

    let x = state.distance * cos_pitch * cos_yaw;
    let y = state.distance * cos_pitch * sin_yaw;
    let z = state.distance * sin_pitch;

    state.focus + Vec3::new(x, y, z)
}

fn generate_terrain_mesh(heightmap: &TerrainHeightmap, track_nodes: &[track_data::TrackNode]) -> Mesh {
    let mut positions: Vec<[f32; 3]> = Vec::new();
    let mut normals: Vec<[f32; 3]> = Vec::new();
    let mut uvs: Vec<[f32; 2]> = Vec::new();
    let mut colors: Vec<[f32; 4]> = Vec::new();
    let mut indices: Vec<u32> = Vec::new();

    // Compute height range for color gradient
    let min_h = heightmap.heights.iter().cloned().fold(f32::INFINITY, f32::min);
    let max_h = heightmap.heights.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
    let height_range = (max_h - min_h).max(1.0);

    // Build a list of track centerline points with their widths for carving
    // We use a wider margin to ensure terrain stays clear of track edges
    let track_carve_margin = 5.0; // Extra meters beyond track width
    let track_depth_below = 3.0;  // How far below track surface to push terrain

    // Helper function: compute distance from point to line segment and interpolation t
    fn point_to_segment_dist(px: f32, py: f32, ax: f32, ay: f32, bx: f32, by: f32) -> (f32, f32) {
        let abx = bx - ax;
        let aby = by - ay;
        let apx = px - ax;
        let apy = py - ay;

        let ab_len_sq = abx * abx + aby * aby;
        if ab_len_sq < 0.0001 {
            // Degenerate segment (a == b)
            return ((apx * apx + apy * apy).sqrt(), 0.0);
        }

        // Project point onto line, clamped to segment
        let t = ((apx * abx + apy * aby) / ab_len_sq).clamp(0.0, 1.0);

        // Closest point on segment
        let closest_x = ax + t * abx;
        let closest_y = ay + t * aby;

        let dx = px - closest_x;
        let dy = py - closest_y;
        ((dx * dx + dy * dy).sqrt(), t)
    }

    // Blend radius: terrain will be blended towards track elevation within this distance
    let blend_radius = 100.0; // Blend terrain over 100 meters from track

    // Helper: find min distance to track and get terrain height adjustment
    // Returns (min_distance, Option<(track_z, blend_factor)>)
    let get_track_info = |world_x: f32, world_y: f32| -> (f32, Option<(f32, f32)>) {
        let mut min_dist = f32::MAX;
        let mut closest_track_z: Option<f32> = None;

        for i in 0..track_nodes.len() {
            let node_a = &track_nodes[i];
            let node_b = &track_nodes[(i + 1) % track_nodes.len()];

            let (dist_2d, t) = point_to_segment_dist(
                world_x, world_y,
                node_a.x, node_a.y,
                node_b.x, node_b.y
            );

            if dist_2d < min_dist {
                min_dist = dist_2d;

                // Interpolate track Z at closest point
                let track_z = node_a.z + (node_b.z - node_a.z) * t;
                closest_track_z = Some(track_z);
            }
        }

        if let Some(track_z) = closest_track_z {
            // Get track half-width for carve calculation
            let get_half_width = |node: &track_data::TrackNode| -> f32 {
                if let (Some(wl), Some(wr)) = (node.width_left, node.width_right) {
                    (wl + wr) / 2.0
                } else if let Some(w) = node.width {
                    w / 2.0
                } else {
                    6.0
                }
            };

            // Use average track width
            let avg_half_width: f32 = track_nodes.iter()
                .map(|n| get_half_width(n))
                .sum::<f32>() / track_nodes.len() as f32;

            let carve_radius = avg_half_width + track_carve_margin;

            if min_dist < carve_radius {
                // Under the track: force terrain below track surface
                return (min_dist, Some((track_z - track_depth_below, 1.0)));
            } else if min_dist < blend_radius {
                // Near the track: blend terrain towards track elevation
                // Blend factor: 1.0 at carve_radius, 0.0 at blend_radius
                let blend_factor = 1.0 - (min_dist - carve_radius) / (blend_radius - carve_radius);
                return (min_dist, Some((track_z, blend_factor)));
            }
        }

        (min_dist, None)
    };

    // Use adaptive resolution: fine near track, coarse far away
    // Base step for coarse areas
    let coarse_step = 2.max(heightmap.width / 256);
    let fine_step = 1.max(coarse_step / 4);  // 4x resolution near track
    let near_track_dist = 30.0;  // Distance threshold for high resolution

    // First pass: determine which coarse cells need subdivision
    let coarse_width = heightmap.width / coarse_step;
    let coarse_height = heightmap.height / coarse_step;

    // Build vertex grid with adaptive resolution
    // We'll use a hashmap to track vertex indices at each grid position
    use std::collections::HashMap;
    let mut vertex_map: HashMap<(usize, usize), u32> = HashMap::new();

    // Helper to add a vertex and return its index
    let add_vertex = |gx: usize, gy: usize, vertex_map: &mut HashMap<(usize, usize), u32>,
                          positions: &mut Vec<[f32; 3]>, normals: &mut Vec<[f32; 3]>,
                          uvs: &mut Vec<[f32; 2]>, colors: &mut Vec<[f32; 4]>| -> u32 {
        if let Some(&idx) = vertex_map.get(&(gx, gy)) {
            return idx;
        }

        let world_x = heightmap.origin_x + gx as f32 * heightmap.cell_size_m;
        let world_y = heightmap.origin_y + gy as f32 * heightmap.cell_size_m;
        let raw_height = heightmap.get_height(gx, gy);

        // Get track info and blend terrain height
        let (_, track_blend) = get_track_info(world_x, world_y);
        let world_z = match track_blend {
            Some((target_z, blend_factor)) => {
                // Blend between raw terrain height and target (track-relative) height
                raw_height * (1.0 - blend_factor) + target_z * blend_factor
            }
            None => raw_height,
        };

        let idx = positions.len() as u32;
        positions.push([world_x, world_y, world_z]);
        normals.push([0.0, 0.0, 1.0]);
        uvs.push([
            gx as f32 / heightmap.width as f32,
            gy as f32 / heightmap.height as f32,
        ]);

        let t = (raw_height - min_h) / height_range;
        let r = 0.2 + t * 0.4;
        let g = 0.5 - t * 0.2;
        let b = 0.1 + t * 0.1;
        colors.push([r, g, b, 1.0]);

        vertex_map.insert((gx, gy), idx);
        idx
    };

    // Generate triangles with adaptive resolution
    for cy in 0..(coarse_height - 1) {
        for cx in 0..(coarse_width - 1) {
            let gx0 = cx * coarse_step;
            let gy0 = cy * coarse_step;
            let gx1 = ((cx + 1) * coarse_step).min(heightmap.width - 1);
            let gy1 = ((cy + 1) * coarse_step).min(heightmap.height - 1);

            // Check if this cell is near the track
            let center_x = heightmap.origin_x + ((gx0 + gx1) / 2) as f32 * heightmap.cell_size_m;
            let center_y = heightmap.origin_y + ((gy0 + gy1) / 2) as f32 * heightmap.cell_size_m;
            let (dist_to_track, _) = get_track_info(center_x, center_y);

            if dist_to_track < near_track_dist {
                // High resolution: subdivide this cell
                for sy in 0..((gy1 - gy0) / fine_step).max(1) {
                    for sx in 0..((gx1 - gx0) / fine_step).max(1) {
                        let x0 = gx0 + sx * fine_step;
                        let y0 = gy0 + sy * fine_step;
                        let x1 = (x0 + fine_step).min(gx1);
                        let y1 = (y0 + fine_step).min(gy1);

                        if x1 <= x0 || y1 <= y0 { continue; }

                        let v00 = add_vertex(x0, y0, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);
                        let v10 = add_vertex(x1, y0, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);
                        let v01 = add_vertex(x0, y1, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);
                        let v11 = add_vertex(x1, y1, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);

                        indices.push(v00);
                        indices.push(v01);
                        indices.push(v10);
                        indices.push(v10);
                        indices.push(v01);
                        indices.push(v11);
                    }
                }
            } else {
                // Coarse resolution: single quad for this cell
                let v00 = add_vertex(gx0, gy0, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);
                let v10 = add_vertex(gx1, gy0, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);
                let v01 = add_vertex(gx0, gy1, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);
                let v11 = add_vertex(gx1, gy1, &mut vertex_map, &mut positions, &mut normals, &mut uvs, &mut colors);

                indices.push(v00);
                indices.push(v01);
                indices.push(v10);
                indices.push(v10);
                indices.push(v01);
                indices.push(v11);
            }
        }
    }

    // Recalculate smooth normals from face normals
    let mut normal_accumulators: Vec<Vec3> = vec![Vec3::ZERO; positions.len()];

    for i in (0..indices.len()).step_by(3) {
        let i0 = indices[i] as usize;
        let i1 = indices[i + 1] as usize;
        let i2 = indices[i + 2] as usize;

        let p0 = Vec3::from_array(positions[i0]);
        let p1 = Vec3::from_array(positions[i1]);
        let p2 = Vec3::from_array(positions[i2]);

        let edge1 = p1 - p0;
        let edge2 = p2 - p0;
        let face_normal = edge1.cross(edge2).normalize_or_zero();

        normal_accumulators[i0] += face_normal;
        normal_accumulators[i1] += face_normal;
        normal_accumulators[i2] += face_normal;
    }

    for (i, acc) in normal_accumulators.iter().enumerate() {
        let n = acc.normalize_or_zero();
        normals[i] = [n.x, n.y, n.z];
    }

    let mut mesh = Mesh::new(bevy::render::mesh::PrimitiveTopology::TriangleList, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);
    mesh.insert_attribute(Mesh::ATTRIBUTE_NORMAL, normals);
    mesh.insert_attribute(Mesh::ATTRIBUTE_UV_0, uvs);
    mesh.insert_attribute(Mesh::ATTRIBUTE_COLOR, colors);
    mesh.insert_indices(bevy::render::mesh::Indices::U32(indices));

    mesh
}

/// Generate a wireframe grid overlay for terrain elevation visualization
fn generate_terrain_grid(heightmap: &TerrainHeightmap, track_nodes: &[track_data::TrackNode]) -> Mesh {
    let mut positions: Vec<[f32; 3]> = Vec::new();

    // Use coarser grid for wireframe (every 20 cells or so)
    let grid_step = 10.max(heightmap.width / 50);

    let track_carve_margin = 5.0;
    let track_depth_below = 3.0;
    let blend_radius = 100.0;

    // Helper: find min distance to track and get blend info (same as in generate_terrain_mesh)
    let get_track_info = |world_x: f32, world_y: f32| -> Option<(f32, f32)> {
        fn point_to_segment_dist(px: f32, py: f32, ax: f32, ay: f32, bx: f32, by: f32) -> (f32, f32) {
            let abx = bx - ax;
            let aby = by - ay;
            let apx = px - ax;
            let apy = py - ay;

            let ab_len_sq = abx * abx + aby * aby;
            if ab_len_sq < 0.0001 {
                return ((apx * apx + apy * apy).sqrt(), 0.0);
            }

            let t = ((apx * abx + apy * aby) / ab_len_sq).clamp(0.0, 1.0);
            let closest_x = ax + t * abx;
            let closest_y = ay + t * aby;
            let dx = px - closest_x;
            let dy = py - closest_y;
            ((dx * dx + dy * dy).sqrt(), t)
        }

        let mut min_dist = f32::MAX;
        let mut closest_track_z: Option<f32> = None;

        for i in 0..track_nodes.len() {
            let node_a = &track_nodes[i];
            let node_b = &track_nodes[(i + 1) % track_nodes.len()];

            let (dist_2d, t) = point_to_segment_dist(
                world_x, world_y,
                node_a.x, node_a.y,
                node_b.x, node_b.y
            );

            if dist_2d < min_dist {
                min_dist = dist_2d;
                let track_z = node_a.z + (node_b.z - node_a.z) * t;
                closest_track_z = Some(track_z);
            }
        }

        if let Some(track_z) = closest_track_z {
            let avg_half_width = 6.0; // Simplified
            let carve_radius = avg_half_width + track_carve_margin;

            if min_dist < carve_radius {
                return Some((track_z - track_depth_below, 1.0));
            } else if min_dist < blend_radius {
                let blend_factor = 1.0 - (min_dist - carve_radius) / (blend_radius - carve_radius);
                return Some((track_z, blend_factor));
            }
        }

        None
    };

    let get_height = |gx: usize, gy: usize| -> f32 {
        let world_x = heightmap.origin_x + gx as f32 * heightmap.cell_size_m;
        let world_y = heightmap.origin_y + gy as f32 * heightmap.cell_size_m;
        let raw_height = heightmap.get_height(gx, gy);
        match get_track_info(world_x, world_y) {
            Some((target_z, blend_factor)) => {
                raw_height * (1.0 - blend_factor) + target_z * blend_factor
            }
            None => raw_height,
        }
    };

    // Generate horizontal lines (along X)
    for gy in (0..heightmap.height).step_by(grid_step) {
        for gx in 0..heightmap.width {
            let world_x = heightmap.origin_x + gx as f32 * heightmap.cell_size_m;
            let world_y = heightmap.origin_y + gy as f32 * heightmap.cell_size_m;
            let world_z = get_height(gx, gy) + 0.2; // Slightly above terrain
            positions.push([world_x, world_y, world_z]);
        }
    }

    // Generate vertical lines (along Y)
    for gx in (0..heightmap.width).step_by(grid_step) {
        for gy in 0..heightmap.height {
            let world_x = heightmap.origin_x + gx as f32 * heightmap.cell_size_m;
            let world_y = heightmap.origin_y + gy as f32 * heightmap.cell_size_m;
            let world_z = get_height(gx, gy) + 0.2;
            positions.push([world_x, world_y, world_z]);
        }
    }

    let mut mesh = Mesh::new(bevy::render::mesh::PrimitiveTopology::LineStrip, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);

    mesh
}

/// Generate a flat ribbon mesh for the raceline visualization
/// Returns the mesh and the total length of the raceline
fn generate_raceline_mesh(raceline: &[RacelinePoint]) -> (Mesh, f32) {
    if raceline.len() < 2 {
        return (Mesh::new(bevy::render::mesh::PrimitiveTopology::TriangleList, default()), 0.0);
    }

    let ribbon_width = 1.0;
    let z_offset = 0.2; // Offset above track surface

    let mut positions: Vec<[f32; 3]> = Vec::new();
    let mut normals: Vec<[f32; 3]> = Vec::new();
    let mut uvs: Vec<[f32; 2]> = Vec::new();
    let mut indices: Vec<u32> = Vec::new();

    // Calculate total length for UV mapping
    let mut total_length = 0.0f32;
    for i in 1..raceline.len() {
        let dx = raceline[i].x - raceline[i - 1].x;
        let dy = raceline[i].y - raceline[i - 1].y;
        total_length += (dx * dx + dy * dy).sqrt();
    }

    let mut accumulated_length = 0.0f32;

    for i in 0..raceline.len() {
        let point = &raceline[i];
        let center = Vec3::new(point.x, point.y, point.z + z_offset);

        // Calculate 2D direction (ignore Z for perpendicular calculation)
        let dir_2d = if i == 0 {
            let next = &raceline[1];
            Vec2::new(next.x - point.x, next.y - point.y).normalize_or_zero()
        } else if i == raceline.len() - 1 {
            let prev = &raceline[i - 1];
            Vec2::new(point.x - prev.x, point.y - prev.y).normalize_or_zero()
        } else {
            // Average of incoming and outgoing directions for smooth corners
            let prev = &raceline[i - 1];
            let next = &raceline[i + 1];
            let d1 = Vec2::new(point.x - prev.x, point.y - prev.y).normalize_or_zero();
            let d2 = Vec2::new(next.x - point.x, next.y - point.y).normalize_or_zero();
            ((d1 + d2) * 0.5).normalize_or_zero()
        };

        // Perpendicular in 2D (rotate 90 degrees)
        let perp = Vec2::new(-dir_2d.y, dir_2d.x) * ribbon_width * 0.5;

        // Left and right vertices
        let left = Vec3::new(center.x + perp.x, center.y + perp.y, center.z);
        let right = Vec3::new(center.x - perp.x, center.y - perp.y, center.z);

        let u = accumulated_length / total_length.max(1.0);

        // Add left vertex
        positions.push([left.x, left.y, left.z]);
        normals.push([0.0, 0.0, 1.0]);
        uvs.push([u, 0.0]);

        // Add right vertex
        positions.push([right.x, right.y, right.z]);
        normals.push([0.0, 0.0, 1.0]);
        uvs.push([u, 1.0]);

        // Update accumulated length
        if i > 0 {
            let prev = &raceline[i - 1];
            let dx = point.x - prev.x;
            let dy = point.y - prev.y;
            accumulated_length += (dx * dx + dy * dy).sqrt();
        }
    }

    // Generate indices for ribbon quads
    for i in 0..(raceline.len() - 1) {
        let base = (i * 2) as u32;
        // Two triangles per quad
        // Triangle 1: left_i, left_i+1, right_i
        indices.push(base);
        indices.push(base + 2);
        indices.push(base + 1);
        // Triangle 2: right_i, left_i+1, right_i+1
        indices.push(base + 1);
        indices.push(base + 2);
        indices.push(base + 3);
    }

    let mut mesh = Mesh::new(bevy::render::mesh::PrimitiveTopology::TriangleList, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);
    mesh.insert_attribute(Mesh::ATTRIBUTE_NORMAL, normals);
    mesh.insert_attribute(Mesh::ATTRIBUTE_UV_0, uvs);
    mesh.insert_indices(bevy::render::mesh::Indices::U32(indices));

    (mesh, total_length)
}

/// Get position and direction along the raceline at a given distance
fn sample_raceline(raceline: &[RacelinePoint], distance: f32) -> (Vec3, Vec3) {
    if raceline.len() < 2 {
        return (Vec3::ZERO, Vec3::X);
    }

    let mut accumulated = 0.0f32;

    for i in 1..raceline.len() {
        let prev = &raceline[i - 1];
        let curr = &raceline[i];
        let dx = curr.x - prev.x;
        let dy = curr.y - prev.y;
        let dz = curr.z - prev.z;
        let segment_len = (dx * dx + dy * dy + dz * dz).sqrt();

        if accumulated + segment_len >= distance {
            // Interpolate within this segment
            let t = (distance - accumulated) / segment_len.max(0.001);
            let pos = Vec3::new(
                prev.x + dx * t,
                prev.y + dy * t,
                prev.z + dz * t,
            );
            let dir = Vec3::new(dx, dy, dz).normalize_or_zero();
            return (pos, dir);
        }

        accumulated += segment_len;
    }

    // Past the end, return last point
    let last = raceline.last().unwrap();
    let prev = &raceline[raceline.len() - 2];
    let dir = Vec3::new(last.x - prev.x, last.y - prev.y, last.z - prev.z).normalize_or_zero();
    (Vec3::new(last.x, last.y, last.z), dir)
}

fn editor_ui_system(
    mut contexts: EguiContexts,
    mut editor_state: ResMut<EditorState>,
    mut next_state: ResMut<NextState<AppState>>,
    camera_state: Res<CameraState>,
    mut test_mode_state: ResMut<TestModeState>,
) {
    // Ctrl+S save shortcut
    let save_requested = contexts.ctx_mut().input(|i| {
        i.modifiers.ctrl && i.key_pressed(egui::Key::S)
    });

    egui::TopBottomPanel::top("top_panel").show(contexts.ctx_mut(), |ui| {
        ui.horizontal(|ui| {
            if ui.button("< Back to Track List").clicked() {
                next_state.set(AppState::Browse);
            }

            ui.separator();

            if let Some(loaded) = &editor_state.loaded_track {
                let dirty_marker = if loaded.dirty { " *" } else { "" };
                ui.label(format!("Track: {}{}", loaded.name, dirty_marker));
                ui.separator();
                ui.label(format!("Nodes: {}", loaded.track_data.nodes.len()));

                if loaded.terrain_data.is_some() {
                    ui.separator();
                    ui.label("Terrain: Loaded");
                }

                if !loaded.track_data.raceline.is_empty() {
                    ui.separator();
                    ui.label(format!("Raceline: {} pts", loaded.track_data.raceline.len()));
                }

                // Count nodes with kerbs
                let kerb_node_count = loaded.track_data.nodes.iter()
                    .filter(|n| n.kerbs.as_ref().map_or(false, |k| k.left.height > 0.0 || k.right.height > 0.0))
                    .count();
                if kerb_node_count > 0 {
                    ui.separator();
                    ui.label(format!("Kerbs: {} nodes", kerb_node_count));
                }
            }

            ui.separator();

            // Save button
            let is_dirty = editor_state.loaded_track.as_ref().map_or(false, |l| l.dirty);
            let save_btn = ui.add_enabled(is_dirty, egui::Button::new("Save"));
            if save_btn.clicked() || (save_requested && is_dirty) {
                save_track_overlay(&mut editor_state);
            }

            ui.separator();
            ui.add_space(20.0);

            // Mode toggle
            let edit_selected = editor_state.editor_mode == EditorMode::Edit;
            let test_selected = editor_state.editor_mode == EditorMode::Test;

            if ui.selectable_label(edit_selected, "Edit Mode").clicked() {
                editor_state.editor_mode = EditorMode::Edit;
            }
            if ui.selectable_label(test_selected, "Test Mode").clicked() {
                editor_state.editor_mode = EditorMode::Test;
            }
        });
    });

    match editor_state.editor_mode {
        EditorMode::Edit => {
            // Tool panel on the left side
            egui::SidePanel::left("tool_panel")
                .default_width(180.0)
                .resizable(false)
                .show(contexts.ctx_mut(), |ui| {
                    ui.heading("Tools");
                    ui.add_space(10.0);

                    // Tool selection buttons
                    let camera_selected = editor_state.active_tool == EditorTool::Camera;
                    let kerb_selected = editor_state.active_tool == EditorTool::KerbBrush;
                    let terrain_selected = editor_state.active_tool == EditorTool::TerrainLevel;
                    let tree_selected = editor_state.active_tool == EditorTool::TreeBrush;

                    ui.vertical(|ui| {
                        if ui.selectable_label(camera_selected, "Camera").clicked() {
                            editor_state.active_tool = EditorTool::Camera;
                        }
                        if ui.selectable_label(kerb_selected, "Kerb Brush").clicked() {
                            editor_state.active_tool = EditorTool::KerbBrush;
                        }
                        if ui.selectable_label(terrain_selected, "Terrain Level").clicked() {
                            editor_state.active_tool = EditorTool::TerrainLevel;
                        }
                        if ui.selectable_label(tree_selected, "Tree Brush").clicked() {
                            editor_state.active_tool = EditorTool::TreeBrush;
                        }
                    });

                    ui.add_space(20.0);
                    ui.separator();
                    ui.add_space(10.0);

                    // Tool-specific options
                    match editor_state.active_tool {
                        EditorTool::Camera => {
                            ui.label("Camera Controls:");
                            ui.add_space(5.0);
                            ui.label("WASD - Move");
                            ui.label("Left-drag - Pan");
                            ui.label("Right-drag - Rotate");
                            ui.label("Scroll - Zoom");
                        }
                        EditorTool::KerbBrush => {
                            ui.label("Kerb Brush Settings:");
                            ui.add_space(10.0);

                            ui.horizontal(|ui| {
                                ui.label("Height rate:");
                                ui.add(egui::Slider::new(&mut editor_state.kerb_brush.height_rate, 0.01..=0.2)
                                    .suffix("m/s")
                                    .fixed_decimals(2));
                            });

                            ui.add_space(5.0);

                            ui.horizontal(|ui| {
                                ui.label("Max height:");
                                ui.add(egui::Slider::new(&mut editor_state.kerb_brush.max_height, 0.05..=0.3)
                                    .suffix("m")
                                    .fixed_decimals(2));
                            });

                            ui.add_space(5.0);

                            ui.horizontal(|ui| {
                                ui.label("Kerb width:");
                                ui.add(egui::Slider::new(&mut editor_state.kerb_brush.default_width, 0.3..=2.0)
                                    .suffix("m")
                                    .fixed_decimals(1));
                            });

                            ui.add_space(15.0);
                            ui.separator();
                            ui.add_space(10.0);

                            ui.label("Usage:");
                            ui.add_space(5.0);
                            ui.label("Hold Left-click on edge");
                            ui.label("  = Increase kerb");
                            ui.add_space(3.0);
                            ui.label("Hold Right-click on edge");
                            ui.label("  = Decrease kerb");
                            ui.add_space(8.0);
                            ui.label("Middle-drag = Pan");
                            ui.label("Scroll = Zoom");
                        }
                        EditorTool::TerrainLevel => {
                            ui.label("Terrain Level Settings:");
                            ui.add_space(10.0);

                            ui.horizontal(|ui| {
                                ui.label("Brush radius:");
                                ui.add(egui::Slider::new(&mut editor_state.terrain_level_brush.radius, 5.0..=100.0)
                                    .suffix("m")
                                    .fixed_decimals(0));
                            });

                            ui.add_space(5.0);

                            ui.horizontal(|ui| {
                                ui.label("Height rate:");
                                ui.add(egui::Slider::new(&mut editor_state.terrain_level_brush.height_rate, 1.0..=20.0)
                                    .suffix("m/s")
                                    .fixed_decimals(1));
                            });

                            ui.add_space(5.0);

                            ui.horizontal(|ui| {
                                ui.label("Falloff:");
                                ui.add(egui::Slider::new(&mut editor_state.terrain_level_brush.falloff, 0.0..=1.0)
                                    .fixed_decimals(2));
                            });

                            ui.add_space(15.0);
                            ui.separator();
                            ui.add_space(10.0);

                            ui.label("Usage:");
                            ui.add_space(5.0);
                            ui.label("Hold Left-click");
                            ui.label("  = Lower terrain");
                            ui.add_space(3.0);
                            ui.label("Hold Right-click");
                            ui.label("  = Raise terrain");
                            ui.add_space(3.0);
                            ui.label("Middle-click");
                            ui.label("  = Reset to ground");
                            ui.add_space(8.0);
                            ui.label("Middle-drag = Pan");
                            ui.label("Scroll = Zoom");
                        }
                        EditorTool::TreeBrush => {
                            ui.label("Tree Brush Settings:");
                            ui.add_space(10.0);

                            // Tree type selector
                            ui.label("Tree type:");
                            ui.add_space(3.0);
                            let tree_types = tree_type_definitions();
                            for (i, tt) in tree_types.iter().enumerate() {
                                let selected = editor_state.tree_brush.selected_type == i;
                                if ui.selectable_label(selected, tt.name).clicked() {
                                    editor_state.tree_brush.selected_type = i;
                                }
                            }

                            ui.add_space(10.0);

                            ui.horizontal(|ui| {
                                ui.label("Brush radius:");
                                ui.add(egui::Slider::new(&mut editor_state.tree_brush.radius, 5.0..=50.0)
                                    .suffix("m")
                                    .fixed_decimals(0));
                            });

                            ui.add_space(5.0);

                            ui.horizontal(|ui| {
                                ui.label("Density:");
                                ui.add(egui::Slider::new(&mut editor_state.tree_brush.density, 1.0..=20.0)
                                    .suffix("/s")
                                    .fixed_decimals(0));
                            });

                            ui.add_space(5.0);

                            ui.label("Scale range:");
                            ui.horizontal(|ui| {
                                ui.add(egui::Slider::new(&mut editor_state.tree_brush.scale_min, 0.3..=1.5)
                                    .prefix("min ")
                                    .fixed_decimals(1));
                            });
                            ui.horizontal(|ui| {
                                ui.add(egui::Slider::new(&mut editor_state.tree_brush.scale_max, 0.5..=2.0)
                                    .prefix("max ")
                                    .fixed_decimals(1));
                            });

                            // Tree count
                            if let Some(loaded) = &editor_state.loaded_track {
                                if let Some(terrain) = &loaded.terrain_data {
                                    ui.add_space(5.0);
                                    ui.label(format!("Trees placed: {}", terrain.placed_trees.len()));
                                }
                            }

                            ui.add_space(15.0);
                            ui.separator();
                            ui.add_space(10.0);

                            ui.label("Usage:");
                            ui.add_space(5.0);
                            ui.label("Hold Left-click");
                            ui.label("  = Paint trees");
                            ui.add_space(3.0);
                            ui.label("Hold Right-click");
                            ui.label("  = Erase trees");
                            ui.add_space(8.0);
                            ui.label("Middle-drag = Pan");
                            ui.label("Scroll = Zoom");
                        }
                    }
                });

            // Camera info window (only show when camera tool is active)
            if editor_state.active_tool == EditorTool::Camera {
                egui::Window::new("Camera Info")
                    .default_pos([200.0, 60.0])
                    .default_size([200.0, 100.0])
                    .show(contexts.ctx_mut(), |ui| {
                        ui.label(format!("Focus: ({:.1}, {:.1}, {:.1})",
                            camera_state.focus.x, camera_state.focus.y, camera_state.focus.z));
                        ui.label(format!("Distance: {:.1}m", camera_state.distance));
                        ui.label(format!("Yaw: {:.1}°", camera_state.yaw.to_degrees()));
                        ui.label(format!("Pitch: {:.1}°", camera_state.pitch.to_degrees()));
                    });
            }
        }
        EditorMode::Test => {
            egui::Window::new("Test Mode Controls")
                .default_pos([10.0, 60.0])
                .default_size([250.0, 180.0])
                .show(contexts.ctx_mut(), |ui| {
                    ui.horizontal(|ui| {
                        if ui.button(if test_mode_state.playing { "Pause" } else { "Play" }).clicked() {
                            test_mode_state.playing = !test_mode_state.playing;
                        }
                        if ui.button("Reset").clicked() {
                            test_mode_state.progress = 0.0;
                        }
                    });

                    ui.add_space(10.0);

                    ui.horizontal(|ui| {
                        ui.label("Speed:");
                        ui.add(egui::Slider::new(&mut test_mode_state.speed, 10.0..=300.0).suffix(" m/s"));
                    });

                    ui.add_space(5.0);

                    let progress_pct = if test_mode_state.raceline_length > 0.0 {
                        (test_mode_state.progress / test_mode_state.raceline_length * 100.0).min(100.0)
                    } else {
                        0.0
                    };
                    ui.label(format!("Progress: {:.1}% ({:.0}m / {:.0}m)",
                        progress_pct,
                        test_mode_state.progress,
                        test_mode_state.raceline_length));

                    ui.add_space(10.0);

                    ui.horizontal(|ui| {
                        ui.label("Camera:");
                        if ui.selectable_label(test_mode_state.camera_type == TestCameraType::Chase, "Chase").clicked() {
                            test_mode_state.camera_type = TestCameraType::Chase;
                        }
                        if ui.selectable_label(test_mode_state.camera_type == TestCameraType::Cockpit, "Cockpit").clicked() {
                            test_mode_state.camera_type = TestCameraType::Cockpit;
                        }
                    });

                    ui.add_space(10.0);
                    ui.label("Press Space to toggle play/pause");
                });
        }
    }
}

fn camera_controller_system(
    mut camera_query: Query<&mut Transform, With<EditorCamera>>,
    mut camera_state: ResMut<CameraState>,
    keyboard: Res<ButtonInput<KeyCode>>,
    mouse_buttons: Res<ButtonInput<MouseButton>>,
    windows: Query<&Window>,
    mut scroll_events: EventReader<MouseWheel>,
    time: Res<Time>,
    mut contexts: EguiContexts,
) {
    // Don't process camera input if egui wants it
    let ctx = contexts.ctx_mut();
    if ctx.wants_pointer_input() || ctx.wants_keyboard_input() {
        scroll_events.clear();
        camera_state.last_cursor_pos = None;
        return;
    }

    let dt = time.delta_seconds();

    // Get cursor position from window
    let cursor_pos = windows
        .get_single()
        .ok()
        .and_then(|w| w.cursor_position());

    // Mouse rotation (right button drag) - use cursor position delta
    if mouse_buttons.pressed(MouseButton::Right) {
        if let (Some(current), Some(last)) = (cursor_pos, camera_state.last_cursor_pos) {
            let dx = current.x - last.x;
            let dy = current.y - last.y;

            camera_state.yaw -= dx * camera_state.mouse_sensitivity;
            // Drag up (negative dy) = look up (decrease pitch toward horizon)
            // Drag down (positive dy) = look down (increase pitch toward top-down)
            camera_state.pitch += dy * camera_state.mouse_sensitivity;
            // Clamp pitch to avoid flipping (5° to 85° elevation)
            camera_state.pitch = camera_state.pitch.clamp(0.09, 1.48);
        }
        camera_state.last_cursor_pos = cursor_pos;
    } else if mouse_buttons.pressed(MouseButton::Left) {
        // Mouse pan (left button drag) - move focus point in screen space
        if let (Some(current), Some(last)) = (cursor_pos, camera_state.last_cursor_pos) {
            let dx = current.x - last.x;
            let dy = current.y - last.y;

            // Scale pan speed with distance for consistent feel at any zoom level
            let pan_speed = camera_state.distance * 0.002;

            // Calculate right and up vectors in world space based on camera yaw
            // Right vector: perpendicular to view direction in XY plane
            let cos_yaw = camera_state.yaw.cos();
            let sin_yaw = camera_state.yaw.sin();
            let right = Vec3::new(sin_yaw, -cos_yaw, 0.0);

            // Forward vector in XY plane (for vertical drag)
            let forward = Vec3::new(cos_yaw, sin_yaw, 0.0);

            // Move focus: drag right moves focus left (so scene moves right), etc.
            camera_state.focus -= right * dx * pan_speed;
            camera_state.focus += forward * dy * pan_speed;
        }
        camera_state.last_cursor_pos = cursor_pos;
    } else {
        camera_state.last_cursor_pos = None;
    }

    // Scroll zoom (logarithmic for smooth feel at all distances)
    for event in scroll_events.read() {
        let zoom_factor = 1.0 - event.y * 0.08;
        camera_state.distance *= zoom_factor;
        camera_state.distance = camera_state.distance.clamp(10.0, 5000.0);
    }

    // WASD movement - relative to camera view direction
    let mut move_dir = Vec3::ZERO;

    if keyboard.pressed(KeyCode::KeyW) {
        move_dir.y += 1.0;  // Forward (toward where camera is looking)
    }
    if keyboard.pressed(KeyCode::KeyS) {
        move_dir.y -= 1.0;  // Backward
    }
    if keyboard.pressed(KeyCode::KeyA) {
        move_dir.x -= 1.0;  // Strafe left
    }
    if keyboard.pressed(KeyCode::KeyD) {
        move_dir.x += 1.0;  // Strafe right
    }
    if keyboard.pressed(KeyCode::KeyQ) {
        move_dir.z -= 1.0;  // Down
    }
    if keyboard.pressed(KeyCode::KeyE) {
        move_dir.z += 1.0;  // Up
    }

    if move_dir != Vec3::ZERO {
        // Camera looks from position toward focus, so view direction in XY plane is:
        // focus - camera_pos, projected to XY. Since camera_pos = focus + offset,
        // the view direction is -offset. The offset direction is (cos_yaw, sin_yaw) in XY,
        // so view forward is (-cos_yaw, -sin_yaw).
        let cos_yaw = camera_state.yaw.cos();
        let sin_yaw = camera_state.yaw.sin();

        // Forward vector: direction camera is looking (toward focus)
        let forward = Vec3::new(-cos_yaw, -sin_yaw, 0.0);
        // Right vector: perpendicular to forward in XY plane
        let right = Vec3::new(-sin_yaw, cos_yaw, 0.0);

        // Scale move speed with camera distance for consistent feel at any zoom level
        let move_speed = camera_state.move_speed * (camera_state.distance / 100.0).clamp(0.5, 10.0);

        let world_move = forward * move_dir.y + right * move_dir.x + Vec3::Z * move_dir.z;

        camera_state.focus += world_move.normalize() * move_speed * dt;
    }

    // Update camera transform
    if let Ok(mut transform) = camera_query.get_single_mut() {
        let camera_pos = calculate_camera_position(&camera_state);
        transform.translation = camera_pos;
        transform.look_at(camera_state.focus, Vec3::Z);
    }
}

fn test_mode_system(
    mut camera_query: Query<&mut Transform, With<EditorCamera>>,
    editor_state: Res<EditorState>,
    mut test_mode_state: ResMut<TestModeState>,
    keyboard: Res<ButtonInput<KeyCode>>,
    time: Res<Time>,
) {
    // Toggle play/pause with space
    if keyboard.just_pressed(KeyCode::Space) {
        test_mode_state.playing = !test_mode_state.playing;
    }

    // Get raceline data
    let raceline = match &editor_state.loaded_track {
        Some(loaded) if !loaded.track_data.raceline.is_empty() => &loaded.track_data.raceline,
        _ => return,
    };

    // Update progress if playing
    if test_mode_state.playing {
        test_mode_state.progress += test_mode_state.speed * time.delta_seconds();

        // Loop back to start when reaching end
        if test_mode_state.progress >= test_mode_state.raceline_length {
            test_mode_state.progress = 0.0;
        }
    }

    // Sample position and direction along raceline
    let (pos, dir) = sample_raceline(raceline, test_mode_state.progress);

    // Update camera based on camera type
    if let Ok(mut transform) = camera_query.get_single_mut() {
        match test_mode_state.camera_type {
            TestCameraType::Chase => {
                // Chase camera: behind and above the current position
                let chase_distance = 15.0;
                let chase_height = 5.0;

                let camera_pos = pos - dir * chase_distance + Vec3::Z * chase_height;
                let look_ahead = pos + dir * 20.0;

                transform.translation = camera_pos;
                transform.look_at(look_ahead, Vec3::Z);
            }
            TestCameraType::Cockpit => {
                // Cockpit camera: at the position, looking forward
                let cockpit_height = 1.5;

                let camera_pos = pos + Vec3::Z * cockpit_height;
                let look_ahead = pos + dir * 50.0 + Vec3::Z * cockpit_height;

                transform.translation = camera_pos;
                transform.look_at(look_ahead, Vec3::Z);
            }
        }
    }
}

/// Kerb brush tool system - handles increasing/decreasing kerbs on track edges
fn kerb_brush_system(
    mut commands: Commands,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    mut editor_state: ResMut<EditorState>,
    mut camera_query: Query<&mut Transform, With<EditorCamera>>,
    camera_read_query: Query<(&Camera, &GlobalTransform), With<EditorCamera>>,
    kerb_mesh_query: Query<Entity, With<KerbMeshEntity>>,
    track_edges_res: Res<TrackEdgesResource>,
    mouse_buttons: Res<ButtonInput<MouseButton>>,
    mut scroll_events: EventReader<MouseWheel>,
    mut camera_state: ResMut<CameraState>,
    windows: Query<&Window>,
    time: Res<Time>,
    mut contexts: EguiContexts,
) {
    // Don't process if egui wants input
    let ctx = contexts.ctx_mut();
    if ctx.wants_pointer_input() {
        scroll_events.clear();
        return;
    }

    // Handle scroll zoom (always available in kerb brush mode)
    for event in scroll_events.read() {
        let zoom_factor = 1.0 - event.y * 0.08;
        camera_state.distance *= zoom_factor;
        camera_state.distance = camera_state.distance.clamp(10.0, 5000.0);
    }

    // Update camera transform for zoom
    if let Ok(mut transform) = camera_query.get_single_mut() {
        let camera_pos = calculate_camera_position(&camera_state);
        transform.translation = camera_pos;
        transform.look_at(camera_state.focus, Vec3::Z);
    }

    // Handle middle mouse button for panning
    if mouse_buttons.pressed(MouseButton::Middle) {
        if let Some(cursor_pos) = windows.get_single().ok().and_then(|w| w.cursor_position()) {
            if let Some(last) = camera_state.last_cursor_pos {
                let dx = cursor_pos.x - last.x;
                let dy = cursor_pos.y - last.y;

                let pan_speed = camera_state.distance * 0.002;
                let cos_yaw = camera_state.yaw.cos();
                let sin_yaw = camera_state.yaw.sin();
                let right = Vec3::new(sin_yaw, -cos_yaw, 0.0);
                let forward = Vec3::new(cos_yaw, sin_yaw, 0.0);

                camera_state.focus -= right * dx * pan_speed;
                camera_state.focus += forward * dy * pan_speed;
            }
            camera_state.last_cursor_pos = Some(cursor_pos);
        }
    } else {
        camera_state.last_cursor_pos = None;
    }

    let left_pressed = mouse_buttons.pressed(MouseButton::Left);
    let right_pressed = mouse_buttons.pressed(MouseButton::Right);

    if !left_pressed && !right_pressed {
        return;
    }

    let Ok((camera, camera_transform)) = camera_read_query.get_single() else {
        return;
    };

    let Some(cursor_pos) = windows.get_single().ok().and_then(|w| w.cursor_position()) else {
        return;
    };

    let Some(edges) = &track_edges_res.edges else {
        return;
    };

    if edges.left.is_empty() {
        return;
    }

    // Ray cast from cursor - intersect with ground plane to get world point
    let Some(ray) = camera.viewport_to_world(camera_transform, cursor_pos) else {
        return;
    };

    // Intersect ray with Z=avg_track_z plane
    let avg_z = edges.centerline.iter().map(|p| p.z).sum::<f32>() / edges.centerline.len() as f32;
    let world_point = ray_plane_intersect(ray.origin, ray.direction.as_vec3(), avg_z);
    let Some(world_point) = world_point else {
        return;
    };

    // Find closest point on left and right edge polylines
    let (left_idx, left_dist) = closest_point_on_polyline(&edges.left, world_point);
    let (right_idx, right_dist) = closest_point_on_polyline(&edges.right, world_point);

    // Determine which side the click is on
    let (edge_idx, closest_side) = if left_dist < right_dist {
        (left_idx, KerbSide::Left)
    } else {
        (right_idx, KerbSide::Right)
    };

    // Map back to the source TrackNode index
    let source_node_idx = edges.source_node[edge_idx];

    let dt = time.delta_seconds();
    let height_rate = editor_state.kerb_brush.height_rate;
    let max_height = editor_state.kerb_brush.max_height;
    let default_width = editor_state.kerb_brush.default_width;

    let mut kerb_changed = false;

    // Handle increasing kerb (left-click hold)
    if left_pressed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            if source_node_idx < loaded.track_data.nodes.len() {
                let node = &mut loaded.track_data.nodes[source_node_idx];

                if node.kerbs.is_none() {
                    node.kerbs = Some(NodeKerbs::default());
                }

                if let Some(ref mut kerbs) = node.kerbs {
                    let kerb = match closest_side {
                        KerbSide::Left => &mut kerbs.left,
                        KerbSide::Right => &mut kerbs.right,
                    };

                    if kerb.height == 0.0 && kerb.width == 0.0 {
                        kerb.width = default_width;
                    }

                    kerb.height = (kerb.height + height_rate * dt).min(max_height);
                    kerb_changed = true;
                }
            }
        }
    }

    // Handle decreasing kerb (right-click hold)
    if right_pressed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            if source_node_idx < loaded.track_data.nodes.len() {
                let node = &mut loaded.track_data.nodes[source_node_idx];

                if let Some(ref mut kerbs) = node.kerbs {
                    let kerb = match closest_side {
                        KerbSide::Left => &mut kerbs.left,
                        KerbSide::Right => &mut kerbs.right,
                    };

                    kerb.height = (kerb.height - height_rate * dt).max(0.0);
                    if kerb.height == 0.0 {
                        kerb.width = 0.0;
                    }
                    kerb_changed = true;
                }
            }
        }
    }

    if kerb_changed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            loaded.dirty = true;
        }
        regenerate_kerb_meshes(
            &mut commands,
            &mut meshes,
            &mut materials,
            &kerb_mesh_query,
            &editor_state,
            &track_edges_res,
        );
    }
}

/// Intersect a ray with a horizontal plane at Z = plane_z.
/// Returns the intersection point, or None if the ray is parallel to the plane.
fn ray_plane_intersect(origin: Vec3, dir: Vec3, plane_z: f32) -> Option<Vec3> {
    if dir.z.abs() < 1e-6 {
        return None; // Ray parallel to plane
    }
    let t = (plane_z - origin.z) / dir.z;
    if t < 0.0 {
        return None; // Intersection behind camera
    }
    Some(origin + dir * t)
}

/// Find the index of the closest point on a polyline to a world point. Returns (index, distance).
fn closest_point_on_polyline(polyline: &[Vec3], point: Vec3) -> (usize, f32) {
    let mut best_idx = 0;
    let mut best_dist = f32::MAX;
    let p2 = Vec2::new(point.x, point.y);

    for (i, edge_pt) in polyline.iter().enumerate() {
        let e2 = Vec2::new(edge_pt.x, edge_pt.y);
        let dist = (p2 - e2).length();
        if dist < best_dist {
            best_dist = dist;
            best_idx = i;
        }
    }

    (best_idx, best_dist)
}

/// Save the current editor state as an overlay archive (.apexedit).
fn save_track_overlay(editor_state: &mut EditorState) {
    let Some(loaded) = &mut editor_state.loaded_track else {
        return;
    };

    let overlay = TrackEditorOverlay::build_from_diff(
        &loaded.track_data,
        &loaded.terrain_data,
        &loaded.original_terrain_heights,
    );

    match overlay.save(&loaded.overlay_path) {
        Ok(()) => {
            loaded.dirty = false;
            eprintln!("Saved overlay to {}", loaded.overlay_path.display());
        }
        Err(e) => {
            eprintln!("Failed to save overlay: {}", e);
        }
    }
}

/// Regenerate all kerb meshes from the current track data
fn regenerate_kerb_meshes(
    commands: &mut Commands,
    meshes: &mut ResMut<Assets<Mesh>>,
    materials: &mut ResMut<Assets<StandardMaterial>>,
    kerb_mesh_query: &Query<Entity, With<KerbMeshEntity>>,
    editor_state: &EditorState,
    track_edges_res: &TrackEdgesResource,
) {
    // Remove existing kerb meshes
    for entity in kerb_mesh_query.iter() {
        commands.entity(entity).despawn_recursive();
    }

    let Some(loaded) = &editor_state.loaded_track else {
        return;
    };
    let Some(edges) = &track_edges_res.edges else {
        return;
    };

    let kerb_color = Color::srgb(0.9, 0.2, 0.1);

    // Generate kerb meshes for each side
    for side in [KerbSide::Left, KerbSide::Right] {
        let kerb_meshes = generate_kerb_meshes_from_edges(
            &loaded.track_data.nodes,
            edges,
            side,
        );

        for mesh in kerb_meshes {
            commands.spawn((
                PbrBundle {
                    mesh: meshes.add(mesh),
                    material: materials.add(StandardMaterial {
                        base_color: kerb_color,
                        perceptual_roughness: 0.7,
                        ..default()
                    }),
                    ..default()
                },
                KerbMeshEntity,
            ));
        }
    }
}

/// Generate kerb meshes for one side using the precomputed edge polylines.
/// Returns a Vec of meshes, one per contiguous run of kerbed nodes.
fn generate_kerb_meshes_from_edges(
    nodes: &[track_data::TrackNode],
    edges: &TrackEdges,
    side: KerbSide,
) -> Vec<Mesh> {
    let mut result = Vec::new();

    if nodes.len() < 2 || edges.left.is_empty() {
        return result;
    }

    // Collect contiguous runs of interpolated points whose source node has a kerb.
    // Each run becomes one mesh.
    let n = edges.left.len();
    let mut run_start: Option<usize> = None;

    let get_kerb = |node_idx: usize| -> (f32, f32) {
        if node_idx >= nodes.len() {
            return (0.0, 0.0);
        }
        let node = &nodes[node_idx];
        node.kerbs.as_ref().map_or((0.0, 0.0), |k| {
            let props = match side {
                KerbSide::Left => &k.left,
                KerbSide::Right => &k.right,
            };
            (props.height, props.width)
        })
    };

    // We iterate over all interpolated points + 1 (sentinel) to flush the last run
    for i in 0..=n {
        let (height, width) = if i < n {
            get_kerb(edges.source_node[i])
        } else {
            (0.0, 0.0) // sentinel to close any open run
        };

        let has_kerb = height > 0.0 && width > 0.0;

        if has_kerb && run_start.is_none() {
            run_start = Some(i);
        } else if !has_kerb && run_start.is_some() {
            // End of a run - generate mesh for run_start..i
            let start = run_start.unwrap();
            if let Some(mesh) = generate_single_kerb_run(nodes, edges, side, start, i) {
                result.push(mesh);
            }
            run_start = None;
        }
    }

    result
}

/// Generate a mesh for one contiguous kerb run (edge points from `start` to `end`, exclusive).
fn generate_single_kerb_run(
    nodes: &[track_data::TrackNode],
    edges: &TrackEdges,
    side: KerbSide,
    start: usize,
    end: usize,
) -> Option<Mesh> {
    use bevy::render::mesh::{Indices, PrimitiveTopology};

    let count = end - start;
    if count < 2 {
        return None;
    }

    let mut positions: Vec<[f32; 3]> = Vec::new();
    let mut normals: Vec<[f32; 3]> = Vec::new();
    let mut uvs: Vec<[f32; 2]> = Vec::new();
    let mut indices: Vec<u32> = Vec::new();

    for i in start..end {
        let node_idx = edges.source_node[i];
        let node = &nodes[node_idx];

        let (kerb_height, kerb_width) = node.kerbs.as_ref().map_or((0.0, 0.0), |k| {
            let props = match side {
                KerbSide::Left => &k.left,
                KerbSide::Right => &k.right,
            };
            (props.height, props.width)
        });

        // Edge point (inner edge of kerb, at track edge)
        let edge_pt = match side {
            KerbSide::Left => edges.left[i],
            KerbSide::Right => edges.right[i],
        };

        // Perpendicular direction pointing outward from track
        let perp = edges.perpendiculars[i];
        let outward = match side {
            KerbSide::Left => perp,
            KerbSide::Right => -perp,
        };

        // Inner vertex (at track edge, slightly above)
        let inner = Vec3::new(edge_pt.x, edge_pt.y, edge_pt.z + 0.01);
        // Outer vertex (kerb_width outward, at kerb_height)
        let outer = Vec3::new(
            edge_pt.x + outward.x * kerb_width,
            edge_pt.y + outward.y * kerb_width,
            edge_pt.z + kerb_height,
        );

        let u = (i - start) as f32 / (count - 1) as f32;

        positions.push([inner.x, inner.y, inner.z]);
        normals.push([0.0, 0.0, 1.0]);
        uvs.push([u, 0.0]);

        positions.push([outer.x, outer.y, outer.z]);
        let n = Vec3::new(outward.x, outward.y, 0.5).normalize();
        normals.push([n.x, n.y, n.z]);
        uvs.push([u, 1.0]);
    }

    // Generate quads between consecutive cross-sections
    // Winding order depends on side so both face upward
    for i in 0..(count - 1) {
        let base = (i * 2) as u32;
        match side {
            KerbSide::Left => {
                indices.push(base);
                indices.push(base + 2);
                indices.push(base + 1);
                indices.push(base + 1);
                indices.push(base + 2);
                indices.push(base + 3);
            }
            KerbSide::Right => {
                indices.push(base);
                indices.push(base + 1);
                indices.push(base + 2);
                indices.push(base + 1);
                indices.push(base + 3);
                indices.push(base + 2);
            }
        }
    }

    if indices.is_empty() {
        return None;
    }

    let mut mesh = Mesh::new(PrimitiveTopology::TriangleList, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);
    mesh.insert_attribute(Mesh::ATTRIBUTE_NORMAL, normals);
    mesh.insert_attribute(Mesh::ATTRIBUTE_UV_0, uvs);
    mesh.insert_indices(Indices::U32(indices));

    Some(mesh)
}

/// Terrain leveling brush tool system - handles raising/lowering/resetting terrain
fn terrain_level_brush_system(
    mut commands: Commands,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    mut editor_state: ResMut<EditorState>,
    mut camera_query: Query<&mut Transform, With<EditorCamera>>,
    camera_read_query: Query<(&Camera, &GlobalTransform), With<EditorCamera>>,
    terrain_mesh_query: Query<Entity, With<TerrainMeshEntity>>,
    brush_query: Query<Entity, With<TerrainBrushEntity>>,
    mouse_buttons: Res<ButtonInput<MouseButton>>,
    mut scroll_events: EventReader<MouseWheel>,
    mut camera_state: ResMut<CameraState>,
    windows: Query<&Window>,
    time: Res<Time>,
    mut contexts: EguiContexts,
) {
    // Don't process if egui wants input
    let ctx = contexts.ctx_mut();
    if ctx.wants_pointer_input() {
        scroll_events.clear();
        return;
    }

    // Handle scroll zoom (always available)
    for event in scroll_events.read() {
        let zoom_factor = 1.0 - event.y * 0.08;
        camera_state.distance *= zoom_factor;
        camera_state.distance = camera_state.distance.clamp(10.0, 5000.0);
    }

    // Update camera transform for zoom
    if let Ok(mut transform) = camera_query.get_single_mut() {
        let camera_pos = calculate_camera_position(&camera_state);
        transform.translation = camera_pos;
        transform.look_at(camera_state.focus, Vec3::Z);
    }

    // Handle middle mouse button for panning
    if mouse_buttons.pressed(MouseButton::Middle) {
        if let Some(cursor_pos) = windows.get_single().ok().and_then(|w| w.cursor_position()) {
            if let Some(last) = camera_state.last_cursor_pos {
                let dx = cursor_pos.x - last.x;
                let dy = cursor_pos.y - last.y;

                let pan_speed = camera_state.distance * 0.002;
                let cos_yaw = camera_state.yaw.cos();
                let sin_yaw = camera_state.yaw.sin();
                let right = Vec3::new(sin_yaw, -cos_yaw, 0.0);
                let forward = Vec3::new(cos_yaw, sin_yaw, 0.0);

                camera_state.focus -= right * dx * pan_speed;
                camera_state.focus += forward * dy * pan_speed;
            }
            camera_state.last_cursor_pos = Some(cursor_pos);
        }
    } else {
        camera_state.last_cursor_pos = None;
    }

    let Ok((camera, camera_transform)) = camera_read_query.get_single() else {
        return;
    };

    let Some(cursor_pos) = windows.get_single().ok().and_then(|w| w.cursor_position()) else {
        // Remove brush visualization if cursor is outside window
        for entity in brush_query.iter() {
            commands.entity(entity).despawn_recursive();
        }
        return;
    };

    // Get terrain data
    let Some(loaded) = &editor_state.loaded_track else {
        return;
    };

    let Some(terrain_data) = &loaded.terrain_data else {
        return;
    };

    let Some(heightmap) = &terrain_data.heightmap else {
        return;
    };

    // Ray cast from cursor to find intersection with terrain
    let Some(ray) = camera.viewport_to_world(camera_transform, cursor_pos) else {
        return;
    };

    // Find intersection with terrain (approximate using XY plane at average height)
    // Sample terrain height at ray intersection
    let brush_pos = ray_terrain_intersection(ray.origin, ray.direction.as_vec3(), heightmap);

    let brush_radius = editor_state.terrain_level_brush.radius;

    // Update brush visualization
    update_terrain_brush_visualization(
        &mut commands,
        &mut meshes,
        &mut materials,
        &brush_query,
        brush_pos,
        brush_radius,
        heightmap,
    );

    let left_pressed = mouse_buttons.pressed(MouseButton::Left);
    let right_pressed = mouse_buttons.pressed(MouseButton::Right);
    let middle_just_pressed = mouse_buttons.just_pressed(MouseButton::Middle);
    let dt = time.delta_seconds();

    let height_rate = editor_state.terrain_level_brush.height_rate;
    let falloff = editor_state.terrain_level_brush.falloff;

    let mut terrain_changed = false;

    // Left-click: lower terrain
    if left_pressed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            if let Some(ref mut terrain) = loaded.terrain_data {
                if let Some(ref mut hm) = terrain.heightmap {
                    modify_terrain_height(hm, brush_pos, brush_radius, falloff, -height_rate * dt);
                    terrain_changed = true;
                }
            }
        }
    }

    // Right-click: raise terrain
    if right_pressed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            if let Some(ref mut terrain) = loaded.terrain_data {
                if let Some(ref mut hm) = terrain.heightmap {
                    modify_terrain_height(hm, brush_pos, brush_radius, falloff, height_rate * dt);
                    terrain_changed = true;
                }
            }
        }
    }

    // Middle-click (just pressed, not held for panning): reset to ground level
    // Note: We use a separate check since middle-click is also used for panning
    // Reset happens on initial click, panning happens on drag
    if middle_just_pressed && camera_state.last_cursor_pos.is_none() {
        if let Some(loaded) = &mut editor_state.loaded_track {
            // Clone track nodes to avoid borrow conflict
            let track_nodes: Vec<_> = loaded.track_data.nodes.iter().map(|n| (n.x, n.y, n.z)).collect();
            if let Some(ref mut terrain) = loaded.terrain_data {
                if let Some(ref mut hm) = terrain.heightmap {
                    reset_terrain_to_ground_level(hm, brush_pos, brush_radius, falloff, &track_nodes);
                    terrain_changed = true;
                }
            }
        }
    }

    // Regenerate terrain meshes if anything changed
    if terrain_changed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            loaded.dirty = true;
        }
        regenerate_terrain_meshes(
            &mut commands,
            &mut meshes,
            &mut materials,
            &terrain_mesh_query,
            &editor_state,
        );
    }
}

/// Find intersection point of ray with terrain heightmap
fn ray_terrain_intersection(origin: Vec3, direction: Vec3, heightmap: &TerrainHeightmap) -> Vec3 {
    // Use iterative ray marching to find terrain intersection
    let mut t = 0.0f32;
    let max_t = 10000.0;
    let step = 5.0; // Step size in meters

    while t < max_t {
        let point = origin + direction * t;
        let terrain_height = heightmap.sample(point.x, point.y);

        if point.z <= terrain_height {
            // Found intersection, refine with smaller steps
            let mut t_fine = t - step;
            let fine_step = 0.5;
            while t_fine < t {
                let p = origin + direction * t_fine;
                let h = heightmap.sample(p.x, p.y);
                if p.z <= h {
                    return Vec3::new(p.x, p.y, h);
                }
                t_fine += fine_step;
            }
            return Vec3::new(point.x, point.y, terrain_height);
        }

        t += step;
    }

    // No intersection found, return point on XY plane at origin height
    if direction.z.abs() > 0.001 {
        let t_plane = -origin.z / direction.z;
        let point = origin + direction * t_plane;
        Vec3::new(point.x, point.y, heightmap.sample(point.x, point.y))
    } else {
        origin
    }
}

/// Modify terrain height within brush radius
fn modify_terrain_height(
    heightmap: &mut TerrainHeightmap,
    center: Vec3,
    radius: f32,
    falloff: f32,
    height_delta: f32,
) {
    // Calculate grid bounds affected by brush
    let min_gx = ((center.x - radius - heightmap.origin_x) / heightmap.cell_size_m).floor().max(0.0) as usize;
    let max_gx = ((center.x + radius - heightmap.origin_x) / heightmap.cell_size_m).ceil().min((heightmap.width - 1) as f32) as usize;
    let min_gy = ((center.y - radius - heightmap.origin_y) / heightmap.cell_size_m).floor().max(0.0) as usize;
    let max_gy = ((center.y + radius - heightmap.origin_y) / heightmap.cell_size_m).ceil().min((heightmap.height - 1) as f32) as usize;

    for gy in min_gy..=max_gy {
        for gx in min_gx..=max_gx {
            let world_x = heightmap.origin_x + gx as f32 * heightmap.cell_size_m;
            let world_y = heightmap.origin_y + gy as f32 * heightmap.cell_size_m;

            let dx = world_x - center.x;
            let dy = world_y - center.y;
            let dist = (dx * dx + dy * dy).sqrt();

            if dist <= radius {
                // Calculate falloff factor
                let t = dist / radius;
                let factor = if falloff > 0.0 {
                    // Smooth falloff: 1 at center, 0 at edge
                    let falloff_start = 1.0 - falloff;
                    if t <= falloff_start {
                        1.0
                    } else {
                        let falloff_t = (t - falloff_start) / falloff;
                        1.0 - falloff_t * falloff_t // Quadratic falloff
                    }
                } else {
                    1.0 // Hard edge
                };

                let idx = gy * heightmap.width + gx;
                heightmap.heights[idx] += height_delta * factor;
            }
        }
    }
}

/// Reset terrain to ground level (estimated from track nodes)
/// track_nodes is a slice of (x, y, z) tuples
fn reset_terrain_to_ground_level(
    heightmap: &mut TerrainHeightmap,
    center: Vec3,
    radius: f32,
    falloff: f32,
    track_nodes: &[(f32, f32, f32)],
) {
    // Find average track height near the brush center as reference ground level
    let mut total_height = 0.0f32;
    let mut count = 0;
    let search_radius = radius * 2.0;

    for &(x, y, z) in track_nodes {
        let dx = x - center.x;
        let dy = y - center.y;
        let dist = (dx * dx + dy * dy).sqrt();
        if dist < search_radius {
            total_height += z;
            count += 1;
        }
    }

    // If no track nodes nearby, use the terrain height at brush center as reference
    let ground_level = if count > 0 {
        total_height / count as f32
    } else {
        heightmap.sample(center.x, center.y)
    };

    // Calculate grid bounds affected by brush
    let min_gx = ((center.x - radius - heightmap.origin_x) / heightmap.cell_size_m).floor().max(0.0) as usize;
    let max_gx = ((center.x + radius - heightmap.origin_x) / heightmap.cell_size_m).ceil().min((heightmap.width - 1) as f32) as usize;
    let min_gy = ((center.y - radius - heightmap.origin_y) / heightmap.cell_size_m).floor().max(0.0) as usize;
    let max_gy = ((center.y + radius - heightmap.origin_y) / heightmap.cell_size_m).ceil().min((heightmap.height - 1) as f32) as usize;

    for gy in min_gy..=max_gy {
        for gx in min_gx..=max_gx {
            let world_x = heightmap.origin_x + gx as f32 * heightmap.cell_size_m;
            let world_y = heightmap.origin_y + gy as f32 * heightmap.cell_size_m;

            let dx = world_x - center.x;
            let dy = world_y - center.y;
            let dist = (dx * dx + dy * dy).sqrt();

            if dist <= radius {
                // Calculate falloff factor
                let t = dist / radius;
                let factor = if falloff > 0.0 {
                    let falloff_start = 1.0 - falloff;
                    if t <= falloff_start {
                        1.0
                    } else {
                        let falloff_t = (t - falloff_start) / falloff;
                        1.0 - falloff_t * falloff_t
                    }
                } else {
                    1.0
                };

                let idx = gy * heightmap.width + gx;
                let current_height = heightmap.heights[idx];
                // Blend toward ground level
                heightmap.heights[idx] = current_height + (ground_level - current_height) * factor;
            }
        }
    }
}

/// Update or create the terrain brush visualization circle
fn update_terrain_brush_visualization(
    commands: &mut Commands,
    meshes: &mut ResMut<Assets<Mesh>>,
    materials: &mut ResMut<Assets<StandardMaterial>>,
    brush_query: &Query<Entity, With<TerrainBrushEntity>>,
    center: Vec3,
    radius: f32,
    heightmap: &TerrainHeightmap,
) {
    // Remove existing brush
    for entity in brush_query.iter() {
        commands.entity(entity).despawn_recursive();
    }

    // Generate circle mesh that follows terrain
    let segments = 64;
    let mut positions: Vec<[f32; 3]> = Vec::new();

    for i in 0..=segments {
        let angle = (i as f32 / segments as f32) * std::f32::consts::TAU;
        let x = center.x + radius * angle.cos();
        let y = center.y + radius * angle.sin();
        let z = heightmap.sample(x, y) + 0.5; // Slightly above terrain
        positions.push([x, y, z]);
    }

    let mut mesh = Mesh::new(bevy::render::mesh::PrimitiveTopology::LineStrip, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);

    commands.spawn((
        PbrBundle {
            mesh: meshes.add(mesh),
            material: materials.add(StandardMaterial {
                base_color: Color::srgba(1.0, 1.0, 0.0, 0.8),
                emissive: bevy::color::LinearRgba::new(1.0, 1.0, 0.0, 1.0),
                unlit: true,
                alpha_mode: bevy::prelude::AlphaMode::Blend,
                ..default()
            }),
            ..default()
        },
        TerrainBrushEntity,
    ));
}

/// Regenerate terrain meshes after modification
fn regenerate_terrain_meshes(
    commands: &mut Commands,
    meshes: &mut ResMut<Assets<Mesh>>,
    materials: &mut ResMut<Assets<StandardMaterial>>,
    terrain_mesh_query: &Query<Entity, With<TerrainMeshEntity>>,
    editor_state: &EditorState,
) {
    // Remove existing terrain meshes
    for entity in terrain_mesh_query.iter() {
        commands.entity(entity).despawn_recursive();
    }

    let Some(loaded) = &editor_state.loaded_track else {
        return;
    };

    let Some(terrain) = &loaded.terrain_data else {
        return;
    };

    let Some(heightmap) = &terrain.heightmap else {
        return;
    };

    let nodes = &loaded.track_data.nodes;

    // Generate main terrain mesh
    let terrain_mesh = generate_terrain_mesh(heightmap, nodes);

    commands.spawn((
        PbrBundle {
            mesh: meshes.add(terrain_mesh),
            material: materials.add(StandardMaterial {
                base_color: Color::WHITE,
                perceptual_roughness: 0.95,
                cull_mode: None,
                ..default()
            }),
            ..default()
        },
        TerrainMeshEntity,
    ));

    // Generate wireframe grid overlay
    let grid_mesh = generate_terrain_grid(heightmap, nodes);
    commands.spawn((
        PbrBundle {
            mesh: meshes.add(grid_mesh),
            material: materials.add(StandardMaterial {
                base_color: Color::srgba(0.0, 0.0, 0.0, 0.5),
                unlit: true,
                alpha_mode: bevy::prelude::AlphaMode::Blend,
                ..default()
            }),
            transform: Transform::from_xyz(0.0, 0.0, 0.1),
            ..default()
        },
        TerrainMeshEntity,
    ));
}

/// Tree brush tool system - handles painting/erasing trees on terrain
fn tree_brush_system(
    mut commands: Commands,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    mut editor_state: ResMut<EditorState>,
    mut camera_query: Query<&mut Transform, With<EditorCamera>>,
    camera_read_query: Query<(&Camera, &GlobalTransform), With<EditorCamera>>,
    tree_mesh_query: Query<Entity, With<TreeMeshEntity>>,
    brush_query: Query<Entity, With<TreeBrushEntity>>,
    mouse_buttons: Res<ButtonInput<MouseButton>>,
    mut scroll_events: EventReader<MouseWheel>,
    mut camera_state: ResMut<CameraState>,
    windows: Query<&Window>,
    time: Res<Time>,
    mut contexts: EguiContexts,
) {
    // Don't process if egui wants input
    let ctx = contexts.ctx_mut();
    if ctx.wants_pointer_input() {
        scroll_events.clear();
        return;
    }

    // Handle scroll zoom
    for event in scroll_events.read() {
        let zoom_factor = 1.0 - event.y * 0.08;
        camera_state.distance *= zoom_factor;
        camera_state.distance = camera_state.distance.clamp(10.0, 5000.0);
    }

    // Update camera transform for zoom
    if let Ok(mut transform) = camera_query.get_single_mut() {
        let camera_pos = calculate_camera_position(&camera_state);
        transform.translation = camera_pos;
        transform.look_at(camera_state.focus, Vec3::Z);
    }

    // Handle middle mouse button for panning
    if mouse_buttons.pressed(MouseButton::Middle) {
        if let Some(cursor_pos) = windows.get_single().ok().and_then(|w| w.cursor_position()) {
            if let Some(last) = camera_state.last_cursor_pos {
                let dx = cursor_pos.x - last.x;
                let dy = cursor_pos.y - last.y;

                let pan_speed = camera_state.distance * 0.002;
                let cos_yaw = camera_state.yaw.cos();
                let sin_yaw = camera_state.yaw.sin();
                let right = Vec3::new(sin_yaw, -cos_yaw, 0.0);
                let forward = Vec3::new(cos_yaw, sin_yaw, 0.0);

                camera_state.focus -= right * dx * pan_speed;
                camera_state.focus += forward * dy * pan_speed;
            }
            camera_state.last_cursor_pos = Some(cursor_pos);
        }
    } else {
        camera_state.last_cursor_pos = None;
    }

    let Ok((camera, camera_transform)) = camera_read_query.get_single() else {
        return;
    };

    let Some(cursor_pos) = windows.get_single().ok().and_then(|w| w.cursor_position()) else {
        for entity in brush_query.iter() {
            commands.entity(entity).despawn_recursive();
        }
        return;
    };

    // Need terrain data for ray intersection and tree placement
    let has_terrain = editor_state.loaded_track.as_ref()
        .and_then(|l| l.terrain_data.as_ref())
        .and_then(|t| t.heightmap.as_ref())
        .is_some();

    if !has_terrain {
        return;
    }

    let Some(ray) = camera.viewport_to_world(camera_transform, cursor_pos) else {
        return;
    };

    // Get heightmap reference for ray intersection and brush visualization
    let heightmap = editor_state.loaded_track.as_ref()
        .unwrap().terrain_data.as_ref()
        .unwrap().heightmap.as_ref().unwrap();

    let brush_pos = ray_terrain_intersection(ray.origin, ray.direction.as_vec3(), heightmap);
    let brush_radius = editor_state.tree_brush.radius;

    // Update brush visualization
    update_tree_brush_visualization(
        &mut commands,
        &mut meshes,
        &mut materials,
        &brush_query,
        brush_pos,
        brush_radius,
        heightmap,
    );

    let left_pressed = mouse_buttons.pressed(MouseButton::Left);
    let right_pressed = mouse_buttons.pressed(MouseButton::Right);
    let dt = time.delta_seconds();

    let mut trees_changed = false;

    // Left-click: paint trees
    if left_pressed {
        let density = editor_state.tree_brush.density;
        let selected_type = editor_state.tree_brush.selected_type;
        let scale_min = editor_state.tree_brush.scale_min;
        let scale_max = editor_state.tree_brush.scale_max;
        let radius = editor_state.tree_brush.radius;

        // Accumulate fractional trees
        editor_state.tree_brush.place_accumulator += density * dt;

        let trees_to_place = editor_state.tree_brush.place_accumulator.floor() as usize;
        editor_state.tree_brush.place_accumulator -= trees_to_place as f32;

        if trees_to_place > 0 {
            if let Some(loaded) = &mut editor_state.loaded_track {
                if let Some(ref mut terrain) = loaded.terrain_data {
                    if let Some(ref hm) = terrain.heightmap {
                        for _ in 0..trees_to_place {
                            // Random position within brush radius using rejection sampling
                            let (rx, ry) = random_point_in_circle(brush_pos.x, brush_pos.y, radius);
                            let z = hm.sample(rx, ry);

                            // Random scale and rotation
                            let scale = scale_min + simple_random_f32() * (scale_max - scale_min);
                            let rotation = simple_random_f32() * std::f32::consts::TAU;

                            terrain.placed_trees.push(PlacedTree {
                                x: rx,
                                y: ry,
                                z,
                                tree_type: selected_type,
                                scale,
                                rotation,
                            });
                        }
                        trees_changed = true;
                    }
                }
            }
        }
    } else {
        editor_state.tree_brush.place_accumulator = 0.0;
    }

    // Right-click: erase trees within brush radius
    if right_pressed {
        let radius = editor_state.tree_brush.radius;
        let radius_sq = radius * radius;

        if let Some(loaded) = &mut editor_state.loaded_track {
            if let Some(ref mut terrain) = loaded.terrain_data {
                let before_len = terrain.placed_trees.len();
                terrain.placed_trees.retain(|tree| {
                    let dx = tree.x - brush_pos.x;
                    let dy = tree.y - brush_pos.y;
                    dx * dx + dy * dy > radius_sq
                });
                if terrain.placed_trees.len() != before_len {
                    trees_changed = true;
                }
            }
        }
    }

    // Regenerate tree meshes if anything changed
    if trees_changed {
        if let Some(loaded) = &mut editor_state.loaded_track {
            loaded.dirty = true;
        }
        regenerate_tree_meshes(
            &mut commands,
            &mut meshes,
            &mut materials,
            &tree_mesh_query,
            &editor_state,
        );
    }
}

/// Simple pseudo-random number generator (no external crate needed)
/// Uses a static mutable seed for simplicity in a single-threaded Bevy app
fn simple_random_f32() -> f32 {
    use std::sync::atomic::{AtomicU32, Ordering};
    static SEED: AtomicU32 = AtomicU32::new(12345);

    let mut s = SEED.load(Ordering::Relaxed);
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    SEED.store(s, Ordering::Relaxed);

    (s as f32) / (u32::MAX as f32)
}

/// Generate a random point within a circle using rejection sampling
fn random_point_in_circle(cx: f32, cy: f32, radius: f32) -> (f32, f32) {
    loop {
        let x = (simple_random_f32() * 2.0 - 1.0) * radius;
        let y = (simple_random_f32() * 2.0 - 1.0) * radius;
        if x * x + y * y <= radius * radius {
            return (cx + x, cy + y);
        }
    }
}

/// Update brush visualization circle for tree brush
fn update_tree_brush_visualization(
    commands: &mut Commands,
    meshes: &mut ResMut<Assets<Mesh>>,
    materials: &mut ResMut<Assets<StandardMaterial>>,
    brush_query: &Query<Entity, With<TreeBrushEntity>>,
    center: Vec3,
    radius: f32,
    heightmap: &TerrainHeightmap,
) {
    for entity in brush_query.iter() {
        commands.entity(entity).despawn_recursive();
    }

    let segments = 64;
    let mut positions: Vec<[f32; 3]> = Vec::new();

    for i in 0..=segments {
        let angle = (i as f32 / segments as f32) * std::f32::consts::TAU;
        let x = center.x + radius * angle.cos();
        let y = center.y + radius * angle.sin();
        let z = heightmap.sample(x, y) + 0.5;
        positions.push([x, y, z]);
    }

    let mut mesh = Mesh::new(bevy::render::mesh::PrimitiveTopology::LineStrip, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, positions);

    commands.spawn((
        PbrBundle {
            mesh: meshes.add(mesh),
            material: materials.add(StandardMaterial {
                base_color: Color::srgba(0.2, 1.0, 0.2, 0.8),
                emissive: bevy::color::LinearRgba::new(0.2, 1.0, 0.2, 1.0),
                unlit: true,
                alpha_mode: bevy::prelude::AlphaMode::Blend,
                ..default()
            }),
            ..default()
        },
        TreeBrushEntity,
    ));
}

/// Generate a batched mesh containing all placed trees of all types
fn generate_all_trees_mesh(
    placed_trees: &[PlacedTree],
) -> Option<Mesh> {
    if placed_trees.is_empty() {
        return None;
    }

    let tree_types = tree_type_definitions();

    let mut all_positions: Vec<[f32; 3]> = Vec::new();
    let mut all_normals: Vec<[f32; 3]> = Vec::new();
    let mut all_colors: Vec<[f32; 4]> = Vec::new();
    let mut all_indices: Vec<u32> = Vec::new();

    for tree in placed_trees {
        let tt = match tree_types.get(tree.tree_type) {
            Some(tt) => tt,
            None => continue,
        };

        let scale = tree.scale;
        let cos_r = tree.rotation.cos();
        let sin_r = tree.rotation.sin();

        // Helper to rotate a point around Z axis and translate
        let transform_point = |lx: f32, ly: f32, lz: f32| -> [f32; 3] {
            let rx = lx * cos_r - ly * sin_r;
            let ry = lx * sin_r + ly * cos_r;
            [tree.x + rx, tree.y + ry, tree.z + lz]
        };

        let transform_normal = |nx: f32, ny: f32, nz: f32| -> [f32; 3] {
            let rx = nx * cos_r - ny * sin_r;
            let ry = nx * sin_r + ny * cos_r;
            [rx, ry, nz]
        };

        // Generate trunk (8-sided prism)
        let trunk_segments = 8;
        let trunk_h = tt.trunk_height * scale;
        let trunk_r = tt.trunk_radius * scale;
        let trunk_color = [tt.trunk_color[0], tt.trunk_color[1], tt.trunk_color[2], 1.0];

        for i in 0..trunk_segments {
            let a0 = (i as f32 / trunk_segments as f32) * std::f32::consts::TAU;
            let a1 = ((i + 1) as f32 / trunk_segments as f32) * std::f32::consts::TAU;

            let (c0, s0) = (a0.cos(), a0.sin());
            let (c1, s1) = (a1.cos(), a1.sin());

            let x0 = trunk_r * c0;
            let y0 = trunk_r * s0;
            let x1 = trunk_r * c1;
            let y1 = trunk_r * s1;

            let idx = all_positions.len() as u32;

            // Bottom-left, bottom-right, top-right, top-left
            all_positions.push(transform_point(x0, y0, 0.0));
            all_positions.push(transform_point(x1, y1, 0.0));
            all_positions.push(transform_point(x1, y1, trunk_h));
            all_positions.push(transform_point(x0, y0, trunk_h));

            // Face normal (average of two vertex normals)
            let nx = (c0 + c1) * 0.5;
            let ny = (s0 + s1) * 0.5;
            let n = transform_normal(nx, ny, 0.0);
            all_normals.push(n);
            all_normals.push(n);
            all_normals.push(n);
            all_normals.push(n);

            all_colors.push(trunk_color);
            all_colors.push(trunk_color);
            all_colors.push(trunk_color);
            all_colors.push(trunk_color);

            all_indices.extend_from_slice(&[idx, idx + 1, idx + 2, idx, idx + 2, idx + 3]);
        }

        // Generate canopy
        let canopy_color = [tt.canopy_color[0], tt.canopy_color[1], tt.canopy_color[2], 1.0];
        let canopy_r = tt.canopy_radius * scale;
        let canopy_h = tt.canopy_height * scale;
        let canopy_base_z = trunk_h;

        match tt.canopy_shape {
            CanopyShape::Cone => {
                generate_cone_geometry(
                    &mut all_positions, &mut all_normals, &mut all_colors, &mut all_indices,
                    canopy_r, canopy_h, canopy_base_z, canopy_color,
                    &transform_point, &transform_normal,
                );
            }
            CanopyShape::Sphere => {
                generate_sphere_geometry(
                    &mut all_positions, &mut all_normals, &mut all_colors, &mut all_indices,
                    canopy_r, canopy_h, canopy_base_z, canopy_color,
                    &transform_point, &transform_normal,
                );
            }
            CanopyShape::MultiCone => {
                // 3 stacked cones of decreasing size
                let cone_h = canopy_h / 3.0;
                for layer in 0..3 {
                    let layer_f = layer as f32;
                    let r = canopy_r * (1.0 - layer_f * 0.25);
                    let z = canopy_base_z + layer_f * cone_h * 0.6; // Overlap slightly
                    generate_cone_geometry(
                        &mut all_positions, &mut all_normals, &mut all_colors, &mut all_indices,
                        r, cone_h, z, canopy_color,
                        &transform_point, &transform_normal,
                    );
                }
            }
        }
    }

    if all_positions.is_empty() {
        return None;
    }

    let mut mesh = Mesh::new(bevy::render::mesh::PrimitiveTopology::TriangleList, default());
    mesh.insert_attribute(Mesh::ATTRIBUTE_POSITION, all_positions);
    mesh.insert_attribute(Mesh::ATTRIBUTE_NORMAL, all_normals);
    mesh.insert_attribute(Mesh::ATTRIBUTE_COLOR, all_colors);
    mesh.insert_indices(bevy::render::mesh::Indices::U32(all_indices));

    Some(mesh)
}

/// Generate cone geometry (used for pine canopy and spruce layers)
fn generate_cone_geometry(
    positions: &mut Vec<[f32; 3]>,
    normals: &mut Vec<[f32; 3]>,
    colors: &mut Vec<[f32; 4]>,
    indices: &mut Vec<u32>,
    radius: f32,
    height: f32,
    base_z: f32,
    color: [f32; 4],
    transform_point: &dyn Fn(f32, f32, f32) -> [f32; 3],
    transform_normal: &dyn Fn(f32, f32, f32) -> [f32; 3],
) {
    let segments = 8;
    let tip_z = base_z + height;
    // Normal slope angle for cone
    let slope = radius / height;

    for i in 0..segments {
        let a0 = (i as f32 / segments as f32) * std::f32::consts::TAU;
        let a1 = ((i + 1) as f32 / segments as f32) * std::f32::consts::TAU;

        let (c0, s0) = (a0.cos(), a0.sin());
        let (c1, s1) = (a1.cos(), a1.sin());

        let idx = positions.len() as u32;

        // Triangle: base-left, base-right, tip
        positions.push(transform_point(radius * c0, radius * s0, base_z));
        positions.push(transform_point(radius * c1, radius * s1, base_z));
        positions.push(transform_point(0.0, 0.0, tip_z));

        // Cone face normal (pointing outward and up)
        let mid_c = (c0 + c1) * 0.5;
        let mid_s = (s0 + s1) * 0.5;
        let n_len = (mid_c * mid_c + mid_s * mid_s + slope * slope).sqrt();
        let n = transform_normal(mid_c / n_len, mid_s / n_len, slope / n_len);
        normals.push(n);
        normals.push(n);
        normals.push(n);

        colors.push(color);
        colors.push(color);
        colors.push(color);

        indices.extend_from_slice(&[idx, idx + 1, idx + 2]);
    }

    // Bottom cap
    let center_idx = positions.len() as u32;
    positions.push(transform_point(0.0, 0.0, base_z));
    normals.push(transform_normal(0.0, 0.0, -1.0));
    colors.push(color);

    for i in 0..segments {
        let a0 = (i as f32 / segments as f32) * std::f32::consts::TAU;
        let a1 = ((i + 1) as f32 / segments as f32) * std::f32::consts::TAU;

        let idx = positions.len() as u32;
        positions.push(transform_point(radius * a1.cos(), radius * a1.sin(), base_z));
        positions.push(transform_point(radius * a0.cos(), radius * a0.sin(), base_z));
        normals.push(transform_normal(0.0, 0.0, -1.0));
        normals.push(transform_normal(0.0, 0.0, -1.0));
        colors.push(color);
        colors.push(color);

        indices.extend_from_slice(&[center_idx, idx, idx + 1]);
    }
}

/// Generate sphere geometry (used for oak/birch canopy)
fn generate_sphere_geometry(
    positions: &mut Vec<[f32; 3]>,
    normals: &mut Vec<[f32; 3]>,
    colors: &mut Vec<[f32; 4]>,
    indices: &mut Vec<u32>,
    radius: f32,
    height: f32,
    base_z: f32,
    color: [f32; 4],
    transform_point: &dyn Fn(f32, f32, f32) -> [f32; 3],
    transform_normal: &dyn Fn(f32, f32, f32) -> [f32; 3],
) {
    let h_segments = 8;  // Horizontal
    let v_segments = 6;  // Vertical

    let center_z = base_z + height * 0.5;
    let radius_h = radius;
    let radius_v = height * 0.5;

    // Generate vertices
    let base_idx = positions.len() as u32;

    for v in 0..=v_segments {
        let phi = (v as f32 / v_segments as f32) * std::f32::consts::PI;
        let cos_phi = phi.cos();
        let sin_phi = phi.sin();

        for h in 0..=h_segments {
            let theta = (h as f32 / h_segments as f32) * std::f32::consts::TAU;
            let cos_theta = theta.cos();
            let sin_theta = theta.sin();

            let lx = radius_h * sin_phi * cos_theta;
            let ly = radius_h * sin_phi * sin_theta;
            let lz = center_z + radius_v * cos_phi;

            positions.push(transform_point(lx, ly, lz));

            // Normal (for an ellipsoid, scale by 1/radius)
            let nx = sin_phi * cos_theta;
            let ny = sin_phi * sin_theta;
            let nz = cos_phi * (radius_h / radius_v);
            let n_len = (nx * nx + ny * ny + nz * nz).sqrt();
            normals.push(transform_normal(nx / n_len, ny / n_len, nz / n_len));

            colors.push(color);
        }
    }

    // Generate indices
    for v in 0..v_segments {
        for h in 0..h_segments {
            let i0 = base_idx + v * (h_segments + 1) as u32 + h as u32;
            let i1 = i0 + 1;
            let i2 = i0 + (h_segments + 1) as u32;
            let i3 = i2 + 1;

            indices.extend_from_slice(&[i0, i2, i1, i1, i2, i3]);
        }
    }
}

/// Regenerate all tree meshes from the current placed trees data
fn regenerate_tree_meshes(
    commands: &mut Commands,
    meshes: &mut ResMut<Assets<Mesh>>,
    materials: &mut ResMut<Assets<StandardMaterial>>,
    tree_mesh_query: &Query<Entity, With<TreeMeshEntity>>,
    editor_state: &EditorState,
) {
    // Remove existing tree meshes
    for entity in tree_mesh_query.iter() {
        commands.entity(entity).despawn_recursive();
    }

    let Some(loaded) = &editor_state.loaded_track else {
        return;
    };

    let Some(terrain) = &loaded.terrain_data else {
        return;
    };

    if terrain.placed_trees.is_empty() {
        return;
    }

    if let Some(mesh) = generate_all_trees_mesh(&terrain.placed_trees) {
        commands.spawn((
            PbrBundle {
                mesh: meshes.add(mesh),
                material: materials.add(StandardMaterial {
                    base_color: Color::WHITE,
                    perceptual_roughness: 0.9,
                    cull_mode: None,
                    ..default()
                }),
                ..default()
            },
            TreeMeshEntity,
        ));
    }
}

fn cleanup_editor(
    mut commands: Commands,
    query: Query<Entity, Or<(With<TrackMeshEntity>, With<TerrainMeshEntity>, With<RacelineMeshEntity>, With<KerbMeshEntity>, With<EditorCamera>, With<TerrainBrushEntity>, With<TreeMeshEntity>, With<TreeBrushEntity>)>>,
) {
    for entity in query.iter() {
        commands.entity(entity).despawn_recursive();
    }
}
