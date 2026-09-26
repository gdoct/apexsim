//! Writing `.uescene.json` exports (and the server's sidecars) to disk.
//!
//! Exports are *generated* files — the `.ats` and the `.yaml` remain the
//! only sources of truth — so they land in their own directory rather than
//! as siblings of the content they were baked from, and that directory is
//! gitignored. A full circuit bakes to tens of megabytes of vertex data;
//! committing 26 of those would dwarf the content it came from.
//!
//! An export is two files (format version 2): `<Stem>.uescene.json`, a
//! small JSON manifest with everything but the vertex data, and
//! `<Stem>.uemesh`, a little-endian binary blob with every mesh's buffers,
//! each zlib-compressed where that helps. Vertex data in JSON is several
//! seconds and hundreds of megabytes of DOM to parse at runtime; the blob is
//! a copy. The manifest's scalar keys come before its first array, so a
//! catalog scanner can read the track's identity from the first few KB. The
//! layout is `track-editor/TRACK_EDITOR.md` §5 and
//! `ApexTrackSceneReader` on the Unreal side reads exactly it.
//!
//! The exceptions are the server's sidecars, `<Track>.ground.msgpack` and
//! `<Track>.curbs.msgpack` and `<Track>.walls.msgpack`: the server reads them from beside the YAML
//! (they are the sim's ground and track limits, not client content), so
//! they are written there — and gitignored there, since they are generated
//! all the same.

use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

use serde::{Deserialize, Serialize};

use crate::project;
use crate::terrain::GroundHeightfield;
use crate::ue_export::{
    self, CurbBands, UeCenterlinePoint, UeDressing, UeGridSlot, UeMaterial, UeMesh, UeMetadata,
    UePitLane, UeProp, UeScene, UeStartFinish, Walls, UE_SCENE_VERSION,
};

/// Where exports go when no destination is given, relative to the repo root.
pub const DEFAULT_EXPORT_DIR: &str = "content/tracks/export";

#[derive(Debug, thiserror::Error)]
pub enum UeExportError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("MessagePack error: {0}")]
    MsgPack(#[from] rmp_serde::encode::Error),
    #[error("{0}")]
    Project(String),
    #[error("track {0} has no usable centerline")]
    Degenerate(String),
    #[error("elevation sidecar: {0}")]
    Dem(#[from] crate::dem::DemError),
    #[error("unsupported .uescene.json version {0}")]
    UnsupportedVersion(u32),
    #[error("mesh {name}: {reason}")]
    InvalidMesh { name: String, reason: String },
    #[error("mesh blob: {0}")]
    MeshBlob(String),
}

/// `Monza.yaml` -> `<dir>/Monza.uescene.json`.
pub fn export_path_for(dir: &Path, track_path: &Path) -> PathBuf {
    let stem = track_path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "Track".to_string());
    dir.join(format!("{stem}.uescene.json"))
}

/// `<dir>/Monza.uescene.json` -> `<dir>/Monza.uemesh`: the mesh blob that
/// goes with a manifest. A path not ending in `.uescene.json` loses its
/// last extension instead.
pub fn mesh_blob_path_for(scene_path: &Path) -> PathBuf {
    let name = scene_path
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    let stem = match name.len().checked_sub(SCENE_SUFFIX.len()) {
        Some(cut)
            if name.is_char_boundary(cut) && name[cut..].eq_ignore_ascii_case(SCENE_SUFFIX) =>
        {
            name[..cut].to_string()
        }
        _ => scene_path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "Track".to_string()),
    };
    scene_path.with_file_name(format!("{stem}{MESH_BLOB_SUFFIX}"))
}

const SCENE_SUFFIX: &str = ".uescene.json";
const MESH_BLOB_SUFFIX: &str = ".uemesh";

/// The CRC-32 (zlib/PNG polynomial; `"123456789"` -> `0xCBF43926`) of a
/// content file's bytes with every carriage return dropped, so a checkout
/// with CRLF line endings hashes the same as one without. The same rule as
/// `server/src/content_crc.rs` and `scripts/build_track_catalog.py`.
pub fn source_crc(bytes: &[u8]) -> u32 {
    let mut crc = flate2::Crc::new();
    for run in bytes.split(|&b| b == b'\r') {
        crc.update(run);
    }
    crc.sum()
}

/// `Monza.yaml` -> `<same dir>/Monza.ground.msgpack`.
pub fn ground_sidecar_path_for(track_path: &Path) -> PathBuf {
    track_path.with_extension("ground.msgpack")
}

/// `Monza.yaml` -> `<same dir>/Monza.curbs.msgpack`.
pub fn curb_sidecar_path_for(track_path: &Path) -> PathBuf {
    track_path.with_extension("curbs.msgpack")
}

/// `Monza.yaml` -> `<same dir>/Monza.walls.msgpack`.
pub fn walls_sidecar_path_for(track_path: &Path) -> PathBuf {
    track_path.with_extension("walls.msgpack")
}

/// What one track's export wrote.
pub struct Exported {
    /// The JSON manifest.
    pub scene_path: PathBuf,
    /// The mesh blob beside it.
    pub mesh_blob_path: PathBuf,
    /// `None` for a track too degenerate to have a terrain.
    pub ground_path: Option<PathBuf>,
    /// `None` for a track with no usable centerline length.
    pub curb_path: Option<PathBuf>,
    /// The walls, always written (a track with nothing to hit gets an
    /// empty file, which the server reads as such).
    pub walls_path: PathBuf,
}

/// Bake the track at `track_path` (plus its sibling `.ats`) into `dir`, and
/// its server sidecars next to the YAML.
///
/// A track whose `.ats` is missing or unreadable still exports: the scene
/// layers are simply empty, and you get the bare road ribbon. That is worth
/// having — it is the difference between "the circuit is drivable in Unreal"
/// and "nothing loads".
pub fn export_track(track_path: &Path, dir: &Path) -> Result<Exported, UeExportError> {
    let opened = project::open_project(track_path).map_err(UeExportError::Project)?;
    let scene = opened.scene.unwrap_or_else(|| {
        crate::ats::AtsScene::new_for_track(
            &opened.track,
            &track_path.file_name().unwrap_or_default().to_string_lossy(),
        )
    });

    // The elevation sidecar is what gives the circuit its real ground and
    // its skyline. A track without one is baked exactly as before, and an
    // unreadable one is reported rather than silently ignored: a horizon
    // that quietly stops appearing is the kind of regression nobody
    // notices until a screenshot.
    let dem_path = crate::dem::dem_path_for(track_path);
    let dem = crate::dem::load_dem(&dem_path).map_err(UeExportError::Dem)?;

    let mut baked = ue_export::bake_all_with_dem(&opened.track, &scene, dem.as_ref())
        .ok_or_else(|| UeExportError::Degenerate(opened.track.name.clone()))?;
    baked.scene.source_crc = Some(source_crc(&fs::read(track_path)?));
    let scene_path = export_path_for(dir, track_path);
    write_scene(&scene_path, &baked.scene)?;
    let ground_path = match &baked.ground {
        Some(ground) => {
            let path = ground_sidecar_path_for(track_path);
            write_ground_sidecar(&path, ground)?;
            Some(path)
        }
        None => None,
    };
    let curb_path = match &baked.curbs {
        Some(curbs) => {
            let path = curb_sidecar_path_for(track_path);
            write_curb_sidecar(&path, curbs)?;
            Some(path)
        }
        None => None,
    };
    let walls_path = walls_sidecar_path_for(track_path);
    write_walls_sidecar(&walls_path, &baked.walls)?;
    Ok(Exported {
        mesh_blob_path: mesh_blob_path_for(&scene_path),
        scene_path,
        ground_path,
        curb_path,
        walls_path,
    })
}

/// The server's ground heightfield, `rmp_serde::to_vec_named`, written
/// temp-then-rename like everything else.
pub fn write_ground_sidecar(path: &Path, ground: &GroundHeightfield) -> Result<(), UeExportError> {
    write_msgpack(path, &rmp_serde::to_vec_named(ground)?)
}

fn write_msgpack(path: &Path, serialized: &[u8]) -> Result<(), UeExportError> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    let tmp = path.with_extension("msgpack.tmp");
    fs::write(&tmp, serialized)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

/// The server's curb bands, written the same way as the ground sidecar.
pub fn write_curb_sidecar(path: &Path, curbs: &CurbBands) -> Result<(), UeExportError> {
    write_msgpack(path, &rmp_serde::to_vec_named(curbs)?)
}

/// The server's walls, written the same way.
pub fn write_walls_sidecar(path: &Path, walls: &Walls) -> Result<(), UeExportError> {
    write_msgpack(path, &rmp_serde::to_vec_named(walls)?)
}

/// Write a scene as its two files: the mesh blob at
/// [`mesh_blob_path_for`]`(path)` first, then the JSON manifest at `path`,
/// each temp-then-rename like the `.ats` saver, so an interrupted export
/// never leaves a half-written file for the commandlet to choke on. The
/// manifest goes last because it is what readers look for. Compact JSON
/// rather than pretty: the manifest is machine-read.
///
/// The scene's own `version` is not what is written: the files are always
/// the current layout, [`UE_SCENE_VERSION`].
pub fn write_scene(path: &Path, scene: &UeScene) -> Result<(), UeExportError> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    let blob_path = mesh_blob_path_for(path);
    let blob_name = blob_path
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();

    let blob = encode_mesh_blob(&scene.meshes)?;
    let manifest = ManifestOut {
        format: &scene.format,
        version: UE_SCENE_VERSION,
        track_id: &scene.track_id,
        track_name: &scene.track_name,
        source_track: &scene.source_track,
        source_crc: scene.source_crc,
        closed_loop: scene.closed_loop,
        length_cm: scene.length_cm,
        metadata: &scene.metadata,
        dressing: &scene.dressing,
        mesh_blob: &blob_name,
        materials: &scene.materials,
        meshes: scene.meshes.iter().map(MeshHeader::of).collect(),
        props: &scene.props,
        grid: &scene.grid,
        centerline: &scene.centerline,
        pit_lane: &scene.pit_lane,
        start_finish: &scene.start_finish,
    };
    let serialized = serde_json::to_vec(&manifest)?;

    let blob_tmp = blob_path.with_extension("uemesh.tmp");
    fs::write(&blob_tmp, &blob)?;
    fs::rename(&blob_tmp, &blob_path)?;
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, &serialized)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

/// Read a scene back: a version 2 manifest and the blob it names (resolved
/// beside the manifest), or a version 1 file with the buffers inline, which
/// is what exports from before the split are.
pub fn read_scene(path: &Path) -> Result<UeScene, UeExportError> {
    let bytes = fs::read(path)?;
    let manifest: ManifestIn = serde_json::from_slice(&bytes)?;
    let meshes = match manifest.version {
        1 => manifest
            .meshes
            .into_iter()
            .map(MeshEntryIn::into_inline)
            .collect::<Result<Vec<_>, _>>()?,
        2 => {
            let name = manifest.mesh_blob.as_deref().ok_or_else(|| {
                UeExportError::MeshBlob(format!("{} names no mesh_blob", path.display()))
            })?;
            if name.is_empty() || Path::new(name).file_name() != Some(std::ffi::OsStr::new(name)) {
                return Err(UeExportError::MeshBlob(format!(
                    "mesh_blob {name:?} is not a plain file name"
                )));
            }
            let blob_path = path.with_file_name(name);
            let blob = fs::read(&blob_path)?;
            let meshes = decode_mesh_blob(&blob)
                .map_err(|e| UeExportError::MeshBlob(format!("{}: {e}", blob_path.display())))?;
            check_against_manifest(&manifest.meshes, &meshes)?;
            meshes
        }
        other => return Err(UeExportError::UnsupportedVersion(other)),
    };
    Ok(UeScene {
        format: manifest.format,
        version: manifest.version,
        track_id: manifest.track_id,
        track_name: manifest.track_name,
        source_track: manifest.source_track,
        source_crc: manifest.source_crc,
        closed_loop: manifest.closed_loop,
        length_cm: manifest.length_cm,
        metadata: manifest.metadata,
        dressing: manifest.dressing,
        materials: manifest.materials,
        meshes,
        props: manifest.props,
        grid: manifest.grid,
        centerline: manifest.centerline,
        pit_lane: manifest.pit_lane,
        start_finish: manifest.start_finish,
    })
}

/// The manifest as written. The field order is the key order in the file,
/// and it is part of the contract: every scalar and object a catalog
/// scanner needs comes before `materials`, the first array.
#[derive(Serialize)]
struct ManifestOut<'a> {
    format: &'a str,
    version: u32,
    track_id: &'a Option<String>,
    track_name: &'a str,
    source_track: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    source_crc: Option<u32>,
    closed_loop: bool,
    length_cm: f32,
    metadata: &'a UeMetadata,
    dressing: &'a UeDressing,
    mesh_blob: &'a str,
    materials: &'a [UeMaterial],
    meshes: Vec<MeshHeader<'a>>,
    props: &'a [UeProp],
    grid: &'a [UeGridSlot],
    centerline: &'a [UeCenterlinePoint],
    pit_lane: &'a Option<UePitLane>,
    start_finish: &'a Option<UeStartFinish>,
}

/// One mesh in the manifest: what it is and how big, in blob order.
#[derive(Serialize)]
struct MeshHeader<'a> {
    name: &'a str,
    material_key: &'a str,
    vertex_count: u32,
    index_count: u32,
}

impl<'a> MeshHeader<'a> {
    fn of(mesh: &'a UeMesh) -> Self {
        MeshHeader {
            name: &mesh.name,
            material_key: &mesh.material_key,
            vertex_count: (mesh.positions.len() / 3) as u32,
            index_count: mesh.indices.len() as u32,
        }
    }
}

/// The manifest as read: version 2's headers or version 1's inline meshes.
#[derive(Deserialize)]
struct ManifestIn {
    format: String,
    version: u32,
    #[serde(default)]
    track_id: Option<String>,
    track_name: String,
    source_track: String,
    #[serde(default)]
    source_crc: Option<u32>,
    closed_loop: bool,
    length_cm: f32,
    metadata: UeMetadata,
    dressing: UeDressing,
    #[serde(default)]
    mesh_blob: Option<String>,
    materials: Vec<UeMaterial>,
    meshes: Vec<MeshEntryIn>,
    props: Vec<UeProp>,
    grid: Vec<UeGridSlot>,
    centerline: Vec<UeCenterlinePoint>,
    #[serde(default)]
    pit_lane: Option<UePitLane>,
    #[serde(default)]
    start_finish: Option<UeStartFinish>,
}

#[derive(Deserialize)]
struct MeshEntryIn {
    name: String,
    material_key: String,
    #[serde(default)]
    vertex_count: Option<u32>,
    #[serde(default)]
    index_count: Option<u32>,
    #[serde(default)]
    positions: Option<Vec<f32>>,
    #[serde(default)]
    normals: Option<Vec<f32>>,
    #[serde(default)]
    uvs: Option<Vec<f32>>,
    #[serde(default)]
    indices: Option<Vec<u32>>,
}

impl MeshEntryIn {
    /// A version 1 mesh, buffers and all.
    fn into_inline(self) -> Result<UeMesh, UeExportError> {
        let (Some(positions), Some(normals), Some(uvs), Some(indices)) =
            (self.positions, self.normals, self.uvs, self.indices)
        else {
            return Err(UeExportError::InvalidMesh {
                name: self.name,
                reason: "a version 1 mesh carries its positions, normals, uvs and indices"
                    .to_string(),
            });
        };
        let mesh = UeMesh {
            name: self.name,
            material_key: self.material_key,
            positions,
            normals,
            uvs,
            indices,
        };
        mesh_counts(&mesh)?;
        Ok(mesh)
    }
}

/// The blob must hold the meshes the manifest lists, in its order.
fn check_against_manifest(headers: &[MeshEntryIn], meshes: &[UeMesh]) -> Result<(), UeExportError> {
    if headers.len() != meshes.len() {
        return Err(UeExportError::MeshBlob(format!(
            "manifest lists {} meshes, blob holds {}",
            headers.len(),
            meshes.len()
        )));
    }
    for (i, (header, mesh)) in headers.iter().zip(meshes).enumerate() {
        let vertices = (mesh.positions.len() / 3) as u32;
        let indices = mesh.indices.len() as u32;
        if header.name != mesh.name
            || header.material_key != mesh.material_key
            || header.vertex_count != Some(vertices)
            || header.index_count != Some(indices)
        {
            return Err(UeExportError::MeshBlob(format!(
                "mesh {i}: manifest says {:?}/{:?} with {:?} vertices and {:?} indices, \
                 blob holds {:?}/{:?} with {vertices} and {indices}",
                header.name,
                header.material_key,
                header.vertex_count,
                header.index_count,
                mesh.name,
                mesh.material_key,
            )));
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// The mesh blob
// ---------------------------------------------------------------------------

/// The first eight bytes of every `.uemesh`.
pub const MESH_BLOB_MAGIC: &[u8; 8] = b"APEXMESH";
/// The blob's own layout version (independent of the manifest's).
pub const MESH_BLOB_VERSION: u32 = 1;
/// Per-mesh flag: the payload is a zlib stream (RFC 1950, with header).
pub const MESH_FLAG_ZLIB: u32 = 1;

/// A mesh's vertex and index counts, after checking its buffers agree:
/// three floats of position and of normal and two of uv per vertex, and
/// every index naming a vertex.
fn mesh_counts(mesh: &UeMesh) -> Result<(u32, u32), UeExportError> {
    let bad = |reason: String| UeExportError::InvalidMesh {
        name: mesh.name.clone(),
        reason,
    };
    if !mesh.positions.len().is_multiple_of(3) {
        return Err(bad(format!(
            "{} position floats is not a whole number of vertices",
            mesh.positions.len()
        )));
    }
    let v = mesh.positions.len() / 3;
    if mesh.normals.len() != 3 * v {
        return Err(bad(format!(
            "{} normal floats for {v} vertices",
            mesh.normals.len()
        )));
    }
    if mesh.uvs.len() != 2 * v {
        return Err(bad(format!(
            "{} uv floats for {v} vertices",
            mesh.uvs.len()
        )));
    }
    if let Some(&i) = mesh.indices.iter().find(|&&i| i as usize >= v) {
        return Err(bad(format!("index {i} past its {v} vertices")));
    }
    let v = u32::try_from(v).map_err(|_| bad("too many vertices".to_string()))?;
    let i = u32::try_from(mesh.indices.len()).map_err(|_| bad("too many indices".to_string()))?;
    Ok((v, i))
}

/// A mesh's payload, uncompressed: positions, normals, uvs, indices.
fn raw_payload(mesh: &UeMesh) -> Vec<u8> {
    let floats = mesh.positions.len() + mesh.normals.len() + mesh.uvs.len();
    let mut out = Vec::with_capacity(4 * (floats + mesh.indices.len()));
    for f in mesh.positions.iter().chain(&mesh.normals).chain(&mesh.uvs) {
        out.extend_from_slice(&f.to_le_bytes());
    }
    for i in &mesh.indices {
        out.extend_from_slice(&i.to_le_bytes());
    }
    out
}

/// A mesh's flags and the payload bytes stored for it.
type Stored = (u32, Vec<u8>);

/// A mesh's stored payload and its flags: zlib at the default level when
/// that is smaller, raw otherwise.
fn stored_payload(mesh: &UeMesh) -> Result<Stored, UeExportError> {
    let raw = raw_payload(mesh);
    let mut encoder = flate2::write::ZlibEncoder::new(
        Vec::with_capacity(raw.len() / 2),
        flate2::Compression::default(),
    );
    encoder.write_all(&raw)?;
    let packed = encoder.finish()?;
    Ok(if packed.len() < raw.len() {
        (MESH_FLAG_ZLIB, packed)
    } else {
        (0, raw)
    })
}

/// Encode meshes as a `.uemesh` blob (layout in `TRACK_EDITOR.md` §5).
///
/// Each mesh is compressed on its own, so the work is spread over the
/// machine's cores; the output depends only on the meshes, never on which
/// thread got which.
pub fn encode_mesh_blob(meshes: &[UeMesh]) -> Result<Vec<u8>, UeExportError> {
    let mut counts = Vec::with_capacity(meshes.len());
    for mesh in meshes {
        counts.push(mesh_counts(mesh)?);
    }
    let count = u32::try_from(meshes.len())
        .map_err(|_| UeExportError::MeshBlob("too many meshes".to_string()))?;

    let workers = std::thread::available_parallelism()
        .map_or(1, |n| n.get())
        .min(meshes.len())
        .max(1);
    let next = AtomicUsize::new(0);
    let mut payloads: Vec<Option<Stored>> = vec![None; meshes.len()];
    std::thread::scope(|scope| -> Result<(), UeExportError> {
        let handles: Vec<_> = (0..workers)
            .map(|_| {
                scope.spawn(|| -> Result<Vec<(usize, Stored)>, UeExportError> {
                    let mut done = Vec::new();
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        let Some(mesh) = meshes.get(i) else {
                            return Ok(done);
                        };
                        done.push((i, stored_payload(mesh)?));
                    }
                })
            })
            .collect();
        for handle in handles {
            let done = handle
                .join()
                .map_err(|_| UeExportError::MeshBlob("a compression thread panicked".into()))??;
            for (i, payload) in done {
                payloads[i] = Some(payload);
            }
        }
        Ok(())
    })?;

    let total: usize = payloads
        .iter()
        .flatten()
        .map(|(_, p)| p.len())
        .sum::<usize>()
        + meshes
            .iter()
            .map(|m| 24 + m.name.len() + m.material_key.len())
            .sum::<usize>();
    let mut out = Vec::with_capacity(16 + total);
    out.extend_from_slice(MESH_BLOB_MAGIC);
    out.extend_from_slice(&MESH_BLOB_VERSION.to_le_bytes());
    out.extend_from_slice(&count.to_le_bytes());
    for ((mesh, (v, i)), payload) in meshes.iter().zip(counts).zip(payloads) {
        let (flags, payload) = payload.expect("every mesh was encoded");
        let stored = u32::try_from(payload.len()).map_err(|_| UeExportError::InvalidMesh {
            name: mesh.name.clone(),
            reason: "payload over 4 GB".to_string(),
        })?;
        for text in [&mesh.name, &mesh.material_key] {
            out.extend_from_slice(&(text.len() as u32).to_le_bytes());
            out.extend_from_slice(text.as_bytes());
        }
        out.extend_from_slice(&v.to_le_bytes());
        out.extend_from_slice(&i.to_le_bytes());
        out.extend_from_slice(&flags.to_le_bytes());
        out.extend_from_slice(&stored.to_le_bytes());
        out.extend_from_slice(&payload);
    }
    Ok(out)
}

/// A cursor over the blob that fails, rather than panics, when it runs out.
struct BlobReader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> BlobReader<'a> {
    fn take(&mut self, n: usize, what: &str) -> Result<&'a [u8], UeExportError> {
        let end = self.at.checked_add(n).filter(|&e| e <= self.bytes.len());
        let Some(end) = end else {
            return Err(UeExportError::MeshBlob(format!(
                "truncated: {what} needs {n} bytes at offset {}, {} left",
                self.at,
                self.bytes.len() - self.at
            )));
        };
        let out = &self.bytes[self.at..end];
        self.at = end;
        Ok(out)
    }

    fn u32(&mut self, what: &str) -> Result<u32, UeExportError> {
        let b = self.take(4, what)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn text(&mut self, what: &str) -> Result<String, UeExportError> {
        let len = self.u32(what)? as usize;
        let bytes = self.take(len, what)?;
        String::from_utf8(bytes.to_vec())
            .map_err(|_| UeExportError::MeshBlob(format!("{what} is not UTF-8")))
    }
}

/// Decode a `.uemesh` blob, checking everything it claims: the magic, the
/// version, that each payload is exactly as long as its counts say (after
/// inflating), that every index names a vertex, and that nothing follows
/// the last mesh.
pub fn decode_mesh_blob(bytes: &[u8]) -> Result<Vec<UeMesh>, UeExportError> {
    let mut r = BlobReader { bytes, at: 0 };
    if r.take(8, "magic")? != MESH_BLOB_MAGIC {
        return Err(UeExportError::MeshBlob(
            "not a mesh blob (bad magic)".into(),
        ));
    }
    let version = r.u32("version")?;
    if version == 0 || version > MESH_BLOB_VERSION {
        return Err(UeExportError::MeshBlob(format!(
            "unsupported blob version {version}"
        )));
    }
    let count = r.u32("mesh count")?;
    // Every mesh takes at least its 24 bytes of header, which bounds the
    // allocation a corrupt count can ask for.
    let mut meshes = Vec::with_capacity((count as usize).min(bytes.len() / 24));
    for m in 0..count {
        let name = r.text("mesh name")?;
        let material_key = r.text("material key")?;
        let v = r.u32("vertex count")? as u64;
        let i = r.u32("index count")? as u64;
        let flags = r.u32("flags")?;
        let stored = r.u32("stored size")? as usize;
        let payload = r.take(stored, "payload")?;
        let bad = |reason: String| UeExportError::MeshBlob(format!("mesh {m} ({name}): {reason}"));
        if flags & !MESH_FLAG_ZLIB != 0 {
            return Err(bad(format!("unknown flags {flags:#x}")));
        }
        let expected = 32 * v + 4 * i;
        let inflated;
        let mut consumed = stored as u64;
        let raw: &[u8] = if flags & MESH_FLAG_ZLIB != 0 {
            let mut decoder = flate2::read::ZlibDecoder::new(payload);
            let mut out = Vec::with_capacity((expected as usize).min(stored.saturating_mul(64)));
            (&mut decoder)
                .take(expected + 1)
                .read_to_end(&mut out)
                .map_err(|e| bad(format!("inflate: {e}")))?;
            consumed = decoder.total_in();
            inflated = out;
            &inflated
        } else {
            payload
        };
        if raw.len() as u64 != expected {
            return Err(bad(format!(
                "payload is {} bytes, {v} vertices and {i} indices need {expected}",
                raw.len()
            )));
        }
        if consumed != stored as u64 {
            return Err(bad(format!(
                "zlib stream ends after {consumed} of its {stored} bytes"
            )));
        }
        let (v, i) = (v as usize, i as usize);
        let floats = |from: usize, n: usize| -> Vec<f32> {
            raw[from..from + 4 * n]
                .chunks_exact(4)
                .map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                .collect()
        };
        let positions = floats(0, 3 * v);
        let normals = floats(12 * v, 3 * v);
        let uvs = floats(24 * v, 2 * v);
        let indices: Vec<u32> = raw[32 * v..]
            .chunks_exact(4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
            .collect();
        if let Some(&bad_index) = indices.iter().find(|&&x| x as usize >= v) {
            return Err(bad(format!("index {bad_index} past its {v} vertices")));
        }
        debug_assert_eq!(indices.len(), i);
        meshes.push(UeMesh {
            name,
            material_key,
            positions,
            normals,
            uvs,
            indices,
        });
    }
    if r.at != bytes.len() {
        return Err(UeExportError::MeshBlob(format!(
            "{} bytes of trailing garbage after the last mesh",
            bytes.len() - r.at
        )));
    }
    Ok(meshes)
}

/// Every `*.yaml` in `dir`, sorted, so a batch export is reproducible.
pub fn track_files_in(dir: &Path) -> Result<Vec<PathBuf>, UeExportError> {
    let mut found: Vec<PathBuf> = fs::read_dir(dir)?
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| {
            p.extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("yaml"))
        })
        .collect();
    found.sort();
    Ok(found)
}
