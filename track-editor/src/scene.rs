//! The 3D viewport: orbit camera, ground grid, the read-only track ribbon,
//! and the editable `.ats` scene layer (curbs, markings, pit lane, props).
//!
//! The track ribbon rebuilds when [`OpenTrack`] changes (i.e. on load —
//! the YAML is read-only, so that's the only time). Scene elements rebuild
//! when [`OpenScene`] or [`Selection`] change, except while a viewport drag
//! is in flight: rebuilding mid-drag would despawn the entity under the
//! pointer, so the rebuild is deferred until the drag ends.

use std::f32::consts::FRAC_PI_2;

use bevy::input::mouse::{MouseMotion, MouseWheel};
use bevy::prelude::*;
use bevy_egui::input::{egui_wants_any_keyboard_input, egui_wants_any_pointer_input};
use bevy_egui::EguiStartupSet;

use crate::ats::{Prop, PropKind};
use crate::coords;
use crate::state::{
    DragActive, OpenScene, OpenTrack, SelectedElement, Selection, StatusLine, UndoStack,
};
use crate::track_mesh;
use crate::track_path::CenterlinePath;

/// The sampled centerline of the open track, shared by mesh builders and
/// the UI (station ranges in the inspector). Rebuilt on track load.
#[derive(Resource, Default)]
pub struct TrackPathRes(pub Option<CenterlinePath>);

/// Marks the read-only track surface ribbon.
#[derive(Component)]
struct TrackRibbon;

/// Marks every entity belonging to the editable scene layer; all of them
/// are despawned and respawned on rebuild.
#[derive(Component)]
struct SceneElement;

/// One rendered piece of a prop stand-in (trunk, crown, pole, panel, …).
/// All pieces of a prop share its id so a drag can move them together.
#[derive(Component)]
struct PropPiece(u64);

/// Draggable handle sphere for a pit-lane node; `.0` indexes
/// `PitLane::nodes`.
#[derive(Component)]
struct PitNodeHandle(usize);

/// Clicking this entity selects the given element.
#[derive(Component)]
struct ClickSelect(SelectedElement);

#[derive(Resource)]
pub struct OrbitCamera {
    pub focus: Vec3,
    pub yaw: f32,
    pub pitch: f32,
    pub distance: f32,
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
            .init_resource::<TrackPathRes>()
            .init_resource::<Selection>()
            .init_resource::<DragActive>()
            .init_resource::<StatusLine>()
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
                    rebuild_track_visual.run_if(resource_changed::<OpenTrack>),
                    rebuild_scene_elements,
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
        orbit.distance = (orbit.distance - w.y * orbit.distance * 0.1).clamp(5.0, 5000.0);
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
    mut open_scene: ResMut<OpenScene>,
    mut status: ResMut<StatusLine>,
) {
    let ctrl = keys.pressed(KeyCode::ControlLeft) || keys.pressed(KeyCode::ControlRight);
    if !ctrl || open_scene.scene.is_none() || !keys.just_pressed(KeyCode::KeyZ) {
        return;
    }
    let shift = keys.pressed(KeyCode::ShiftLeft) || keys.pressed(KeyCode::ShiftRight);
    let current = open_scene.scene.clone().expect("checked above");

    let restored = if shift {
        undo_stack.redo(current)
    } else {
        undo_stack.undo(current)
    };
    if let Some(restored) = restored {
        open_scene.scene = Some(restored);
        open_scene.dirty = true;
        status.0 = if shift {
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

// ---------------------------------------------------------------------
// Track ribbon (read-only)
// ---------------------------------------------------------------------

fn rebuild_track_visual(
    mut commands: Commands,
    open_track: Res<OpenTrack>,
    mut track_path: ResMut<TrackPathRes>,
    mut orbit: ResMut<OrbitCamera>,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    ribbon_query: Query<Entity, With<TrackRibbon>>,
) {
    for entity in &ribbon_query {
        commands.entity(entity).despawn();
    }

    track_path.0 = open_track
        .track
        .as_ref()
        .and_then(CenterlinePath::from_track);
    let Some(path) = &track_path.0 else {
        return;
    };

    if let Some(mesh) = track_mesh::build_track_ribbon(path) {
        commands
            .spawn((
                Mesh3d(meshes.add(mesh)),
                MeshMaterial3d(materials.add(StandardMaterial {
                    base_color: Color::WHITE,
                    perceptual_roughness: 0.95,
                    ..default()
                })),
                Transform::IDENTITY,
                TrackRibbon,
            ))
            .observe(on_track_click);
    }

    // Frame the whole track in the viewport.
    let mut min = Vec3::splat(f32::MAX);
    let mut max = Vec3::splat(f32::MIN);
    for s in path.samples() {
        let p = coords::to_bevy(s.pos.0, s.pos.1, s.pos.2);
        min = min.min(p);
        max = max.max(p);
    }
    orbit.focus = (min + max) / 2.0;
    orbit.distance = ((max - min).length() * 0.7).clamp(50.0, 4000.0);
}

fn on_track_click(
    _event: On<Pointer<Click>>,
    mut selection: ResMut<Selection>,
    mut status: ResMut<StatusLine>,
) {
    if selection.0.is_some() {
        selection.0 = None;
        status.0 = "Deselected.".to_string();
    }
}

// ---------------------------------------------------------------------
// Scene layer (curbs, markings, pit lane, props)
// ---------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
fn rebuild_scene_elements(
    mut commands: Commands,
    open_scene: Res<OpenScene>,
    selection: Res<Selection>,
    track_path: Res<TrackPathRes>,
    drag: Res<DragActive>,
    mut pending: Local<bool>,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    existing: Query<Entity, With<SceneElement>>,
) {
    if open_scene.is_changed() || selection.is_changed() || track_path.is_changed() {
        *pending = true;
    }
    if !*pending || drag.0 {
        return;
    }
    *pending = false;

    for entity in &existing {
        commands.entity(entity).despawn();
    }

    let Some(scene) = &open_scene.scene else {
        return;
    };
    let Some(path) = &track_path.0 else {
        return;
    };
    let selected = selection.0;

    let mut strip_material = |selected: bool| {
        materials.add(StandardMaterial {
            base_color: Color::WHITE,
            perceptual_roughness: 0.9,
            emissive: if selected {
                LinearRgba::rgb(0.25, 0.25, 0.08)
            } else {
                LinearRgba::BLACK
            },
            ..default()
        })
    };

    for curb in &scene.curbs {
        if let Some(mesh) = track_mesh::build_curb_mesh(path, curb) {
            let is_selected = selected == Some(SelectedElement::Curb(curb.id));
            commands
                .spawn((
                    Mesh3d(meshes.add(mesh)),
                    MeshMaterial3d(strip_material(is_selected)),
                    Transform::IDENTITY,
                    SceneElement,
                    ClickSelect(SelectedElement::Curb(curb.id)),
                ))
                .observe(on_element_click);
        }
    }

    for marking in &scene.markings {
        if let Some(mesh) = track_mesh::build_marking_mesh(path, marking) {
            let is_selected = selected == Some(SelectedElement::Marking(marking.id));
            commands
                .spawn((
                    Mesh3d(meshes.add(mesh)),
                    MeshMaterial3d(strip_material(is_selected)),
                    Transform::IDENTITY,
                    SceneElement,
                    ClickSelect(SelectedElement::Marking(marking.id)),
                ))
                .observe(on_element_click);
        }
    }

    if let Some(pit) = &scene.pit_lane {
        let is_selected = selected == Some(SelectedElement::PitLane);
        if let Some(pit_path) = CenterlinePath::from_polyline(&pit.nodes, pit.width_m / 2.0) {
            if let Some(mesh) = track_mesh::build_strip_mesh(
                &pit_path,
                0.0,
                pit_path.total_length_m(),
                |s| s.width_left_m,
                |s| -s.width_right_m,
                0.01,
                |s| s.surface_color,
            ) {
                commands
                    .spawn((
                        Mesh3d(meshes.add(mesh)),
                        MeshMaterial3d(strip_material(is_selected)),
                        Transform::IDENTITY,
                        SceneElement,
                        ClickSelect(SelectedElement::PitLane),
                    ))
                    .observe(on_element_click);
            }
        }

        let handle_mesh = meshes.add(Sphere::new(0.8));
        let handle_material = materials.add(StandardMaterial {
            base_color: Color::srgb(0.2, 0.6, 1.0),
            emissive: if is_selected {
                LinearRgba::rgb(0.1, 0.2, 0.4)
            } else {
                LinearRgba::BLACK
            },
            ..default()
        });
        for (i, n) in pit.nodes.iter().enumerate() {
            let mut position = coords::to_bevy(n[0], n[1], n[2]);
            position.y += 0.5;
            commands
                .spawn((
                    Mesh3d(handle_mesh.clone()),
                    MeshMaterial3d(handle_material.clone()),
                    Transform::from_translation(position),
                    SceneElement,
                    PitNodeHandle(i),
                    ClickSelect(SelectedElement::PitLane),
                ))
                .observe(on_element_click)
                .observe(on_scene_drag_start)
                .observe(on_pit_node_drag)
                .observe(on_scene_drag_end);
        }
    }

    for prop in &scene.props {
        let is_selected = selected == Some(SelectedElement::Prop(prop.id));
        spawn_prop(
            &mut commands,
            meshes.as_mut(),
            materials.as_mut(),
            prop,
            is_selected,
        );
    }
}

fn on_element_click(
    event: On<Pointer<Click>>,
    targets: Query<&ClickSelect>,
    mut selection: ResMut<Selection>,
) {
    if let Ok(ClickSelect(element)) = targets.get(event.entity) {
        selection.0 = Some(*element);
    }
}

/// Filter matching any draggable scene entity (prop pieces and pit-lane
/// node handles).
type DraggableFilter = Or<(With<PropPiece>, With<PitNodeHandle>)>;

fn on_scene_drag_start(
    event: On<Pointer<DragStart>>,
    draggable: Query<(), DraggableFilter>,
    open_scene: Res<OpenScene>,
    mut undo_stack: ResMut<UndoStack>,
    mut drag: ResMut<DragActive>,
) {
    if draggable.get(event.entity).is_err() {
        return;
    }
    if let Some(scene) = &open_scene.scene {
        undo_stack.push(scene.clone());
    }
    drag.0 = true;
}

fn on_scene_drag_end(
    event: On<Pointer<DragEnd>>,
    draggable: Query<(), DraggableFilter>,
    mut drag: ResMut<DragActive>,
) {
    if draggable.get(event.entity).is_ok() {
        drag.0 = false;
    }
}

fn on_pit_node_drag(
    event: On<Pointer<Drag>>,
    handles: Query<&PitNodeHandle>,
    camera: Single<(&Camera, &GlobalTransform)>,
    mut transforms: Query<&mut Transform, With<PitNodeHandle>>,
    mut open_scene: ResMut<OpenScene>,
) {
    let Ok(PitNodeHandle(index)) = handles.get(event.entity) else {
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

    let index = *index;
    if let Some(node) = open_scene
        .scene
        .as_mut()
        .and_then(|s| s.pit_lane.as_mut())
        .and_then(|p| p.nodes.get_mut(index))
    {
        let (tx, ty, _tz) = coords::from_bevy(point.with_y(plane_y));
        node[0] = tx;
        node[1] = ty;
        open_scene.dirty = true;
    }
}

fn on_prop_drag(
    event: On<Pointer<Drag>>,
    pieces: Query<&PropPiece>,
    camera: Single<(&Camera, &GlobalTransform)>,
    mut piece_transforms: Query<(&PropPiece, &mut Transform)>,
    mut open_scene: ResMut<OpenScene>,
) {
    let Ok(PropPiece(id)) = pieces.get(event.entity) else {
        return;
    };
    let id = *id;
    let Some(prop) = open_scene.scene.as_ref().and_then(|s| s.prop(id)).cloned() else {
        return;
    };

    let (camera, camera_transform) = *camera;
    let old_origin = coords::to_bevy(prop.x, prop.y, prop.z);
    let Some(point) = pointer_on_horizontal_plane(
        camera,
        camera_transform,
        event.pointer_location.position,
        old_origin.y,
    ) else {
        return;
    };
    let delta = Vec3::new(point.x - old_origin.x, 0.0, point.z - old_origin.z);

    for (piece, mut transform) in &mut piece_transforms {
        if piece.0 == id {
            transform.translation += delta;
        }
    }

    let (tx, ty, _tz) = coords::from_bevy(point.with_y(old_origin.y));
    if let Some(prop) = open_scene.scene.as_mut().and_then(|s| s.prop_mut(id)) {
        prop.x = tx;
        prop.y = ty;
        open_scene.dirty = true;
    }
}

// ---------------------------------------------------------------------
// Prop stand-ins
// ---------------------------------------------------------------------

/// A prop stand-in piece: mesh, color, offset from the prop's origin and
/// extra rotation, both in the prop's local (un-yawed) frame, Bevy axes.
struct Piece {
    mesh: Mesh,
    color: Color,
    offset: Vec3,
    emissive: bool,
}

fn prop_pieces(prop: &Prop) -> Vec<Piece> {
    let s = prop.scale;
    match prop.kind {
        PropKind::Tree => vec![
            Piece {
                mesh: Cylinder::new(0.14 * s, 1.6 * s).into(),
                color: Color::srgb(0.42, 0.28, 0.15),
                offset: Vec3::new(0.0, 0.8 * s, 0.0),
                emissive: false,
            },
            Piece {
                mesh: Cone {
                    radius: 1.5 * s,
                    height: 3.4 * s,
                }
                .into(),
                color: Color::srgb(0.13, 0.42, 0.15),
                offset: Vec3::new(0.0, 3.1 * s, 0.0),
                emissive: false,
            },
        ],
        PropKind::Sign => vec![
            Piece {
                mesh: Cylinder::new(0.06 * s, 2.4 * s).into(),
                color: Color::srgb(0.5, 0.5, 0.55),
                offset: Vec3::new(0.0, 1.2 * s, 0.0),
                emissive: false,
            },
            Piece {
                mesh: Cuboid::new(1.6 * s, 1.0 * s, 0.08 * s).into(),
                color: Color::srgb(0.12, 0.3, 0.75),
                offset: Vec3::new(0.0, 2.9 * s, 0.0),
                emissive: false,
            },
        ],
        PropKind::Barrier => vec![Piece {
            mesh: Cuboid::new(6.0 * s, 1.0 * s, 0.35 * s).into(),
            color: Color::srgb(0.8, 0.82, 0.85),
            offset: Vec3::new(0.0, 0.5 * s, 0.0),
            emissive: false,
        }],
        PropKind::TireWall => vec![Piece {
            mesh: Cuboid::new(4.0 * s, 0.9 * s, 0.8 * s).into(),
            color: Color::srgb(0.1, 0.1, 0.12),
            offset: Vec3::new(0.0, 0.45 * s, 0.0),
            emissive: false,
        }],
        PropKind::Building => vec![Piece {
            mesh: Cuboid::new(10.0 * s, 6.0 * s, 8.0 * s).into(),
            color: Color::srgb(0.72, 0.68, 0.6),
            offset: Vec3::new(0.0, 3.0 * s, 0.0),
            emissive: false,
        }],
        PropKind::Grandstand => vec![Piece {
            mesh: Cuboid::new(16.0 * s, 5.0 * s, 7.0 * s).into(),
            color: Color::srgb(0.35, 0.4, 0.55),
            offset: Vec3::new(0.0, 2.5 * s, 0.0),
            emissive: false,
        }],
        PropKind::Light => vec![
            Piece {
                mesh: Cylinder::new(0.08 * s, 8.0 * s).into(),
                color: Color::srgb(0.45, 0.45, 0.5),
                offset: Vec3::new(0.0, 4.0 * s, 0.0),
                emissive: false,
            },
            Piece {
                mesh: Cuboid::new(1.2 * s, 0.3 * s, 0.4 * s).into(),
                color: Color::srgb(0.95, 0.9, 0.6),
                offset: Vec3::new(0.0, 8.1 * s, 0.0),
                emissive: true,
            },
        ],
        PropKind::Cone => vec![Piece {
            mesh: Cone {
                radius: 0.2 * s,
                height: 0.5 * s,
            }
            .into(),
            color: Color::srgb(0.95, 0.45, 0.05),
            offset: Vec3::new(0.0, 0.25 * s, 0.0),
            emissive: false,
        }],
        PropKind::Misc => vec![Piece {
            mesh: Cuboid::new(1.0 * s, 1.0 * s, 1.0 * s).into(),
            color: Color::srgb(0.6, 0.4, 0.7),
            offset: Vec3::new(0.0, 0.5 * s, 0.0),
            emissive: false,
        }],
    }
}

fn spawn_prop(
    commands: &mut Commands,
    meshes: &mut Assets<Mesh>,
    materials: &mut Assets<StandardMaterial>,
    prop: &Prop,
    is_selected: bool,
) {
    let origin = coords::to_bevy(prop.x, prop.y, prop.z);
    // Track yaw is CCW about track +Z (up); with the (x, y, z) -> (x, z, -y)
    // mapping that is exactly a rotation by the same angle about Bevy +Y.
    let rotation = Quat::from_rotation_y(prop.yaw_rad);

    for piece in prop_pieces(prop) {
        let selection_glow = if is_selected {
            LinearRgba::rgb(0.3, 0.3, 0.1)
        } else {
            LinearRgba::BLACK
        };
        let emissive = if piece.emissive {
            LinearRgba::rgb(1.5, 1.4, 0.9)
        } else {
            selection_glow
        };
        commands
            .spawn((
                Mesh3d(meshes.add(piece.mesh)),
                MeshMaterial3d(materials.add(StandardMaterial {
                    base_color: piece.color,
                    perceptual_roughness: 0.85,
                    emissive,
                    ..default()
                })),
                Transform::from_translation(origin + rotation * piece.offset)
                    .with_rotation(rotation),
                SceneElement,
                PropPiece(prop.id),
                ClickSelect(SelectedElement::Prop(prop.id)),
            ))
            .observe(on_element_click)
            .observe(on_scene_drag_start)
            .observe(on_prop_drag)
            .observe(on_scene_drag_end);
    }
}
