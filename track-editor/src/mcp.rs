//! MCP server: lets an external agent (e.g. Claude Code) inspect and edit
//! the currently open track alongside the human user, live.
//!
//! Streamable HTTP, bound to `127.0.0.1` only. Runs on its own background
//! OS thread with its own single-threaded-friendly Tokio runtime — Bevy's
//! `App::update()` loop never blocks on it. Each MCP tool call is turned
//! into an [`EditorCommand`] sent over a channel and applied to
//! [`OpenTrack`]/[`UndoStack`] by [`drain_mcp_commands`], which runs as an
//! ordinary Bevy system on the main thread. That keeps exactly one place
//! that ever mutates track state — MCP tool calls, node drags, and
//! undo/redo hotkeys all funnel through the same resources, so the 3D
//! viewport rebuild (`resource_changed::<OpenTrack>`, see `scene.rs`) picks
//! up MCP-driven edits exactly like any other edit.

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

use crate::state::{OpenTrack, UndoStack};
use crate::track_data::{Checkpoint, TrackFile, TrackNode};
use crate::track_io;

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
struct MoveNodeParams {
    /// Index of the node to move.
    index: usize,
    /// New X coordinate, in track space (meters).
    x: f32,
    /// New Y coordinate, in track space (meters).
    y: f32,
    /// New Z (elevation), in track space (meters). Leave unset to keep it.
    z: Option<f32>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct AddNodeParams {
    /// Position to insert at (0-based). Defaults to appending at the end.
    index: Option<usize>,
    /// X coordinate, in track space (meters).
    x: f32,
    /// Y coordinate, in track space (meters).
    y: f32,
    /// Z (elevation), in track space (meters). Defaults to 0.
    #[serde(default)]
    z: f32,
    /// Total track width in meters (split evenly left/right). Ignored if
    /// `width_left`/`width_right` are given.
    width: Option<f32>,
    /// Track width to the left of the centerline, in meters.
    width_left: Option<f32>,
    /// Track width to the right of the centerline, in meters.
    width_right: Option<f32>,
    /// Banking angle in radians (positive banks toward the inside/left).
    banking: Option<f32>,
    /// Local grip multiplier (1.0 = normal).
    friction: Option<f32>,
    /// One of Asphalt, Curb, Grass, Gravel, Sand, Wet, Concrete.
    surface_type: Option<String>,
}

#[derive(Debug, Clone, Deserialize, schemars::JsonSchema)]
struct SetNodePropertyParams {
    /// Index of the node to edit.
    index: usize,
    /// Total track width in meters (split evenly left/right).
    width: Option<f32>,
    /// Track width to the left of the centerline, in meters.
    width_left: Option<f32>,
    /// Track width to the right of the centerline, in meters.
    width_right: Option<f32>,
    /// Banking angle in radians (positive banks toward the inside/left).
    banking: Option<f32>,
    /// Local grip multiplier (1.0 = normal).
    friction: Option<f32>,
    /// One of Asphalt, Curb, Grass, Gravel, Sand, Wet, Concrete.
    surface_type: Option<String>,
}

// ---------------------------------------------------------------------
// Bevy <-> Tokio bridge
// ---------------------------------------------------------------------

/// One MCP tool call, packaged with a reply channel. Sent from the Tokio
/// thread, applied on the main thread by [`drain_mcp_commands`].
enum EditorCommand {
    GetTrackInfo(oneshot::Sender<Value>),
    ListNodes(ListNodesParams, oneshot::Sender<Value>),
    GetNode(NodeIndexParams, oneshot::Sender<Result<Value, String>>),
    MoveNode(MoveNodeParams, oneshot::Sender<Result<(), String>>),
    AddNode(AddNodeParams, oneshot::Sender<Result<usize, String>>),
    RemoveNode(NodeIndexParams, oneshot::Sender<Result<(), String>>),
    SetNodeProperty(SetNodePropertyParams, oneshot::Sender<Result<(), String>>),
    Undo(oneshot::Sender<Result<(), String>>),
    Redo(oneshot::Sender<Result<(), String>>),
    Save(oneshot::Sender<Result<(), String>>),
}

#[derive(Resource)]
struct McpCommandReceiver(mpsc::UnboundedReceiver<EditorCommand>);

fn drain_mcp_commands(
    mut receiver: ResMut<McpCommandReceiver>,
    mut open_track: ResMut<OpenTrack>,
    mut undo_stack: ResMut<UndoStack>,
) {
    while let Ok(cmd) = receiver.0.try_recv() {
        match cmd {
            EditorCommand::GetTrackInfo(reply) => {
                let _ = reply.send(track_info_json(&open_track));
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
                let result = with_track(&open_track, |track| {
                    track
                        .nodes
                        .get(params.index)
                        .map(|node| node_json(params.index, node))
                        .ok_or_else(|| out_of_range(params.index, track.nodes.len()))
                });
                let _ = reply.send(result);
            }
            EditorCommand::MoveNode(params, reply) => {
                let result = mutate_track(&mut open_track, &mut undo_stack, |track| {
                    let count = track.nodes.len();
                    let node = track
                        .nodes
                        .get_mut(params.index)
                        .ok_or_else(|| out_of_range(params.index, count))?;
                    node.x = params.x;
                    node.y = params.y;
                    if let Some(z) = params.z {
                        node.z = z;
                    }
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::AddNode(params, reply) => {
                let result = mutate_track(&mut open_track, &mut undo_stack, |track| {
                    let at = params
                        .index
                        .unwrap_or(track.nodes.len())
                        .min(track.nodes.len());
                    track.nodes.insert(
                        at,
                        TrackNode {
                            x: params.x,
                            y: params.y,
                            z: params.z,
                            width: params.width,
                            width_left: params.width_left,
                            width_right: params.width_right,
                            banking: params.banking,
                            friction: params.friction,
                            surface_type: params.surface_type.clone(),
                        },
                    );
                    reindex_checkpoints_on_insert(&mut track.checkpoints, at);
                    Ok(at)
                });
                let _ = reply.send(result);
            }
            EditorCommand::RemoveNode(params, reply) => {
                let result = mutate_track(&mut open_track, &mut undo_stack, |track| {
                    if track.nodes.len() <= 2 {
                        return Err(
                            "cannot remove node: a track needs at least 2 nodes".to_string()
                        );
                    }
                    if params.index >= track.nodes.len() {
                        return Err(out_of_range(params.index, track.nodes.len()));
                    }
                    track.nodes.remove(params.index);
                    reindex_checkpoints_on_remove(&mut track.checkpoints, params.index);
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::SetNodeProperty(params, reply) => {
                let result = mutate_track(&mut open_track, &mut undo_stack, |track| {
                    let count = track.nodes.len();
                    let node = track
                        .nodes
                        .get_mut(params.index)
                        .ok_or_else(|| out_of_range(params.index, count))?;
                    if let Some(v) = params.width {
                        node.width = Some(v);
                    }
                    if let Some(v) = params.width_left {
                        node.width_left = Some(v);
                    }
                    if let Some(v) = params.width_right {
                        node.width_right = Some(v);
                    }
                    if let Some(v) = params.banking {
                        node.banking = Some(v);
                    }
                    if let Some(v) = params.friction {
                        node.friction = Some(v);
                    }
                    if let Some(v) = params.surface_type.clone() {
                        node.surface_type = Some(v);
                    }
                    Ok(())
                });
                let _ = reply.send(result);
            }
            EditorCommand::Undo(reply) => {
                let result =
                    apply_history(&mut open_track, |current| undo_stack.undo(current), "undo");
                let _ = reply.send(result);
            }
            EditorCommand::Redo(reply) => {
                let result =
                    apply_history(&mut open_track, |current| undo_stack.redo(current), "redo");
                let _ = reply.send(result);
            }
            EditorCommand::Save(reply) => {
                let result = match (&open_track.path, &open_track.track) {
                    (Some(path), Some(track)) => {
                        track_io::save_track_file(path, track).map_err(|e| e.to_string())
                    }
                    (None, _) => Err("track has no associated file path".to_string()),
                    (_, None) => Err("no track open".to_string()),
                };
                if result.is_ok() {
                    open_track.status = "Saved (MCP).".to_string();
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
    open_track: &mut OpenTrack,
    pop: impl FnOnce(TrackFile) -> Option<TrackFile>,
    verb: &str,
) -> Result<(), String> {
    let Some(current) = open_track.track.clone() else {
        return Err("no track open".to_string());
    };
    match pop(current) {
        Some(restored) => {
            open_track.track = Some(restored);
            open_track.status = format!("{}{} (MCP).", verb[..1].to_uppercase(), &verb[1..]);
            Ok(())
        }
        None => Err(format!("nothing to {verb}")),
    }
}

fn out_of_range(index: usize, count: usize) -> String {
    format!("node index {index} out of range (0..{count})")
}

fn with_track<T>(
    open_track: &OpenTrack,
    f: impl FnOnce(&TrackFile) -> Result<T, String>,
) -> Result<T, String> {
    match &open_track.track {
        Some(track) => f(track),
        None => Err("no track open".to_string()),
    }
}

/// Mutates the open track and records an undo entry, but only on success.
/// Contract: `f` must validate everything *before* it starts mutating, so an
/// `Err` return never leaves a partially-applied edit live without a
/// matching undo entry. Every call site above upholds this (index/bounds
/// checks precede any field write).
fn mutate_track<T>(
    open_track: &mut OpenTrack,
    undo_stack: &mut UndoStack,
    f: impl FnOnce(&mut TrackFile) -> Result<T, String>,
) -> Result<T, String> {
    let Some(track) = open_track.track.as_mut() else {
        return Err("no track open".to_string());
    };
    let snapshot = track.clone();
    let result = f(track)?;
    undo_stack.push(snapshot);
    open_track.status = "Edited (MCP).".to_string();
    Ok(result)
}

fn reindex_checkpoints_on_insert(checkpoints: &mut [Checkpoint], inserted_at: usize) {
    for cp in checkpoints.iter_mut() {
        if cp.index_start >= inserted_at {
            cp.index_start += 1;
        }
        if cp.index_end >= inserted_at {
            cp.index_end += 1;
        }
    }
}

/// Drops any checkpoint that referenced the removed node and shifts the rest
/// down by one. There's no sensible way to "repair" a checkpoint whose
/// anchor node no longer exists, so it's dropped rather than left dangling.
fn reindex_checkpoints_on_remove(checkpoints: &mut Vec<Checkpoint>, removed_at: usize) {
    checkpoints.retain(|cp| cp.index_start != removed_at && cp.index_end != removed_at);
    for cp in checkpoints.iter_mut() {
        if cp.index_start > removed_at {
            cp.index_start -= 1;
        }
        if cp.index_end > removed_at {
            cp.index_end -= 1;
        }
    }
}

fn track_info_json(open_track: &OpenTrack) -> Value {
    match &open_track.track {
        Some(track) => json!({
            "loaded": true,
            "path": open_track.path.as_ref().map(|p| p.display().to_string()),
            "name": track.name,
            "track_id": track.track_id,
            "node_count": track.nodes.len(),
            "closed_loop": track.closed_loop,
            "default_width": track.default_width,
            "checkpoint_count": track.checkpoints.len(),
            "spawn_point_count": track.spawn_points.len(),
            "raceline_point_count": track.raceline.len(),
            "metadata": track.metadata,
            "status": open_track.status,
        }),
        None => json!({ "loaded": false, "status": open_track.status }),
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
/// `commands` sender, so every session talks to the same open track.
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
        description = "Get info about the currently open track: name, file path, node/checkpoint/spawn counts, closed_loop, default width, and metadata. loaded=false if nothing is open."
    )]
    async fn get_track_info(&self) -> Result<CallToolResult, McpError> {
        let value = self.request(EditorCommand::GetTrackInfo).await?;
        Ok(json_result(value))
    }

    #[tool(
        description = "List the track's centerline nodes (index, x, y, z, width, banking, friction, surface_type). Use offset/limit to page through large tracks."
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

    #[tool(description = "Get a single node's full detail by index.")]
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
        description = "Move a node to a new (x, y) position in track space (meters); optionally set z (elevation). Records an undo entry."
    )]
    async fn move_node(
        &self,
        Parameters(params): Parameters<MoveNodeParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::MoveNode(params, reply))
            .await?;
        Ok(text_result("moved"))
    }

    #[tool(
        description = "Insert a new centerline node. `index` is the insertion position (defaults to appending at the end). Records an undo entry."
    )]
    async fn add_node(
        &self,
        Parameters(params): Parameters<AddNodeParams>,
    ) -> Result<CallToolResult, McpError> {
        let index = self
            .request_fallible(|reply| EditorCommand::AddNode(params, reply))
            .await?;
        Ok(text_result(format!("added node at index {index}")))
    }

    #[tool(
        description = "Remove a centerline node by index. Refuses to drop the track below 2 nodes; drops any checkpoint anchored to the removed node. Records an undo entry."
    )]
    async fn remove_node(
        &self,
        Parameters(params): Parameters<NodeIndexParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::RemoveNode(params, reply))
            .await?;
        Ok(text_result("removed"))
    }

    #[tool(
        description = "Set one or more physical properties on a node: width, width_left, width_right, banking (radians), friction, surface_type. Only the fields you provide change. Records an undo entry."
    )]
    async fn set_node_property(
        &self,
        Parameters(params): Parameters<SetNodePropertyParams>,
    ) -> Result<CallToolResult, McpError> {
        self.request_fallible(|reply| EditorCommand::SetNodeProperty(params, reply))
            .await?;
        Ok(text_result("updated"))
    }

    #[tool(description = "Undo the last node edit (from either Claude or the human user).")]
    async fn undo(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::Undo).await?;
        Ok(text_result("undone"))
    }

    #[tool(description = "Redo the last undone edit.")]
    async fn redo(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::Redo).await?;
        Ok(text_result("redone"))
    }

    #[tool(description = "Save the open track back to its file path.")]
    async fn save(&self) -> Result<CallToolResult, McpError> {
        self.request_fallible(EditorCommand::Save).await?;
        Ok(text_result("saved"))
    }
}
