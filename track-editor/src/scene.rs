//! The 3D viewport: orbit camera, ground grid, track ribbon, and draggable
//! node handles.
//!
//! The viewport rebuilds whenever [`OpenTrack`] changes (load, node drag,
//! undo/redo) — driven by Bevy's own change detection
//! (`resource_changed::<OpenTrack>`) rather than a hand-rolled dirty flag.

use std::f32::consts::FRAC_PI_2;

use bevy::input::mouse::{MouseMotion, MouseWheel};
use bevy::prelude::*;
use bevy_egui::input::{egui_wants_any_keyboard_input, egui_wants_any_pointer_input};
use bevy_egui::EguiStartupSet;

use crate::coords;
use crate::state::{OpenTrack, UndoStack};
use crate::track_mesh;

/// Marks a node's draggable handle sphere; `.0` is its index into
/// `TrackFile::nodes`.
#[derive(Component)]
pub struct TrackNodeHandle(pub usize);

#[derive(Component)]
struct TrackRibbon;

#[derive(Resource)]
struct OrbitCamera {
    focus: Vec3,
    yaw: f32,
    pitch: f32,
    distance: f32,
}

impl Default for OrbitCamera {
    fn default() -> Self {
        Self {
            focus: Vec3::ZERO,
            yaw: -FRAC_PI_2 / 2.0,
            pitch: 0.55,
            distance: 100.0,
        }
    }
}

pub struct ScenePlugin;

impl Plugin for ScenePlugin {
    fn build(&self, app: &mut App) {
        app.init_resource::<OrbitCamera>()
            .init_resource::<UndoStack>()
            .add_systems(
                PreStartup,
                setup_camera.before(EguiStartupSet::InitContexts),
            )
            .add_systems(Startup, setup_lights)
            .add_systems(
                Update,
                (
                    orbit_camera_input.run_if(not(egui_wants_any_pointer_input)),
                    apply_orbit_camera,
                    draw_ground_grid,
                    undo_redo_input.run_if(not(egui_wants_any_keyboard_input)),
                    rebuild_track_scene.run_if(resource_changed::<OpenTrack>),
                )
                    .chain(),
            );
    }
}

fn setup_camera(mut commands: Commands) {
    commands.spawn(Camera3d::default());
}

fn setup_lights(mut commands: Commands) {
    commands.spawn((
        DirectionalLight {
            illuminance: 8_000.0,
            shadow_maps_enabled: false,
            ..default()
        },
        Transform::from_xyz(60.0, 120.0, 40.0).looking_at(Vec3::ZERO, Vec3::Y),
    ));
    commands.insert_resource(GlobalAmbientLight {
        color: Color::WHITE,
        brightness: 300.0,
        ..default()
    });
}

fn orbit_camera_input(
    mouse_buttons: Res<ButtonInput<MouseButton>>,
    mut motion: MessageReader<MouseMotion>,
    mut wheel: MessageReader<MouseWheel>,
    mut orbit: ResMut<OrbitCamera>,
) {
    let mut delta = Vec2::ZERO;
    for m in motion.read() {
        delta += m.delta;
    }

    if mouse_buttons.pressed(MouseButton::Right) {
        orbit.yaw -= delta.x * 0.005;
        orbit.pitch = (orbit.pitch - delta.y * 0.005).clamp(-1.5, 1.5);
    } else if mouse_buttons.pressed(MouseButton::Middle) {
        let pan_speed = 0.05 * (orbit.distance / 60.0).max(0.2);
        let right = Vec3::new(orbit.yaw.cos(), 0.0, -orbit.yaw.sin());
        orbit.focus -= right * delta.x * pan_speed;
        orbit.focus += Vec3::Y * delta.y * pan_speed;
    }

    for w in wheel.read() {
        orbit.distance = (orbit.distance - w.y * orbit.distance * 0.1).clamp(5.0, 3000.0);
    }
}

fn apply_orbit_camera(orbit: Res<OrbitCamera>, mut camera: Single<&mut Transform, With<Camera3d>>) {
    let rotation = Quat::from_euler(EulerRot::YXZ, orbit.yaw, -orbit.pitch, 0.0);
    camera.translation = orbit.focus + rotation * Vec3::new(0.0, 0.0, orbit.distance);
    camera.look_at(orbit.focus, Vec3::Y);
}

fn draw_ground_grid(mut gizmos: Gizmos) {
    gizmos.grid(
        Isometry3d::from_rotation(Quat::from_rotation_x(FRAC_PI_2)),
        UVec2::splat(60),
        Vec2::splat(10.0),
        Color::srgba(0.35, 0.35, 0.4, 0.35),
    );
}

fn undo_redo_input(
    keys: Res<ButtonInput<KeyCode>>,
    mut undo_stack: ResMut<UndoStack>,
    mut open_track: ResMut<OpenTrack>,
) {
    let ctrl = keys.pressed(KeyCode::ControlLeft) || keys.pressed(KeyCode::ControlRight);
    if !ctrl || open_track.track.is_none() || !keys.just_pressed(KeyCode::KeyZ) {
        return;
    }
    let shift = keys.pressed(KeyCode::ShiftLeft) || keys.pressed(KeyCode::ShiftRight);
    let current = open_track.track.clone().expect("checked above");

    let restored = if shift {
        undo_stack.redo(current)
    } else {
        undo_stack.undo(current)
    };
    if let Some(restored) = restored {
        open_track.track = Some(restored);
        open_track.status = if shift {
            "Redo.".to_string()
        } else {
            "Undo.".to_string()
        };
    }
}

/// Ray-casts the pointer onto the horizontal plane through `plane_y` and
/// returns the world-space hit point, or `None` if the camera is looking
/// too edge-on to the plane to get a stable intersection.
fn pointer_on_horizontal_plane(
    camera: &Camera,
    camera_transform: &GlobalTransform,
    pointer_position: Vec2,
    plane_y: f32,
) -> Option<Vec3> {
    let ray = camera
        .viewport_to_world(camera_transform, pointer_position)
        .ok()?;
    let direction: Vec3 = *ray.direction;
    if direction.y.abs() < 1e-4 {
        return None;
    }
    let t = (plane_y - ray.origin.y) / direction.y;
    if t < 0.0 {
        return None;
    }
    Some(ray.origin + direction * t)
}

fn on_node_drag_start(
    event: On<Pointer<DragStart>>,
    node_query: Query<&TrackNodeHandle>,
    open_track: Res<OpenTrack>,
    mut undo_stack: ResMut<UndoStack>,
) {
    if node_query.get(event.entity).is_err() {
        return;
    }
    if let Some(track) = &open_track.track {
        undo_stack.push(track.clone());
    }
}

fn on_node_drag(
    event: On<Pointer<Drag>>,
    node_query: Query<&TrackNodeHandle>,
    camera: Single<(&Camera, &GlobalTransform)>,
    mut transforms: Query<&mut Transform, With<TrackNodeHandle>>,
    mut open_track: ResMut<OpenTrack>,
) {
    let Ok(TrackNodeHandle(index)) = node_query.get(event.entity) else {
        return;
    };
    let Ok(mut transform) = transforms.get_mut(event.entity) else {
        return;
    };
    let (camera, camera_transform) = *camera;
    let plane_y = transform.translation.y;
    let Some(point) = pointer_on_horizontal_plane(
        camera,
        camera_transform,
        event.pointer_location.position,
        plane_y,
    ) else {
        return;
    };

    transform.translation.x = point.x;
    transform.translation.z = point.z;

    if let Some(node) = open_track
        .track
        .as_mut()
        .and_then(|t| t.nodes.get_mut(*index))
    {
        let (tx, ty, _tz) = coords::from_bevy(point.with_y(plane_y));
        node.x = tx;
        node.y = ty;
    }
}

fn rebuild_track_scene(
    mut commands: Commands,
    open_track: Res<OpenTrack>,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    ribbon_query: Query<Entity, With<TrackRibbon>>,
    node_query: Query<Entity, With<TrackNodeHandle>>,
) {
    for entity in &ribbon_query {
        commands.entity(entity).despawn();
    }
    for entity in &node_query {
        commands.entity(entity).despawn();
    }

    let Some(track) = &open_track.track else {
        return;
    };

    if let Some(mesh) = track_mesh::build_ribbon_mesh(track) {
        commands.spawn((
            Mesh3d(meshes.add(mesh)),
            MeshMaterial3d(materials.add(StandardMaterial {
                base_color: Color::WHITE,
                perceptual_roughness: 0.95,
                ..default()
            })),
            Transform::IDENTITY,
            TrackRibbon,
        ));
    }

    let node_mesh = meshes.add(Sphere::new(1.0));
    let node_material = materials.add(StandardMaterial {
        base_color: Color::srgb(1.0, 0.82, 0.1),
        emissive: LinearRgba::rgb(0.25, 0.2, 0.0),
        unlit: false,
        ..default()
    });

    for (index, node) in track.nodes.iter().enumerate() {
        let mut position = track_mesh::node_bevy_position(node);
        position.y += 0.5; // keep the handle clickable above the ribbon surface
        commands
            .spawn((
                Mesh3d(node_mesh.clone()),
                MeshMaterial3d(node_material.clone()),
                Transform::from_translation(position),
                TrackNodeHandle(index),
            ))
            .observe(on_node_drag_start)
            .observe(on_node_drag);
    }
}
