//! MCP server: lets an external agent (e.g. Claude Code) inspect the open
//! track and edit its `.ats` scene alongside the human user, live.
//!
//! The source `track.yaml` is read-only — the node tools are inspection
//! only. All mutating tools operate on the `.ats` scene (curbs, markings,
//! pit lane, props) and share the same undo stack as the GUI.
//!
//! Streamable HTTP, bound to `127.0.0.1` only. Runs on its own background
//! OS thread with its own Tokio runtime — Bevy's `App::update()` loop never
//! blocks on it. Each MCP tool call is turned into an [`EditorCommand`]
//! sent over a channel and applied to the editor resources by
//! [`drain_mcp_commands`], which runs as an ordinary Bevy system on the
//! main thread, so MCP edits drive viewport rebuilds exactly like GUI
//! edits.

use bevy::prelude::*;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::model::ContentBlock;
use rmcp::schemars;
use rmcp::tool;
use rmcp::tool_router;
use rmcp::transport::streamable_http_server::session::local::LocalSessionManager;
use rmcp::transport::streamable_http_server::{StreamableHttpServerConfig, StreamableHttpService};
use rmcp::ErrorData as McpError;
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::{mpsc, oneshot};

use crate::ats::{AtsScene, Curb, Marking, MarkingKind, PitLane, Prop, PropKind, Side};
use crate::ats_io;
use crate::project;
use crate::scene::TrackPathRes;
use crate::state::{OpenScene, OpenTrack, Selection, StatusLine, UndoStack};
use crate::track_data::{TrackFile, TrackNode};

const DEFAULT_MCP_PORT: u16 = 8420;

fn mcp_port() -> u16 {
    std::env::var("APEXSIM_TRACK_EDITOR_MCP_PORT")
        .ok()
        .and_then(|v| v.parse::<u16>().ok())
        .unwrap_or(DEFAULT_MCP_PORT)
}

/// The URL an MCP client should connect to, e.g. for the editor's own
/// "connect here" hint in the side panel. Kept in sync with
/// [`run_mcp_server`] by sharing [`mcp_port`] rather than duplicating the
/// env var/default logic.
pub fn endpoint_url() -> String {
    format!("http://127.0.0.1:{}/mcp", mcp_port())
}

pub struct McpPlugin;

impl Plugin for McpPlugin {
    fn build(&self, app: &mut App) {
        let (tx, rx) = mpsc::unbounded_channel::<EditorCommand>();
        app.insert_resource(McpCommandReceiver(rx))
            .add_systems(Update, drain_mcp_commands);

        // The MCP server needs its own async runtime; Bevy's own task pools
        // aren't a Tokio reactor. Run it on a detached OS thread so a slow
        // or misbehaving HTTP client can never stall the render/update loop.
        std::thread::spawn(move || match tokio::runtime::Runtime::new() {
            Ok(runtime) => runtime.block_on(run_mcp_server(tx)),
            Err(e) => eprintln!("track-editor: failed to start MCP runtime: {e}"),
        });
    }
}

async fn run_mcp_server(commands: mpsc::UnboundedSender<EditorCommand>) {
    let addr = format!("127.0.0.1:{}", mcp_port());

    let service = StreamableHttpService::new(
        move || Ok(TrackEditorMcp::new(commands.clone())),
        LocalSessionManager::default().into(),
        StreamableHttpServerConfig::default(),
    );
    let router = axum::Router::new().nest_service("/mcp", service);

    let listener = match tokio::net::TcpListener::bind(&addr).await {
        Ok(listener) => listener,
        Err(e) => {
            eprintln!(
                "track-editor: MCP server disabled, couldn't bind {addr}: {e} \
                 (another instance already running?)"
            );
            return;
        }
    };
    println!("track-editor: MCP server listening on http://{addr}/mcp");
    if let Err(e) = axum::serve(listener, router).await {
        eprintln!("track-editor: MCP server stopped: {e}");
    }
}

// ---------------------------------------------------------------------
// Tool parameter types
// ---------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct OpenTrackParams {
    /// Path to the source track file (.yaml/.yml/.json), absolute or
    /// relative to the editor's working directory.
    path: String,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct NodeIndexParams {
    /// Index into the track's node list (0-based).
    index: usize,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct ListNodesParams {
    /// First node index to return. Defaults to 0.
    offset: Option<usize>,
    /// Maximum number of nodes to return. Defaults to 200.
    limit: Option<usize>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct AddCurbParams {
    /// Which track edge the curb hugs: "left" or "right".
    side: String,
    /// Station (meters along the centerline) where the curb starts.
    start_m: f32,
    /// Station where it ends. On closed loops end < start wraps through
    /// the start/finish line.
    end_m: f32,
    /// Curb width outward from the track edge, meters. Defaults to 1.2.
    width_m: Option<f32>,
    /// Style key: red_white (default), yellow_black, green_white, blue_white.
    style: Option<String>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct UpdateCurbParams {
    /// Id of the curb to edit.
    id: u64,
    /// New side ("left"/"right"), if changing.
    side: Option<String>,
    start_m: Option<f32>,
    end_m: Option<f32>,
    width_m: Option<f32>,
    style: Option<String>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct AddMarkingParams {
    /// Kind: start_finish, grid_slot, edge_line, pit_entry, pit_exit, custom.
    kind: String,
    /// Station where the marking starts, meters.
    start_m: f32,
    /// Station where it ends.
    end_m: f32,
    /// Lateral extent from the centerline, meters; positive is left.
    lat_from_m: f32,
    lat_to_m: f32,
    /// Linear RGBA paint color; defaults to opaque white.
    color: Option<[f32; 4]>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct UpdateMarkingParams {
    /// Id of the marking to edit.
    id: u64,
    kind: Option<String>,
    start_m: Option<f32>,
    end_m: Option<f32>,
    lat_from_m: Option<f32>,
    lat_to_m: Option<f32>,
    color: Option<[f32; 4]>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct AddPropParams {
    /// Kind: tree, sign, barrier, tire_wall, building, grandstand, light,
    /// cone, misc.
    kind: String,
    /// Position in track space, meters.
    x: f32,
    y: f32,
    /// Elevation; defaults to 0.
    #[serde(default)]
    z: f32,
    /// Yaw, CCW from +X, radians. Defaults to 0.
    yaw_rad: Option<f32>,
    /// Uniform scale. Defaults to 1.
    scale: Option<f32>,
    /// Asset key for the Unreal importer; defaults to "<kind>_generic".
    asset: Option<String>,
    /// Optional sign/board text.
    text: Option<String>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct UpdatePropParams {
    /// Id of the prop to edit.
    id: u64,
    kind: Option<String>,
    x: Option<f32>,
    y: Option<f32>,
    z: Option<f32>,
    yaw_rad: Option<f32>,
    scale: Option<f32>,
    asset: Option<String>,
    /// New sign text; pass an empty string to clear it.
    text: Option<String>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct ElementIdParams {
    /// Id of the curb, marking, or prop to remove.
    id: u64,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct SetPitLaneParams {
    /// Pit-lane centerline nodes as [x, y, z] in track space, at least 2.
    nodes: Vec<[f32; 3]>,
    /// Lane width, meters. Defaults to 8.
    width_m: Option<f32>,
    /// Number of pit boxes. Defaults to 10.
    box_count: Option<u32>,
    /// Speed limit, km/h. Defaults to 80.
    speed_limit_kmh: Option<f32>,
}

// ---------------------------------------------------------------------
// Bevy <-> Tokio bridge
// ---------------------------------------------------------------------

/// One MCP tool call, packaged with a reply channel. Sent from the Tokio
/// thread, applied on the main thread by [`drain_mcp_commands`].
enum EditorCommand {
    OpenTrack(OpenTrackParams, oneshot::Sender<Result<String, String>>),
    GetTrackInfo(oneshot::Sender<Value>),
    ListNodes(ListNodesParams, oneshot::Sender<Value>),
    GetNode(NodeIndexParams, oneshot::Sender<Result<Value, String>>),
    GetScene(oneshot::Sender<Result<Value, String>>),
    AddCurb(AddCurbParams, oneshot::Sender<Result<u64, String>>),
    UpdateCurb(UpdateCurbParams, oneshot::Sender<Result<(), String>>),
    AddMarking(AddMarkingParams, oneshot::Sender<Result<u64, String>>),
    UpdateMarking(UpdateMarkingParams, oneshot::Sender<Result<(), String>>),
    AddProp(AddPropParams, oneshot::Sender<Result<u64, String>>),
    UpdateProp(UpdatePropParams, oneshot::Sender<Result<(), String>>),
    RemoveElement(ElementIdParams, oneshot::Sender<Result<(), String>>),
    SetPitLane(SetPitLaneParams, oneshot::Sender<Result<(), String>>),
    ClearPitLane(oneshot::Sender<Result<(), String>>),
    Undo(oneshot::Sender<Result<(), String>>),
    Redo(oneshot::Sender<Result<(), String>>),
    Save(oneshot::Sender<Result<(), String>>),
}

#[derive(Resource)]
struct McpCommandReceiver(mpsc::UnboundedReceiver<EditorCommand>);

#[allow(clippy::too_many_arguments)]
fn drain_mcp_commands(
    mut receiver: ResMut<McpCommandReceiver>,
    mut open_track: ResMut<OpenTrack>,
    track_path: Res<TrackPathRes>,
    mut open_scene: ResMut<OpenScene>,
    mut undo_stack: ResMut<UndoStack>,
    mut selection: ResMut<Selection>,
    mut status: ResMut<StatusLine>,
) {
    while let Ok(cmd) = receiver.0.try_recv() {
        match cmd {
            EditorCommand::OpenTrack(params, reply) => {
                let result = match project::open_project(std::path::Path::new(&params.path)) {
                    Ok(project) => {
                        status.0 = project.message.clone();
                        open_track.path = Some(project.track_path);
                        open_track.track = Some(project.track);
                        open_scene.path = Some(project.ats_path);
                        open_scene.scene = project.scene;
                        open_scene.dirty = project.dirty;
                        undo_stack.clear();
                        selection.0 = None;
                        Ok(project.message)
                    }
                    Err(e) => Err(e),
                };
                let _ = reply.send(result);
            }
            EditorCommand::GetTrackInfo(reply) => {
                let _ = reply.send(track_info_json(&open_track, &track_path, &open_scene));
            }
            EditorCommand::ListNodes(params, reply) => {
                let value = match &open_track.track {
                    Some(track) => list_nodes_json(
                        track,
                        params.offset.unwrap_or(0),
                        params.limit.unwrap_or(200),
                    ),
                    None => json!({ "total": 0, "offset": 0, "nodes": [] }),
                };
                let _ = reply.send(value);
            }
            EditorCommand::GetNode(params, reply) => {
                let result = match &open_track.track {
                    Some(track) => track
                        .nodes
                        .get(params.index)
                        .map(|node| node_json(params.index, node))
                        .ok_or_else(|| out_of_range(params.index, track.nodes.len())),
                    None => Err("no track open".to_string()),
                };
                let _ = reply.send(result);
            }
            EditorCommand::GetScene(reply) => {
                let result = match &open_scene.scene {
                    Some(scene) => {
                        serde_json::to_value(scene).map_err(|e| format!("serialize: {e}"))
                    }
                    None => Err("no scene open".to_string()),
                };
                let _ = reply.send(result);
            }
            EditorCommand::AddCurb(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    let side = Side::parse(&params.side)?;
                    let id = s.alloc_id();
                    s.curbs.push(Curb {
                        id,
                        side,
                        start_m: params.start_m,
                        end_m: params.end_m,
                        width_m: params.width_m.unwrap_or(1.2),
                        style: params.style.clone().unwrap_or_else(|| "red_white".into()),
                    });
                    Ok(id)
                });
                let _ = reply.send(result);
            }
            EditorCommand::UpdateCurb(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    let side = params.side.as_deref().map(Side::parse).transpose()?;
                    let curb = s
                        .curb_mut(params.id)
                        .ok_or_else(|| format!("no curb with id {}", params.id))?;
                    if let Some(side) = side {
                        curb.side = side;
                    }
                    if let Some(v) = params.start_m {
                        curb.start_m = v;
                    }
                    if let Some(v) = params.end_m {
                        curb.end_m = v;
                    }
                    if let Some(v) = params.width_m {
                        curb.width_m = v;
                    }
                    if let Some(v) = params.style.clone() {
                        curb.style = v;
                    }
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::AddMarking(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    let kind = MarkingKind::parse(&params.kind)?;
                    let id = s.alloc_id();
                    s.markings.push(Marking {
                        id,
                        kind,
                        start_m: params.start_m,
                        end_m: params.end_m,
                        lat_from_m: params.lat_from_m,
                        lat_to_m: params.lat_to_m,
                        color: params.color.unwrap_or([1.0, 1.0, 1.0, 1.0]),
                    });
                    Ok(id)
                });
                let _ = reply.send(result);
            }
            EditorCommand::UpdateMarking(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    let kind = params.kind.as_deref().map(MarkingKind::parse).transpose()?;
                    let marking = s
                        .marking_mut(params.id)
                        .ok_or_else(|| format!("no marking with id {}", params.id))?;
                    if let Some(kind) = kind {
                        marking.kind = kind;
                    }
                    if let Some(v) = params.start_m {
                        marking.start_m = v;
                    }
                    if let Some(v) = params.end_m {
                        marking.end_m = v;
                    }
                    if let Some(v) = params.lat_from_m {
                        marking.lat_from_m = v;
                    }
                    if let Some(v) = params.lat_to_m {
                        marking.lat_to_m = v;
                    }
                    if let Some(v) = params.color {
                        marking.color = v;
                    }
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::AddProp(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    let kind = PropKind::parse(&params.kind)?;
                    let id = s.alloc_id();
                    s.props.push(Prop {
                        id,
                        kind,
                        asset: params
                            .asset
                            .clone()
                            .unwrap_or_else(|| format!("{}_generic", kind.label())),
                        x: params.x,
                        y: params.y,
                        z: params.z,
                        yaw_rad: params.yaw_rad.unwrap_or(0.0),
                        scale: params.scale.unwrap_or(1.0),
                        text: params.text.clone(),
                    });
                    Ok(id)
                });
                let _ = reply.send(result);
            }
            EditorCommand::UpdateProp(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    let kind = params.kind.as_deref().map(PropKind::parse).transpose()?;
                    let prop = s
                        .prop_mut(params.id)
                        .ok_or_else(|| format!("no prop with id {}", params.id))?;
                    if let Some(kind) = kind {
                        prop.kind = kind;
                    }
                    if let Some(v) = params.x {
                        prop.x = v;
                    }
                    if let Some(v) = params.y {
                        prop.y = v;
                    }
                    if let Some(v) = params.z {
                        prop.z = v;
                    }
                    if let Some(v) = params.yaw_rad {
                        prop.yaw_rad = v;
                    }
                    if let Some(v) = params.scale {
                        prop.scale = v;
                    }
                    if let Some(v) = params.asset.clone() {
                        prop.asset = v;
                    }
                    if let Some(v) = params.text.clone() {
                        prop.text = if v.is_empty() { None } else { Some(v) };
                    }
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::RemoveElement(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    if s.remove_element(params.id) {
                        Ok(())
                    } else {
                        Err(format!("no element with id {}", params.id))
                    }
                });
                let _ = reply.send(result);
            }
            EditorCommand::SetPitLane(params, reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    if params.nodes.len() < 2 {
                        return Err("pit lane needs at least 2 nodes".to_string());
                    }
                    s.pit_lane = Some(PitLane {
                        nodes: params.nodes.clone(),
                        width_m: params.width_m.unwrap_or(8.0),
                        box_count: params.box_count.unwrap_or(10),
                        speed_limit_kmh: params.speed_limit_kmh.unwrap_or(80.0),
                    });
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::ClearPitLane(reply) => {
                let result = mutate_scene(&mut open_scene, &mut undo_stack, &mut status, |s| {
                    if s.pit_lane.take().is_none() {
                        return Err("no pit lane to clear".to_string());
                    }
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::Undo(reply) => {
                let result = apply_history(
                    &mut open_scene,
                    &mut status,
                    |current| undo_stack.undo(current),
                    "undo",
                );
                let _ = reply.send(result);
            }
            EditorCommand::Redo(reply) => {
                let result = apply_history(
                    &mut open_scene,
                    &mut status,
                    |current| undo_stack.redo(current),
                    "redo",
                );
                let _ = reply.send(result);
            }
            EditorCommand::Save(reply) => {
                let result = match (&open_scene.path, &open_scene.scene) {
                    (Some(path), Some(scene)) => {
                        ats_io::save_ats(path, scene).map_err(|e| e.to_string())
                    }
                    (None, _) => Err("scene has no associated file path".to_string()),
                    (_, None) => Err("no scene open".to_string()),
                };
                if result.is_ok() {
                    open_scene.dirty = false;
                    status.0 = "Saved (MCP).".to_string();
                }
                let _ = reply.send(result);
            }
        }
    }
}

/// Shared undo/redo apply logic: both directions need "clone current, swap
/// in the popped snapshot, describe what happened" and differ only in which
/// `UndoStack` method they call.
fn apply_history(
    open_scene: &mut OpenScene,
    status: &mut StatusLine,
    pop: impl FnOnce(AtsScene) -> Option<AtsScene>,
    verb: &str,
) -> Result<(), String> {
    let Some(current) = open_scene.scene.clone() else {
        return Err("no scene open".to_string());
    };
    match pop(current) {
        Some(restored) => {
            open_scene.scene = Some(restored);
            open_scene.dirty = true;
            status.0 = format!("{}{} (MCP).", verb[..1].to_uppercase(), &verb[1..]);
            Ok(())
        }
        None => Err(format!("nothing to {verb}")),
    }
}

fn out_of_range(index: usize, count: usize) -> String {
    format!("node index {index} out of range (0..{count})")
}

/// Mutates the open scene and records an undo entry, but only on success.
/// Contract: `f` must validate everything *before* it starts mutating, so an
/// `Err` return never leaves a partially-applied edit live without a
/// matching undo entry. Every call site above upholds this (parse/lookup
/// checks precede any field write).
fn mutate_scene<T>(
    open_scene: &mut OpenScene,
    undo_stack: &mut UndoStack,
    status: &mut StatusLine,
    f: impl FnOnce(&mut AtsScene) -> Result<T, String>,
) -> Result<T, String> {
    let Some(scene) = open_scene.scene.as_mut() else {
        return Err("no scene open".to_string());
    };
    let snapshot = scene.clone();
    let result = f(scene)?;
    undo_stack.push(snapshot);
    open_scene.dirty = true;
    status.0 = "Edited (MCP).".to_string();
    Ok(result)
}

fn track_info_json(
    open_track: &OpenTrack,
    track_path: &TrackPathRes,
    open_scene: &OpenScene,
) -> Value {
    match &open_track.track {
        Some(track) => json!({
            "loaded": true,
            "path": open_track.path.as_ref().map(|p| p.display().to_string()),
            "name": track.name,
            "track_id": track.track_id,
            "node_count": track.nodes.len(),
            "closed_loop": track.closed_loop,
            "default_width": track.default_width,
            "length_m": track_path.0.as_ref().map(|p| p.total_length_m()),
            "yaml_read_only": true,
            "scene": open_scene.scene.as_ref().map(|s| json!({
                "path": open_scene.path.as_ref().map(|p| p.display().to_string()),
                "dirty": open_scene.dirty,
                "curbs": s.curbs.len(),
                "markings": s.markings.len(),
                "props": s.props.len(),
                "pit_lane": s.pit_lane.is_some(),
            })),
        }),
        None => json!({ "loaded": false }),
    }
}

fn node_json(index: usize, node: &TrackNode) -> Value {
    json!({
        "index": index,
        "x": node.x,
        "y": node.y,
        "z": node.z,
        "width": node.width,
        "width_left": node.width_left,
        "width_right": node.width_right,
        "banking": node.banking,
        "friction": node.friction,
        "surface_type": node.surface_type,
    })
}

fn list_nodes_json(track: &TrackFile, offset: usize, limit: usize) -> Value {
    let total = track.nodes.len();
    let nodes: Vec<Value> = track
        .nodes
        .iter()
        .enumerate()
        .skip(offset)
        .take(limit)
        .map(|(i, node)| node_json(i, node))
        .collect();
    json!({ "total": total, "offset": offset, "nodes": nodes })
}

fn json_result(value: Value) -> CallToolResult {
    CallToolResult::success(vec![ContentBlock::text(value.to_string())])
}

fn text_result(text: impl Into<String>) -> CallToolResult {
    CallToolResult::success(vec![ContentBlock::text(text.into())])
}

// ---------------------------------------------------------------------
// The MCP handler itself
// ---------------------------------------------------------------------

/// One instance is constructed per MCP session (see `StreamableHttpService`
/// setup in `run_mcp_server`), but all instances share the same
/// `commands` sender, so every session talks to the same open scene.
#[derive(Clone)]
struct TrackEditorMcp {
    commands: mpsc::UnboundedSender<EditorCommand>,
}

impl TrackEditorMcp {
    fn new(commands: mpsc::UnboundedSender<EditorCommand>) -> Self {
        Self { commands }
    }

    async fn request<T: Send + 'static>(
        &self,
        make: impl FnOnce(oneshot::Sender<T>) -> EditorCommand,
    ) -> Result<T, McpError> {
        let (tx, rx) = oneshot::channel();
        self.commands
            .send(make(tx))
            .map_err(|_| McpError::internal_error("track editor is not running", None))?;
        rx.await
            .map_err(|_| McpError::internal_error("track editor did not respond", None))
    }

    async fn request_fallible<T: Send + 'static>(
        &self,
        make: impl FnOnce(oneshot::Sender<Result<T, String>>) -> EditorCommand,
    ) -> Result<T, McpError> {
        self.request(make)
            .await?
            .map_err(|e| McpError::invalid_params(e, None))
    }
}

#[tool_router(server_handler)]
impl TrackEditorMcp {
    #[tool(
        description = "Open a source track file (.yaml/.yml/.json) read-only and load or create its sibling .ats scene. A missing .ats is created fresh in memory (saved on the first save call); the YAML is never written."
    )]
    async fn open_track(
        &self,
        Parameters(params): Parameters<OpenTrackParams>,
    ) -> Result<CallToolResult, McpError> {
        let message = self
            .request_fallible(|reply| EditorCommand::OpenTrack(params, reply))
            .await?;
        Ok(text_result(message))
    }

    #[tool(
        description = "Get info about the open track (read-only source YAML) and its .ats scene: name, node count, length, and per-layer element counts. loaded=false if nothing is open."
    )]
    async fn get_track_info(&self) -> Result<CallToolResult, McpError> {
        let value = self.request(EditorCommand::GetTrackInfo).await?;
        Ok(json_result(value))
    }

    #[tool(
        description = "List the track's centerline nodes (read-only; the YAML source cannot be edited). Use offset/limit to page through large tracks."
    )]
    async fn list_nodes(
        &self,
        Parameters(params): Parameters<ListNodesParams>,
    ) -> Result<CallToolResult, McpError> {
        let value = self
            .request(|reply| EditorCommand::ListNodes(params, reply))
            .await?;
        Ok(json_result(value))
    }

    #[tool(description = "Get a single centerline node's full detail by index (read-only).")]
    async fn get_node(
        &self,
        Parameters(params): Parameters<NodeIndexParams>,
    ) -> Result<CallToolResult, McpError> {
        let value = self
            .request_fallible(|reply| EditorCommand::GetNode(params, reply))
            .await?;
        Ok(json_result(value))
    }

    #[tool(
        description = "Get the full .ats scene as JSON: curbs, markings, pit lane, and props with their ids."
    )]
    async fn get_scene(&self) -> Result<CallToolResult, McpError> {
        let value = self.request_fallible(EditorCommand::GetScene).await?;
        Ok(json_result(value))
    }

    #[tool(
        description = "Add a curb strip along a station span of the track edge. Stations are meters along the centerline. Returns the new curb's id. Records an undo entry."
    )]
    async fn add_curb(
        &self,
        Parameters(params): Parameters<AddCurbParams>,
    ) -> Result<CallToolResult, McpError> {
        let id = self
            .request_fallible(|reply| EditorCommand::AddCurb(params, reply))
            .await?;
        Ok(text_result(format!("added curb {id}")))
    }

    #[tool(
        description = "Update fields on an existing curb by id; only provided fields change. Records an undo entry."
    )]
    async fn update_curb(
        &self,
        Parameters(params): Parameters<UpdateCurbParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::UpdateCurb(params, reply))
            .await?;
        Ok(text_result("updated"))
    }

    #[tool(
        description = "Add a painted marking rectangle in station/lateral space (positive lateral = left of centerline). Returns the new marking's id. Records an undo entry."
    )]
    async fn add_marking(
        &self,
        Parameters(params): Parameters<AddMarkingParams>,
    ) -> Result<CallToolResult, McpError> {
        let id = self
            .request_fallible(|reply| EditorCommand::AddMarking(params, reply))
            .await?;
        Ok(text_result(format!("added marking {id}")))
    }

    #[tool(
        description = "Update fields on an existing marking by id; only provided fields change. Records an undo entry."
    )]
    async fn update_marking(
        &self,
        Parameters(params): Parameters<UpdateMarkingParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::UpdateMarking(params, reply))
            .await?;
        Ok(text_result("updated"))
    }

    #[tool(
        description = "Add a world-anchored prop (tree, sign, barrier, tire_wall, building, grandstand, light, cone, misc) at a track-space position. Returns the new prop's id. Records an undo entry."
    )]
    async fn add_prop(
        &self,
        Parameters(params): Parameters<AddPropParams>,
    ) -> Result<CallToolResult, McpError> {
        let id = self
            .request_fallible(|reply| EditorCommand::AddProp(params, reply))
            .await?;
        Ok(text_result(format!("added prop {id}")))
    }

    #[tool(
        description = "Update fields on an existing prop by id; only provided fields change. Pass text=\"\" to clear sign text. Records an undo entry."
    )]
    async fn update_prop(
        &self,
        Parameters(params): Parameters<UpdatePropParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::UpdateProp(params, reply))
            .await?;
        Ok(text_result("updated"))
    }

    #[tool(
        description = "Remove a scene element (curb, marking, or prop) by id. Records an undo entry."
    )]
    async fn remove_element(
        &self,
        Parameters(params): Parameters<ElementIdParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::RemoveElement(params, reply))
            .await?;
        Ok(text_result("removed"))
    }

    #[tool(
        description = "Create or replace the pit lane from a list of [x, y, z] track-space nodes (its own centerline, at least 2 nodes). Records an undo entry."
    )]
    async fn set_pit_lane(
        &self,
        Parameters(params): Parameters<SetPitLaneParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::SetPitLane(params, reply))
            .await?;
        Ok(text_result("pit lane set"))
    }

    #[tool(description = "Remove the pit lane entirely. Records an undo entry.")]
    async fn clear_pit_lane(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::ClearPitLane).await?;
        Ok(text_result("pit lane cleared"))
    }

    #[tool(description = "Undo the last scene edit (from either Claude or the human user).")]
    async fn undo(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::Undo).await?;
        Ok(text_result("undone"))
    }

    #[tool(description = "Redo the last undone scene edit.")]
    async fn redo(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::Redo).await?;
        Ok(text_result("redone"))
    }

    #[tool(
        description = "Save the scene to its .ats file. The source track.yaml is never written."
    )]
    async fn save(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::Save).await?;
        Ok(text_result("saved"))
    }
}
