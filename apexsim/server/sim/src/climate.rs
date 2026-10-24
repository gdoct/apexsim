//! Where a circuit is, as its weather sees it: the climate a dated session's
//! air and humidity are worked out in (`SessionConditions::climate_offset_c`,
//! `SessionConditions::auto_humidity_pct_at`), and the dew and fog an
//! overnight cooling leaves (`dew_point_c`, `dew_strength`, `visibility_m`).
//!
//! The weather's own air (`SessionConditions::auto_air_temperature_c`) is a
//! sunny late-May day at 50° N on the sea. A session that names a date is
//! moved from there to the circuit: the sea-level mean of its latitude, its
//! climate's annual swing (a maritime coast swings less than a continent's
//! interior), the surface lapse with its height, and in the tropics a wet and
//! a dry season in the humidity. A session without a date is the reference
//! day to the bit, wherever it is.

use crate::data::{DAYS_PER_YEAR, DEFAULT_LATITUDE_DEG, MODEL_DAY_OF_YEAR, WARMEST_DAY_OF_YEAR};

/// Where a circuit is: its latitude, its longitude when the track says, and
/// its height. A bare latitude is a site at sea level whose climate is the
/// reference's (no longitude, no region).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClimateSite {
    pub latitude_deg: f32,
    pub longitude_deg: Option<f32>,
    pub altitude_m: f32,
}

impl ClimateSite {
    /// The reference the weather's own air is filed at: 50° N on the sea.
    pub const REFERENCE: ClimateSite = ClimateSite {
        latitude_deg: DEFAULT_LATITUDE_DEG,
        longitude_deg: None,
        altitude_m: 0.0,
    };

    /// The climate of the region the site lies in ([`climate_of`]).
    pub fn climate(&self) -> Climate {
        climate_of(self.latitude_deg, self.longitude_deg)
    }
}

impl From<f32> for ClimateSite {
    fn from(latitude_deg: f32) -> Self {
        ClimateSite {
            latitude_deg,
            ..ClimateSite::REFERENCE
        }
    }
}

/// A region's climate against the latitude's own.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Climate {
    /// Its annual range over the latitude's ([`crate::data::SEASON_RANGE_PER_DEG_C`]):
    /// under 1 on a maritime coast, over 1 in a continent's interior and on
    /// an east coast under a winter monsoon.
    pub range_factor: f32,
    /// Its annual mean over the latitude's sea-level mean, °C: a desert
    /// warmer, a winter-cold continent colder.
    pub mean_offset_c: f32,
    /// Its share of the tropical wet season's humidity swing
    /// ([`WET_SEASON_HUMIDITY_PCT`]): none in a desert.
    pub wet_season: f32,
}

impl Climate {
    pub const REFERENCE: Climate = Climate {
        range_factor: 1.0,
        mean_offset_c: 0.0,
        wet_season: 1.0,
    };
}

/// The sea-level annual mean at the equator, °C.
pub const EQUATOR_MEAN_C: f32 = 27.0;
/// How much colder the sea-level mean is at 50° than at the equator, °C; it
/// falls with the square of the latitude (17 °C at 50°, 6 °C at 30°).
pub const MEAN_DROP_AT_50_C: f32 = 17.0;
/// The air's fall with height at the surface, °C a kilometre: under the free
/// atmosphere's 6.5, as a plateau's ground warms its air.
pub const LAPSE_C_PER_KM: f32 = 5.0;
/// The tropical wet season's humidity over the dry's, half of it either way,
/// percent of relative humidity.
pub const WET_SEASON_HUMIDITY_PCT: f32 = 12.0;

/// The sea-level annual mean air over a latitude, °C.
pub fn sea_level_mean_c(latitude_deg: f32) -> f32 {
    let x = latitude_deg.abs().min(90.0) / 50.0;
    EQUATOR_MEAN_C - MEAN_DROP_AT_50_C * x * x
}

/// The climate a circuit `latitude_deg` north and `longitude_deg` east lies
/// in: a coarse map of where racing happens, the reference's (1, 0, 1)
/// elsewhere and for a site without a longitude. Checked against the
/// circuits' climate normals (`the_climate_follows_the_circuit`).
pub fn climate_of(latitude_deg: f32, longitude_deg: Option<f32>) -> Climate {
    let Some(lon) = longitude_deg.filter(|l| l.is_finite()) else {
        return Climate::REFERENCE;
    };
    let lat = latitude_deg;
    let region = |range_factor: f32, mean_offset_c: f32, wet_season: f32| Climate {
        range_factor,
        mean_offset_c,
        wet_season,
    };
    let within = |lo: f32, hi: f32, v: f32| (lo..hi).contains(&v);
    // Europe's Atlantic edge, warmed by the Gulf Stream: Iberia's west
    // coast (Portimão, Estoril), and north of the Mediterranean everything
    // west of the Rhine (Silverstone, Spa, Zandvoort, the Nürburgring).
    if (within(35.0, 44.5, lat) && within(-11.0, -6.5, lon))
        || (within(44.5, 62.0, lat) && within(-11.0, 7.0, lon))
    {
        return region(0.7, 1.5, 1.0);
    }
    // The Mediterranean (Barcelona, Jerez, Paul Ricard, Imola, Monaco).
    if within(35.0, 44.5, lat) && within(-6.5, 26.0, lon) {
        return region(1.15, 0.0, 1.0);
    }
    // Central and eastern Europe (Monza, the Red Bull Ring, the Hungaroring).
    if within(44.5, 62.0, lat) && within(7.0, 40.0, lon) {
        return region(1.2, 0.0, 1.0);
    }
    // The Gulf and Arabia: desert, hot and dry (Bahrain, Yas, Losail,
    // Jeddah).
    if within(12.0, 32.0, lat) && within(34.0, 60.0, lon) {
        return region(1.8, 4.0, 0.0);
    }
    // Anatolia and the Caspian (Istanbul, Baku).
    if within(32.0, 45.0, lat) && within(26.0, 60.0, lon) {
        return region(1.4, -2.0, 1.0);
    }
    // East Asia under the winter monsoon (Suzuka, Shanghai, Fuji).
    if within(20.0, 55.0, lat) && within(100.0, 146.0, lon) {
        return region(1.8, -3.0, 1.0);
    }
    // Florida's peninsula (Miami, Sebring, Daytona).
    if within(24.0, 31.0, lat) && within(-88.0, -79.0, lon) {
        return region(1.15, 0.0, 1.0);
    }
    // North America east of the Rockies: a continent's swing, its winters
    // colder the further north (Austin, Indianapolis, Montréal).
    if within(25.0, 60.0, lat) && within(-105.0, -52.0, lon) {
        return region(1.7, -0.4 * (lat - 30.0).max(0.0), 1.0);
    }
    // The Pacific coast under its cold current (Laguna Seca, Long Beach).
    if within(30.0, 50.0, lat) && within(-125.0, -117.0, lon) {
        return region(0.7, -2.0, 1.0);
    }
    // The interior west (Las Vegas).
    if within(30.0, 50.0, lat) && within(-117.0, -105.0, lon) {
        return region(1.8, 0.0, 1.0);
    }
    // The rest of the tropics swing little over the year (Mexico City,
    // Interlagos, Sepang, Singapore).
    if latitude_deg.abs() < 25.0 {
        return region(0.6, 0.0, 1.0);
    }
    // The southern mid-latitudes, under the ocean (Melbourne, Phillip
    // Island, Buenos Aires, Kyalami).
    if lat < -25.0 {
        return region(0.8, 0.0, 1.0);
    }
    Climate::REFERENCE
}

/// The season's air on `day` over a site against its year's mean, °C:
/// [`crate::data::season_air_c`]'s cosine, its range scaled by the climate.
pub fn season_c(day: u16, site: &ClimateSite) -> f32 {
    site.climate().range_factor * crate::data::season_air_c(day, site.latitude_deg)
}

/// The mean air on `day` at a site, °C: the latitude's sea-level mean, the
/// region's offset, the season, and the lapse with height.
pub fn mean_air_c(day: u16, site: &ClimateSite) -> f32 {
    sea_level_mean_c(site.latitude_deg) + site.climate().mean_offset_c + season_c(day, site)
        - LAPSE_C_PER_KM * site.altitude_m.max(0.0) / 1000.0
}

/// How much warmer `day` at a site is than the reference day the weather's
/// own air is filed at (the model day at [`ClimateSite::REFERENCE`]), °C:
/// exactly 0 there.
pub fn climate_offset_c(day: u16, site: &ClimateSite) -> f32 {
    mean_air_c(day, site) - mean_air_c(MODEL_DAY_OF_YEAR, &ClimateSite::REFERENCE)
}

/// How much the season of `day` at a site has warmed it over the model day
/// there, °C: what the humidity reads (a cold month's air stands nearer
/// saturation). The latitude's mean and the height are not seasons.
pub fn season_offset_c(day: u16, site: &ClimateSite) -> f32 {
    season_c(day, site) - season_c(MODEL_DAY_OF_YEAR, site)
}

/// The tropics' share of the wet season's swing: none within 3° of the
/// equator (wet all year), full from 10° to 22°, none again past 30°.
fn tropical_share(latitude_deg: f32) -> f32 {
    let lat = latitude_deg.abs();
    ((lat - 3.0) / 7.0).clamp(0.0, 1.0) * ((30.0 - lat) / 8.0).clamp(0.0, 1.0)
}

/// The tropical wet season's humidity on `day` at a site against the model
/// day's there, percent: wettest a month after midsummer (the monsoon in
/// July north of the equator, January south of it), driest half a year on.
pub fn wet_season_pct(day: u16, site: &ClimateSite) -> f32 {
    let amplitude = 0.5
        * WET_SEASON_HUMIDITY_PCT
        * site.climate().wet_season
        * tropical_share(site.latitude_deg);
    if amplitude == 0.0 {
        return 0.0;
    }
    let wettest = if site.latitude_deg < 0.0 {
        WARMEST_DAY_OF_YEAR + 0.5 * DAYS_PER_YEAR as f32
    } else {
        WARMEST_DAY_OF_YEAR
    };
    let swing = |d: u16| {
        amplitude * ((d as f32 - wettest) / DAYS_PER_YEAR as f32 * std::f32::consts::TAU).cos()
    };
    swing(day) - swing(MODEL_DAY_OF_YEAR)
}

/// The dew point of air at `air_c` holding `humidity` (0..1) of what it
/// can, °C (Magnus, over water).
pub fn dew_point_c(air_c: f32, humidity: f32) -> f32 {
    const A: f32 = 17.62;
    const B: f32 = 243.12;
    let gamma = humidity.clamp(0.01, 1.0).ln() + A * air_c / (B + air_c);
    B * gamma / (A - gamma)
}

/// How far a clear, calm night's asphalt radiates under the air, °C.
pub const SKY_COOLING_C: f32 = 4.0;
/// How far the asphalt lies under the dew point at full dew, °C.
pub const DEW_SPREAD_C: f32 = 3.0;
/// How near the air comes to its dew point before a fog stands, °C: none
/// from this spread, thickest at none.
pub const FOG_SPREAD_C: f32 = 2.0;
/// The sight through clear air, m: what [`visibility_m`] reads without fog.
pub const CLEAR_VISIBILITY_M: f32 = 10_000.0;
/// The sight through the thickest radiation fog, m.
pub const FOG_VISIBILITY_M: f32 = 150.0;

/// The night's share of the sky's cooling: all of it with the sun under
/// the horizon, none from 8° up.
pub fn night_share(sun_elevation_deg: f32) -> f32 {
    ((8.0 - sun_elevation_deg) / 8.0).clamp(0.0, 1.0)
}

/// The calm's share: a breeze stirs the cold air off the ground, so dew and
/// fog want still air. All of it under 6 km/h, none from 20.
pub fn calm_share(wind_kph: f32) -> f32 {
    (1.0 - (wind_kph - 6.0) / 14.0).clamp(0.0, 1.0)
}

/// What the night sky takes off the asphalt under the air, °C: a clear,
/// still night's [`SKY_COOLING_C`], less under cloud and in a breeze, none
/// by day.
pub fn sky_cooling_c(sun_elevation_deg: f32, cloud: f32, wind_kph: f32) -> f32 {
    SKY_COOLING_C
        * (1.0 - cloud).clamp(0.0, 1.0)
        * night_share(sun_elevation_deg)
        * calm_share(wind_kph)
}

/// How hard dew is forming, 0..1: the asphalt the night has cooled
/// (`track_c` less [`sky_cooling_c`]) under the dew point, full
/// [`DEW_SPREAD_C`] under it. None while rain or snow falls (the road is
/// wet or white already). Exactly 0 on a dry day.
pub fn dew_strength(
    track_c: f32,
    dew_point_c: f32,
    sun_elevation_deg: f32,
    cloud: f32,
    wind_kph: f32,
    falling: f32,
) -> f32 {
    if falling > 0.0 {
        return 0.0;
    }
    let surface = track_c - sky_cooling_c(sun_elevation_deg, cloud, wind_kph);
    ((dew_point_c - surface) / DEW_SPREAD_C).clamp(0.0, 1.0)
}

/// How thick a radiation fog stands, 0..1: the air cooled to within
/// [`FOG_SPREAD_C`] of its dew point, in still air, burnt off as the sun
/// climbs past 15°.
pub fn fog_share(air_c: f32, dew_point_c: f32, sun_elevation_deg: f32, wind_kph: f32) -> f32 {
    let near = ((FOG_SPREAD_C - (air_c - dew_point_c)) / FOG_SPREAD_C).clamp(0.0, 1.0);
    let low_sun = ((15.0 - sun_elevation_deg) / 15.0).clamp(0.0, 1.0);
    near * low_sun * calm_share(wind_kph)
}

/// The sight through a fog of `fog` (0..1), m: [`CLEAR_VISIBILITY_M`] in
/// clear air down to [`FOG_VISIBILITY_M`], evenly on a log scale.
pub fn visibility_m(fog: f32) -> f32 {
    CLEAR_VISIBILITY_M * (FOG_VISIBILITY_M / CLEAR_VISIBILITY_M).powf(fog.clamp(0.0, 1.0))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The client's `ApexSim.UI.CreateSession.SeasonAir` figures, printed
    /// from the server's own functions: weather, minutes, latitude,
    /// longitude (NaN none), height, day (-1 none), solar noon (-1 none),
    /// then `auto_air_temperature_c_at`, `resolve_at`'s air,
    /// `snow_air_drop_c` and `is_winter` of the resolved conditions.
    /// `cargo test --lib client_season_air_figures -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn client_season_air_figures() {
        use crate::data::{SessionConditions, Weather};
        let sites = [
            ClimateSite::from(52.07),
            ClimateSite::from(-37.85),
            ClimateSite::from(50.0),
            // Silverstone, Albert Park, Mexico City up its plateau, Bahrain.
            ClimateSite {
                latitude_deg: 52.07,
                longitude_deg: Some(-1.02),
                altitude_m: 150.0,
            },
            ClimateSite {
                latitude_deg: -37.85,
                longitude_deg: Some(144.97),
                altitude_m: 0.0,
            },
            ClimateSite {
                latitude_deg: 19.40,
                longitude_deg: Some(-99.09),
                altitude_m: 2240.0,
            },
            ClimateSite {
                latitude_deg: 26.03,
                longitude_deg: Some(50.51),
                altitude_m: 0.0,
            },
        ];
        for weather in [Weather::Sunny, Weather::HeavyRain, Weather::Snow] {
            for minutes in [240u16, 900] {
                for site in sites {
                    for day in [None, Some(15u16), Some(196)] {
                        for noon in [None, Some(726u16)] {
                            let c = SessionConditions {
                                weather,
                                time_of_day_minutes: minutes,
                                day_of_year: day,
                                solar_noon_min: noon,
                                ..Default::default()
                            };
                            let resolved = c.resolve_at(0, site);
                            println!(
                                "\t\t{{{}, {}, {:.2}f, {}f, {:.1}f, {}, {}, {:.4}f, {}, {:.4}f, {}}},",
                                weather as u8,
                                minutes,
                                site.latitude_deg,
                                site.longitude_deg
                                    .map_or("NAN".to_string(), |l| format!("{l:.2}")),
                                site.altitude_m,
                                day.map_or(-1, i32::from),
                                noon.map_or(-1, i32::from),
                                c.auto_air_temperature_c_at(site),
                                resolved.air_temp_c.unwrap_or(0),
                                c.snow_air_drop_c(site),
                                crate::conditions::is_winter(&resolved),
                            );
                        }
                    }
                }
            }
        }
    }

    /// The dew point against the tables; dew on a still clear night and
    /// none by day or in a breeze; fog only near saturation.
    #[test]
    fn dew_and_fog_come_with_the_cold_still_night() {
        assert!((dew_point_c(20.0, 0.5) - 9.3).abs() < 0.2);
        assert!((dew_point_c(10.0, 1.0) - 10.0).abs() < 0.01);
        assert!((dew_point_c(30.0, 0.7) - 23.9).abs() < 0.3);
        let td = dew_point_c(23.0, 0.5);
        // A clear still night at 13 °C after a 23 °C afternoon at 50%: dew.
        assert!(dew_strength(13.0, td, -5.0, 0.0, 5.0, 0.0) > 0.5);
        // The same at noon, in a gale, in the rain, under overcast: none.
        assert_eq!(dew_strength(40.0, td, 60.0, 0.0, 5.0, 0.0), 0.0);
        assert_eq!(dew_strength(13.0, td, -5.0, 0.0, 25.0, 0.0), 0.0);
        assert_eq!(dew_strength(13.0, td, -5.0, 0.0, 5.0, 0.5), 0.0);
        assert_eq!(dew_strength(13.0, td, -5.0, 1.0, 5.0, 0.0), 0.0);
        assert_eq!(fog_share(22.0, td, 60.0, 5.0), 0.0);
        assert_eq!(visibility_m(0.0), CLEAR_VISIBILITY_M);
        assert!(fog_share(12.5, td, -2.0, 3.0) > 0.7);
        assert!(visibility_m(1.0) - FOG_VISIBILITY_M < 1.0);
    }

    fn site(lat: f32, lon: f32, alt: f32) -> ClimateSite {
        ClimateSite {
            latitude_deg: lat,
            longitude_deg: Some(lon),
            altitude_m: alt,
        }
    }

    /// The reference is its own reference; a bare latitude is the old
    /// latitude-only season.
    #[test]
    fn the_reference_day_is_zero_to_the_bit() {
        assert_eq!(
            climate_offset_c(MODEL_DAY_OF_YEAR, &ClimateSite::REFERENCE),
            0.0
        );
        for lat in [-37.8, 1.3, 25.0, 50.0, 52.1] {
            let bare = ClimateSite::from(lat);
            assert_eq!(climate_of(lat, None), Climate::REFERENCE);
            assert_eq!(season_offset_c(MODEL_DAY_OF_YEAR, &bare), 0.0);
            assert_eq!(wet_season_pct(MODEL_DAY_OF_YEAR, &bare), 0.0);
            assert_eq!(
                season_c(15, &bare),
                crate::data::season_air_c(15, lat),
                "{lat}"
            );
        }
    }

    /// The model's monthly mean air against the circuits' climate normals
    /// (January and July, °C, within 3.5): the latitude's mean, the
    /// region's swing and offset and the lapse with height together.
    #[test]
    fn the_climate_follows_the_circuit() {
        let circuits = [
            ("Silverstone", site(52.07, -1.02, 150.0), 4.5, 17.0),
            ("Spa", site(50.44, 5.97, 450.0), 1.5, 16.0),
            ("Monza", site(45.62, 9.28, 160.0), 2.5, 24.0),
            ("Barcelona", site(41.57, 2.26, 110.0), 8.5, 24.5),
            ("Hungaroring", site(47.58, 19.25, 240.0), 0.0, 22.0),
            ("Bahrain", site(26.03, 50.51, 10.0), 17.5, 35.0),
            ("Suzuka", site(34.84, 136.54, 40.0), 4.5, 26.5),
            ("Shanghai", site(31.34, 121.22, 5.0), 4.5, 28.0),
            ("Austin", site(30.13, -97.64, 150.0), 10.5, 29.5),
            ("Montreal", site(45.5, -73.52, 15.0), -9.5, 21.0),
            ("Melbourne", site(-37.85, 144.97, 5.0), 21.0, 10.5),
            ("Singapore", site(1.29, 103.86, 15.0), 27.0, 28.0),
            ("Mexico City", site(19.4, -99.09, 2240.0), 14.0, 17.5),
            ("Interlagos", site(-23.7, -46.7, 760.0), 22.5, 16.5),
        ];
        for (name, at, jan, jul) in circuits {
            let (j, l) = (mean_air_c(15, &at), mean_air_c(196, &at));
            println!("{name}: Jan {j:.1} (normal {jan}), Jul {l:.1} (normal {jul})");
            assert!((j - jan).abs() < 3.5, "{name} January {j}");
            assert!((l - jul).abs() < 3.5, "{name} July {l}");
        }
    }

    /// The tropics have a wet and a dry season in the humidity; the equator
    /// and the desert none.
    #[test]
    fn the_tropics_have_a_wet_season() {
        let mexico = site(19.4, -99.09, 2240.0);
        assert!(wet_season_pct(213, &mexico) > 1.0);
        assert!(wet_season_pct(30, &mexico) < -4.0);
        let interlagos = site(-23.7, -46.7, 760.0);
        assert!(wet_season_pct(30, &interlagos) > wet_season_pct(213, &interlagos) + 5.0);
        assert_eq!(wet_season_pct(30, &site(1.29, 103.86, 15.0)), 0.0);
        assert_eq!(wet_season_pct(30, &site(26.03, 50.51, 10.0)), 0.0);
        assert_eq!(wet_season_pct(30, &site(52.07, -1.02, 150.0)), 0.0);
    }
}
