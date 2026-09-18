//! The authored prop kit as the editor knows it (`content/props`, catalogued
//! in `docs/PROPS.md`): which asset keys exist per kind, how big each is,
//! and which one a kind falls back to. Data only — the GLBs themselves are
//! never read here; the sizes are the authored footprints, so the editor
//! can preview a placement at the size it imports and the groomer can push
//! a prop clear of the road by its real extent.
//!
//! Variants the Unreal importer picks on its own are not listed: the
//! `_crowd` stands and `_autumn` trees are chosen from the scene's
//! [`crate::ats::Dressing`], and the wedge bays (`bay_10m_curve6` …) from
//! the bend radius at the stand's station. An `.ats` names the base asset.

use crate::ats::PropKind;

/// One authored asset: its kind, key and footprint at scale 1.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct KitAsset {
    pub kind: PropKind,
    pub asset: &'static str,
    /// Extent along the prop's local X (along the road), metres.
    pub length_m: f32,
    /// Extent across the prop (local Y), metres. For everything but a
    /// bridge or a sky prop the footprint starts at the pivot and reaches
    /// away from the road.
    pub depth_m: f32,
    pub height_m: f32,
}

impl KitAsset {
    /// Radius of the circle round the footprint, for a push-off.
    pub fn footprint_radius_m(&self) -> f32 {
        (self.length_m / 2.0).hypot(self.depth_m / 2.0)
    }
}

macro_rules! kit {
    ($($kind:ident $asset:literal $l:literal x $d:literal x $h:literal),* $(,)?) => {
        &[$(KitAsset {
            kind: PropKind::$kind,
            asset: $asset,
            length_m: $l,
            depth_m: $d,
            height_m: $h,
        }),*]
    };
}

/// Every authored asset, in `docs/PROPS.md` order. The first entry of a
/// kind is its default.
pub const KIT: &[KitAsset] = kit![
    // Track edge
    Barrier "armco_4m" 4.0 x 0.2 x 1.0,
    Barrier "armco_4m_fence" 4.0 x 0.4 x 3.5,
    Barrier "armco_end" 2.0 x 0.2 x 1.1,
    Barrier "concrete_4m" 4.0 x 0.6 x 1.0,
    Barrier "concrete_4m_rail" 4.0 x 0.6 x 1.55,
    Barrier "tecpro_2m" 2.0 x 1.0 x 1.15,
    TireWall "tires_4m" 4.0 x 1.3 x 0.8,
    TireWall "tires_corner" 1.7 x 1.7 x 0.8,
    Board "hoarding_3m" 3.0 x 0.3 x 2.0,
    Board "hoarding_6m" 6.0 x 0.3 x 2.0,
    Board "braking_marker" 0.75 x 0.2 x 1.9,
    Board "light_panel" 1.0 x 0.3 x 2.1,
    Sign "marshal_post" 3.2 x 2.4 x 4.8,
    Sign "pit_speed_limit" 0.9 x 0.3 x 2.8,
    Sign "pit_exit_light" 0.6 x 0.4 x 4.2,
    Sign "flag_pole" 1.8 x 0.5 x 8.1,
    Fence "mesh_4m" 4.0 x 0.1 x 2.5,
    Fence "mesh_4m_hoarding" 4.0 x 0.1 x 2.5,
    Fence "wood_4m" 4.0 x 0.1 x 1.2,
    Fence "hedge_4m" 4.0 x 1.1 x 1.6,
    // Overhead
    Bridge "start_gantry" 2.0 x 20.0 x 7.2,
    Bridge "truss_bridge" 2.7 x 20.4 x 8.7,
    Bridge "tyre_bridge" 4.0 x 23.7 x 13.6,
    Bridge "timing_gantry" 2.0 x 20.0 x 10.5,
    Bridge "span_building" 8.2 x 20.7 x 22.0,
    Light "floodlight_tower" 4.9 x 3.0 x 30.3,
    Light "lamp_post" 0.6 x 2.5 x 8.1,
    // Pit complex
    Pit "garage_6m" 6.0 x 14.2 x 9.9,
    Pit "garage_6m_closed" 6.0 x 14.2 x 9.9,
    Pit "garage_end" 6.1 x 14.2 x 15.9,
    Pit "pit_wall_6m" 6.0 x 2.3 x 3.8,
    Pit "pit_wall_plain_6m" 6.0 x 0.4 x 2.9,
    Pit "box_kit" 5.5 x 2.6 x 2.4,
    Building "clubhouse" 25.6 x 18.9 x 11.5,
    Building "hospitality_3f" 30.6 x 12.6 x 12.4,
    Building "media_centre" 40.6 x 15.6 x 21.0,
    Building "control_tower" 13.2 x 13.2 x 29.0,
    Building "observation_tower" 8.3 x 8.3 x 60.5,
    // Skyline: city backdrop for street circuits, set well back.
    Building "skyline_lowrise" 26.2 x 18.2 x 40.8,
    Building "skyline_crane" 34.5 x 16.1 x 76.3,
    Building "skyline_slab_a" 24.1 x 16.1 x 96.2,
    Building "skyline_pyramid" 20.1 x 20.1 x 112.0,
    Building "skyline_podium" 40.2 x 30.2 x 121.2,
    Building "skyline_slab_b" 18.1 x 18.1 x 135.0,
    Building "skyline_cylinder" 20.3 x 20.3 x 145.0,
    Building "skyline_step" 30.1 x 22.1 x 150.0,
    Building "skyline_twin" 32.1 x 12.1 x 161.2,
    Building "skyline_needle" 14.1 x 14.1 x 190.0,
    // Spectators: a stand family is one bay; the stand's length lays more.
    Grandstand "bay_10m" 10.0 x 9.1 x 5.6,
    Grandstand "bay_10m_roof" 10.0 x 10.4 x 10.6,
    Grandstand "bay_10m_large" 10.0 x 14.4 x 8.3,
    Grandstand "bay_10m_large_roof" 10.0 x 15.8 x 13.3,
    Grandstand "bay_10m_stadium_roof" 10.6 x 35.1 x 28.0,
    Grandstand "scaffold_10m" 10.1 x 5.1 x 4.1,
    Grandstand "banking_seats" 10.0 x 6.0 x 2.6,
    Attraction "tent_6m" 6.5 x 6.5 x 5.2,
    Attraction "video_screen" 12.1 x 2.0 x 11.1,
    Attraction "camera_tower" 4.4 x 4.4 x 13.2,
    Attraction "ferris_wheel" 62.8 x 16.0 x 69.5,
    Attraction "portaloo_row" 5.4 x 1.2 x 2.4,
    Attraction "fanzone_stage" 12.7 x 8.0 x 8.5,
    // Landscape
    Tree "broadleaf_m" 8.0 x 7.2 x 10.0,
    Tree "broadleaf_s" 5.0 x 4.5 x 6.0,
    Tree "broadleaf_l" 13.0 x 11.7 x 16.0,
    Tree "conifer_m" 5.0 x 4.3 x 12.0,
    Tree "conifer_l" 8.0 x 6.9 x 20.0,
    Tree "poplar" 4.0 x 3.5 x 18.0,
    Tree "bush_cluster" 4.0 x 3.6 x 3.0,
    Tree "palm_oil" 4.4 x 4.4 x 8.4,
    Tree "palm_ornamental" 3.8 x 4.8 x 13.2,
    Vehicle "car_a" 3.9 x 1.7 x 1.4,
    Vehicle "car_b" 4.7 x 1.7 x 1.4,
    Vehicle "car_c" 4.5 x 1.7 x 1.8,
    Vehicle "fire_truck" 5.5 x 2.5 x 3.3,
    Vehicle "ambulance" 6.0 x 2.2 x 2.9,
    Vehicle "tractor" 4.6 x 2.3 x 2.9,
    Vehicle "race_truck" 8.6 x 2.6 x 4.0,
    Vehicle "motorhome" 10.0 x 4.6 x 3.5,
    Misc "bollard" 0.2 x 0.2 x 0.9,
    Misc "kerb_marker" 0.1 x 0.1 x 0.7,
    Misc "generator" 2.2 x 1.2 x 1.9,
    Misc "photographer_stand" 2.0 x 2.0 x 2.5,
    Misc "rock_cluster" 0.9 x 0.7 x 0.5,
    Misc "scrub_clump" 0.7 x 0.6 x 0.3,
    // The Red Bull Ring's trackside bull statue -- centred on its own
    // footprint like the tower/ferris_wheel landmarks, not road-facing.
    Misc "bull_statue" 3.2 x 1.8 x 2.5,
    // Sky: origin at the hull centre, sized round it.
    Sky "blimp" 60.0 x 19.5 x 19.7,
    Sky "balloon" 16.0 x 16.0 x 22.1,
    Sky "helicopter" 14.6 x 11.0 x 4.2,
];

/// The authored assets of a kind, default first. Empty for `cone`, which
/// has no kit asset.
pub fn assets_for(kind: PropKind) -> impl Iterator<Item = &'static KitAsset> {
    KIT.iter().filter(move |a| a.kind == kind)
}

/// The asset a new prop of the kind starts as: the kit's default where
/// there is one, else the pre-kit `<kind>_generic` key the importer maps
/// to a stand-in.
pub fn default_asset(kind: PropKind) -> String {
    assets_for(kind)
        .next()
        .map(|a| a.asset.to_string())
        .unwrap_or_else(|| format!("{}_generic", kind.label()))
}

/// The catalogue entry for a prop's key, if the kit has it.
pub fn find(kind: PropKind, asset: &str) -> Option<&'static KitAsset> {
    assets_for(kind).find(|a| a.asset == asset)
}

/// The pre-kit keys the groomer and old scenes carry, mapped onto the
/// kit entry the Unreal importer resolves them to, so their previews and
/// footprints match what is built.
pub fn resolve(kind: PropKind, asset: &str) -> Option<&'static KitAsset> {
    let (kind, asset) = match (kind, asset) {
        (PropKind::Tree, "tree_generic") => (PropKind::Tree, "broadleaf_m"),
        (PropKind::Barrier, "armco_generic") => (PropKind::Barrier, "armco_4m"),
        (PropKind::TireWall, "tire_wall_generic") => (PropKind::TireWall, "tires_4m"),
        (PropKind::Sign, "board_200m" | "board_100m" | "board_50m") => {
            (PropKind::Board, "braking_marker")
        }
        (PropKind::Grandstand, "grandstand_main") => (PropKind::Grandstand, "bay_10m_large_roof"),
        (PropKind::Grandstand, "grandstand_corner") => (PropKind::Grandstand, "bay_10m_roof"),
        (PropKind::Building, "pit_garage") => (PropKind::Pit, "garage_6m"),
        other => other,
    };
    find(kind, asset)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_kind_but_cone_has_a_default_in_the_kit() {
        for kind in PropKind::ALL {
            let default = default_asset(kind);
            if kind == PropKind::Cone {
                assert_eq!(default, "cone_generic");
            } else {
                assert!(
                    find(kind, &default).is_some(),
                    "{} defaults to {default}, which is not in the kit",
                    kind.label()
                );
            }
        }
    }

    #[test]
    fn defaults_match_the_importer() {
        // The Unreal side's per-kind defaults (ApexPropLibrary.cpp); a
        // prop the editor adds previews as what the importer will build.
        for (kind, asset) in [
            (PropKind::Barrier, "armco_4m"),
            (PropKind::TireWall, "tires_4m"),
            (PropKind::Board, "hoarding_3m"),
            (PropKind::Fence, "mesh_4m"),
            (PropKind::Grandstand, "bay_10m"),
            (PropKind::Pit, "garage_6m"),
            (PropKind::Bridge, "start_gantry"),
            (PropKind::Light, "floodlight_tower"),
            (PropKind::Tree, "broadleaf_m"),
            (PropKind::Sky, "blimp"),
            (PropKind::Sign, "marshal_post"),
            (PropKind::Vehicle, "car_a"),
            (PropKind::Attraction, "tent_6m"),
            (PropKind::Building, "clubhouse"),
            (PropKind::Misc, "bollard"),
        ] {
            assert_eq!(default_asset(kind), asset, "{}", kit_label(kind));
        }
    }

    fn kit_label(kind: PropKind) -> &'static str {
        kind.label()
    }

    #[test]
    fn keys_are_unique_and_kit_files_exist() {
        let mut seen = std::collections::HashSet::new();
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/props");
        for entry in KIT {
            assert!(
                seen.insert((entry.kind.label(), entry.asset)),
                "duplicate {}/{}",
                entry.kind.label(),
                entry.asset
            );
            assert!(entry.length_m > 0.0 && entry.depth_m > 0.0 && entry.height_m > 0.0);
            let glb = root
                .join(entry.kind.label())
                .join(format!("{}.glb", entry.asset));
            if root.exists() {
                assert!(glb.exists(), "no {}", glb.display());
            }
        }
    }

    #[test]
    fn every_kit_file_is_catalogued() {
        // A GLB added to content/props without a KIT row is invisible to the
        // inspector's dropdown and gets no footprint. Variants the importer
        // picks by itself (dressing, bend radius, stand ends) are exempt.
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/props");
        let Ok(kinds) = std::fs::read_dir(&root) else {
            return;
        };
        let mut missing = Vec::new();
        for kind_dir in kinds.flatten() {
            let Some(kind) = PropKind::ALL
                .into_iter()
                .find(|k| k.label() == kind_dir.file_name().to_string_lossy())
            else {
                continue;
            };
            for file in std::fs::read_dir(kind_dir.path()).unwrap().flatten() {
                let name = file.file_name().to_string_lossy().into_owned();
                let Some(asset) = name.strip_suffix(".glb") else {
                    continue;
                };
                let variant = asset.ends_with("_crowd")
                    || asset.ends_with("_autumn")
                    || asset.contains("_curve")
                    || asset.starts_with("end_cap");
                if !variant && find(kind, asset).is_none() {
                    missing.push(format!("{}/{asset}", kind.label()));
                }
            }
        }
        assert!(missing.is_empty(), "not in props::KIT: {missing:?}");
    }

    #[test]
    fn legacy_keys_resolve_to_kit_entries() {
        assert_eq!(
            resolve(PropKind::Tree, "tree_generic").map(|a| a.asset),
            Some("broadleaf_m")
        );
        assert_eq!(
            resolve(PropKind::Sign, "board_100m").map(|a| (a.kind, a.asset)),
            Some((PropKind::Board, "braking_marker"))
        );
        assert_eq!(
            resolve(PropKind::Building, "pit_garage").map(|a| a.kind),
            Some(PropKind::Pit)
        );
        assert!(resolve(PropKind::Misc, "spaceship").is_none());
        assert!(
            (find(PropKind::Attraction, "tent_6m")
                .unwrap()
                .footprint_radius_m()
                - 4.6)
                .abs()
                < 0.01
        );
    }
}
