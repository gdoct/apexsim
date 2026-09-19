use crate::data::{
    GridSlot, RacelinePoint, SurfaceType, TrackConfig, TrackMetadata, TrackPoint, TrackSurface,
};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;
use tracing::{debug, info, warn};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackFileFormat {
    pub name: String,
    #[serde(default)]
    pub track_id: Option<String>,
    pub nodes: Vec<TrackNode>,
    #[serde(default)]
    pub checkpoints: Vec<Checkpoint>,
    /// Node indices of the sector boundaries past the start line, in order.
    /// Two of them (three sectors); anything else is ignored and the lap is
    /// split into even thirds instead.
    #[serde(default)]
    pub sectors: Vec<usize>,
    #[serde(default)]
    pub spawn_points: Vec<SpawnPoint>,
    #[serde(default)]
    pub default_width: f32,
    #[serde(default)]
    pub closed_loop: bool,
    /// Optional raceline (optimal racing line) for AI and visualization
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    /// Track metadata
    #[serde(default)]
    pub metadata: Option<TrackMetadata>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackNode {
    pub x: f32,
    pub y: f32,
    #[serde(default)]
    pub z: f32,
    #[serde(default)]
    pub width: Option<f32>,
    #[serde(default)]
    pub width_left: Option<f32>,
    #[serde(default)]
    pub width_right: Option<f32>,
    #[serde(default)]
    pub banking: Option<f32>,
    #[serde(default)]
    pub friction: Option<f32>,
    #[serde(default)]
    pub surface_type: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Checkpoint {
    pub index_start: usize,
    pub index_end: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SpawnPoint {
    pub position: usize,
    #[serde(default)]
    pub offset_x: f32,
    #[serde(default)]
    pub offset_y: f32,
}

#[derive(Debug)]
pub enum TrackLoadError {
    IoError(std::io::Error),
    ParseError(String),
    InvalidData(String),
}

impl From<std::io::Error> for TrackLoadError {
    fn from(err: std::io::Error) -> Self {
        TrackLoadError::IoError(err)
    }
}

impl std::fmt::Display for TrackLoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TrackLoadError::IoError(e) => write!(f, "IO error: {}", e),
            TrackLoadError::ParseError(e) => write!(f, "Parse error: {}", e),
            TrackLoadError::InvalidData(e) => write!(f, "Invalid data: {}", e),
        }
    }
}

impl std::error::Error for TrackLoadError {}

pub struct TrackLoader;

impl TrackLoader {
    pub fn load_from_file<P: AsRef<Path>>(path: P) -> Result<TrackConfig, TrackLoadError> {
        let path_ref = path.as_ref();
        let content = fs::read_to_string(path_ref)?;
        Self::load_from_string_with_path(&content, Some(path_ref))
    }

    pub fn load_from_string(content: &str) -> Result<TrackConfig, TrackLoadError> {
        Self::load_from_string_with_path(content, None)
    }

    fn load_from_string_with_path(
        content: &str,
        track_path: Option<&Path>,
    ) -> Result<TrackConfig, TrackLoadError> {
        let track_file: TrackFileFormat = if content.trim_start().starts_with('{') {
            serde_json::from_str(content)
                .map_err(|e| TrackLoadError::ParseError(format!("JSON parse error: {}", e)))?
        } else {
            serde_yaml::from_str(content)
                .map_err(|e| TrackLoadError::ParseError(format!("YAML parse error: {}", e)))?
        };

        Self::validate(&track_file)?;
        let mut config = Self::build_track_config(track_file, track_path)?;
        config.content_crc = crate::content_crc::content_crc(content.as_bytes());
        Ok(config)
    }

    fn validate(track: &TrackFileFormat) -> Result<(), TrackLoadError> {
        if track.nodes.len() < 2 {
            return Err(TrackLoadError::InvalidData(
                "Track must have at least 2 nodes".to_string(),
            ));
        }

        if track.default_width <= 0.0 && track.nodes.iter().all(|n| n.width.is_none()) {
            return Err(TrackLoadError::InvalidData(
                "Track must have a default_width or per-node width values".to_string(),
            ));
        }

        for checkpoint in &track.checkpoints {
            if checkpoint.index_start >= track.nodes.len()
                || checkpoint.index_end >= track.nodes.len()
            {
                return Err(TrackLoadError::InvalidData(format!(
                    "Checkpoint indices out of bounds: start={}, end={}, nodes={}",
                    checkpoint.index_start,
                    checkpoint.index_end,
                    track.nodes.len()
                )));
            }
        }

        Ok(())
    }

    fn build_track_config(
        track_file: TrackFileFormat,
        track_path: Option<&Path>,
    ) -> Result<TrackConfig, TrackLoadError> {
        let default_width = if track_file.default_width > 0.0 {
            track_file.default_width
        } else {
            12.0
        };

        let mut centerline_points = SplineInterpolator::interpolate_spline(
            &track_file.nodes,
            track_file.closed_loop,
            default_width,
        )?;

        let metadata = track_file.metadata.clone().unwrap_or_default();

        // Load or generate procedural world if metadata specifies it
        let procedural_world = Self::load_or_generate_procedural_world(
            &track_file.name,
            &mut centerline_points,
            &metadata,
            track_path,
        );

        let start_positions = Self::generate_start_positions(&track_file, &centerline_points);

        let ground =
            track_path.and_then(|path| Self::load_ground_heightfield(&track_file.name, path));
        let curbs = track_path.and_then(|path| Self::load_curb_bands(&track_file.name, path));
        let walls = track_path.and_then(|path| Self::load_walls(&track_file.name, path));

        // Use track_id from file if provided, otherwise generate new UUID
        let track_id = if let Some(track_id_str) = &track_file.track_id {
            uuid::Uuid::parse_str(track_id_str).map_err(|e| {
                TrackLoadError::InvalidData(format!("Invalid track_id format: {}", e))
            })?
        } else {
            uuid::Uuid::new_v4()
        };

        // Convert raceline points to the data structure
        let raceline: Vec<crate::data::RacelinePoint> = track_file
            .raceline
            .into_iter()
            .map(|rl| crate::data::RacelinePoint {
                x: rl.x,
                y: rl.y,
                z: rl.z,
            })
            .collect();

        // Convert checkpoint node indices to centerline distances. The file
        // stores checkpoints as raw node indices; the centerline density is
        // adaptive (interpolated), so indices cannot be used directly —
        // resolve each checkpoint node to its nearest centerline point and
        // keep the distance along the track.
        let mut checkpoints: Vec<f32> = track_file
            .checkpoints
            .iter()
            .filter_map(|cp| {
                let node = track_file.nodes.get(cp.index_start)?;
                centerline_points
                    .iter()
                    .min_by(|a, b| {
                        let da = (a.x - node.x).powi(2) + (a.y - node.y).powi(2);
                        let db = (b.x - node.x).powi(2) + (b.y - node.y).powi(2);
                        da.partial_cmp(&db).unwrap_or(std::cmp::Ordering::Equal)
                    })
                    .map(|p| p.distance_from_start_m)
            })
            .collect();
        checkpoints.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        checkpoints.dedup();

        // Sector boundaries resolve the same way: a node index in the file
        // becomes a station along the interpolated centerline.
        let track_length = centerline_points
            .last()
            .map(|p| p.distance_from_start_m)
            .unwrap_or(0.0);
        let sector_stations: Vec<f32> = track_file
            .sectors
            .iter()
            .filter_map(|idx| {
                let node = track_file.nodes.get(*idx)?;
                centerline_points
                    .iter()
                    .min_by(|a, b| {
                        let da = (a.x - node.x).powi(2) + (a.y - node.y).powi(2);
                        let db = (b.x - node.x).powi(2) + (b.y - node.y).powi(2);
                        da.partial_cmp(&db).unwrap_or(std::cmp::Ordering::Equal)
                    })
                    .map(|p| p.distance_from_start_m)
            })
            .collect();
        let sectors = crate::laps::sectors_from_stations(sector_stations, track_length);

        let mut config = TrackConfig {
            id: track_id,
            name: track_file.name,
            centerline: centerline_points,
            width_m: default_width,
            source_path: None,
            content_crc: 0,
            start_positions,
            track_surface: TrackSurface {
                base_grip: 1.0,
                curb_grip: 0.85,
                off_track_grip: 0.6,
                off_track_drag_mps2: crate::data::OFF_TRACK_DRAG_MPS2,
            },
            pit_lane: None,
            raceline,
            raceline_distances: Vec::new(),
            checkpoints,
            sectors,
            metadata,
            procedural_world,
            ground,
            curbs,
            walls,
        };
        config.rebuild_raceline_distances();
        Ok(config)
    }

    /// Load the baked ground heightfield the track editor writes next to the
    /// track file, if present. Missing is normal (the sidecar is generated,
    /// not committed); a present-but-broken file is a warning, and the track
    /// falls back to centerline elevation everywhere.
    fn load_ground_heightfield(
        track_name: &str,
        track_path: &Path,
    ) -> Option<crate::ground::GroundHeightfield> {
        let sidecar = crate::ground::GroundHeightfield::sidecar_path(track_path);
        if !sidecar.exists() {
            debug!(
                "No ground heightfield for {} ({}); off-track elevation follows the centerline",
                track_name,
                sidecar.display()
            );
            return None;
        }
        match crate::ground::GroundHeightfield::load(&sidecar) {
            Ok(field) => {
                info!(
                    "Loaded ground heightfield for {}: {}x{} cells of {} m",
                    track_name, field.cols, field.rows, field.cell_m
                );
                Some(field)
            }
            Err(e) => {
                warn!(
                    "Ignoring ground heightfield {} for {}: {}",
                    sidecar.display(),
                    track_name,
                    e
                );
                None
            }
        }
    }

    /// Load the baked curb bands the track editor writes next to the track
    /// file, if present. Missing is normal (the sidecar is generated, not
    /// committed); a present-but-broken file is a warning, and the road edge
    /// is the track limit as it was before curbs were baked at all.
    fn load_curb_bands(track_name: &str, track_path: &Path) -> Option<crate::curbs::CurbBands> {
        let sidecar = crate::curbs::CurbBands::sidecar_path(track_path);
        if !sidecar.exists() {
            debug!(
                "No curb bands for {} ({}); the road edge is the track limit",
                track_name,
                sidecar.display()
            );
            return None;
        }
        match crate::curbs::CurbBands::load(&sidecar) {
            Ok(bands) => {
                info!(
                    "Loaded curb bands for {}: {} stations of {} m",
                    track_name,
                    bands.len(),
                    bands.step_m
                );
                Some(bands)
            }
            Err(e) => {
                warn!(
                    "Ignoring curb bands {} for {}: {}",
                    sidecar.display(),
                    track_name,
                    e
                );
                None
            }
        }
    }

    /// Load the baked walls the track editor writes next to the track file,
    /// if present. Missing is normal (the sidecar is generated, not
    /// committed) and means nothing stops a car off the road, as before; a
    /// present-but-broken file is a warning.
    fn load_walls(track_name: &str, track_path: &Path) -> Option<crate::walls::Walls> {
        let sidecar = crate::walls::Walls::sidecar_path(track_path);
        if !sidecar.exists() {
            debug!(
                "No walls for {} ({}); nothing stops a car off the road",
                track_name,
                sidecar.display()
            );
            return None;
        }
        match crate::walls::Walls::load(&sidecar) {
            Ok(walls) => {
                info!("Loaded walls for {}: {} segments", track_name, walls.len());
                Some(walls)
            }
            Err(e) => {
                warn!(
                    "Ignoring walls {} for {}: {}",
                    sidecar.display(),
                    track_name,
                    e
                );
                None
            }
        }
    }

    fn load_or_generate_procedural_world(
        track_name: &str,
        centerline_points: &mut [TrackPoint],
        metadata: &TrackMetadata,
        track_path: Option<&Path>,
    ) -> Option<crate::procgen::ProceduralWorldData> {
        // Check if procedural generation is requested
        let _environment_type = metadata.environment_type.as_ref()?;

        // Try to load from cache first
        if let Some(path) = track_path {
            if let Some(cached) = crate::procgen::terrain::load_terrain_cache(path) {
                debug!("✅ Loaded cached terrain for: {}", track_name);

                // Apply elevation from cached heightmap
                if let Some(ref heightmap) = cached.heightmap {
                    crate::procgen::terrain::apply_track_elevation(centerline_points, heightmap);
                }

                return Some(cached);
            }
        }

        // No cache. `environment_type` is also a scenery hint, so a missing
        // cache is only a problem for tracks that have no elevation of their
        // own: the surveyed circuits carry real z on every node, and
        // procedural terrain would overwrite it rather than add anything.
        if centerline_points.iter().any(|p| p.z != 0.0) {
            debug!(
                "No terrain cache for {}; using the track's own elevation data",
                track_name
            );
        } else {
            warn!(
                "No terrain cache and no elevation data for {} — the track will be flat. \
                 Run with --generate-terrain to create terrain data",
                track_name
            );
        }

        // Return None - track will have flat terrain until terrain is generated
        None
    }

    pub fn generate_procedural_world_for_track(
        track_name: &str,
        centerline_points: &mut [TrackPoint],
        metadata: &TrackMetadata,
    ) -> Option<crate::procgen::ProceduralWorldData> {
        // This function is called during --generate-terrain mode
        let environment_type = metadata.environment_type.as_ref()?;

        info!("🌍 Generating procedural world for track: {}", track_name);
        debug!("   Environment type: {}", environment_type);

        // Get or create seed
        let seed = metadata.terrain_seed.unwrap_or_else(|| {
            // Generate deterministic seed from track name
            use std::collections::hash_map::DefaultHasher;
            use std::hash::{Hash, Hasher};
            let mut hasher = DefaultHasher::new();
            track_name.hash(&mut hasher);
            hasher.finish() as u32
        });

        // Get environment preset
        let preset = match crate::procgen::environment_presets::get_preset(environment_type) {
            Some(p) => p,
            None => {
                warn!("❌ Unknown environment type: {}", environment_type);
                return None;
            }
        };

        // Get parameters with defaults
        let terrain_scale = metadata.terrain_scale.unwrap_or(1.0);
        let blend_width = metadata.terrain_blend_width.unwrap_or(20.0);
        let object_density = metadata.object_density.unwrap_or(0.8);
        let decal_profile = metadata
            .decal_profile
            .clone()
            .unwrap_or_else(|| "default".to_string());

        // Generate procedural world
        match crate::procgen::terrain::generate_procedural_world(
            centerline_points,
            environment_type.clone(),
            seed,
            preset.clone(),
            terrain_scale,
            blend_width,
            object_density,
            decal_profile,
        ) {
            Ok(world_data) => {
                // Apply elevation to track points
                if let Some(ref heightmap) = world_data.heightmap {
                    crate::procgen::terrain::apply_track_elevation(centerline_points, heightmap);
                }

                debug!("✅ Procedural world generated successfully");
                Some(world_data)
            }
            Err(e) => {
                warn!("❌ Failed to generate procedural world: {}", e);
                warn!("   Track will use flat terrain");
                None
            }
        }
    }

    /// Elevation of the road surface at a world position: the nearest
    /// centerline point's height plus the banking shear at that lateral
    /// offset (the same formula the track editor bakes the ribbon with, and
    /// that physics uses at runtime). Load-time only, so a full scan is fine.
    fn road_surface_z(centerline: &[TrackPoint], x: f32, y: f32) -> f32 {
        let Some(nearest) = centerline.iter().min_by(|a, b| {
            let da = (a.x - x).powi(2) + (a.y - y).powi(2);
            let db = (b.x - x).powi(2) + (b.y - y).powi(2);
            da.partial_cmp(&db).unwrap_or(std::cmp::Ordering::Equal)
        }) else {
            return 0.0;
        };
        let dx = x - nearest.x;
        let dy = y - nearest.y;
        // Positive = right of the centerline, matching physics.
        let lateral_right = dx * nearest.heading_rad.sin() - dy * nearest.heading_rad.cos();
        nearest.z - lateral_right * nearest.banking_rad.sin()
    }

    fn generate_start_positions(
        track_file: &TrackFileFormat,
        centerline: &[TrackPoint],
    ) -> Vec<GridSlot> {
        if !track_file.spawn_points.is_empty() {
            track_file
                .spawn_points
                .iter()
                .enumerate()
                .filter_map(|(idx, spawn)| {
                    if spawn.position < centerline.len() {
                        let point = &centerline[spawn.position];
                        let x = point.x + spawn.offset_x;
                        let y = point.y + spawn.offset_y;
                        Some(GridSlot {
                            position: idx as u8 + 1,
                            x,
                            y,
                            z: Self::road_surface_z(centerline, x, y),
                            yaw_rad: point.heading_rad,
                        })
                    } else {
                        None
                    }
                })
                .collect()
        } else {
            if centerline.is_empty() {
                return vec![];
            }

            let start_point = &centerline[0];
            let grid_spacing = 8.0;
            let lateral_spacing = 3.0;

            (0..16)
                .map(|i| {
                    let row = i / 2;
                    let column = i % 2;
                    let offset_forward = -(row as f32) * grid_spacing;
                    let offset_lateral = (column as f32 - 0.5) * lateral_spacing;

                    let cos_h = start_point.heading_rad.cos();
                    let sin_h = start_point.heading_rad.sin();
                    let x = start_point.x + offset_forward * cos_h - offset_lateral * sin_h;
                    let y = start_point.y + offset_forward * sin_h + offset_lateral * cos_h;

                    // The back of the grid is 56 m down the road: seat every
                    // slot on the asphalt under it, not on the start line's
                    // elevation, or the rear rows float (or sink) until the
                    // first physics tick snaps them.
                    GridSlot {
                        position: i + 1,
                        x,
                        y,
                        z: Self::road_surface_z(centerline, x, y),
                        yaw_rad: start_point.heading_rad,
                    }
                })
                .collect()
        }
    }
}

pub struct SplineInterpolator;

impl SplineInterpolator {
    pub fn interpolate_spline(
        nodes: &[TrackNode],
        closed_loop: bool,
        default_width: f32,
    ) -> Result<Vec<TrackPoint>, TrackLoadError> {
        if nodes.len() < 2 {
            return Err(TrackLoadError::InvalidData(
                "Need at least 2 nodes for interpolation".to_string(),
            ));
        }

        let mut track_points = Vec::new();
        // Target arc-length spacing between interpolated points. Density
        // adapts to segment length instead of a fixed 20 points/segment,
        // which oversampled long straights ~10x (Monza: ~46k points → ~7k)
        // and inflated every nearest-point query proportionally.
        const TARGET_POINT_SPACING_M: f32 = 1.0;
        const MIN_POINTS_PER_SEGMENT: usize = 2;
        const MAX_POINTS_PER_SEGMENT: usize = 50;

        for i in 0..nodes.len() {
            let p0_idx = if i == 0 && closed_loop {
                nodes.len() - 1
            } else if i == 0 {
                0
            } else {
                i - 1
            };

            let p1_idx = i;
            let p2_idx = (i + 1) % nodes.len();
            let p3_idx = if closed_loop {
                (i + 2) % nodes.len()
            } else {
                (i + 2).min(nodes.len() - 1)
            };

            if i == nodes.len() - 1 && !closed_loop {
                break;
            }

            let p0 = &nodes[p0_idx];
            let p1 = &nodes[p1_idx];
            let p2 = &nodes[p2_idx];
            let p3 = &nodes[p3_idx];

            let chord_len = ((p2.x - p1.x).powi(2) + (p2.y - p1.y).powi(2)).sqrt();
            let points_per_segment = ((chord_len / TARGET_POINT_SPACING_M).ceil() as usize)
                .clamp(MIN_POINTS_PER_SEGMENT, MAX_POINTS_PER_SEGMENT);

            // The road's *shape* is interpolated across the segment rather
            // than held at its start node, so the track limits follow the
            // same smooth edge the client draws. Holding it made width and
            // banking a staircase with one tread per node. The surface kind
            // and its grip are material properties and stay steps.
            let (wl_a, wr_a) = Self::node_half_widths(p1, default_width);
            let (wl_b, wr_b) = Self::node_half_widths(p2, default_width);
            let banking_a = p1.banking.unwrap_or(0.0);
            let banking_b = p2.banking.unwrap_or(0.0);
            let friction = p1.friction.unwrap_or(1.0);
            let surface_type = Self::parse_surface_type(p1.surface_type.as_deref());

            for j in 0..points_per_segment {
                let t = j as f32 / points_per_segment as f32;
                let point = Self::catmull_rom_point(p0, p1, p2, p3, t);

                track_points.push(TrackPoint {
                    x: point.0,
                    y: point.1,
                    z: point.2,
                    distance_from_start_m: 0.0,
                    width_left_m: wl_a + (wl_b - wl_a) * t,
                    width_right_m: wr_a + (wr_b - wr_a) * t,
                    banking_rad: banking_a + (banking_b - banking_a) * t,
                    camber_rad: 0.0,
                    slope_rad: 0.0,
                    heading_rad: 0.0,
                    surface_type,
                    grip_modifier: friction,
                });
            }
        }

        Self::compute_derived_properties(&mut track_points, closed_loop);

        Ok(track_points)
    }

    /// A node's half-widths, falling back through `width` to the track's
    /// default the same way the file format does.
    fn node_half_widths(node: &TrackNode, default_width: f32) -> (f32, f32) {
        if let (Some(wl), Some(wr)) = (node.width_left, node.width_right) {
            (wl, wr)
        } else if let Some(w) = node.width {
            (w / 2.0, w / 2.0)
        } else {
            (default_width / 2.0, default_width / 2.0)
        }
    }

    fn catmull_rom_point(
        p0: &TrackNode,
        p1: &TrackNode,
        p2: &TrackNode,
        p3: &TrackNode,
        t: f32,
    ) -> (f32, f32, f32) {
        let t2 = t * t;
        let t3 = t2 * t;

        let x = 0.5
            * ((2.0 * p1.x)
                + (-p0.x + p2.x) * t
                + (2.0 * p0.x - 5.0 * p1.x + 4.0 * p2.x - p3.x) * t2
                + (-p0.x + 3.0 * p1.x - 3.0 * p2.x + p3.x) * t3);

        let y = 0.5
            * ((2.0 * p1.y)
                + (-p0.y + p2.y) * t
                + (2.0 * p0.y - 5.0 * p1.y + 4.0 * p2.y - p3.y) * t2
                + (-p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y) * t3);

        let z = 0.5
            * ((2.0 * p1.z)
                + (-p0.z + p2.z) * t
                + (2.0 * p0.z - 5.0 * p1.z + 4.0 * p2.z - p3.z) * t2
                + (-p0.z + 3.0 * p1.z - 3.0 * p2.z + p3.z) * t3);

        (x, y, z)
    }

    fn compute_derived_properties(points: &mut [TrackPoint], closed_loop: bool) {
        if points.is_empty() {
            return;
        }

        let mut cumulative_distance = 0.0;
        points[0].distance_from_start_m = 0.0;

        for i in 1..points.len() {
            let dx = points[i].x - points[i - 1].x;
            let dy = points[i].y - points[i - 1].y;
            let dz = points[i].z - points[i - 1].z;
            let segment_length = (dx * dx + dy * dy + dz * dz).sqrt();
            cumulative_distance += segment_length;
            points[i].distance_from_start_m = cumulative_distance;
        }

        for i in 0..points.len() {
            let next_idx = if i == points.len() - 1 {
                if closed_loop {
                    0
                } else {
                    i
                }
            } else {
                i + 1
            };

            if next_idx != i {
                let dx = points[next_idx].x - points[i].x;
                let dy = points[next_idx].y - points[i].y;
                let dz = points[next_idx].z - points[i].z;
                let dist_2d = (dx * dx + dy * dy).sqrt();

                points[i].heading_rad = dy.atan2(dx);

                if dist_2d > 0.001 {
                    points[i].slope_rad = dz.atan2(dist_2d);
                }
            } else if i > 0 {
                points[i].heading_rad = points[i - 1].heading_rad;
                points[i].slope_rad = points[i - 1].slope_rad;
            }
        }
    }

    fn parse_surface_type(s: Option<&str>) -> SurfaceType {
        match s {
            Some("Asphalt") | Some("asphalt") => SurfaceType::Asphalt,
            Some("Curb") | Some("curb") => SurfaceType::Curb,
            Some("Grass") | Some("grass") => SurfaceType::Grass,
            Some("Gravel") | Some("gravel") => SurfaceType::Gravel,
            Some("Wet") | Some("wet") => SurfaceType::Wet,
            Some("Sand") | Some("sand") => SurfaceType::Sand,
            Some("Concrete") | Some("concrete") => SurfaceType::Concrete,
            _ => SurfaceType::Asphalt,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_simple_track_json() {
        let json = r#"{
            "name": "Test Track",
            "default_width": 10.0,
            "closed_loop": true,
            "nodes": [
                {"x": 0.0, "y": 0.0, "z": 0.0},
                {"x": 100.0, "y": 0.0, "z": 0.0},
                {"x": 100.0, "y": 100.0, "z": 0.0},
                {"x": 0.0, "y": 100.0, "z": 0.0}
            ]
        }"#;

        let track = TrackLoader::load_from_string(json).unwrap();
        assert_eq!(track.name, "Test Track");
        assert!(track.centerline.len() > 4);
        assert_eq!(track.width_m, 10.0);
    }

    #[test]
    fn test_load_simple_track_yaml() {
        let yaml = r#"
name: "Test Track"
default_width: 10.0
closed_loop: true
nodes:
  - x: 0.0
    y: 0.0
    z: 0.0
  - x: 100.0
    y: 0.0
    z: 0.0
  - x: 100.0
    y: 100.0
    z: 0.0
  - x: 0.0
    y: 100.0
    z: 0.0
"#;

        let track = TrackLoader::load_from_string(yaml).unwrap();
        assert_eq!(track.name, "Test Track");
        assert!(track.centerline.len() > 4);
    }

    #[test]
    fn test_track_with_banking_and_friction() {
        let json = r#"{
            "name": "Banked Track",
            "default_width": 12.0,
            "closed_loop": false,
            "nodes": [
                {"x": 0.0, "y": 0.0, "z": 0.0, "banking": 0.0, "friction": 1.0},
                {"x": 100.0, "y": 0.0, "z": 5.0, "banking": 0.1, "friction": 0.9}
            ]
        }"#;

        let track = TrackLoader::load_from_string(json).unwrap();
        assert!(track.centerline.len() >= 2);
    }

    #[test]
    fn test_invalid_track_too_few_nodes() {
        let json = r#"{
            "name": "Invalid",
            "default_width": 10.0,
            "nodes": [
                {"x": 0.0, "y": 0.0, "z": 0.0}
            ]
        }"#;

        let result = TrackLoader::load_from_string(json);
        assert!(result.is_err());
    }

    #[test]
    fn test_catmull_rom_interpolation() {
        let nodes = vec![
            TrackNode {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
            TrackNode {
                x: 10.0,
                y: 0.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
            TrackNode {
                x: 20.0,
                y: 10.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
        ];

        let points = SplineInterpolator::interpolate_spline(&nodes, false, 10.0).unwrap();
        assert!(points.len() > 3);
        assert!(points[0].distance_from_start_m == 0.0);
        assert!(points.last().unwrap().distance_from_start_m > 0.0);
    }

    #[test]
    fn test_heading_calculation() {
        let nodes = vec![
            TrackNode {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
            TrackNode {
                x: 10.0,
                y: 0.0,
                z: 0.0,
                width: None,
                width_left: None,
                width_right: None,
                banking: None,
                friction: None,
                surface_type: None,
            },
        ];

        let points = SplineInterpolator::interpolate_spline(&nodes, false, 10.0).unwrap();
        assert!(points[0].heading_rad.abs() < 0.1);
    }

    /// The grid stretches 56 m back down the road; every slot has to sit on
    /// the asphalt under it, not at the start line's elevation. Spa's start
    /// straight climbs about a metre over the grid, which used to leave the
    /// back rows hanging in the air until the first physics tick.
    #[test]
    fn grid_slots_sit_on_the_road_under_them() {
        let path = Path::new(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../content/tracks/real/Spa.yaml"
        ));
        if !path.exists() {
            eprintln!("skipping: {} not present", path.display());
            return;
        }
        let track = TrackLoader::load_from_file(path).expect("Spa loads");
        assert_eq!(track.start_positions.len(), 16);
        let mut spread = 0.0f32;
        for slot in &track.start_positions {
            let nearest = track
                .centerline
                .iter()
                .min_by(|a, b| {
                    let da = (a.x - slot.x).powi(2) + (a.y - slot.y).powi(2);
                    let db = (b.x - slot.x).powi(2) + (b.y - slot.y).powi(2);
                    da.partial_cmp(&db).unwrap()
                })
                .unwrap();
            assert!(
                (slot.z - nearest.z).abs() < 0.1,
                "slot {} z {} vs road {}",
                slot.position,
                slot.z,
                nearest.z
            );
            spread = spread.max((slot.z - track.start_positions[0].z).abs());
        }
        assert!(
            spread > 0.3,
            "expected the grid to follow the climb, spread {spread}"
        );
    }

    #[test]
    fn content_crc_is_the_source_text_checksum() {
        let yaml = concat!(
            "name: Crc Track\n",
            "track_id: 6f1b5a2e-4c3d-4e2f-9a1b-0c2d3e4f5a6b\n",
            "default_width: 12.0\n",
            "nodes:\n",
            "  - {x: 0.0, y: 0.0}\n",
            "  - {x: 100.0, y: 0.0}\n",
            "  - {x: 100.0, y: 50.0}\n",
            "  - {x: 0.0, y: 50.0}\n",
        );
        let track = TrackLoader::load_from_string(yaml).unwrap();
        assert_eq!(
            track.content_crc,
            crate::content_crc::content_crc(yaml.as_bytes())
        );
        assert_ne!(track.content_crc, 0);
        // Same file checked out with CRLF line endings: same checksum.
        let crlf = yaml.replace('\n', "\r\n");
        assert_eq!(
            TrackLoader::load_from_string(&crlf).unwrap().content_crc,
            track.content_crc
        );
        assert_eq!(TrackConfig::default().content_crc, 0);
    }
}
