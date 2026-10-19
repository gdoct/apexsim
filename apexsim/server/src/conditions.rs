//! The sky through a session: the clock moving, the weather changing, the
//! air and the asphalt following them.
//!
//! A session's host picks its conditions once (`SessionConditions`), and
//! until now they held for its life. [`LiveConditions`] is what they are
//! *now*: the day's clock runs at the host's `time_scale` from the hour
//! they picked (a 24-hour race at 1 runs through the night; 24 runs a day
//! an hour), and with `changeable` set the weather follows a forecast
//! ([`forecast`], worked out from the session's id, so everyone and every
//! replay sees the same, or the host's own, `SessionConditions::forecast`). The rain does not switch: it builds and eases
//! over a few minutes ([`RAIN_EASE_S`]), and the road takes it from there
//! (`crate::road_state`: water rising in the dips, then draining, a line
//! the cars dry). The air follows the hour and the cloud, and the asphalt
//! lags it ([`TRACK_EASE_S`]: it holds heat), cools in the evening and
//! sits under the air while it is wet. Snow falls the same way (it builds
//! over [`RAIN_EASE_S`] and the road's cover follows, `crate::road_state`),
//! but only in a winter sky: a forecast that starts in snow, or overcast
//! with the air under [`WINTER_BELOW_C`], swings between overcast and snow and
//! never into rain, and its air keeps the winter's cold either way.
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

use crate::data::{air_density, solar_gain_c, SessionConditions, Weather};
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
/// The wind's mean and the humidity follow the weather over this long, s:
/// a front's wind arrives with its rain.
pub const WIND_EASE_S: f32 = 180.0;
/// Each change of weather turns the wind's mean by this much at least,
/// degrees, and up to [`FRONT_VEER_SPREAD_DEG`] more (hashed per change).
pub const FRONT_VEER_MIN_DEG: f32 = 12.0;
pub const FRONT_VEER_SPREAD_DEG: f32 = 23.0;
/// What of the turn the wind already took a change keeps, 0..1: the
/// direction is drawn back toward the host's over a run of fronts.
const FRONT_VEER_KEEP: f32 = 0.75;
/// The furthest the fronts turn the wind from the host's direction, degrees.
pub const FRONT_VEER_MAX_DEG: f32 = 90.0;
/// Salt for a change's turn of the wind ([`LiveConditions::veer_target_deg`]).
const VEER_SALT: u64 = 0xF1_0000;
/// The road counts as wet from this much water on it.
pub const WET_FROM_WATER: f32 = 0.05;
/// An overcast start with the air under this, °C, is a winter sky: its
/// forecast swings between overcast and snow.
pub const WINTER_BELOW_C: f32 = 1.0;
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
    if !start.forecast.is_empty() {
        // The host wrote one: theirs, in session time whatever the clock.
        return start
            .forecast
            .steps()
            .iter()
            .map(|s| WeatherChange {
                at_s: s.at_min as f32 * 60.0,
                weather: s.weather,
            })
            .collect();
    }
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
    if is_winter(start) {
        // Overcast and snow, snowier when the host started in it and when
        // the sky is stormy.
        let home_lean = if start.weather.is_snow() { 0.15 } else { -0.15 };
        let p_snow = (0.5 + 0.1 * (level as f32 - 2.0) + home_lean).clamp(0.1, 0.9);
        let mut weather = start.weather;
        let mut at_s = 0.0f32;
        let mut out = Vec::new();
        for k in 0..MAX_CHANGES as u64 {
            let gap_min = gap_lo + (gap_hi - gap_lo) * hash01(seed, 0xF0_0000 + 3 * k);
            at_s += (gap_min * 60.0 / scale).max(MIN_GAP_S);
            if at_s > HORIZON_S {
                break;
            }
            let next = if hash01(seed, 0xF0_0001 + 3 * k) < p_snow {
                Weather::Snow
            } else {
                Weather::Overcast
            };
            if next != weather {
                weather = next;
                out.push(WeatherChange { at_s, weather });
            }
        }
        return out;
    }
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

/// Whether a session started under `start` has a winter sky: snow, or an
/// overcast one with the air under [`WINTER_BELOW_C`].
pub fn is_winter(start: &SessionConditions) -> bool {
    start.weather.is_snow()
        || (start.weather == Weather::Overcast && start.air_temperature_c() < WINTER_BELOW_C)
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
    /// Snow falling, 0 or 1, eased toward the weather's the same way.
    pub snowfall: f32,
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
    /// The snow cover on the line, 0..1 (`RoadState::mean_line_snow`).
    pub line_snow: f32,
    /// The wind's mean now, km/h: the weather's own times the host's
    /// share of it ([`LiveConditions::wind_target_kph`]), eased over
    /// [`WIND_EASE_S`]. What the gusts blow about.
    pub wind_kph: f32,
    /// The air's relative humidity now, 0..1: the weather's own plus the
    /// host's offset, eased over [`WIND_EASE_S`].
    pub humidity: f32,
    /// How far the fronts have turned the wind from the host's direction,
    /// degrees, positive veering (clockwise seen from above), eased over
    /// [`WIND_EASE_S`] toward [`LiveConditions::veer_target_deg`]. 0 to
    /// the bit until the weather first changes.
    pub wind_veer_deg: f32,
    /// The seed the forecast was laid out from: each change's turn of the
    /// wind is hashed from it.
    seed: u64,
    /// Whether the sky is a winter one ([`is_winter`]).
    winter: bool,
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
        let winter = is_winter(&start);
        Self {
            start,
            latitude_deg,
            altitude_m,
            forecast: forecast(&start, seed),
            clock_min: (start.time_of_day_minutes % SessionConditions::MINUTES_PER_DAY) as f32,
            weather: start.weather,
            rain: start.weather.rainfall(),
            snowfall: start.weather.snowfall(),
            cloud: start.weather.cloud(),
            air_c,
            track_c,
            density_ratio: start.air_density_ratio(altitude_m),
            road_water: start.weather.water(),
            line_rubber: start.track_rubber(),
            line_snow: start.weather.snowfall(),
            wind_kph: start.wind_kph.unwrap_or(start.auto_wind_kph()) as f32,
            humidity: start.humidity(),
            wind_veer_deg: 0.0,
            seed,
            winter,
            air_offset_c: air_c - Self::auto_air_c(&start, winter, latitude_deg),
        }
    }

    /// The weather's and the clock's own air, °C, for `conditions` over a
    /// circuit `latitude_deg` north in the season of its date: a winter sky
    /// that is not snowing sits as far under its overcast as snow does
    /// (`SessionConditions::snow_air_drop_c`).
    fn auto_air_c(conditions: &SessionConditions, winter: bool, latitude_deg: f32) -> f32 {
        let drop = if winter && !conditions.weather.is_snow() {
            conditions.snow_air_drop_c(latitude_deg)
        } else {
            0.0
        };
        conditions.auto_air_temperature_c_at(latitude_deg) - drop
    }

    /// The wind's mean the weather `weather` brings, km/h: its own
    /// (`SessionConditions::auto_wind_kph`) times the share of the start
    /// weather's own the host picked, so a calm pick stays calm and a gale
    /// stays a gale, and a storm front blows harder than the sun before it.
    /// The host's figure exactly while the weather is the start's.
    pub fn wind_target_kph(&self, weather: Weather) -> f32 {
        let picked = self.start.wind_kph.unwrap_or(self.start.auto_wind_kph()) as f32;
        if weather == self.start.weather {
            return picked;
        }
        let own = |w: Weather| {
            SessionConditions {
                weather: w,
                ..SessionConditions::DEFAULT
            }
            .auto_wind_kph() as f32
        };
        (own(weather) * picked / own(self.start.weather).max(1.0)).clamp(0.0, 60.0)
    }

    /// The humidity the weather `weather` brings, 0..1: its own plus the
    /// host's offset from the start weather's own. The start's exactly
    /// while the weather is the start's.
    pub fn humidity_target(&self, weather: Weather) -> f32 {
        if weather == self.start.weather {
            return self.start.humidity();
        }
        let own = |w: Weather| {
            SessionConditions {
                weather: w,
                ..SessionConditions::DEFAULT
            }
            .auto_humidity_pct() as f32
                / 100.0
        };
        (own(weather) + self.start.humidity() - own(self.start.weather)).clamp(0.0, 1.0)
    }

    /// How far the fronts up to `at_s` seconds into the session have
    /// turned the wind from the host's direction, degrees, positive
    /// veering (clockwise seen from above). A change to wetter weather (a
    /// front coming in) backs it, a change to drier (the front gone
    /// through, the sky clearing behind it) veers it, by
    /// [`FRONT_VEER_MIN_DEG`] plus up to [`FRONT_VEER_SPREAD_DEG`] hashed
    /// from the session's seed and the change; the turn already taken is
    /// kept at [`FRONT_VEER_KEEP`], so a run of fronts wanders about the
    /// host's direction within [`FRONT_VEER_MAX_DEG`]. South of the
    /// equator the same weather turns it the other way. 0 before the
    /// first change.
    pub fn veer_target_deg(&self, at_s: f32) -> f32 {
        let hemisphere = if self.latitude_deg < 0.0 { -1.0 } else { 1.0 };
        let mut previous = self.start.weather;
        let mut veer = 0.0f32;
        for (k, change) in self.forecast.iter().enumerate() {
            if change.at_s > at_s {
                break;
            }
            if change.weather == previous {
                continue;
            }
            let wetter = (change.weather as u8) > (previous as u8);
            let size = FRONT_VEER_MIN_DEG
                + FRONT_VEER_SPREAD_DEG * hash01(self.seed, VEER_SALT + k as u64);
            let turn = if wetter { -size } else { size };
            veer = (veer * FRONT_VEER_KEEP + hemisphere * turn)
                .clamp(-FRONT_VEER_MAX_DEG, FRONT_VEER_MAX_DEG);
            previous = change.weather;
        }
        veer
    }

    /// Where the wind's mean blows from now, degrees from the start
    /// straight's direction (`SessionConditions::wind_from_deg`, 90 from
    /// its left): the host's, turned by the fronts ([`Self::wind_veer_deg`];
    /// the frame's angles run anticlockwise, so a veer takes them down).
    pub fn wind_from_deg(&self) -> f32 {
        self.start.wind_from_deg.unwrap_or(0) as f32 - self.wind_veer_deg
    }

    /// The wind's mean now, m/s toward where it blows, in the start
    /// straight's frame (`SessionConditions::wind_mps` at [`Self::wind_kph`],
    /// from [`Self::wind_from_deg`]).
    pub fn wind_mps(&self) -> [f32; 2] {
        let speed = self.wind_kph / 3.6;
        let from = self.wind_from_deg().to_radians();
        [-speed * from.cos(), -speed * from.sin()]
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

    /// The changes after `at_s`, soonest first, at most `n` of them: what a
    /// driver reads the sky ahead off.
    pub fn changes_after(&self, at_s: f32, n: usize) -> impl Iterator<Item = WeatherChange> + '_ {
        self.forecast
            .iter()
            .filter(move |c| c.at_s > at_s)
            .take(n)
            .copied()
    }

    /// The sun's elevation now, degrees.
    pub fn sun_elevation_deg(&self) -> f32 {
        self.start
            .sun_elevation_at(self.latitude_deg, self.clock_min)
    }

    /// Whether a car's lights are on when nobody has touched the switch:
    /// as the sun goes (under 6°) and in the rain.
    pub fn headlights_needed(&self) -> bool {
        self.sun_elevation_deg() < 6.0 || self.rain > 0.1 || self.snowfall > 0.1
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
        ease(&mut self.rain, self.weather.rainfall(), RAIN_EASE_S);
        ease(&mut self.snowfall, self.weather.snowfall(), RAIN_EASE_S);
        ease(&mut self.cloud, self.weather.cloud(), CLOUD_EASE_S);
        let wind = self.wind_target_kph(self.weather);
        ease(&mut self.wind_kph, wind, WIND_EASE_S);
        let humidity = self.humidity_target(self.weather);
        ease(&mut self.humidity, humidity, WIND_EASE_S);
        let veer = self.veer_target_deg(session_s);
        ease(&mut self.wind_veer_deg, veer, WIND_EASE_S);
        // The air and the asphalt warm and cool by the day's clock: at 24x
        // the evening comes in an hour, and so does its chill.
        let day_ease = |value: &mut f32, target: f32, tau: f32| {
            *value += (target - *value) * (dt * scale.max(1.0) / tau).min(1.0);
        };
        let air = Self::auto_air_c(&self.now(), self.winter, self.latitude_deg) + self.air_offset_c;
        day_ease(&mut self.air_c, air, AIR_EASE_S);
        // Dry asphalt sits over the air by what the sun puts in through the
        // cloud; water on it holds it a degree under the air.
        let dry = self.air_c + solar_gain_c(self.sun_elevation_deg(), self.cloud);
        let wet = (road_water / 0.25).clamp(0.0, 1.0);
        let track = dry + (self.air_c - 1.0 - dry) * wet;
        day_ease(&mut self.track_c, track, TRACK_EASE_S);
        let reference = SessionConditions::DEFAULT.air_density(0.0);
        self.density_ratio = air_density(self.altitude_m, self.air_c, self.humidity) / reference;
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

    /// The forecast the client's create screen shows is its own port of
    /// `forecast` (`ApexCreateSession::Forecast`), pinned against these
    /// figures (`ApexSim.UI.CreateSession.Forecast`): a change here moves
    /// that test too. `cargo test the_forecast_reference -- --nocapture`
    /// prints them.
    #[test]
    fn the_forecast_reference() {
        const SEED: u64 = 0x0123_4567_89AB_CDEF;
        let summer = SessionConditions {
            sky_seed: Some(SEED),
            ..picked(Weather::Cloudy, 15, 4, 2)
        };
        let out = forecast(&summer, summer.sky_seed_or(1));
        for c in &out {
            println!("summer {:?} s {:?}", c.at_s, c.weather);
        }
        let first: Vec<(f32, Weather)> = out.iter().take(5).map(|c| (c.at_s, c.weather)).collect();
        assert_eq!(
            first,
            vec![
                (979.83295, Weather::Sunny),
                (1437.4385, Weather::Cloudy),
                (2386.9404, Weather::Overcast),
                (3194.0688, Weather::LightRain),
                (3735.432, Weather::Overcast),
            ]
        );
        let winter = SessionConditions {
            sky_seed: Some(SEED),
            ..picked(Weather::Snow, 9, 1, 3)
        };
        let out = forecast(&winter, winter.sky_seed_or(1));
        for c in &out {
            println!("winter {:?} s {:?}", c.at_s, c.weather);
        }
        let first: Vec<(f32, Weather)> = out.iter().take(3).map(|c| (c.at_s, c.weather)).collect();
        assert_eq!(
            first,
            vec![
                (21565.688, Weather::Overcast),
                (23245.703, Weather::Snow),
                (24461.414, Weather::Overcast),
            ]
        );
    }

    /// A host's forecast is made valid on the way in (sorted, spaced, cut
    /// at the limit, its weather kept to the sky's season) and replaces
    /// the seeded one, moving a sky that would otherwise hold.
    #[test]
    fn a_host_written_forecast_is_held_to_the_rules_and_drives_the_sky() {
        use crate::data::{ForecastStep, HostForecast};
        let step = |at_min, weather| ForecastStep { at_min, weather };
        let written = SessionConditions {
            weather: Weather::Cloudy,
            forecast: HostForecast::from_steps(
                Weather::Cloudy,
                &[
                    step(60, Weather::HeavyRain),
                    step(10, Weather::LightRain),
                    step(12, Weather::Overcast),  // too soon after 10
                    step(90, Weather::HeavyRain), // no change
                    step(120, Weather::Sunny),
                    step(5000, Weather::Cloudy), // past the horizon
                ],
            ),
            ..SessionConditions::DEFAULT
        };
        assert_eq!(
            written.forecast.steps(),
            &[
                step(10, Weather::LightRain),
                step(60, Weather::HeavyRain),
                step(120, Weather::Sunny)
            ]
        );
        // From the wire too: unknown weather dropped, order made good.
        let raw = rmp_serde::to_vec(&vec![(200u64, 3u64), (30, 9), (2, 1), (50, 2)]).unwrap();
        let read: HostForecast = rmp_serde::from_slice(&raw).unwrap();
        let clamped = SessionConditions {
            forecast: read,
            ..SessionConditions::DEFAULT
        }
        .clamp();
        assert_eq!(
            clamped.forecast.steps(),
            &[step(50, Weather::Overcast), step(200, Weather::LightRain)],
            "the 2-minute step is inside the first gap"
        );
        let too_many: Vec<ForecastStep> = (1..40)
            .map(|k| {
                step(
                    k * 10,
                    if k % 2 == 0 {
                        Weather::Sunny
                    } else {
                        Weather::Overcast
                    },
                )
            })
            .collect();
        assert_eq!(
            HostForecast::from_steps(Weather::Sunny, &too_many)
                .steps()
                .len(),
            HostForecast::MAX_STEPS
        );

        // It drives the sky with no `changeable`, ignoring the seed.
        let start = written.resolve(3);
        assert!(!start.is_static());
        assert_eq!(forecast(&start, 3), forecast(&start, 4));
        let mut live = LiveConditions::new(start, 50.0, 0.0, 3);
        assert_eq!(live.forecast.len(), 3);
        assert_eq!(live.forecast[1].at_s, 3600.0);
        for s in 1..=3700 {
            live.step(s as f32, 1.0, live.rain);
        }
        assert_eq!(live.weather, Weather::HeavyRain);
        assert!(live.rain > 0.2);

        // A winter sky's rain falls as snow; a summer sky's snow as rain.
        let winter = SessionConditions {
            weather: Weather::Snow,
            forecast: HostForecast::from_steps(
                Weather::Snow,
                &[step(20, Weather::Overcast), step(40, Weather::HeavyRain)],
            ),
            ..SessionConditions::DEFAULT
        }
        .resolve(3);
        assert_eq!(
            winter.forecast.steps(),
            &[step(20, Weather::Overcast), step(40, Weather::Snow)]
        );
        let summer = SessionConditions {
            forecast: HostForecast::from_steps(Weather::Sunny, &[step(20, Weather::Snow)]),
            ..SessionConditions::DEFAULT
        }
        .resolve(3);
        assert_eq!(summer.forecast.steps(), &[step(20, Weather::LightRain)]);
        // None written: the seeded forecast, the bytes and the sky as before.
        assert!(SessionConditions::DEFAULT.resolve(3).forecast.is_empty());
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
    fn the_sky_ahead_names_the_next_three_changes_with_their_etas() {
        let mut live = LiveConditions::new(picked(Weather::Sunny, 13, 1, 2), 50.0, 0.0, 3);
        live.forecast = vec![
            WeatherChange {
                at_s: 600.0,
                weather: Weather::Cloudy,
            },
            WeatherChange {
                at_s: 1800.0,
                weather: Weather::LightRain,
            },
            WeatherChange {
                at_s: 2400.0,
                weather: Weather::HeavyRain,
            },
            WeatherChange {
                at_s: 4000.0,
                weather: Weather::Overcast,
            },
        ];
        let sky = crate::network::SkyNow::of(&live, [0.0, 0.0], 100.0);
        assert_eq!(
            (sky.next_weather, sky.next_in_s),
            (Weather::Cloudy as u8, 500)
        );
        assert_eq!(
            (sky.next2_weather, sky.next2_in_s),
            (Weather::LightRain as u8, 1700)
        );
        assert_eq!(
            (sky.next3_weather, sky.next3_in_s),
            (Weather::HeavyRain as u8, 2300)
        );
        // Past the third: only the last change is left, and none after it.
        let late = crate::network::SkyNow::of(&live, [0.0, 0.0], 2500.0);
        assert_eq!(
            (late.next_weather, late.next_in_s),
            (Weather::Overcast as u8, 1500)
        );
        assert_eq!((late.next2_in_s, late.next3_in_s), (0, 0));
    }

    #[test]
    fn a_winter_forecast_swings_between_overcast_and_snow_and_never_rains() {
        let snow = SessionConditions {
            weather: Weather::Snow,
            time_scale: Some(1),
            changeable: Some(2),
            ..SessionConditions::DEFAULT
        }
        .resolve(7);
        assert!(is_winter(&snow));
        let a = forecast(&snow, 41);
        assert_eq!(a, forecast(&snow, 41));
        assert!(a.len() > 3);
        assert!(a
            .iter()
            .all(|c| matches!(c.weather, Weather::Overcast | Weather::Snow)));
        assert!(a.windows(2).all(|w| w[0].weather != w[1].weather));
        // An overcast sky is winter only when the air is under 1 degree.
        let cold = SessionConditions {
            weather: Weather::Overcast,
            air_temp_c: Some(-2),
            changeable: Some(2),
            time_scale: Some(1),
            ..SessionConditions::DEFAULT
        }
        .resolve(7);
        assert!(is_winter(&cold));
        assert!(forecast(&cold, 41).iter().any(|c| c.weather.is_snow()));
        let mild = SessionConditions {
            air_temp_c: Some(12),
            ..cold
        };
        assert!(!is_winter(&mild));
        assert!(forecast(&mild, 41).iter().all(|c| !c.weather.is_snow()));
    }

    #[test]
    fn snow_falls_over_minutes_and_the_winter_air_stays_cold_through_the_forecast() {
        let start = SessionConditions {
            weather: Weather::Overcast,
            air_temp_c: Some(-3),
            time_of_day_minutes: 10 * 60,
            time_scale: Some(1),
            changeable: Some(3),
            ..SessionConditions::DEFAULT
        }
        .resolve(5);
        let mut live = LiveConditions::new(start, 50.0, 0.0, 5);
        let first = *live.forecast.iter().find(|c| c.weather.is_snow()).unwrap();
        assert_eq!(live.snowfall, 0.0);
        let (mut s, mut lowest, mut highest) = (0.0, live.air_c, live.air_c);
        while s < first.at_s + 600.0 {
            s += 1.0;
            live.step(s, 1.0, 0.0);
            lowest = lowest.min(live.air_c);
            highest = highest.max(live.air_c);
        }
        assert_eq!(live.weather, first.weather);
        assert!(live.snowfall > 0.2, "ten minutes on: {}", live.snowfall);
        assert_eq!(live.rain, 0.0, "snow is not rain");
        assert!(live.headlights_needed());
        assert!(highest < 4.0 && lowest > -12.0, "{lowest}..{highest}");
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

    #[test]
    fn a_front_brings_its_wind_and_its_damp_air() {
        let start = picked(Weather::Sunny, 13, 1, 3);
        let mut live = LiveConditions::new(start, 50.0, 0.0, 3);
        live.forecast = vec![WeatherChange {
            at_s: 60.0,
            weather: Weather::HeavyRain,
        }];
        assert_eq!(live.wind_kph, 8.0);
        assert_eq!(live.humidity, 0.5);
        let mut s = 0.0;
        while s < 60.0 {
            s += 1.0;
            live.step(s, 1.0, 0.0);
        }
        // Still the sun's breeze to the bit until the front arrives.
        assert!(live.wind_kph > 7.9 && live.wind_kph < 9.0);
        while s < 60.0 + 900.0 {
            s += 1.0;
            live.step(s, 1.0, live.rain);
        }
        assert!(live.wind_kph > 27.0, "{}", live.wind_kph);
        assert!(live.humidity > 0.95, "{}", live.humidity);
        let [wx, wy] = live.wind_mps();
        assert!(((wx * wx + wy * wy).sqrt() * 3.6 - live.wind_kph).abs() < 1e-3);
        // A host's calm stays calm; a host's gale scales with the weather.
        let calm = LiveConditions::new(
            SessionConditions {
                wind_kph: Some(0),
                ..start
            },
            50.0,
            0.0,
            3,
        );
        assert_eq!(calm.wind_target_kph(Weather::HeavyRain), 0.0);
        let windy = LiveConditions::new(
            SessionConditions {
                wind_kph: Some(16),
                ..start
            },
            50.0,
            0.0,
            3,
        );
        assert_eq!(windy.wind_target_kph(Weather::HeavyRain), 56.0);
        assert_eq!(windy.wind_target_kph(Weather::Sunny), 16.0);
    }

    #[test]
    fn a_front_backs_the_wind_and_the_clearing_veers_it() {
        let start = SessionConditions {
            wind_from_deg: Some(90),
            ..picked(Weather::Sunny, 13, 1, 3)
        };
        let mut live = LiveConditions::new(start, 50.0, 0.0, 3);
        live.forecast = vec![
            WeatherChange {
                at_s: 60.0,
                weather: Weather::HeavyRain,
            },
            WeatherChange {
                at_s: 1260.0,
                weather: Weather::Sunny,
            },
        ];
        assert_eq!(live.veer_target_deg(59.0), 0.0);
        let backed = live.veer_target_deg(60.0);
        assert!(
            (-FRONT_VEER_MIN_DEG - FRONT_VEER_SPREAD_DEG..=-FRONT_VEER_MIN_DEG).contains(&backed),
            "{backed}"
        );
        let cleared = live.veer_target_deg(1260.0);
        assert!(cleared > backed + FRONT_VEER_MIN_DEG - 1e-3, "{cleared}");
        // Smooth: the direction eases over minutes, never jumps a step.
        let mut s = 0.0;
        let mut last = live.wind_from_deg();
        while s < 1200.0 {
            s += 1.0;
            live.step(s, 1.0, live.rain);
            let from = live.wind_from_deg();
            assert!((from - last).abs() < 0.5, "a second's turn {}", from - last);
            last = from;
        }
        assert!(
            (live.wind_veer_deg - backed).abs() < 0.5,
            "{}",
            live.wind_veer_deg
        );
        // Backing turns the direction it comes from anticlockwise (up from
        // the left toward behind); the mean blows at its speed from there.
        assert!(live.wind_from_deg() > 90.0 + FRONT_VEER_MIN_DEG - 1.0);
        let [wx, wy] = live.wind_mps();
        let from = live.wind_from_deg().to_radians();
        assert!((wx + live.wind_kph / 3.6 * from.cos()).abs() < 1e-4);
        assert!((wy + live.wind_kph / 3.6 * from.sin()).abs() < 1e-4);
        // South of the equator the same front veers it.
        let south = LiveConditions {
            latitude_deg: -37.8,
            ..live.clone()
        };
        assert_eq!(south.veer_target_deg(60.0), -backed);
        // Deterministic, and a run of fronts stays within the bound.
        let stormy = LiveConditions::new(picked(Weather::Cloudy, 13, 60, 3), 50.0, 0.0, 11);
        assert!(stormy.forecast.len() > 20);
        let turns: Vec<f32> = stormy
            .forecast
            .iter()
            .map(|c| stormy.veer_target_deg(c.at_s))
            .collect();
        assert!(turns.iter().all(|t| t.abs() <= FRONT_VEER_MAX_DEG));
        assert!(turns.iter().any(|t| *t > 0.0) && turns.iter().any(|t| *t < 0.0));
        let again = LiveConditions::new(picked(Weather::Cloudy, 13, 60, 3), 50.0, 0.0, 11);
        assert_eq!(again.veer_target_deg(1e9), stormy.veer_target_deg(1e9));
    }

    #[test]
    fn a_sky_that_keeps_its_weather_keeps_its_wind_to_the_bit() {
        let mut live = LiveConditions::new(picked(Weather::Cloudy, 13, 4, 0), 50.0, 0.0, 3);
        let (wind, humidity) = (live.wind_kph, live.humidity);
        for s in 1..3600 {
            live.step(s as f32, 1.0, 0.0);
        }
        assert_eq!((live.wind_kph, live.humidity), (wind, humidity));
    }

    /// Solar noon on the circuit's own clock on the model day (25 May),
    /// against the almanac's figure for that date, within three minutes.
    #[test]
    fn solar_noon_stands_where_the_circuit_is() {
        use crate::data::{solar_noon_minutes, MODEL_DAY_OF_YEAR};
        let cases = [
            ("Silverstone", 52.07, -1.02, 13 * 60 + 1),
            ("Spa", 50.44, 5.98, 13 * 60 + 33),
            ("Zandvoort", 52.39, 4.54, 13 * 60 + 39),
            ("Barcelona", 41.57, 2.26, 13 * 60 + 48),
            ("Monza", 45.62, 9.27, 13 * 60 + 20),
            ("Suzuka", 34.84, 136.54, 11 * 60 + 51),
            ("Austin", 30.13, -97.64, 13 * 60 + 28),
            ("Mexico City", 19.40, -99.09, 12 * 60 + 33),
            ("Interlagos", -23.70, -46.70, 12 * 60 + 4),
            ("Melbourne", -37.85, 144.97, 12 * 60 + 17),
            ("Marina Bay", 1.29, 103.86, 13 * 60),
            ("Yas Marina", 24.47, 54.60, 12 * 60 + 18),
        ];
        for (name, lat, lon, almanac) in cases {
            let noon = solar_noon_minutes(lat, lon, MODEL_DAY_OF_YEAR) as i32;
            println!("{name}: {:02}:{:02}", noon / 60, noon % 60);
            assert!(
                (noon - almanac).abs() <= 3,
                "{name}: {noon} against {almanac}"
            );
        }
        // Summer time goes with the season: Silverstone in January, Melbourne
        // in its own summer.
        assert_eq!(solar_noon_minutes(52.07, -1.02, 15) / 60, 12);
        assert_eq!(solar_noon_minutes(-37.85, 144.97, 15) / 60, 13);
    }

    /// A session with no date is the model day to the bit; a date moves
    /// the sun's height and the length of the day.
    #[test]
    fn a_date_moves_the_sun_and_no_date_is_late_may() {
        let may = SessionConditions::DEFAULT;
        assert_eq!(may.sun_declination_deg(), crate::data::SUN_DECLINATION_DEG);
        assert_eq!(may.solar_noon_hours(), crate::data::SOLAR_NOON_HOURS);
        let winter = SessionConditions {
            day_of_year: Some(355),
            ..may
        };
        let summer = SessionConditions {
            day_of_year: Some(172),
            ..may
        };
        assert!((winter.sun_declination_deg() + 23.44).abs() < 0.1);
        assert!((summer.sun_declination_deg() - 23.44).abs() < 0.1);
        let noon = |c: &SessionConditions| c.sun_elevation_at(50.0, 13.0 * 60.0);
        assert!((noon(&winter) - 16.6).abs() < 0.2, "{}", noon(&winter));
        assert!(noon(&summer) > noon(&may));
        // At 50° N a December 17:30 is night, a June one broad day.
        assert!(winter.sun_elevation_at(50.0, 17.5 * 60.0) < 0.0);
        assert!(summer.sun_elevation_at(50.0, 17.5 * 60.0) > 20.0);
        // The asphalt feels it.
        assert!(winter.track_temperature_c_at(50.0) < summer.track_temperature_c_at(50.0) - 5.0);
    }

    /// A date moves the weather's own air and humidity with the season, by
    /// latitude, the south's turned half a year; no date, and the model
    /// day named outright, are the late-May figures to the bit.
    #[test]
    fn a_date_moves_the_air_with_the_season_and_no_date_is_late_may() {
        use crate::data::MODEL_DAY_OF_YEAR;
        for weather in Weather::ALL {
            for hh in [3, 9, 13, 15, 21] {
                for lat in [-37.8, 1.3, 25.0, 50.0, 52.1] {
                    let undated = SessionConditions {
                        weather,
                        time_of_day_minutes: hh * 60,
                        ..SessionConditions::DEFAULT
                    };
                    let may = SessionConditions {
                        day_of_year: Some(MODEL_DAY_OF_YEAR),
                        ..undated
                    };
                    for c in [undated, may] {
                        assert_eq!(c.season_offset_c(lat), 0.0);
                        assert_eq!(
                            c.auto_air_temperature_c_at(lat),
                            undated.auto_air_temperature_c()
                        );
                        assert_eq!(c.auto_humidity_pct_at(lat), undated.auto_humidity_pct());
                        assert_eq!(c.snow_air_drop_c(lat), Weather::SNOW_AIR_DROP_C);
                    }
                    let resolved = undated.resolve_at(7, lat);
                    assert_eq!(resolved, undated.resolve(7), "no date: {lat}");
                }
            }
        }
        let on = |day: u16, lat: f32| {
            SessionConditions {
                time_of_day_minutes: 15 * 60,
                day_of_year: Some(day),
                ..SessionConditions::DEFAULT
            }
            .resolve_at(3, lat)
        };
        let air = |c: SessionConditions| c.air_temp_c.unwrap();
        // Silverstone: a 15:00 in mid-January near 10 °C, mid-July warmer
        // than late May.
        let (jan, may, jul) = (on(15, 52.07), on(145, 52.07), on(196, 52.07));
        println!(
            "Silverstone 15:00 air: Jan {} May {} Jul {}; humidity {} {} {}",
            air(jan),
            air(may),
            air(jul),
            jan.humidity_pct.unwrap(),
            may.humidity_pct.unwrap(),
            jul.humidity_pct.unwrap()
        );
        assert!((8..=12).contains(&air(jan)), "{}", air(jan));
        assert!(air(jul) > air(may));
        assert!(jan.humidity_pct > may.humidity_pct && jul.humidity_pct < may.humidity_pct);
        // Melbourne's January is its summer; the equator has no seasons.
        assert!(air(on(15, -37.85)) > air(on(196, -37.85)) + 5);
        assert_eq!(air(on(15, 1.29)), air(on(196, 1.29)));
        // The snow line: a January overcast night at 50° N resolves under
        // freezing and is a winter sky; the same night in late May is not.
        let night = |day: Option<u16>| {
            SessionConditions {
                weather: Weather::Overcast,
                time_of_day_minutes: 4 * 60,
                changeable: Some(2),
                time_scale: Some(1),
                day_of_year: day,
                ..SessionConditions::DEFAULT
            }
            .resolve_at(3, 50.0)
        };
        assert!(
            is_winter(&night(Some(15))),
            "{:?}",
            night(Some(15)).air_temp_c
        );
        assert!(!is_winter(&night(None)));
        // Snow's own air is the same in any month; the winter overcast
        // between the snow sits as far above it as the season leaves.
        let snow = |day| SessionConditions {
            weather: Weather::Snow,
            day_of_year: Some(day),
            ..SessionConditions::DEFAULT
        };
        assert_eq!(
            snow(15).auto_air_temperature_c_at(50.0),
            snow(145).auto_air_temperature_c_at(50.0)
        );
        let mut live = LiveConditions::new(night(Some(15)), 50.0, 0.0, 3);
        let start_air = live.air_c;
        for s in 1..1800 {
            live.step(s as f32, 1.0, 0.0);
        }
        assert!(
            (live.air_c - start_air).abs() < 3.0,
            "{start_air} -> {}",
            live.air_c
        );
    }

    /// The server names solar noon from the track on create; a track that
    /// does not say where it is keeps 13:00, and a create's own figure is
    /// never kept.
    #[test]
    fn solar_noon_is_named_from_the_track() {
        let mut track = crate::data::TrackConfig::default();
        let asked = SessionConditions {
            solar_noon_min: Some(600),
            ..SessionConditions::DEFAULT
        };
        assert_eq!(asked.at_site(&track).solar_noon_min, None);
        track.metadata.latitude_deg = Some(50.44);
        track.metadata.longitude_deg = Some(5.98);
        let spa = asked.at_site(&track);
        assert!(spa
            .solar_noon_min
            .is_some_and(|m| (13 * 60 + 30..13 * 60 + 36).contains(&m)));
        // Its air follows the later sun: 13:00 at Spa is a little cooler
        // than 13:00 where noon is at 13:00, and the live sky agrees.
        assert!(spa.auto_air_temperature_c() < SessionConditions::DEFAULT.auto_air_temperature_c());
        let live = LiveConditions::new(spa.resolve(3), 50.44, 0.0, 3);
        assert_eq!(
            live.sun_elevation_deg(),
            spa.sun_elevation_at(50.44, 13.0 * 60.0)
        );
    }
}
