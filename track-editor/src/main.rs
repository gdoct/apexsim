mod track_data;
mod track_mesh;

use bevy::prelude::*;
use bevy::input::mouse::{MouseMotion, MouseWheel};
use bevy_egui::{egui, EguiContexts, EguiPlugin};
use std::path::PathBuf;

use track_data::{TrackFileFormat, ProceduralWorldData, TerrainHeightmap};

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
        .init_state::<AppState>()
        .insert_resource(EditorState::default())
        .insert_resource(CameraState::default())
        .add_systems(Update, splash_screen_system.run_if(in_state(AppState::Splash)))
        .add_systems(OnEnter(AppState::Browse), setup_browse_state)
        .add_systems(Update, browse_screen_system.run_if(in_state(AppState::Browse)))
        .add_systems(OnEnter(AppState::Editor), setup_editor)
        .add_systems(Update, (
            editor_ui_system,
            camera_controller_system,
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

#[derive(Resource, Default)]
struct EditorState {
    tracks_folder: Option<PathBuf>,
    available_tracks: Vec<TrackEntry>,
    selected_track_index: Option<usize>,
    loaded_track: Option<LoadedTrack>,
    splash_timer: f32,
    folder_input: String,
}

#[derive(Clone)]
struct TrackEntry {
    name: String,
    yaml_path: PathBuf,
    terrain_path: PathBuf,
}

struct LoadedTrack {
    name: String,
    track_data: TrackFileFormat,
    terrain_data: Option<ProceduralWorldData>,
}

#[derive(Resource)]
struct CameraState {
    yaw: f32,
    pitch: f32,
    distance: f32,
    focus: Vec3,
    mouse_sensitivity: f32,
    move_speed: f32,
}

impl Default for CameraState {
    fn default() -> Self {
        Self {
            yaw: 0.0,
            pitch: -0.5,
            distance: 500.0,
            focus: Vec3::ZERO,
            mouse_sensitivity: 0.003,
            move_speed: 200.0,
        }
    }
}

#[derive(Component)]
struct EditorCamera;

#[derive(Component)]
struct TrackMeshEntity;

#[derive(Component)]
struct TerrainMeshEntity;

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

                        if terrain_path.exists() {
                            Some(TrackEntry {
                                name: stem,
                                yaml_path: path,
                                terrain_path,
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
    egui::CentralPanel::default().show(contexts.ctx_mut(), |ui| {
        ui.vertical_centered(|ui| {
            ui.add_space(40.0);
            ui.heading(egui::RichText::new("ApexSim Track Editor").size(36.0));
            ui.add_space(10.0);
            ui.label("Visualize and edit race track data");
            ui.add_space(30.0);
        });

        ui.horizontal(|ui| {
            ui.label("Tracks folder:");

            // Initialize folder_input from current folder if empty
            if editor_state.folder_input.is_empty() {
                if let Some(folder) = &editor_state.tracks_folder {
                    editor_state.folder_input = folder.display().to_string();
                }
            }

            let response = ui.add(
                egui::TextEdit::singleline(&mut editor_state.folder_input)
                    .desired_width(400.0)
                    .hint_text("Enter path to tracks folder...")
            );

            if response.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) {
                let path = PathBuf::from(&editor_state.folder_input);
                if path.exists() && path.is_dir() {
                    editor_state.tracks_folder = Some(path);
                    scan_tracks_folder(&mut editor_state);
                }
            }

            if ui.button("Load").clicked() {
                let path = PathBuf::from(&editor_state.folder_input);
                if path.exists() && path.is_dir() {
                    editor_state.tracks_folder = Some(path);
                    scan_tracks_folder(&mut editor_state);
                }
            }

            if ui.button("Browse...").clicked() {
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

        ui.add_space(20.0);
        ui.separator();
        ui.add_space(10.0);

        if editor_state.available_tracks.is_empty() {
            ui.label("No tracks found. Select a folder containing .yaml and .terrain.msgpack files.");
        } else {
            ui.label(format!("Found {} tracks:", editor_state.available_tracks.len()));
            ui.add_space(10.0);

            let mut new_selection = editor_state.selected_track_index;
            egui::ScrollArea::vertical()
                .max_height(400.0)
                .show(ui, |ui| {
                    for (idx, track) in editor_state.available_tracks.iter().enumerate() {
                        let is_selected = editor_state.selected_track_index == Some(idx);

                        if ui.selectable_label(is_selected, &track.name).clicked() {
                            new_selection = Some(idx);
                        }
                    }
                });
            editor_state.selected_track_index = new_selection;

            ui.add_space(20.0);

            let can_edit = editor_state.selected_track_index.is_some();
            ui.add_enabled_ui(can_edit, |ui| {
                if ui.button(egui::RichText::new("Edit Track").size(18.0)).clicked() {
                    if let Some(idx) = editor_state.selected_track_index {
                        let track_entry = editor_state.available_tracks[idx].clone();

                        // Load track data
                        if let Ok(yaml_content) = std::fs::read_to_string(&track_entry.yaml_path) {
                            if let Ok(track_data) = serde_yaml::from_str::<TrackFileFormat>(&yaml_content) {
                                // Load terrain data
                                let terrain_data = std::fs::read(&track_entry.terrain_path)
                                    .ok()
                                    .and_then(|bytes| rmp_serde::from_slice::<ProceduralWorldData>(&bytes).ok());

                                editor_state.loaded_track = Some(LoadedTrack {
                                    name: track_entry.name.clone(),
                                    track_data,
                                    terrain_data,
                                });

                                next_state.set(AppState::Editor);
                            }
                        }
                    }
                }
            });
        }
    });
}

fn setup_editor(
    mut commands: Commands,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    editor_state: Res<EditorState>,
    mut camera_state: ResMut<CameraState>,
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
        // Calculate track center for camera focus
        let mut center = Vec3::ZERO;
        let node_count = loaded.track_data.nodes.len();

        if node_count > 0 {
            for node in &loaded.track_data.nodes {
                center.x += node.x;
                center.y += node.y;
                center.z += node.z;
            }
            center /= node_count as f32;
        }

        camera_state.focus = center;
        camera_state.distance = 500.0;
        camera_state.yaw = 0.0;
        camera_state.pitch = -0.7;

        // Generate track mesh from centerline nodes
        let track_mesh = track_mesh::generate_track_mesh(&loaded.track_data.nodes, true);

        commands.spawn((
            PbrBundle {
                mesh: meshes.add(track_mesh),
                material: materials.add(StandardMaterial {
                    base_color: Color::srgb(0.3, 0.3, 0.35),
                    perceptual_roughness: 0.8,
                    ..default()
                }),
                ..default()
            },
            TrackMeshEntity,
        ));

        // Generate terrain mesh if available
        if let Some(terrain) = &loaded.terrain_data {
            if let Some(heightmap) = &terrain.heightmap {
                let terrain_mesh = generate_terrain_mesh(heightmap);

                commands.spawn((
                    PbrBundle {
                        mesh: meshes.add(terrain_mesh),
                        material: materials.add(StandardMaterial {
                            base_color: Color::srgb(
                                terrain.preset.ground_color[0],
                                terrain.preset.ground_color[1],
                                terrain.preset.ground_color[2],
                            ),
                            perceptual_roughness: 0.95,
                            ..default()
                        }),
                        ..default()
                    },
                    TerrainMeshEntity,
                ));
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
    let x = state.distance * state.pitch.cos() * state.yaw.cos();
    let y = state.distance * state.pitch.cos() * state.yaw.sin();
    let z = state.distance * state.pitch.sin().abs();

    state.focus + Vec3::new(x, y, z)
}

fn generate_terrain_mesh(heightmap: &TerrainHeightmap) -> Mesh {
    let mut positions: Vec<[f32; 3]> = Vec::new();
    let mut normals: Vec<[f32; 3]> = Vec::new();
    let mut uvs: Vec<[f32; 2]> = Vec::new();
    let mut indices: Vec<u32> = Vec::new();

    // Downsample for performance (render every nth cell)
    let step = 2.max(heightmap.width / 256);

    let sampled_width = heightmap.width / step;
    let sampled_height = heightmap.height / step;

    // Generate vertices
    for gy in 0..sampled_height {
        for gx in 0..sampled_width {
            let x = gx * step;
            let y = gy * step;

            let world_x = heightmap.origin_x + x as f32 * heightmap.cell_size_m;
            let world_y = heightmap.origin_y + y as f32 * heightmap.cell_size_m;
            let world_z = heightmap.get_height(x, y);

            positions.push([world_x, world_y, world_z]);
            normals.push([0.0, 0.0, 1.0]); // Will be calculated properly
            uvs.push([
                gx as f32 / sampled_width as f32,
                gy as f32 / sampled_height as f32,
            ]);
        }
    }

    // Generate indices
    for gy in 0..(sampled_height - 1) {
        for gx in 0..(sampled_width - 1) {
            let v0 = (gy * sampled_width + gx) as u32;
            let v1 = (gy * sampled_width + gx + 1) as u32;
            let v2 = ((gy + 1) * sampled_width + gx) as u32;
            let v3 = ((gy + 1) * sampled_width + gx + 1) as u32;

            indices.push(v0);
            indices.push(v2);
            indices.push(v1);

            indices.push(v1);
            indices.push(v2);
            indices.push(v3);
        }
    }

    // Calculate normals
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
    mesh.insert_indices(bevy::render::mesh::Indices::U32(indices));

    mesh
}

fn editor_ui_system(
    mut contexts: EguiContexts,
    editor_state: Res<EditorState>,
    mut next_state: ResMut<NextState<AppState>>,
    camera_state: Res<CameraState>,
) {
    egui::TopBottomPanel::top("top_panel").show(contexts.ctx_mut(), |ui| {
        ui.horizontal(|ui| {
            if ui.button("< Back to Track List").clicked() {
                next_state.set(AppState::Browse);
            }

            ui.separator();

            if let Some(loaded) = &editor_state.loaded_track {
                ui.label(format!("Track: {}", loaded.name));
                ui.separator();
                ui.label(format!("Nodes: {}", loaded.track_data.nodes.len()));

                if loaded.terrain_data.is_some() {
                    ui.separator();
                    ui.label("Terrain: Loaded");
                }
            }
        });
    });

    egui::Window::new("Camera Info")
        .default_pos([10.0, 60.0])
        .default_size([200.0, 100.0])
        .show(contexts.ctx_mut(), |ui| {
            ui.label(format!("Focus: ({:.1}, {:.1}, {:.1})",
                camera_state.focus.x, camera_state.focus.y, camera_state.focus.z));
            ui.label(format!("Distance: {:.1}m", camera_state.distance));
            ui.label(format!("Yaw: {:.1}°", camera_state.yaw.to_degrees()));
            ui.label(format!("Pitch: {:.1}°", camera_state.pitch.to_degrees()));
            ui.add_space(10.0);
            ui.label("Controls:");
            ui.label("WASD - Move camera");
            ui.label("Right-drag - Rotate view");
            ui.label("Scroll - Zoom in/out");
        });
}

fn camera_controller_system(
    mut camera_query: Query<&mut Transform, With<EditorCamera>>,
    mut camera_state: ResMut<CameraState>,
    keyboard: Res<ButtonInput<KeyCode>>,
    mouse_buttons: Res<ButtonInput<MouseButton>>,
    mut mouse_motion: EventReader<MouseMotion>,
    mut scroll_events: EventReader<MouseWheel>,
    time: Res<Time>,
    mut contexts: EguiContexts,
) {
    // Don't process camera input if egui wants it
    let ctx = contexts.ctx_mut();
    if ctx.wants_pointer_input() || ctx.wants_keyboard_input() {
        mouse_motion.clear();
        scroll_events.clear();
        return;
    }

    let dt = time.delta_seconds();

    // Mouse rotation (right button)
    if mouse_buttons.pressed(MouseButton::Right) {
        for event in mouse_motion.read() {
            camera_state.yaw -= event.delta.x * camera_state.mouse_sensitivity;
            camera_state.pitch -= event.delta.y * camera_state.mouse_sensitivity;
            camera_state.pitch = camera_state.pitch.clamp(-1.5, -0.1);
        }
    } else {
        mouse_motion.clear();
    }

    // Scroll zoom
    for event in scroll_events.read() {
        camera_state.distance -= event.y * camera_state.distance * 0.1;
        camera_state.distance = camera_state.distance.clamp(10.0, 5000.0);
    }

    // WASD movement
    let mut move_dir = Vec3::ZERO;

    if keyboard.pressed(KeyCode::KeyW) {
        move_dir.y += 1.0;
    }
    if keyboard.pressed(KeyCode::KeyS) {
        move_dir.y -= 1.0;
    }
    if keyboard.pressed(KeyCode::KeyA) {
        move_dir.x -= 1.0;
    }
    if keyboard.pressed(KeyCode::KeyD) {
        move_dir.x += 1.0;
    }
    if keyboard.pressed(KeyCode::KeyQ) {
        move_dir.z -= 1.0;
    }
    if keyboard.pressed(KeyCode::KeyE) {
        move_dir.z += 1.0;
    }

    if move_dir != Vec3::ZERO {
        // Transform movement direction based on camera yaw
        let cos_yaw = camera_state.yaw.cos();
        let sin_yaw = camera_state.yaw.sin();
        let move_speed = camera_state.move_speed;

        let world_move = Vec3::new(
            move_dir.x * cos_yaw - move_dir.y * sin_yaw,
            move_dir.x * sin_yaw + move_dir.y * cos_yaw,
            move_dir.z,
        );

        camera_state.focus += world_move.normalize() * move_speed * dt;
    }

    // Update camera transform
    if let Ok(mut transform) = camera_query.get_single_mut() {
        let camera_pos = calculate_camera_position(&camera_state);
        transform.translation = camera_pos;
        transform.look_at(camera_state.focus, Vec3::Z);
    }
}

fn cleanup_editor(
    mut commands: Commands,
    query: Query<Entity, Or<(With<TrackMeshEntity>, With<TerrainMeshEntity>, With<EditorCamera>)>>,
) {
    for entity in query.iter() {
        commands.entity(entity).despawn_recursive();
    }
}
