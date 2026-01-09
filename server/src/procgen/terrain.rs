/// Core terrain generation logic
use super::noise::TerrainNoise;
use super::world_data::{EnvironmentPreset, ProceduralWorldData, TerrainHeightmap};
use crate::data::TrackPoint;
use std::path::Path;

/// Generate complete procedural world data for a track
///
/// This is the main entry point for procedural generation. It creates
/// terrain, applies track corridor carving, and packages everything
/// into a ProceduralWorldData structure.
pub fn generate_procedural_world(
    track_points: &[TrackPoint],
    environment_type: String,
    seed: u32,
    preset: EnvironmentPreset,
    terrain_scale: f32,
    blend_width: f32,
    object_density: f32,
    decal_profile: String,
) -> Result<ProceduralWorldData, String> {
    // Generate heightmap
    let mut heightmap = generate_terrain(track_points, seed, &preset, terrain_scale)?;

    // Carve track corridor
    carve_track_corridor(&mut heightmap, track_points, blend_width);

    Ok(ProceduralWorldData {
        environment_type,
        seed,
        heightmap: Some(heightmap),
        blend_width,
        object_density,
        decal_profile,
        preset,
    })
}

/// Generate terrain heightmap around track
///
/// Creates a heightmap using layered Perlin noise based on the
/// environment preset parameters.
pub fn generate_terrain(
    track_points: &[TrackPoint],
    seed: u32,
    preset: &EnvironmentPreset,
    terrain_scale: f32,
) -> Result<TerrainHeightmap, String> {
    if track_points.is_empty() {
        return Err("Cannot generate terrain for empty track".to_string());
    }

    // Calculate track bounding box
    let (min_x, min_y, max_x, max_y) = calculate_bounds(track_points);

    // Add padding around track
    let padding = 200.0; // meters
    let min_x = min_x - padding;
    let min_y = min_y - padding;
    let max_x = max_x + padding;
    let max_y = max_y + padding;

    // Determine heightmap resolution
    let cell_size = 5.0; // 5 meters per cell
    let width = ((max_x - min_x) / cell_size).ceil() as usize;
    let height = ((max_y - min_y) / cell_size).ceil() as usize;

    // Limit maximum size to prevent excessive memory usage
    let max_size = 2048;
    if width > max_size || height > max_size {
        return Err(format!(
            "Heightmap too large: {}x{} (max: {}x{})",
            width, height, max_size, max_size
        ));
    }

    println!(
        "Generating terrain heightmap: {}x{} cells ({:.1}m x {:.1}m)",
        width,
        height,
        width as f32 * cell_size,
        height as f32 * cell_size
    );

    // Create heightmap
    let mut heightmap = TerrainHeightmap::new(width, height, cell_size, min_x, min_y);

    // Generate noise-based terrain
    let noise = TerrainNoise::new(seed);

    for y in 0..height {
        for x in 0..width {
            let world_x = min_x + x as f32 * cell_size;
            let world_y = min_y + y as f32 * cell_size;

            // Sample noise
            let noise_value = noise.sample(
                world_x,
                world_y,
                preset.detail_noise_freq,
            );

            // Map noise to height (noise is roughly [-1.75, 1.75], map to [0, max_height])
            let normalized = (noise_value + 1.75) / 3.5; // Map to roughly [0, 1]
            let height_value = normalized * preset.max_height * terrain_scale;

            heightmap.set_height(x, y, height_value);
        }
    }

    Ok(heightmap)
}

/// Carve track corridor by flattening terrain around track
///
/// This creates a smooth transition from the flat track surface
/// to the surrounding terrain within the blend_width distance.
pub fn carve_track_corridor(
    heightmap: &mut TerrainHeightmap,
    track_points: &[TrackPoint],
    blend_width: f32,
) {
    println!("Carving track corridor with {} meter blend width", blend_width);

    for y in 0..heightmap.height {
        for x in 0..heightmap.width {
            let world_x = heightmap.origin_x + x as f32 * heightmap.cell_size_m;
            let world_y = heightmap.origin_y + y as f32 * heightmap.cell_size_m;

            // Find distance to nearest track point
            let (dist_to_track, nearest_track_z) = find_nearest_track_distance(
                track_points,
                world_x,
                world_y,
            );

            // Apply smooth blend within blend_width
            if dist_to_track < blend_width {
                let current_height = heightmap.get_height(x, y);
                let target_height = nearest_track_z; // Track surface height

                // Smooth blend factor (0 at track, 1 at blend_width)
                let blend = (dist_to_track / blend_width).clamp(0.0, 1.0);
                // Use smoothstep for better visual result
                let blend_smooth = blend * blend * (3.0 - 2.0 * blend);

                // Interpolate between track height and terrain height
                let blended_height = target_height * (1.0 - blend_smooth) + current_height * blend_smooth;

                heightmap.set_height(x, y, blended_height);
            }
        }
    }
}

/// Apply terrain elevation to track points
///
/// Modifies the Z coordinate of each track point based on the terrain
/// heightmap, then recalculates all derived properties.
pub fn apply_track_elevation(
    track_points: &mut [TrackPoint],
    heightmap: &TerrainHeightmap,
) {
    println!("Applying terrain elevation to {} track points", track_points.len());

    for point in track_points.iter_mut() {
        let terrain_height = heightmap.sample(point.x, point.y);
        // Place track slightly above terrain (20cm)
        point.z = terrain_height + 0.2;
    }

    // Recalculate all derived properties
    recalculate_track_geometry(track_points);
}

/// Recalculate track geometry after elevation changes
///
/// This is critical for AI navigation - must use 3D distance including Z.
fn recalculate_track_geometry(points: &mut [TrackPoint]) {
    if points.is_empty() {
        return;
    }

    // Recalculate cumulative distance (NOW INCLUDING Z ELEVATION)
    let mut cumulative_distance = 0.0;
    points[0].distance_from_start_m = 0.0;

    for i in 1..points.len() {
        let dx = points[i].x - points[i - 1].x;
        let dy = points[i].y - points[i - 1].y;
        let dz = points[i].z - points[i - 1].z; // Include elevation change

        let segment_length = (dx * dx + dy * dy + dz * dz).sqrt();
        cumulative_distance += segment_length;
        points[i].distance_from_start_m = cumulative_distance;
    }

    // Recalculate heading and slope
    for i in 0..points.len() {
        let next_idx = if i == points.len() - 1 { 0 } else { i + 1 };

        let dx = points[next_idx].x - points[i].x;
        let dy = points[next_idx].y - points[i].y;
        let dz = points[next_idx].z - points[i].z;

        let dist_2d = (dx * dx + dy * dy).sqrt();

        // Heading is 2D direction (unchanged)
        points[i].heading_rad = dy.atan2(dx);

        // Slope is vertical angle
        points[i].slope_rad = if dist_2d > 0.001 {
            dz.atan2(dist_2d)
        } else {
            0.0
        };
    }

    println!(
        "Recalculated track geometry. Total length: {:.1}m",
        points.last().map(|p| p.distance_from_start_m).unwrap_or(0.0)
    );
}

/// Calculate bounding box of track points
fn calculate_bounds(track_points: &[TrackPoint]) -> (f32, f32, f32, f32) {
    let mut min_x = f32::MAX;
    let mut min_y = f32::MAX;
    let mut max_x = f32::MIN;
    let mut max_y = f32::MIN;

    for point in track_points {
        min_x = min_x.min(point.x);
        min_y = min_y.min(point.y);
        max_x = max_x.max(point.x);
        max_y = max_y.max(point.y);
    }

    (min_x, min_y, max_x, max_y)
}

/// Find distance to nearest track point and its elevation
fn find_nearest_track_distance(
    track_points: &[TrackPoint],
    world_x: f32,
    world_y: f32,
) -> (f32, f32) {
    let mut min_dist = f32::MAX;
    let mut nearest_z = 0.0;

    for point in track_points {
        let dx = world_x - point.x;
        let dy = world_y - point.y;
        let dist = (dx * dx + dy * dy).sqrt();

        if dist < min_dist {
            min_dist = dist;
            nearest_z = point.z;
        }
    }

    (min_dist, nearest_z)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::SurfaceType;

    fn create_test_track() -> Vec<TrackPoint> {
        vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 100.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 100.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 100.0,
                y: 100.0,
                z: 0.0,
                distance_from_start_m: 200.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
        ]
    }

    #[test]
    fn test_calculate_bounds() {
        let track = create_test_track();
        let (min_x, min_y, max_x, max_y) = calculate_bounds(&track);

        assert_eq!(min_x, 0.0);
        assert_eq!(min_y, 0.0);
        assert_eq!(max_x, 100.0);
        assert_eq!(max_y, 100.0);
    }

    #[test]
    fn test_generate_terrain() {
        let track = create_test_track();
        let preset = EnvironmentPreset::plains();
        let seed = 12345;

        let heightmap = generate_terrain(&track, seed, &preset, 1.0).unwrap();

        assert!(heightmap.width > 0);
        assert!(heightmap.height > 0);
        assert_eq!(heightmap.heights.len(), heightmap.width * heightmap.height);
    }

    #[test]
    fn test_deterministic_terrain() {
        let track = create_test_track();
        let preset = EnvironmentPreset::plains();
        let seed = 12345;

        let heightmap1 = generate_terrain(&track, seed, &preset, 1.0).unwrap();
        let heightmap2 = generate_terrain(&track, seed, &preset, 1.0).unwrap();

        assert_eq!(heightmap1.heights, heightmap2.heights);
    }

    #[test]
    fn test_track_corridor_carving() {
        let track = create_test_track();
        let preset = EnvironmentPreset::plains();
        let seed = 12345;

        let mut heightmap = generate_terrain(&track, seed, &preset, 1.0).unwrap();
        carve_track_corridor(&mut heightmap, &track, 20.0);

        // Verify track corridor is flattened
        for point in &track {
            let height = heightmap.sample(point.x, point.y);
            assert!(
                height.abs() < 1.0,
                "Track corridor not properly flattened: height = {}",
                height
            );
        }
    }

    #[test]
    fn test_elevation_application() {
        let mut track = create_test_track();
        let preset = EnvironmentPreset::plains();
        let seed = 12345;

        let heightmap = generate_terrain(&track, seed, &preset, 1.0).unwrap();
        apply_track_elevation(&mut track, &heightmap);

        // Verify elevation was applied (should be non-zero for at least some points)
        let has_elevation = track.iter().any(|p| p.z.abs() > 0.1);
        assert!(has_elevation, "No elevation applied to track");

        // Verify distances were recalculated
        assert!(track[1].distance_from_start_m > 0.0);
    }

    #[test]
    fn test_3d_distance_calculation() {
        let mut track = vec![
            TrackPoint {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
            TrackPoint {
                x: 100.0,
                y: 0.0,
                z: 10.0, // 10m elevation gain
                distance_from_start_m: 0.0,
                width_left_m: 5.0,
                width_right_m: 5.0,
                banking_rad: 0.0,
                camber_rad: 0.0,
                slope_rad: 0.0,
                heading_rad: 0.0,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            },
        ];

        recalculate_track_geometry(&mut track);

        // Distance should be sqrt(100^2 + 10^2) = sqrt(10100) ≈ 100.5
        let distance = track[1].distance_from_start_m;
        assert!(
            (distance - 100.5).abs() < 0.1,
            "Expected ~100.5m, got {}m",
            distance
        );
    }
}

/// Batch generate terrain for all tracks in a directory
///
/// This function scans the tracks directory, finds all tracks with
/// `environment_type` metadata, generates terrain for them, and saves
/// the procedural data to cache files.
pub fn generate_all_terrain(tracks_dir: &str) -> Result<usize, String> {
    use std::path::Path;

    let tracks_path = Path::new(tracks_dir);
    if !tracks_path.exists() {
        return Err(format!("Tracks directory not found: {}", tracks_dir));
    }

    println!("Scanning tracks directory: {}", tracks_dir);

    let mut generated_count = 0;
    let mut files_to_process = Vec::new();

    // Collect all YAML/JSON track files
    collect_track_files(tracks_path, &mut files_to_process)?;

    println!("Found {} track file(s)", files_to_process.len());

    for track_file in files_to_process {
        match process_track_for_terrain(&track_file) {
            Ok(true) => {
                generated_count += 1;
                println!("  ✅ Generated terrain for: {}", track_file.display());
            }
            Ok(false) => {
                println!("  ⏭️  Skipped (no environment_type): {}", track_file.display());
            }
            Err(e) => {
                eprintln!("  ❌ Failed to process {}: {}", track_file.display(), e);
            }
        }
    }

    Ok(generated_count)
}

fn collect_track_files(dir: &Path, files: &mut Vec<std::path::PathBuf>) -> Result<(), String> {
    use std::fs;

    let entries = fs::read_dir(dir)
        .map_err(|e| format!("Failed to read directory {:?}: {}", dir, e))?;

    for entry in entries {
        let entry = entry.map_err(|e| format!("Failed to read dir entry: {}", e))?;
        let path = entry.path();

        if path.is_dir() {
            // Recursively scan subdirectories
            collect_track_files(&path, files)?;
        } else if let Some(ext) = path.extension() {
            let ext_str = ext.to_string_lossy();
            if ext_str == "yaml" || ext_str == "yml" || ext_str == "json" {
                files.push(path);
            }
        }
    }

    Ok(())
}

fn process_track_for_terrain(track_file: &Path) -> Result<bool, String> {
    use crate::track_loader::{TrackLoader, SplineInterpolator};
    use std::fs;

    // Load track file to get metadata
    let content = fs::read_to_string(track_file)
        .map_err(|e| format!("Failed to read track file: {}", e))?;

    let track_file_format: crate::track_loader::TrackFileFormat = if content.trim_start().starts_with('{') {
        serde_json::from_str(&content)
            .map_err(|e| format!("JSON parse error: {}", e))?
    } else {
        serde_yaml::from_str(&content)
            .map_err(|e| format!("YAML parse error: {}", e))?
    };

    let metadata = track_file_format.metadata.clone().unwrap_or_default();

    // Check if it needs procedural generation
    if metadata.environment_type.is_none() {
        return Ok(false); // Skip, no procedural metadata
    }

    // Check if terrain cache already exists and is up-to-date
    let cache_path = get_terrain_cache_path(track_file);
    if cache_path.exists() {
        // Check if cache is newer than source file
        let source_modified = fs::metadata(track_file)
            .and_then(|m| m.modified())
            .ok();
        let cache_modified = fs::metadata(&cache_path)
            .and_then(|m| m.modified())
            .ok();

        if let (Some(src), Some(cache)) = (source_modified, cache_modified) {
            if cache >= src {
                println!("  ℹ️  Cache up-to-date: {}", cache_path.display());
                return Ok(false);
            }
        }
    }

    // Generate centerline points for terrain generation
    let default_width = if track_file_format.default_width > 0.0 {
        track_file_format.default_width
    } else {
        12.0
    };

    let mut centerline_points = SplineInterpolator::interpolate_spline(
        &track_file_format.nodes,
        track_file_format.closed_loop,
        default_width,
    ).map_err(|e| format!("Failed to interpolate spline: {}", e))?;

    // Generate procedural world
    let procedural_world = TrackLoader::generate_procedural_world_for_track(
        &track_file_format.name,
        &mut centerline_points,
        &metadata,
    );

    if let Some(procedural_world) = procedural_world {
        // Save to cache file
        save_terrain_cache(&cache_path, &procedural_world)?;
        return Ok(true);
    }

    Ok(false)
}

fn get_terrain_cache_path(track_file: &Path) -> std::path::PathBuf {
    let mut cache_path = track_file.to_path_buf();
    let stem = cache_path.file_stem().unwrap().to_string_lossy();
    cache_path.set_file_name(format!("{}.terrain.msgpack", stem));
    cache_path
}

fn save_terrain_cache(cache_path: &Path, procedural_world: &ProceduralWorldData) -> Result<(), String> {
    use std::fs;

    let msgpack_data = rmp_serde::to_vec(procedural_world)
        .map_err(|e| format!("Failed to serialize terrain data: {}", e))?;

    fs::write(cache_path, msgpack_data)
        .map_err(|e| format!("Failed to write terrain cache: {}", e))?;

    Ok(())
}

/// Load terrain data from cache file
pub fn load_terrain_cache(track_file: &Path) -> Option<ProceduralWorldData> {
    use std::fs;

    let cache_path = get_terrain_cache_path(track_file);

    if !cache_path.exists() {
        return None;
    }

    let content = fs::read(&cache_path).ok()?;
    let procedural_world: ProceduralWorldData = rmp_serde::from_slice(&content)
        .map_err(|e| {
            eprintln!("Failed to parse terrain cache {}: {}", cache_path.display(), e);
            e
        })
        .ok()?;

    Some(procedural_world)
}
