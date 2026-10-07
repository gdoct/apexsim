//! The sky through a session: the clock moving, the weather changing, the
//! air and the asphalt following them.
//!
//! A session's host picks its conditions once (`SessionConditions`), and
//! until now they held for its life. [`LiveConditions`] is what they are
//! *now*: the day's clock runs at the host's `time_scale` from the hour
//! they picked (a 24-hour race at 1 runs through the night; 24 runs a day
//! an hour), and with `changeable` set the weather follows a forecast
//! ([`forecast`], worked out from the session's id, so everyone and every
//! replay sees the same). The rain does not switch: it builds and eases
//! over a few minutes ([`RAIN_EASE_S`]), and the road takes it from there
//! (`crate::road_state`: water rising in the dips, then draining, a line
//! the cars dry). The air follows the hour and the cloud, and the asphalt
//! lags it ([`TRACK_EASE_S`]: it holds heat), cools in the evening and
//! sits under the air while it is wet.
//!
//! The session steps it once a second, before the physics
//! (`GameSession::update_sky`), and bakes the result onto its own copy of
//! the track: the air and track temperatures the tyres and brakes read,
//! the air's density, how much water the road holds for the compound the
//! weather calls for. A session whose clock stands and whose sky holds
//! never steps, so it is the track to the bit as it always was.
//!
//! Telemetry carries the result to every client (`network::SkyNow`), which
//! lights its world by it.

use crate::data::{air_density, solar_gain_c, sun_elevation_deg, SessionConditions, Weather};
use crate::wind::hash01;

/// The rain builds toward the weather's and eases off over this long, s.
pub const RAIN_EASE_S: f32 = 180.0;
/// The cloud comes and goes over this long, s.
pub const CLOUD_EASE_S: f32 = 300.0;
/// The air follows the hour and the cloud over this long, s of the day's
/// clock.
pub const AIR_EASE_S: f32 = 600.0;
/// The asphalt follows the air and the sun over this long, s of the day's
/// clock.
pub const TRACK_EASE_S: f32 = 900.0;
/// The road counts as wet from this much water on it.
pub const WET_FROM_WATER: f32 = 0.05;
/// A forecast changes the weather no sooner than this, s of session time,
/// however fast the clock runs: rain takes minutes to arrive.
const MIN_GAP_S: f32 = 240.0;
/// How far ahead a forecast is worked out, s of session time.
const HORIZON_S: f32 = 26.0 * 3600.0;
/// At most this many changes.
const MAX_CHANGES: usize = 400;

/// One change in a forecast: from `at_s` seconds into the session the
/// weather is `weather`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WeatherChange {
    pub at_s: f32,
    pub weather: Weather,
}

/// The forecast for a session starting under `start`, from `seed` (the
/// session's id): empty unless the host made the weather changeable.
///
/// Each step lasts a stretch of the day (40 to 120 minutes settled, 25 to
/// 75 changeable, 15 to 45 stormy; run through the time scale, never under
/// [`MIN_GAP_S`] of the session) and moves the sky one step wetter or drier
/// (two now and then above settled), drawn back toward the host's pick.
pub fn forecast(start: &SessionConditions, seed: u64) -> Vec<WeatherChange> {
    let level = start.changeability();
    if level == 0 {
        return Vec::new();
    }
    let (gap_lo, gap_hi) = match level {
        1 => (40.0, 120.0),
        2 => (25.0, 75.0),
        _ => (15.0, 45.0),
    };
    let scale = start.clock_scale().max(1) as f32;
    let home = start.weather as i32;
    let mut weather = home;
    let mut at_s = 0.0f32;
    let mut out = Vec::new();
    for k in 0..MAX_CHANGES as u64 {
        let gap_min = gap_lo + (gap_hi - gap_lo) * hash01(seed, 0xF0_0000 + 3 * k);
        at_s += (gap_min * 60.0 / scale).max(MIN_GAP_S);
        if at_s > HORIZON_S {
            break;
        }
        // Wetter or drier: even odds at home, drawn back toward it away
        // from it, and a stormy sky leans wet.
        let lean = 0.1 * (level as f32 - 2.0) - 0.15 * (weather - home) as f32;
        let wetter = hash01(seed, 0xF0_0001 + 3 * k) < (0.5 + lean).clamp(0.1, 0.9);
        let two = level > 1 && hash01(seed, 0xF0_0002 + 3 * k) < 0.15 * (level - 1) as f32;
        let step = if two { 2 } else { 1 };
        let mut next = if wetter {
            weather + step
        } else {
            weather - step
        };
        if !(0..=4).contains(&next) {
            next = if wetter { weather - 1 } else { weather + 1 };
        }
        weather = next.clamp(0, 4);
        out.push(WeatherChange {
            at_s,
            weather: Weather::from_u8(weather as u8).unwrap_or_default(),
        });
    }
    out
}

/// The conditions as they are now.
#[derive(Debug, Clone, PartialEq)]
pub struct LiveConditions {
    /// What the host picked, every figure resolved.
    pub start: SessionConditions,
    /// The circuit's latitude, degrees north: where the sun is.
    pub latitude_deg: f32,
    /// Its height above sea level, m: the air's density.
    pub altitude_m: f32,
    pub forecast: Vec<WeatherChange>,
    /// The day's clock, minutes after midnight, 0..1440.
    pub clock_min: f32,
    /// The weather the forecast has reached.
    pub weather: Weather,
    /// Rain falling, in the weather's water units (0 dry, 0.5 light, 1
    /// heavy), eased toward the weather's.
    pub rain: f32,
    /// Cloud cover, 0..1, eased toward the weather's.
    pub cloud: f32,
    /// The air and the asphalt, °C.
    pub air_c: f32,
    pub track_c: f32,
    /// The air's density against the reference day.
    pub density_ratio: f32,
    /// Water on the road and rubber on its line, as the session last
    /// measured them (`crate::road_state`), for telemetry.
    pub road_water: f32,
    pub line_rubber: f32,
    /// What the host's air temperature puts on the weather's own, °C.
    air_offset_c: f32,
}

impl LiveConditions {
    /// The sky at a session's start: `start` resolved (every figure named),
    /// over a circuit `latitude_deg` north and `altitude_m` up; `seed` the
    /// session's id, which lays out the forecast.
    pub fn new(start: SessionConditions, latitude_deg: f32, altitude_m: f32, seed: u64) -> Self {
        let air_c = start.air_temperature_c();
        let track_c = start.track_temperature_c_at(latitude_deg);
        Self {
            start,
            latitude_deg,
            altitude_m,
            forecast: forecast(&start, seed),
            clock_min: (start.time_of_day_minutes % SessionConditions::MINUTES_PER_DAY) as f32,
            weather: start.weather,
            rain: start.weather.water(),
            cloud: start.weather.cloud(),
            air_c,
            track_c,
            density_ratio: start.air_density_ratio(altitude_m),
            road_water: start.weather.water(),
            line_rubber: start.track_rubber(),
            air_offset_c: air_c - start.auto_air_temperature_c(),
        }
    }

    /// Whether the sky moves at all.
    pub fn is_static(&self) -> bool {
        self.start.is_static()
    }

    /// The weather the forecast holds `at_s` seconds into the session.
    pub fn weather_at(&self, at_s: f32) -> Weather {
        self.forecast
            .iter()
            .take_while(|c| c.at_s <= at_s)
            .last()
            .map_or(self.start.weather, |c| c.weather)
    }

    /// The next change after `at_s`, if the forecast has one.
    pub fn next_change(&self, at_s: f32) -> Option<WeatherChange> {
        self.forecast.iter().find(|c| c.at_s > at_s).copied()
    }

    /// The sun's elevation now, degrees.
    pub fn sun_elevation_deg(&self) -> f32 {
        sun_elevation_deg(self.latitude_deg, self.clock_min)
    }

    /// Whether a car's lights are on when nobody has touched the switch:
    /// as the sun goes (under 6°) and in the rain.
    pub fn headlights_needed(&self) -> bool {
        self.sun_elevation_deg() < 6.0 || self.rain > 0.1
    }

    /// How fast the road dries against a mild sky: more under a high sun
    /// through a clear sky, on warm asphalt, in a breeze; slower at night.
    pub fn evaporation(&self) -> f32 {
        let sun = solar_gain_c(self.sun_elevation_deg(), self.cloud) / 20.0;
        let warm = ((self.track_c - 15.0) / 20.0).clamp(-0.4, 1.5);
        (0.6 + 0.8 * sun + 0.4 * warm).clamp(0.2, 3.0)
    }

    /// The conditions as the host would have picked them for now: the
    /// clock and the weather moved on, the rest as at the start.
    pub fn now(&self) -> SessionConditions {
        SessionConditions {
            weather: self.weather,
            time_of_day_minutes: (self.clock_min.floor() as u16)
                % SessionConditions::MINUTES_PER_DAY,
            ..self.start
        }
    }

    /// Advance to `session_s` seconds into the session, `dt` after the last
    /// step, with `road_water` on the road now (the road's mean,
    /// `RoadState::mean_water`).
    pub fn step(&mut self, session_s: f32, dt: f32, road_water: f32) {
        self.road_water = road_water;
        if self.is_static() {
            return;
        }
        let scale = self.start.clock_scale() as f32;
        let start_min =
            (self.start.time_of_day_minutes % SessionConditions::MINUTES_PER_DAY) as f32;
        self.clock_min = (start_min + session_s * scale / 60.0)
            .rem_euclid(SessionConditions::MINUTES_PER_DAY as f32);
        self.weather = self.weather_at(session_s);
        let ease = |value: &mut f32, target: f32, tau: f32| {
            *value += (target - *value) * (dt / tau).min(1.0);
        };
        ease(&mut self.rain, self.weather.water(), RAIN_EASE_S);
        ease(&mut self.cloud, self.weather.cloud(), CLOUD_EASE_S);
        // The air and the asphalt warm and cool by the day's clock: at 24x
        // the evening comes in an hour, and so does its chill.
        let day_ease = |value: &mut f32, target: f32, tau: f32| {
            *value += (target - *value) * (dt * scale.max(1.0) / tau).min(1.0);
        };
        let air = self.now().auto_air_temperature_c() + self.air_offset_c;
        day_ease(&mut self.air_c, air, AIR_EASE_S);
        // Dry asphalt sits over the air by what the sun puts in through the
        // cloud; water on it holds it a degree under the air.
        let dry = self.air_c + solar_gain_c(self.sun_elevation_deg(), self.cloud);
        let wet = (road_water / 0.25).clamp(0.0, 1.0);
        let track = dry + (self.air_c - 1.0 - dry) * wet;
        day_ease(&mut self.track_c, track, TRACK_EASE_S);
        let humidity = self.start.humidity();
        let reference = SessionConditions::DEFAULT.air_density(0.0);
        self.density_ratio = air_density(self.altitude_m, self.air_c, humidity) / reference;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn picked(weather: Weather, hh: u16, scale: u8, changeable: u8) -> SessionConditions {
        SessionConditions {
            weather,
            time_of_day_minutes: hh * 60,
            time_scale: Some(scale),
            changeable: Some(changeable),
            ..SessionConditions::DEFAULT
        }
        .resolve(7)
    }

    #[test]
    fn a_held_sky_never_moves() {
        let start = SessionConditions::DEFAULT.resolve(7);
        let mut live = LiveConditions::new(start, 45.6, 160.0, 7);
        let before = live.clone();
        for s in 1..3600 {
            live.step(s as f32, 1.0, 0.0);
        }
        assert_eq!(live, before);
        assert!(live.forecast.is_empty());
        assert_eq!(live.track_c, start.track_temperature_c_at(45.6));
    }

    #[test]
    fn the_clock_runs_at_the_hosts_scale_and_wraps() {
        let mut live = LiveConditions::new(picked(Weather::Sunny, 23, 24, 0), 50.0, 0.0, 7);
        live.step(1800.0, 1.0, 0.0);
        // Half an hour at 24x is twelve hours: 23:00 -> 11:00.
        assert!(
            (live.clock_min - 11.0 * 60.0).abs() < 0.01,
            "{}",
            live.clock_min
        );
        assert_eq!(live.now().time_of_day_minutes, 660);
    }

    #[test]
    fn the_evening_cools_the_air_and_the_asphalt_and_puts_the_lights_on() {
        // A 24-hour race from 15:00 at real time, held sunny.
        let mut live = LiveConditions::new(picked(Weather::Sunny, 15, 1, 0), 47.95, 50.0, 7);
        let (air0, track0) = (live.air_c, live.track_c);
        assert!(!live.headlights_needed());
        let mut s = 0.0;
        while s < 7.0 * 3600.0 {
            s += 1.0;
            live.step(s, 1.0, 0.0);
        }
        // 22:00 at Le Mans.
        println!(
            "15:00 air {air0:.1} track {track0:.1} -> 22:00 air {:.1} track {:.1}",
            live.air_c, live.track_c
        );
        assert!(live.air_c < air0 - 3.0);
        assert!(live.track_c < track0 - 10.0, "the asphalt loses the sun");
        assert!(live.track_c > live.air_c - 0.5);
        assert!(live.headlights_needed(), "dark at 22:00");
        assert!(
            live.density_ratio
                > LiveConditions::new(picked(Weather::Sunny, 15, 1, 0), 47.95, 50.0, 7)
                    .density_ratio
        );
    }

    #[test]
    fn a_forecast_is_deterministic_spaced_and_stays_in_range() {
        let start = picked(Weather::Cloudy, 13, 1, 2);
        let a = forecast(&start, 99);
        assert_eq!(a, forecast(&start, 99));
        assert_ne!(a, forecast(&start, 100));
        assert!(!a.is_empty());
        let mut last = 0.0;
        for change in &a {
            assert!(change.at_s - last >= MIN_GAP_S - 1e-3);
            last = change.at_s;
        }
        // A stormy sky rains at some point in a day; a settled one strays
        // less from the host's pick.
        let stormy = forecast(&picked(Weather::Cloudy, 13, 1, 3), 5);
        assert!(stormy.iter().any(|c| c.weather.is_wet()));
        let mean_stray = |f: &[WeatherChange]| {
            f.iter()
                .map(|c| (c.weather as i32 - 1).abs() as f32)
                .sum::<f32>()
                / f.len() as f32
        };
        let settled = forecast(&picked(Weather::Cloudy, 13, 1, 1), 5);
        assert!(mean_stray(&settled) <= mean_stray(&stormy) + 0.5);
        // A fast clock packs the changes in, never closer than the minimum.
        let fast = forecast(&picked(Weather::Cloudy, 13, 60, 2), 5);
        assert!(fast.len() > a.len());
    }

    #[test]
    fn rain_arrives_over_minutes_and_wets_the_asphalt_cool() {
        let start = picked(Weather::Sunny, 13, 1, 3);
        let mut live = LiveConditions::new(start, 50.0, 0.0, 3);
        let first_rain = live
            .forecast
            .iter()
            .find(|c| c.weather.is_wet())
            .copied()
            .expect("a stormy day rains");
        let dry_track = live.track_c;
        let mut s = 0.0;
        while s < first_rain.at_s + 1.0 {
            s += 1.0;
            live.step(s, 1.0, 0.0);
        }
        assert_eq!(live.weather, first_rain.weather);
        assert!(live.rain < 0.05, "it has only just started: {}", live.rain);
        let until = s + 600.0;
        while s < until {
            s += 1.0;
            live.step(s, 1.0, live.rain);
            if live.weather != first_rain.weather {
                break;
            }
        }
        assert!(live.rain > 0.25, "ten minutes on: {}", live.rain);
        assert!(live.headlights_needed());
        assert!(
            live.track_c < dry_track - 5.0,
            "{dry_track} -> {}",
            live.track_c
        );
    }
}
