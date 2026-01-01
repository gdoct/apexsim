use apexsim_server::track_loader::TrackLoader;
use apexsim_server::track_mesh::TrackMeshGenerator;
use std::env;
use std::fs;

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        eprintln!("Usage: cargo run --example track_export <track_file.json|yaml> [output.obj]");
        eprintln!("\nExamples:");
        eprintln!("  cargo run --example track_export content/tracks/simple_oval.yaml");
        eprintln!("  cargo run --example track_export content/tracks/simple_oval.yaml track.obj");
        std::process::exit(1);
    }

    let track_path = &args[1];

    println!("Loading track from: {}", track_path);

    match TrackLoader::load_from_file(track_path) {
        Ok(track_config) => {
            println!("\n✓ Track loaded successfully!");
            println!("  Name: {}", track_config.name);
            println!("  ID: {}", track_config.id);
            println!("  Width: {}m", track_config.width_m);
            println!("  Centerline points: {}", track_config.centerline.len());
            println!("  Start positions: {}", track_config.start_positions.len());

            if let Some(first) = track_config.centerline.first() {
                if let Some(last) = track_config.centerline.last() {
                    println!(
                        "  Total length: {:.2}m",
                        last.distance_from_start_m
                    );

                    let dx = last.x - first.x;
                    let dy = last.y - first.y;
                    let dz = last.z - first.z;
                    let gap = (dx * dx + dy * dy + dz * dz).sqrt();

                    if gap < 10.0 {
                        println!("  Track forms a closed loop (gap: {:.2}m)", gap);
                    } else {
                        println!("  Track is open (gap: {:.2}m)", gap);
                    }
                }
            }

            println!("\nGenerating track mesh...");
            let mesh = TrackMeshGenerator::generate_mesh(&track_config.centerline, true);

            println!("  Vertices: {}", mesh.vertices.len());
            println!("  Triangles: {}", mesh.indices.len() / 3);
            println!("  Normals: {}", mesh.normals.len());
            println!("  UVs: {}", mesh.uvs.len());

            if args.len() >= 3 {
                let output_path = &args[2];
                println!("\nExporting to OBJ: {}", output_path);

                let obj_content = TrackMeshGenerator::export_obj(&mesh);

                match fs::write(output_path, obj_content) {
                    Ok(_) => {
                        println!("✓ Successfully exported to {}", output_path);
                        println!("\nYou can now import this OBJ file into:");
                        println!("  - Blender");
                        println!("  - Maya");
                        println!("  - Unreal Engine");
                        println!("  - Unity");
                        println!("  - Most 3D modeling tools");
                    }
                    Err(e) => {
                        eprintln!("✗ Failed to write OBJ file: {}", e);
                        std::process::exit(1);
                    }
                }
            } else {
                println!("\nTo export the mesh, provide an output filename:");
                println!("  cargo run --example track_export {} track.obj", track_path);
            }

            println!("\n=== Track Statistics ===");
            analyze_track(&track_config.centerline);
        }
        Err(e) => {
            eprintln!("✗ Failed to load track: {}", e);
            std::process::exit(1);
        }
    }
}

fn analyze_track(centerline: &[apexsim_server::data::TrackPoint]) {
    if centerline.is_empty() {
        return;
    }

    let mut max_elevation = f32::MIN;
    let mut min_elevation = f32::MAX;
    let mut max_banking = 0.0f32;
    let mut total_curvature = 0.0f32;

    for point in centerline {
        max_elevation = max_elevation.max(point.z);
        min_elevation = min_elevation.min(point.z);
        max_banking = max_banking.max(point.banking_rad.abs());
    }

    for i in 1..centerline.len() {
        let heading_change = (centerline[i].heading_rad - centerline[i - 1].heading_rad).abs();
        total_curvature += heading_change;
    }

    println!("Elevation range: {:.2}m to {:.2}m (Δ{:.2}m)", min_elevation, max_elevation, max_elevation - min_elevation);
    println!("Max banking: {:.3} rad ({:.1}°)", max_banking, max_banking.to_degrees());
    println!("Total curvature: {:.2} rad ({:.1}°)", total_curvature, total_curvature.to_degrees());

    let mut surface_counts = std::collections::HashMap::new();
    for point in centerline {
        *surface_counts.entry(format!("{:?}", point.surface_type)).or_insert(0) += 1;
    }

    println!("\nSurface type distribution:");
    for (surface, count) in surface_counts {
        let pct = (count as f32 / centerline.len() as f32) * 100.0;
        println!("  {}: {} points ({:.1}%)", surface, count, pct);
    }
}
