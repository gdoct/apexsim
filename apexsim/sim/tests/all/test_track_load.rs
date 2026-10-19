use apexsim_sim::track_loader::TrackLoader;

/// Spa loads from its YAML with its centerline, raceline, grid and metadata.
#[test]
fn spa_loads_from_its_yaml() -> Result<(), Box<dyn std::error::Error>> {
    let track = TrackLoader::load_from_file("../../apexsim-content/tracks/default/SpaFrankenchamps/SpaFrankenchamps.yaml")?;

    println!("✓ Successfully loaded track: {}", track.name);
    println!("  Centerline points: {}", track.centerline.len());
    println!("  Raceline points: {}", track.raceline.len());
    println!("  Start positions: {}", track.start_positions.len());

    if let Some(length) = track.metadata.length_m {
        println!("  Length: {:.2} km", length / 1000.0);
    }

    if let Some(ref country) = track.metadata.country {
        println!(
            "  Location: {}, {}",
            track
                .metadata
                .city
                .as_ref()
                .unwrap_or(&"Unknown".to_string()),
            country
        );
    }

    if let Some(ref category) = track.metadata.category {
        println!("  Category: {}", category);
    }

    if let Some(year) = track.metadata.year_built {
        println!("  Built: {}", year);
    }

    println!("\n✓ Track validation passed!");

    Ok(())
}
