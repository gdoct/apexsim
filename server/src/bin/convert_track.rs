/// Track Converter Tool
/// 
/// Converts race track data from the racetrack-database CSV format
/// to ApexSim's YAML/JSON track format.
/// 
/// Usage:
///   cargo run --bin convert_track -- --tracks-csv /path/to/tracks/Monaco.csv \
///                                     --raceline-csv /path/to/racelines/Monaco.csv \
///                                     --output /path/to/output/monaco.yaml \
///                                     --name "Monaco Grand Prix" \
///                                     --country Monaco \
///                                     --category F1

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use clap::Parser;
use apexsim_server::track_loader::{TrackFileFormat, TrackNode};
use apexsim_server::data::{RacelinePoint, TrackMetadata};

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Path to the track centerline CSV file (x_m,y_m,w_tr_right_m,w_tr_left_m)
    #[arg(short = 't', long)]
    tracks_csv: PathBuf,

    /// Path to the raceline CSV file (x_m,y_m) - optional
    #[arg(short = 'r', long)]
    raceline_csv: Option<PathBuf>,

    /// Output file path (.yaml or .json)
    #[arg(short = 'o', long)]
    output: PathBuf,

    /// Track name
    #[arg(short = 'n', long)]
    name: String,

    /// Country where the track is located
    #[arg(long)]
    country: Option<String>,

    /// City where the track is located
    #[arg(long)]
    city: Option<String>,

    /// Track category (e.g., F1, DTM, IndyCar)
    #[arg(long)]
    category: Option<String>,

    /// Year the track was built
    #[arg(long)]
    year_built: Option<u32>,

    /// Track description
    #[arg(long)]
    description: Option<String>,

    /// Output format: yaml or json (auto-detected from extension if not specified)
    #[arg(short = 'f', long)]
    format: Option<String>,

    /// Elevation mode: flat (z=0) or auto-compute from data
    #[arg(long, default_value = "flat")]
    elevation: String,

    /// Default friction coefficient for all nodes
    #[arg(long, default_value = "1.0")]
    friction: f32,

    /// Track is a closed loop
    #[arg(long, default_value = "true")]
    closed_loop: bool,
}

#[derive(Debug)]
struct TrackCSVPoint {
    x: f32,
    y: f32,
    width_right: f32,
    width_left: f32,
}

#[derive(Debug)]
struct RacelineCSVPoint {
    x: f32,
    y: f32,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    println!("Converting track: {}", args.name);
    println!("Reading centerline from: {}", args.tracks_csv.display());

    // Read track centerline CSV
    let track_points = read_track_csv(&args.tracks_csv)?;
    println!("  Loaded {} centerline points", track_points.len());

    // Read raceline CSV if provided
    let raceline_points = if let Some(ref raceline_path) = args.raceline_csv {
        println!("Reading raceline from: {}", raceline_path.display());
        let points = read_raceline_csv(raceline_path)?;
        println!("  Loaded {} raceline points", points.len());
        points
    } else {
        Vec::new()
    };

    // Convert to TrackFileFormat
    let track_file = convert_to_track_format(
        track_points,
        raceline_points,
        &args,
    );

    // Calculate track length
    let length_m = calculate_track_length(&track_file.nodes);
    println!("  Track length: {:.2} meters ({:.2} km)", length_m, length_m / 1000.0);

    // Update metadata with calculated length
    let mut track_file = track_file;
    if let Some(ref mut metadata) = track_file.metadata {
        metadata.length_m = Some(length_m);
    }

    // Determine output format
    let format = if let Some(ref fmt) = args.format {
        fmt.to_lowercase()
    } else {
        args.output
            .extension()
            .and_then(|s| s.to_str())
            .unwrap_or("yaml")
            .to_lowercase()
    };

    // Write output file
    println!("Writing to: {}", args.output.display());
    let output_file = File::create(&args.output)?;
    
    match format.as_str() {
        "json" => {
            serde_json::to_writer_pretty(output_file, &track_file)?;
        }
        "yaml" | "yml" => {
            serde_yaml::to_writer(output_file, &track_file)?;
        }
        _ => {
            return Err(format!("Unsupported output format: {}", format).into());
        }
    }

    println!("✓ Conversion completed successfully!");
    println!("  Output: {}", args.output.display());

    Ok(())
}

fn read_track_csv(path: &PathBuf) -> Result<Vec<TrackCSVPoint>, Box<dyn std::error::Error>> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut points = Vec::new();

    for (line_num, line) in reader.lines().enumerate() {
        let line = line?;
        
        // Skip header and comments
        if line.starts_with('#') || line.trim().is_empty() || line_num == 0 {
            continue;
        }

        let parts: Vec<&str> = line.split(',').collect();
        if parts.len() < 4 {
            eprintln!("Warning: Skipping malformed line {}: {}", line_num + 1, line);
            continue;
        }

        let x = parts[0].trim().parse::<f32>()?;
        let y = parts[1].trim().parse::<f32>()?;
        let width_right = parts[2].trim().parse::<f32>()?;
        let width_left = parts[3].trim().parse::<f32>()?;

        points.push(TrackCSVPoint {
            x,
            y,
            width_right,
            width_left,
        });
    }

    Ok(points)
}

fn read_raceline_csv(path: &PathBuf) -> Result<Vec<RacelineCSVPoint>, Box<dyn std::error::Error>> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut points = Vec::new();

    for (line_num, line) in reader.lines().enumerate() {
        let line = line?;
        
        // Skip header and comments
        if line.starts_with('#') || line.trim().is_empty() || line_num == 0 {
            continue;
        }

        let parts: Vec<&str> = line.split(',').collect();
        if parts.len() < 2 {
            eprintln!("Warning: Skipping malformed line {}: {}", line_num + 1, line);
            continue;
        }

        let x = parts[0].trim().parse::<f32>()?;
        let y = parts[1].trim().parse::<f32>()?;

        points.push(RacelineCSVPoint { x, y });
    }

    Ok(points)
}

fn convert_to_track_format(
    track_points: Vec<TrackCSVPoint>,
    raceline_points: Vec<RacelineCSVPoint>,
    args: &Args,
) -> TrackFileFormat {
    let nodes: Vec<TrackNode> = track_points
        .into_iter()
        .map(|p| TrackNode {
            x: p.x,
            y: p.y,
            z: if args.elevation == "flat" { 0.0 } else { 0.0 }, // Could compute from data
            width: None, // Use width_left/width_right instead
            width_left: Some(p.width_left),
            width_right: Some(p.width_right),
            banking: Some(0.0), // Could be computed from track geometry
            friction: Some(args.friction),
            surface_type: Some("Asphalt".to_string()),
        })
        .collect();

    let raceline: Vec<RacelinePoint> = raceline_points
        .into_iter()
        .map(|p| RacelinePoint {
            x: p.x,
            y: p.y,
            z: 0.0,
        })
        .collect();

    let metadata = TrackMetadata {
        country: args.country.clone(),
        city: args.city.clone(),
        length_m: None, // Will be calculated later
        description: args.description.clone(),
        year_built: args.year_built,
        category: args.category.clone(),
        // Procedural generation fields (not used for converted tracks)
        environment_type: None,
        terrain_seed: None,
        terrain_scale: None,
        terrain_detail: None,
        terrain_blend_width: None,
        object_density: None,
        decal_profile: None,
    };

    // Calculate a reasonable default width from the nodes
    let avg_total_width = if !nodes.is_empty() {
        let sum: f32 = nodes.iter()
            .map(|n| n.width_left.unwrap_or(7.5) + n.width_right.unwrap_or(7.5))
            .sum();
        sum / nodes.len() as f32
    } else {
        15.0
    };

    TrackFileFormat {
        name: args.name.clone(),
        track_id: Some(uuid::Uuid::new_v4().to_string()),
        nodes,
        checkpoints: Vec::new(), // Could be auto-generated based on track sectors
        spawn_points: Vec::new(), // Could be auto-generated at start/finish
        default_width: avg_total_width,
        closed_loop: args.closed_loop,
        raceline,
        metadata: Some(metadata),
    }
}

fn calculate_track_length(nodes: &[TrackNode]) -> f32 {
    if nodes.len() < 2 {
        return 0.0;
    }

    let mut total_length = 0.0;
    for i in 0..nodes.len() - 1 {
        let dx = nodes[i + 1].x - nodes[i].x;
        let dy = nodes[i + 1].y - nodes[i].y;
        let dz = nodes[i + 1].z - nodes[i].z;
        total_length += (dx * dx + dy * dy + dz * dz).sqrt();
    }

    // Add closing segment if it's a loop
    if nodes.len() > 2 {
        let dx = nodes[0].x - nodes[nodes.len() - 1].x;
        let dy = nodes[0].y - nodes[nodes.len() - 1].y;
        let dz = nodes[0].z - nodes[nodes.len() - 1].z;
        total_length += (dx * dx + dy * dy + dz * dz).sqrt();
    }

    total_length
}
