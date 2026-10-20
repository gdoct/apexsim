//! `showcase.yml`: which AI races are rendered as showcases, and the render
//! options each one is. `apexsim-replay render --showcase` and the physics
//! library's `apx_showcase_render` both read the list here, so the build's
//! file and the one the game renders from the same entry are the same race,
//! byte for byte.
//!
//! The list (at the code root; its comments say what each key means):
//! `defaults` (cars, laps, seed, seeds, ai_level), `variants` (a sky by name:
//! weather, time, and optionally air, wind and wind_from) and `showcases`
//! (track stem, class, variants, and any default overridden). A showcase is
//! named `<track>.<class in lower case>.<variant>`, its file that name plus
//! `.apxs`.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::data::SessionConditions;
use crate::render::{
    car_of_class, render_best_unless, render_stream_unless, RenderOptions, RenderedStream,
};
use crate::seeded_race::{load_car_folder, SimulateOptions};

/// What every showcase takes unless it says otherwise.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShowcaseDefaults {
    #[serde(default = "default_cars")]
    pub cars: u8,
    #[serde(default = "default_laps")]
    pub laps: u8,
    #[serde(default)]
    pub seed: u64,
    #[serde(default)]
    pub seeds: Option<u32>,
    #[serde(default)]
    pub ai_level: Option<u8>,
}

fn default_cars() -> u8 {
    20
}

fn default_laps() -> u8 {
    2
}

impl Default for ShowcaseDefaults {
    fn default() -> Self {
        Self {
            cars: default_cars(),
            laps: default_laps(),
            seed: 0,
            seeds: None,
            ai_level: None,
        }
    }
}

/// A sky by name. `weather` and `time` as `apexsim-replay render` takes
/// them (`sunny`, `13:00`).
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShowcaseVariant {
    pub weather: String,
    pub time: String,
    #[serde(default)]
    pub air: Option<i8>,
    #[serde(default)]
    pub wind: Option<u8>,
    #[serde(default)]
    pub wind_from: Option<u16>,
}

/// One line of `showcases`.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShowcaseEntry {
    pub track: String,
    pub class: String,
    pub variants: Vec<String>,
    #[serde(default)]
    pub cars: Option<u8>,
    #[serde(default)]
    pub laps: Option<u8>,
    #[serde(default)]
    pub seed: Option<u64>,
    #[serde(default)]
    pub seeds: Option<u32>,
    #[serde(default)]
    pub ai_level: Option<u8>,
}

/// `showcase.yml`, read.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShowcaseList {
    #[serde(default)]
    pub defaults: ShowcaseDefaults,
    pub variants: BTreeMap<String, ShowcaseVariant>,
    pub showcases: Vec<ShowcaseEntry>,
}

/// One showcase to render: its name and the render's options.
#[derive(Debug, Clone)]
pub struct ShowcaseRender {
    /// `<track>.<class>.<variant>`, the file's name without `.apxs`.
    pub name: String,
    pub track_stem: String,
    pub options: RenderOptions,
    /// Seeds rendered from the options' seed up, the best kept (1: just it).
    pub seeds: u32,
}

/// A showcase's name: `<track>.<class in lower case>.<variant>`.
pub fn showcase_name(track: &str, class: &str, variant: &str) -> String {
    format!("{track}.{}.{variant}", class.to_lowercase())
}

/// A track's YAML by stem under a tracks folder: `default/`, then `custom/`,
/// then the folder itself.
pub fn find_track_yaml(tracks_dir: &Path, stem: &str) -> Option<PathBuf> {
    ["default", "custom", ""]
        .iter()
        .map(|sub| tracks_dir.join(sub).join(stem).join(format!("{stem}.yaml")))
        .find(|path| path.is_file())
}

impl ShowcaseList {
    /// The list from its text; every variant a showcase names must be defined.
    pub fn parse(text: &str) -> Result<Self, String> {
        let list: ShowcaseList =
            serde_yaml::from_str(text).map_err(|e| format!("showcase.yml: {e}"))?;
        for entry in &list.showcases {
            for variant in &entry.variants {
                if !list.variants.contains_key(variant) {
                    return Err(format!(
                        "showcase.yml: {} names a variant '{variant}' that 'variants' does not define",
                        entry.track
                    ));
                }
            }
        }
        Ok(list)
    }

    pub fn load(path: &Path) -> Result<Self, String> {
        let text = std::fs::read_to_string(path)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        Self::parse(&text)
    }

    /// Every showcase's name, in the list's order.
    pub fn names(&self) -> Vec<String> {
        self.showcases
            .iter()
            .flat_map(|entry| {
                entry
                    .variants
                    .iter()
                    .map(|v| showcase_name(&entry.track, &entry.class, v))
            })
            .collect()
    }

    /// The render of the showcase called `name` (`Zandervoort.gt3.day`), its
    /// track found under `tracks_dir` and its field of `cars_dir`, with what
    /// `apexsim-replay render` takes when not given.
    pub fn render(
        &self,
        name: &str,
        tracks_dir: &Path,
        cars_dir: &Path,
    ) -> Result<ShowcaseRender, String> {
        let (entry, variant_name) = self
            .showcases
            .iter()
            .find_map(|entry| {
                entry
                    .variants
                    .iter()
                    .find(|v| showcase_name(&entry.track, &entry.class, v) == name)
                    .map(|v| (entry, v))
            })
            .ok_or_else(|| format!("showcase.yml lists no showcase '{name}'"))?;
        let sky = &self.variants[variant_name];
        let track_path = find_track_yaml(tracks_dir, &entry.track)
            .ok_or_else(|| format!("no track '{}' under {}", entry.track, tracks_dir.display()))?;
        let (configs, folders) = load_car_folder(cars_dir)?;
        let host_car = car_of_class(&configs, &folders, &entry.class)
            .map(|id| id.to_string())
            .ok_or_else(|| {
                format!(
                    "no car of class '{}' under {}",
                    entry.class,
                    cars_dir.display()
                )
            })?;
        let defaults = RenderOptions::default();
        let options = RenderOptions {
            race: SimulateOptions {
                track_path,
                cars_dir: cars_dir.to_path_buf(),
                host_car,
                same_car: false,
                ai_count: entry.cars.unwrap_or(self.defaults.cars),
                laps: entry.laps.unwrap_or(self.defaults.laps),
                conditions: SessionConditions {
                    weather: crate::render::parse_weather(&sky.weather)?,
                    time_of_day_minutes: crate::render::parse_time_of_day(&sky.time)?,
                    air_temp_c: sky.air,
                    humidity_pct: None,
                    wind_kph: sky.wind,
                    wind_from_deg: sky.wind_from,
                    ..SessionConditions::DEFAULT
                }
                .clamp(),
                seed: Some(entry.seed.unwrap_or(self.defaults.seed)),
                ai_level: entry.ai_level.or(self.defaults.ai_level),
                ..defaults.race
            },
            ..defaults
        };
        Ok(ShowcaseRender {
            name: name.to_string(),
            track_stem: entry.track.clone(),
            options,
            seeds: entry.seeds.or(self.defaults.seeds).unwrap_or(1).max(1),
        })
    }
}

impl ShowcaseRender {
    /// Render it: the one seed, or the best of `seeds`.
    pub fn run(&self) -> Result<RenderedStream, String> {
        self.run_unless(&|| false)
    }

    /// [`Self::run`], given up as soon as `cancelled` says so
    /// ([`crate::render::render_stream_unless`]).
    pub fn run_unless(&self, cancelled: &dyn Fn() -> bool) -> Result<RenderedStream, String> {
        if self.seeds > 1 {
            render_best_unless(&self.options, self.seeds, cancelled)
        } else {
            render_stream_unless(&self.options, cancelled)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIST: &str = r#"
defaults:
  cars: 20
  laps: 2
  seed: 7
variants:
  day:   { weather: sunny, time: "13:00" }
  dusk:  { weather: cloudy, time: "19:30", air: -3, wind: 12, wind_from: 90 }
showcases:
  - { track: Zandervoort, class: GT3, variants: [day, dusk] }
  - { track: Monzarella, class: F1, variants: [day], cars: 12, seeds: 4 }
"#;

    #[test]
    fn the_list_names_every_variant_of_every_showcase() {
        let list = ShowcaseList::parse(LIST).unwrap();
        assert_eq!(
            list.names(),
            [
                "Zandervoort.gt3.day",
                "Zandervoort.gt3.dusk",
                "Monzarella.f1.day"
            ]
        );
        assert_eq!(list.defaults.seed, 7);
        assert_eq!(list.variants["dusk"].air, Some(-3));
        assert_eq!(list.showcases[1].cars, Some(12));
        assert_eq!(list.showcases[1].seeds, Some(4));
    }

    #[test]
    fn an_undefined_variant_or_an_unknown_key_is_refused() {
        let missing = LIST.replace("variants: [day, dusk]", "variants: [day, rain]");
        assert!(ShowcaseList::parse(&missing)
            .unwrap_err()
            .contains("'rain'"));
        let typo = LIST.replace("cars: 12", "carz: 12");
        assert!(ShowcaseList::parse(&typo).is_err());
    }

    /// The code root's own list reads, and every showcase in it is unique.
    #[test]
    fn the_shipped_list_reads() {
        let list = ShowcaseList::load(Path::new("../showcase.yml")).unwrap();
        let names = list.names();
        assert!(!names.is_empty());
        let unique: std::collections::BTreeSet<_> = names.iter().collect();
        assert_eq!(unique.len(), names.len());
    }
}
