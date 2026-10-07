//! The spectator stream (`.apxs`): what somebody watching a race receives.
//!
//! A racer's telemetry (`CompactCarState`, 37 fields a car) carries tyres,
//! brakes, fuel and everything else a driver's HUD shows. A spectator draws
//! the cars and nothing more, so the stream is a sequence of small
//! self-contained **records** that can be written to a file or sent over the
//! wire unchanged (docs/SPECTATOR.md):
//!
//! ```text
//! [u32 big-endian length][MessagePack body]
//! ```
//!
//! The body is a positional MessagePack array. Its first element is the
//! record type and, for every record a viewer receives, its second is the
//! **epoch** written as a full `uint 32` (`0xCE` and four bytes), so the
//! epoch sits at bytes 3..7 of every body. A showcase channel that loops
//! takes a fresh epoch and stamps it into the bytes it read from the file
//! ([`patch_epoch`]) without decoding anything.
//!
//! | Type | Record | Body |
//! |---|---|---|
//! | 1 | [`StreamHeader`] | `[1, epoch, version, stream_id, tick_rate, frame_rate, row_size, track, conditions, session_kind, game_mode, lap_limit, ticks, render]` |
//! | 2 | [`StreamRoster`] | `[2, epoch, revision, [[car_index, car_config_id, content_crc, livery, name, is_ai], ...]]` |
//! | 3 | [`StreamFrame`] | `[3, epoch, tick, revision, state, countdown_ms, part, parts, bin rows]` |
//! | 4 | [`StreamEvent`] | `[4, epoch, tick, kind, payload]` |
//! | 5 | [`Block`] | `[5, first_tick, last_tick, records, raw_len, bin zlib]` (file only) |
//! | 6 | [`Index`] | `[6, [[first_tick, offset, records], ...]]` (file only) |
//! | 7 | [`StreamPath`] | `[7, epoch, spacing_m, bin points]` |
//!
//! Golden bytes for the client's codec come from
//! `cargo test spectator_wire_format -- --nocapture`
//! (`ApexSpectatorGoldenBlobs.h`).

use std::io::{Read, Write};

use crate::data::*;
use crate::game_session::GameSession;
use crate::network::LapTimingData;

/// Version of the record layout, carried in every [`StreamHeader`].
pub const FORMAT_VERSION: u8 = 1;
/// The first four bytes of an `.apxs` file.
pub const FILE_MAGIC: &[u8; 4] = b"APXS";
/// Version of the file's container (magic, blocks, index, trailer).
pub const FILE_VERSION: u16 = 1;
/// Bytes of one car in a frame's `rows` as this build writes them. A later
/// version only ever appends: a reader takes the fields it knows and skips
/// the rest, and fills what an older stream's shorter rows lack as unknown.
pub const ROW_SIZE: usize = 52;
/// The first row layout's size (no tyres): the shortest row a reader takes.
pub const ROW_SIZE_V1: usize = 44;
/// Rows that fit one datagram under a 1 400 byte budget; a bigger field is
/// sent in parts.
pub const MAX_ROWS_PER_PART: usize = 26;
/// `tyre_wear` of a tyre the stream does not know (a converted replay, a car
/// before its set is fitted, a v1 row).
pub const TYRE_WEAR_UNKNOWN: u8 = u8::MAX;
/// `countdown_ms` when nothing is counting down.
pub const NO_COUNTDOWN: u16 = u16::MAX;
/// Metres between the points of a [`StreamPath`].
pub const PATH_SPACING_M: f32 = 10.0;

pub const RECORD_HEADER: u8 = 1;
pub const RECORD_ROSTER: u8 = 2;
pub const RECORD_FRAME: u8 = 3;
pub const RECORD_EVENT: u8 = 4;
pub const RECORD_BLOCK: u8 = 5;
pub const RECORD_INDEX: u8 = 6;
pub const RECORD_PATH: u8 = 7;

/// `CarRow::status` bits.
pub const STATUS_ON_TRACK: u8 = 1;
pub const STATUS_COLLIDING: u8 = 2;
pub const STATUS_IN_GARAGE: u8 = 4;
pub const STATUS_RETIRED: u8 = 8;
pub const STATUS_FINISHED: u8 = 16;

#[derive(Debug, thiserror::Error)]
pub enum StreamError {
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),
    #[error("not a spectator stream: {0}")]
    Malformed(String),
    #[error("unsupported {what} version {found} (this build reads {supported})")]
    Version {
        what: &'static str,
        found: u32,
        supported: u32,
    },
}

fn malformed<T>(why: impl Into<String>) -> Result<T, StreamError> {
    Err(StreamError::Malformed(why.into()))
}

// --- MessagePack, by hand -----------------------------------------------------
//
// Written with `rmp` rather than derived: the layout is the contract (the
// client's codec is hand-written too), and some integers are deliberately
// wider than their value needs.

mod wr {
    use rmp::encode as e;

    pub fn array(out: &mut Vec<u8>, len: usize) {
        e::write_array_len(out, len as u32).expect("vec write");
    }
    /// The smallest encoding of an unsigned integer.
    pub fn uint(out: &mut Vec<u8>, v: u64) {
        e::write_uint(out, v).expect("vec write");
    }
    pub fn sint(out: &mut Vec<u8>, v: i64) {
        e::write_sint(out, v).expect("vec write");
    }
    /// Always `0xCE` and four bytes: a field another program patches in place.
    pub fn u32_fixed(out: &mut Vec<u8>, v: u32) {
        e::write_u32(out, v).expect("vec write");
    }
    pub fn f32(out: &mut Vec<u8>, v: f32) {
        e::write_f32(out, v).expect("vec write");
    }
    pub fn bool(out: &mut Vec<u8>, v: bool) {
        e::write_bool(out, v).expect("vec write");
    }
    pub fn nil(out: &mut Vec<u8>) {
        e::write_nil(out).expect("vec write");
    }
    pub fn str(out: &mut Vec<u8>, v: &str) {
        e::write_str(out, v).expect("vec write");
    }
    pub fn bin(out: &mut Vec<u8>, v: &[u8]) {
        e::write_bin(out, v).expect("vec write");
    }
}

/// A cursor over one record body.
struct Rd<'a>(&'a [u8]);

impl<'a> Rd<'a> {
    fn array(&mut self) -> Result<usize, StreamError> {
        rmp::decode::read_array_len(&mut self.0)
            .map(|n| n as usize)
            .map_err(|e| StreamError::Malformed(format!("array: {e}")))
    }
    fn uint(&mut self) -> Result<u64, StreamError> {
        rmp::decode::read_int(&mut self.0).map_err(|e| StreamError::Malformed(format!("uint: {e}")))
    }
    fn sint(&mut self) -> Result<i64, StreamError> {
        rmp::decode::read_int(&mut self.0).map_err(|e| StreamError::Malformed(format!("int: {e}")))
    }
    fn f32(&mut self) -> Result<f32, StreamError> {
        rmp::decode::read_f32(&mut self.0).map_err(|e| StreamError::Malformed(format!("f32: {e}")))
    }
    fn bool(&mut self) -> Result<bool, StreamError> {
        rmp::decode::read_bool(&mut self.0)
            .map_err(|e| StreamError::Malformed(format!("bool: {e}")))
    }
    fn is_nil(&mut self) -> bool {
        if self.0.first() == Some(&0xC0) {
            self.0 = &self.0[1..];
            true
        } else {
            false
        }
    }
    fn str(&mut self) -> Result<String, StreamError> {
        let len = rmp::decode::read_str_len(&mut self.0)
            .map_err(|e| StreamError::Malformed(format!("str: {e}")))? as usize;
        let bytes = self.take(len)?;
        String::from_utf8(bytes.to_vec()).map_err(|e| StreamError::Malformed(format!("str: {e}")))
    }
    fn bin(&mut self) -> Result<&'a [u8], StreamError> {
        let len = rmp::decode::read_bin_len(&mut self.0)
            .map_err(|e| StreamError::Malformed(format!("bin: {e}")))? as usize;
        self.take(len)
    }
    fn take(&mut self, len: usize) -> Result<&'a [u8], StreamError> {
        if self.0.len() < len {
            return malformed(format!("{len} bytes wanted, {} left", self.0.len()));
        }
        let (head, tail) = self.0.split_at(len);
        self.0 = tail;
        Ok(head)
    }
    fn uuid(&mut self) -> Result<uuid::Uuid, StreamError> {
        let text = self.str()?;
        uuid::Uuid::parse_str(&text).map_err(|e| StreamError::Malformed(format!("uuid: {e}")))
    }
    /// Skip elements a newer writer appended to an array this build knows
    /// `known` of.
    fn skip_rest(&mut self, len: usize, known: usize) -> Result<(), StreamError> {
        for _ in known..len {
            skip_value(&mut self.0)?;
        }
        Ok(())
    }
}

/// Step over one MessagePack value of any kind.
fn skip_value(input: &mut &[u8]) -> Result<(), StreamError> {
    use rmp::Marker;
    let marker = rmp::decode::read_marker(input)
        .map_err(|_| StreamError::Malformed("truncated value".into()))?;
    let skip = |n: usize, input: &mut &[u8]| -> Result<(), StreamError> {
        if input.len() < n {
            return malformed("truncated value");
        }
        *input = &input[n..];
        Ok(())
    };
    let be = |n: usize, input: &mut &[u8]| -> Result<usize, StreamError> {
        if input.len() < n {
            return malformed("truncated length");
        }
        let v = input[..n]
            .iter()
            .fold(0usize, |a, b| (a << 8) | *b as usize);
        *input = &input[n..];
        Ok(v)
    };
    match marker {
        Marker::FixPos(_) | Marker::FixNeg(_) | Marker::Null | Marker::True | Marker::False => {
            Ok(())
        }
        Marker::U8 | Marker::I8 => skip(1, input),
        Marker::U16 | Marker::I16 => skip(2, input),
        Marker::U32 | Marker::I32 | Marker::F32 => skip(4, input),
        Marker::U64 | Marker::I64 | Marker::F64 => skip(8, input),
        Marker::FixStr(n) => skip(n as usize, input),
        Marker::Str8 | Marker::Bin8 => {
            let n = be(1, input)?;
            skip(n, input)
        }
        Marker::Str16 | Marker::Bin16 => {
            let n = be(2, input)?;
            skip(n, input)
        }
        Marker::Str32 | Marker::Bin32 => {
            let n = be(4, input)?;
            skip(n, input)
        }
        Marker::FixArray(n) => (0..n).try_for_each(|_| skip_value(input)),
        Marker::Array16 => {
            let n = be(2, input)?;
            (0..n).try_for_each(|_| skip_value(input))
        }
        Marker::Array32 => {
            let n = be(4, input)?;
            (0..n).try_for_each(|_| skip_value(input))
        }
        Marker::FixMap(n) => (0..n as usize * 2).try_for_each(|_| skip_value(input)),
        Marker::Map16 => {
            let n = be(2, input)?;
            (0..n * 2).try_for_each(|_| skip_value(input))
        }
        Marker::Map32 => {
            let n = be(4, input)?;
            (0..n * 2).try_for_each(|_| skip_value(input))
        }
        other => malformed(format!("unsupported value {other:?}")),
    }
}

/// `[type, epoch]`: how every record a viewer receives begins.
fn begin(out: &mut Vec<u8>, len: usize, record: u8, epoch: u32) {
    debug_assert!(len <= 15, "the epoch's offset assumes a fixarray");
    wr::array(out, len);
    wr::uint(out, record as u64);
    wr::u32_fixed(out, epoch);
}

/// The record type of a body, without decoding it.
pub fn record_type(body: &[u8]) -> Option<u8> {
    match body {
        [head, kind, ..] if head & 0xF0 == 0x90 && *kind < 0x80 => Some(*kind),
        _ => None,
    }
}

/// The epoch of a viewer-facing record, without decoding it.
pub fn record_epoch(body: &[u8]) -> Option<u32> {
    match body {
        [_, _, 0xCE, a, b, c, d, ..] => Some(u32::from_be_bytes([*a, *b, *c, *d])),
        _ => None,
    }
}

/// The tick of a [`StreamFrame`] or [`StreamEvent`], without decoding it.
pub fn record_tick(body: &[u8]) -> Option<u32> {
    match (record_type(body)?, body) {
        (RECORD_FRAME | RECORD_EVENT, [_, _, 0xCE, _, _, _, _, 0xCE, a, b, c, d, ..]) => {
            Some(u32::from_be_bytes([*a, *b, *c, *d]))
        }
        _ => None,
    }
}

/// Stamp another epoch into a record's bytes. False (and nothing written)
/// when the bytes are not a record with an epoch.
pub fn patch_epoch(body: &mut [u8], epoch: u32) -> bool {
    if record_epoch(body).is_none() {
        return false;
    }
    body[3..7].copy_from_slice(&epoch.to_be_bytes());
    true
}

/// `body` with the stream's framing in front: the length, big-endian.
pub fn framed(body: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(body.len() + 4);
    out.extend_from_slice(&(body.len() as u32).to_be_bytes());
    out.extend_from_slice(body);
    out
}

// --- Header -------------------------------------------------------------------

/// The track a stream was raced on.
#[derive(Debug, Clone, PartialEq)]
pub struct StreamTrack {
    pub track_id: TrackConfigId,
    /// The YAML's stem: what the client's export and catalog are named by.
    pub stem: String,
    /// What a screen shows (never the real, trademarked name).
    pub display_name: String,
    /// `content_crc` of the track YAML the race was simulated on.
    pub source_crc: u32,
    pub length_m: f32,
}

/// How a rendered file came to be, so a rebake is reproducible.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct StreamRender {
    pub seed: Option<u64>,
    /// The seed's score when it was picked from several ([`RaceScore`]).
    pub score: Option<f32>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct StreamHeader {
    /// Counts restarts of the stream; see the module docs.
    pub epoch: u32,
    pub version: u8,
    pub stream_id: uuid::Uuid,
    /// Ticks per second of every tick number in the stream.
    pub tick_rate: u16,
    /// Frames per second.
    pub frame_rate: u16,
    pub row_size: u8,
    pub track: StreamTrack,
    /// Resolved: the AI raced in exactly this.
    pub conditions: SessionConditions,
    pub session_kind: SessionKind,
    pub game_mode: GameMode,
    pub lap_limit: u8,
    /// The tick the lights went out, when the content holds a start.
    pub race_start_tick: Option<u32>,
    /// First and last tick of the content.
    pub start_tick: u32,
    pub end_tick: u32,
    pub render: StreamRender,
}

impl StreamHeader {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(160);
        begin(&mut out, 14, RECORD_HEADER, self.epoch);
        wr::uint(&mut out, self.version as u64);
        wr::str(&mut out, &self.stream_id.to_string());
        wr::uint(&mut out, self.tick_rate as u64);
        wr::uint(&mut out, self.frame_rate as u64);
        wr::uint(&mut out, self.row_size as u64);

        wr::array(&mut out, 5);
        wr::str(&mut out, &self.track.track_id.to_string());
        wr::str(&mut out, &self.track.stem);
        wr::str(&mut out, &self.track.display_name);
        wr::uint(&mut out, self.track.source_crc as u64);
        wr::f32(&mut out, self.track.length_m);

        let c = &self.conditions;
        wr::array(&mut out, 6);
        wr::uint(&mut out, c.weather as u64);
        wr::uint(&mut out, c.time_of_day_minutes as u64);
        match c.air_temp_c {
            Some(v) => wr::sint(&mut out, v as i64),
            None => wr::nil(&mut out),
        }
        match c.humidity_pct {
            Some(v) => wr::uint(&mut out, v as u64),
            None => wr::nil(&mut out),
        }
        match c.wind_kph {
            Some(v) => wr::uint(&mut out, v as u64),
            None => wr::nil(&mut out),
        }
        match c.wind_from_deg {
            Some(v) => wr::uint(&mut out, v as u64),
            None => wr::nil(&mut out),
        }

        wr::uint(&mut out, self.session_kind as u64);
        wr::uint(&mut out, self.game_mode as u64);
        wr::uint(&mut out, self.lap_limit as u64);

        wr::array(&mut out, 3);
        match self.race_start_tick {
            Some(v) => wr::uint(&mut out, v as u64),
            None => wr::nil(&mut out),
        }
        wr::uint(&mut out, self.start_tick as u64);
        wr::uint(&mut out, self.end_tick as u64);

        wr::array(&mut out, 2);
        match self.render.seed {
            Some(v) => wr::uint(&mut out, v),
            None => wr::nil(&mut out),
        }
        match self.render.score {
            Some(v) => wr::f32(&mut out, v),
            None => wr::nil(&mut out),
        }
        out
    }

    fn decode(rd: &mut Rd, len: usize, epoch: u32) -> Result<Self, StreamError> {
        if len < 14 {
            return malformed(format!("header of {len} fields"));
        }
        let version = rd.uint()? as u8;
        if version != FORMAT_VERSION {
            return Err(StreamError::Version {
                what: "stream",
                found: version as u32,
                supported: FORMAT_VERSION as u32,
            });
        }
        let stream_id = rd.uuid()?;
        let tick_rate = rd.uint()? as u16;
        let frame_rate = rd.uint()? as u16;
        let row_size = rd.uint()? as u8;

        let n = rd.array()?;
        if n < 5 {
            return malformed("track of too few fields");
        }
        let track = StreamTrack {
            track_id: rd.uuid()?,
            stem: rd.str()?,
            display_name: rd.str()?,
            source_crc: rd.uint()? as u32,
            length_m: rd.f32()?,
        };
        rd.skip_rest(n, 5)?;

        let n = rd.array()?;
        if n < 6 {
            return malformed("conditions of too few fields");
        }
        let weather = match rd.uint()? {
            0 => Weather::Sunny,
            1 => Weather::Cloudy,
            2 => Weather::Overcast,
            3 => Weather::LightRain,
            4 => Weather::HeavyRain,
            other => return malformed(format!("weather {other}")),
        };
        let time_of_day_minutes = rd.uint()? as u16;
        let air_temp_c = if rd.is_nil() {
            None
        } else {
            Some(rd.sint()? as i8)
        };
        let humidity_pct = if rd.is_nil() {
            None
        } else {
            Some(rd.uint()? as u8)
        };
        let wind_kph = if rd.is_nil() {
            None
        } else {
            Some(rd.uint()? as u8)
        };
        let wind_from_deg = if rd.is_nil() {
            None
        } else {
            Some(rd.uint()? as u16)
        };
        rd.skip_rest(n, 6)?;
        let conditions = SessionConditions {
            weather,
            time_of_day_minutes,
            air_temp_c,
            humidity_pct,
            wind_kph,
            wind_from_deg,
            time_scale: None,
            changeable: None,
            track_rubber_pct: None,
        };

        let session_kind = match rd.uint()? {
            1 => SessionKind::Practice,
            2 => SessionKind::Sandbox,
            3 => SessionKind::Demo,
            _ => SessionKind::Multiplayer,
        };
        let game_mode = game_mode_from(rd.uint()? as u8);
        let lap_limit = rd.uint()? as u8;

        let n = rd.array()?;
        if n < 3 {
            return malformed("ticks of too few fields");
        }
        let race_start_tick = if rd.is_nil() {
            None
        } else {
            Some(rd.uint()? as u32)
        };
        let start_tick = rd.uint()? as u32;
        let end_tick = rd.uint()? as u32;
        rd.skip_rest(n, 3)?;

        let n = rd.array()?;
        if n < 2 {
            return malformed("render of too few fields");
        }
        let seed = if rd.is_nil() { None } else { Some(rd.uint()?) };
        let score = if rd.is_nil() { None } else { Some(rd.f32()?) };
        rd.skip_rest(n, 2)?;
        rd.skip_rest(len, 14)?;

        Ok(Self {
            epoch,
            version,
            stream_id,
            tick_rate,
            frame_rate,
            row_size,
            track,
            conditions,
            session_kind,
            game_mode,
            lap_limit,
            race_start_tick,
            start_tick,
            end_tick,
            render: StreamRender { seed, score },
        })
    }

    /// Seconds of content.
    pub fn duration_s(&self) -> f32 {
        self.end_tick.saturating_sub(self.start_tick) as f32 / self.tick_rate.max(1) as f32
    }
}

fn game_mode_from(v: u8) -> GameMode {
    match v {
        1 => GameMode::Sandbox,
        2 => GameMode::Countdown,
        3 => GameMode::DemoLap,
        4 => GameMode::FreePractice,
        5 => GameMode::Replay,
        6 => GameMode::Qualification,
        7 => GameMode::Race,
        8 => GameMode::Hotlap,
        _ => GameMode::Lobby,
    }
}

// --- Roster -------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct StreamRosterEntry {
    pub car_index: u8,
    pub car_config_id: CarConfigId,
    /// `content_crc` of the car.toml the car was simulated from.
    pub content_crc: u32,
    pub livery: u8,
    pub name: String,
    pub is_ai: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct StreamRoster {
    pub epoch: u32,
    /// Counts roster changes within an epoch.
    pub revision: u16,
    pub entries: Vec<StreamRosterEntry>,
}

impl StreamRoster {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(16 + self.entries.len() * 64);
        begin(&mut out, 4, RECORD_ROSTER, self.epoch);
        wr::uint(&mut out, self.revision as u64);
        wr::array(&mut out, self.entries.len());
        for e in &self.entries {
            wr::array(&mut out, 6);
            wr::uint(&mut out, e.car_index as u64);
            wr::str(&mut out, &e.car_config_id.to_string());
            wr::uint(&mut out, e.content_crc as u64);
            wr::uint(&mut out, e.livery as u64);
            wr::str(&mut out, &e.name);
            wr::bool(&mut out, e.is_ai);
        }
        out
    }

    fn decode(rd: &mut Rd, len: usize, epoch: u32) -> Result<Self, StreamError> {
        if len < 4 {
            return malformed("roster of too few fields");
        }
        let revision = rd.uint()? as u16;
        let count = rd.array()?;
        let mut entries = Vec::with_capacity(count.min(256));
        for _ in 0..count {
            let n = rd.array()?;
            if n < 6 {
                return malformed("roster entry of too few fields");
            }
            entries.push(StreamRosterEntry {
                car_index: rd.uint()? as u8,
                car_config_id: rd.uuid()?,
                content_crc: rd.uint()? as u32,
                livery: rd.uint()? as u8,
                name: rd.str()?,
                is_ai: rd.bool()?,
            });
            rd.skip_rest(n, 6)?;
        }
        rd.skip_rest(len, 4)?;
        Ok(Self {
            epoch,
            revision,
            entries,
        })
    }

    /// The roster of a session: its cars in car-index order, liveries dealt
    /// as a live session's roster deals them.
    pub fn from_session(session: &GameSession, epoch: u32, revision: u16) -> Self {
        let roster = session.build_roster(&std::collections::HashMap::new());
        Self {
            epoch,
            revision,
            entries: roster
                .entries
                .into_iter()
                .map(|e| StreamRosterEntry {
                    car_index: e.car_index,
                    content_crc: session
                        .car_configs
                        .get(&e.car_config_id)
                        .map_or(0, |c| c.content_crc),
                    car_config_id: e.car_config_id,
                    livery: e.livery,
                    name: e.player_name,
                    is_ai: e.is_ai,
                })
                .collect(),
        }
    }
}

// --- Car rows and frames --------------------------------------------------------

/// One car in a frame: what a spectator draws. 52 bytes, little-endian
/// (the first 44 are version 1's, which a reader still takes).
///
/// | Offset | Type | Field |
/// |---|---|---|
/// | 0 | u8 | car index |
/// | 1 | u8 | status ([`STATUS_ON_TRACK`]...) |
/// | 2, 6, 10 | i32 | x, y, z, mm |
/// | 14 | u16 | yaw, 2π/65536 rad |
/// | 16, 18 | i16 | pitch, roll, π/32768 rad |
/// | 20 | u16 | speed, cm/s |
/// | 22 | i8 | steering, /127 |
/// | 23, 24 | u8 | throttle, brake, /255 |
/// | 25 | i8 | gear |
/// | 26 | u16 | engine rpm |
/// | 28 | u16 | lap |
/// | 30 | u32 | station, cm |
/// | 34 | u8 | finish position, 0 none |
/// | 35 | u8 | lap flags, as `CompactCarState.lap_flags` |
/// | 36 | u8 | pit flags, as `CompactCarState.pit_flags` |
/// | 37 | u8 | compound, 255 unknown |
/// | 38 | u8 x 5 | damage, percent: front, rear, left, right, engine |
/// | 43 | u8 | ERS flags, as `CompactCarState.ers_flags` |
/// | 44 | u8 x 4 | tyre wear, percent, FL FR RL RR; [`TYRE_WEAR_UNKNOWN`] unknown |
/// | 48 | u8 x 4 | tread temperature, °C as `CompactCarState.tyre_c` (0 unknown) |
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CarRow {
    pub car_index: u8,
    pub status: u8,
    pub x_mm: i32,
    pub y_mm: i32,
    pub z_mm: i32,
    pub yaw: u16,
    pub pitch: i16,
    pub roll: i16,
    pub speed_cms: u16,
    pub steering: i8,
    pub throttle: u8,
    pub brake: u8,
    pub gear: i8,
    pub engine_rpm: u16,
    pub lap: u16,
    pub station_cm: u32,
    pub finish_position: u8,
    pub lap_flags: u8,
    pub pit_flags: u8,
    pub compound: u8,
    pub damage: [u8; 5],
    pub ers_flags: u8,
    pub tyre_wear: [u8; 4],
    pub tyre_c: [u8; 4],
}

fn metres_to_mm(m: f32) -> i32 {
    (m as f64 * 1000.0)
        .round()
        .clamp(i32::MIN as f64, i32::MAX as f64) as i32
}

fn yaw_to_u16(rad: f32) -> u16 {
    let turns = (rad as f64 / std::f64::consts::TAU).rem_euclid(1.0);
    ((turns * 65536.0).round() as u32 & 0xFFFF) as u16
}

fn angle_to_i16(rad: f32) -> i16 {
    (rad as f64 / std::f64::consts::PI * 32768.0)
        .round()
        .clamp(i16::MIN as f64, i16::MAX as f64) as i16
}

fn unit_to_u8(v: f32) -> u8 {
    (v.clamp(0.0, 1.0) * 255.0).round() as u8
}

impl CarRow {
    pub fn from_car_state(state: &CarState, car_index: u8) -> Self {
        let mut status = 0;
        if state.is_on_track {
            status |= STATUS_ON_TRACK;
        }
        if state.is_colliding {
            status |= STATUS_COLLIDING;
        }
        if state.in_garage {
            status |= STATUS_IN_GARAGE;
        }
        if !state.damage.is_drivable {
            status |= STATUS_RETIRED;
        }
        if state.finish_position.is_some() {
            status |= STATUS_FINISHED;
        }
        let d = &state.damage;
        Self {
            car_index,
            status,
            x_mm: metres_to_mm(state.pos_x),
            y_mm: metres_to_mm(state.pos_y),
            z_mm: metres_to_mm(state.pos_z),
            yaw: yaw_to_u16(state.yaw_rad),
            pitch: angle_to_i16(state.pitch_rad),
            roll: angle_to_i16(state.roll_rad),
            speed_cms: (state.speed_mps.abs() * 100.0).round().min(u16::MAX as f32) as u16,
            steering: (state.steering_input.clamp(-1.0, 1.0) * 127.0).round() as i8,
            throttle: unit_to_u8(state.throttle_input),
            brake: unit_to_u8(state.brake_input),
            gear: state.gear,
            engine_rpm: state.engine_rpm.round().clamp(0.0, u16::MAX as f32) as u16,
            lap: state.current_lap,
            station_cm: (state.track_progress.max(0.0) as f64 * 100.0).round() as u32,
            finish_position: state.finish_position.unwrap_or(0),
            lap_flags: crate::network::lap_flags_of(state),
            pit_flags: crate::network::pit_flags_of(state),
            compound: if state.tyres_fitted {
                state.tyre_compound
            } else {
                crate::network::COMPOUND_UNKNOWN
            },
            damage: [
                d.front_damage_percent,
                d.rear_damage_percent,
                d.left_damage_percent,
                d.right_damage_percent,
                d.engine_damage_percent,
            ]
            .map(|p| p.round().clamp(0.0, 100.0) as u8),
            ers_flags: crate::hybrid::telemetry_bytes(state).2,
            tyre_wear: if state.tyres_fitted {
                state
                    .tires
                    .each()
                    .map(|t| t.wear_percent.round().clamp(0.0, 100.0) as u8)
            } else {
                [TYRE_WEAR_UNKNOWN; 4]
            },
            tyre_c: crate::network::tyre_bytes(state).0,
        }
    }

    /// A row from a replay's telemetry, which lacks what a replay never
    /// recorded: the pit state, the compound, the damage and the hybrid.
    pub fn from_replay(car: &crate::network::CarStateTelemetry, car_index: u8) -> Self {
        let mut status = 0;
        if car.is_on_track {
            status |= STATUS_ON_TRACK;
        }
        if car.is_colliding {
            status |= STATUS_COLLIDING;
        }
        if car.lap_flags & crate::network::LAP_FLAG_IN_GARAGE != 0 {
            status |= STATUS_IN_GARAGE;
        }
        if car.finish_position.is_some() {
            status |= STATUS_FINISHED;
        }
        Self {
            car_index,
            status,
            x_mm: metres_to_mm(car.pos_x),
            y_mm: metres_to_mm(car.pos_y),
            z_mm: metres_to_mm(car.pos_z),
            yaw: yaw_to_u16(car.yaw_rad),
            pitch: angle_to_i16(car.pitch_rad),
            roll: angle_to_i16(car.roll_rad),
            speed_cms: (car.speed_mps.abs() * 100.0).round().min(u16::MAX as f32) as u16,
            steering: (car.steering.clamp(-1.0, 1.0) * 127.0).round() as i8,
            throttle: unit_to_u8(car.throttle),
            brake: unit_to_u8(car.brake),
            gear: car.gear,
            engine_rpm: car.engine_rpm.round().clamp(0.0, u16::MAX as f32) as u16,
            lap: car.current_lap,
            station_cm: (car.track_progress.max(0.0) as f64 * 100.0).round() as u32,
            finish_position: car.finish_position.unwrap_or(0),
            lap_flags: car.lap_flags,
            pit_flags: 0,
            compound: crate::network::COMPOUND_UNKNOWN,
            damage: [0; 5],
            ers_flags: 0,
            tyre_wear: [TYRE_WEAR_UNKNOWN; 4],
            tyre_c: [0; 4],
        }
    }

    pub fn write(&self, out: &mut Vec<u8>) {
        let start = out.len();
        out.push(self.car_index);
        out.push(self.status);
        out.extend_from_slice(&self.x_mm.to_le_bytes());
        out.extend_from_slice(&self.y_mm.to_le_bytes());
        out.extend_from_slice(&self.z_mm.to_le_bytes());
        out.extend_from_slice(&self.yaw.to_le_bytes());
        out.extend_from_slice(&self.pitch.to_le_bytes());
        out.extend_from_slice(&self.roll.to_le_bytes());
        out.extend_from_slice(&self.speed_cms.to_le_bytes());
        out.push(self.steering as u8);
        out.push(self.throttle);
        out.push(self.brake);
        out.push(self.gear as u8);
        out.extend_from_slice(&self.engine_rpm.to_le_bytes());
        out.extend_from_slice(&self.lap.to_le_bytes());
        out.extend_from_slice(&self.station_cm.to_le_bytes());
        out.push(self.finish_position);
        out.push(self.lap_flags);
        out.push(self.pit_flags);
        out.push(self.compound);
        out.extend_from_slice(&self.damage);
        out.push(self.ers_flags);
        out.extend_from_slice(&self.tyre_wear);
        out.extend_from_slice(&self.tyre_c);
        debug_assert_eq!(out.len() - start, ROW_SIZE);
    }

    /// Read a row of at least [`ROW_SIZE_V1`] bytes; anything a later
    /// version appended is ignored, and the tyres of a version 1 row are
    /// unknown.
    pub fn read(row: &[u8]) -> Option<Self> {
        if row.len() < ROW_SIZE_V1 {
            return None;
        }
        let tyres = row.len() >= ROW_SIZE;
        let quad = |o: usize, unknown: u8| {
            if tyres {
                [row[o], row[o + 1], row[o + 2], row[o + 3]]
            } else {
                [unknown; 4]
            }
        };
        let i32_at = |o: usize| i32::from_le_bytes([row[o], row[o + 1], row[o + 2], row[o + 3]]);
        let u16_at = |o: usize| u16::from_le_bytes([row[o], row[o + 1]]);
        Some(Self {
            car_index: row[0],
            status: row[1],
            x_mm: i32_at(2),
            y_mm: i32_at(6),
            z_mm: i32_at(10),
            yaw: u16_at(14),
            pitch: u16_at(16) as i16,
            roll: u16_at(18) as i16,
            speed_cms: u16_at(20),
            steering: row[22] as i8,
            throttle: row[23],
            brake: row[24],
            gear: row[25] as i8,
            engine_rpm: u16_at(26),
            lap: u16_at(28),
            station_cm: i32_at(30) as u32,
            finish_position: row[34],
            lap_flags: row[35],
            pit_flags: row[36],
            compound: row[37],
            damage: [row[38], row[39], row[40], row[41], row[42]],
            ers_flags: row[43],
            tyre_wear: quad(44, TYRE_WEAR_UNKNOWN),
            tyre_c: quad(48, 0),
        })
    }

    pub fn x_m(&self) -> f32 {
        self.x_mm as f32 / 1000.0
    }
    pub fn y_m(&self) -> f32 {
        self.y_mm as f32 / 1000.0
    }
    pub fn z_m(&self) -> f32 {
        self.z_mm as f32 / 1000.0
    }
    pub fn yaw_rad(&self) -> f32 {
        self.yaw as f32 * std::f32::consts::TAU / 65536.0
    }
    pub fn speed_mps(&self) -> f32 {
        self.speed_cms as f32 / 100.0
    }
    pub fn station_m(&self) -> f32 {
        self.station_cm as f32 / 100.0
    }
}

/// One snapshot of (part of) the field. Self-contained: no frame refers to
/// another, so a lost datagram costs one frame and a viewer can join at any.
#[derive(Debug, Clone, PartialEq)]
pub struct StreamFrame {
    pub epoch: u32,
    pub tick: u32,
    pub roster_revision: u16,
    pub state: SessionState,
    /// [`NO_COUNTDOWN`] when nothing counts down.
    pub countdown_ms: u16,
    /// This datagram's place among the `parts` the frame was split into.
    pub part: u8,
    pub parts: u8,
    /// `row_size x cars` bytes.
    pub rows: Vec<u8>,
}

impl StreamFrame {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(32 + self.rows.len());
        begin(&mut out, 9, RECORD_FRAME, self.epoch);
        wr::u32_fixed(&mut out, self.tick);
        wr::uint(&mut out, self.roster_revision as u64);
        wr::uint(&mut out, self.state as u64);
        wr::uint(&mut out, self.countdown_ms as u64);
        wr::uint(&mut out, self.part as u64);
        wr::uint(&mut out, self.parts as u64);
        wr::bin(&mut out, &self.rows);
        out
    }

    fn decode(rd: &mut Rd, len: usize, epoch: u32) -> Result<Self, StreamError> {
        if len < 9 {
            return malformed("frame of too few fields");
        }
        let tick = rd.uint()? as u32;
        let roster_revision = rd.uint()? as u16;
        let state = match rd.uint()? {
            0 => SessionState::Lobby,
            1 => SessionState::Countdown,
            2 => SessionState::Racing,
            3 => SessionState::Finished,
            other => return malformed(format!("session state {other}")),
        };
        let countdown_ms = rd.uint()? as u16;
        let part = rd.uint()? as u8;
        let parts = rd.uint()? as u8;
        let rows = rd.bin()?.to_vec();
        rd.skip_rest(len, 9)?;
        Ok(Self {
            epoch,
            tick,
            roster_revision,
            state,
            countdown_ms,
            part,
            parts,
            rows,
        })
    }

    /// The rows, `row_size` bytes apart (the header's; a newer stream's rows
    /// are longer than this build reads).
    pub fn cars(&self, row_size: usize) -> impl Iterator<Item = CarRow> + '_ {
        self.rows
            .chunks_exact(row_size.max(ROW_SIZE_V1))
            .filter_map(CarRow::read)
    }
}

/// Encode a field as one frame record per [`MAX_ROWS_PER_PART`] rows.
pub fn encode_frames(
    epoch: u32,
    tick: u32,
    roster_revision: u16,
    state: SessionState,
    countdown_ms: Option<u16>,
    rows: &[CarRow],
) -> Vec<Vec<u8>> {
    let chunks: Vec<&[CarRow]> = if rows.is_empty() {
        vec![rows]
    } else {
        rows.chunks(MAX_ROWS_PER_PART).collect()
    };
    let parts = chunks.len() as u8;
    chunks
        .into_iter()
        .enumerate()
        .map(|(part, chunk)| {
            let mut bytes = Vec::with_capacity(chunk.len() * ROW_SIZE);
            for row in chunk {
                row.write(&mut bytes);
            }
            StreamFrame {
                epoch,
                tick,
                roster_revision,
                state,
                countdown_ms: countdown_ms.map_or(NO_COUNTDOWN, |ms| ms.min(NO_COUNTDOWN - 1)),
                part: part as u8,
                parts,
                rows: bytes,
            }
            .encode()
        })
        .collect()
}

// --- Events -------------------------------------------------------------------

pub const EVENT_LAP_TIMING: u8 = 1;
pub const EVENT_TRACK_SECTORS: u8 = 2;
pub const EVENT_SESSION_STATE: u8 = 3;
pub const EVENT_FINISH: u8 = 4;
pub const EVENT_RETIRED: u8 = 5;
pub const EVENT_PIT_STOP: u8 = 6;
pub const EVENT_CONTACT: u8 = 7;

/// `PitStop` phases.
pub const PIT_ENTERED: u8 = 0;
pub const PIT_SERVICED: u8 = 1;
pub const PIT_LEFT: u8 = 2;

/// `Contact::other` when the sim does not say what was hit.
pub const CONTACT_UNKNOWN: u8 = 255;

#[derive(Debug, Clone, PartialEq)]
pub enum EventKind {
    /// A car crossed a timing line: the `LapTiming` message's own fields.
    LapTiming(LapTimingData),
    /// Where the sector lines are; once, after the header.
    TrackSectors {
        track_length_m: f32,
        boundaries_m: Vec<f32>,
    },
    SessionState(SessionState),
    Finish {
        car_index: u8,
        position: u8,
    },
    /// A car is out. `reason` 1: damage.
    Retired {
        car_index: u8,
        reason: u8,
    },
    PitStop {
        car_index: u8,
        phase: u8,
    },
    /// A car hit something: the TV director's incident cue. `speed_cms` is
    /// the car's own speed as it happened.
    Contact {
        car_index: u8,
        other: u8,
        speed_cms: u16,
    },
    /// A kind this build does not know; kept so a file passes through.
    Unknown(u8),
}

/// Something that happened, delivered reliably and stamped with its tick.
#[derive(Debug, Clone, PartialEq)]
pub struct StreamEvent {
    pub epoch: u32,
    pub tick: u32,
    pub kind: EventKind,
}

impl StreamEvent {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(48);
        begin(&mut out, 5, RECORD_EVENT, self.epoch);
        wr::u32_fixed(&mut out, self.tick);
        match &self.kind {
            EventKind::LapTiming(t) => {
                wr::uint(&mut out, EVENT_LAP_TIMING as u64);
                wr::array(&mut out, 8);
                wr::uint(&mut out, t.car_index as u64);
                wr::uint(&mut out, t.lap as u64);
                wr::uint(&mut out, t.sector as u64);
                wr::uint(&mut out, t.sector_time_ms as u64);
                wr::uint(&mut out, t.lap_time_ms as u64);
                wr::bool(&mut out, t.is_lap_end);
                wr::bool(&mut out, t.valid);
                wr::uint(&mut out, t.flags as u64);
            }
            EventKind::TrackSectors {
                track_length_m,
                boundaries_m,
            } => {
                wr::uint(&mut out, EVENT_TRACK_SECTORS as u64);
                wr::array(&mut out, 2);
                wr::f32(&mut out, *track_length_m);
                wr::array(&mut out, boundaries_m.len());
                for b in boundaries_m {
                    wr::f32(&mut out, *b);
                }
            }
            EventKind::SessionState(state) => {
                wr::uint(&mut out, EVENT_SESSION_STATE as u64);
                wr::array(&mut out, 1);
                wr::uint(&mut out, *state as u64);
            }
            EventKind::Finish {
                car_index,
                position,
            } => {
                wr::uint(&mut out, EVENT_FINISH as u64);
                wr::array(&mut out, 2);
                wr::uint(&mut out, *car_index as u64);
                wr::uint(&mut out, *position as u64);
            }
            EventKind::Retired { car_index, reason } => {
                wr::uint(&mut out, EVENT_RETIRED as u64);
                wr::array(&mut out, 2);
                wr::uint(&mut out, *car_index as u64);
                wr::uint(&mut out, *reason as u64);
            }
            EventKind::PitStop { car_index, phase } => {
                wr::uint(&mut out, EVENT_PIT_STOP as u64);
                wr::array(&mut out, 2);
                wr::uint(&mut out, *car_index as u64);
                wr::uint(&mut out, *phase as u64);
            }
            EventKind::Contact {
                car_index,
                other,
                speed_cms,
            } => {
                wr::uint(&mut out, EVENT_CONTACT as u64);
                wr::array(&mut out, 3);
                wr::uint(&mut out, *car_index as u64);
                wr::uint(&mut out, *other as u64);
                wr::uint(&mut out, *speed_cms as u64);
            }
            EventKind::Unknown(kind) => {
                wr::uint(&mut out, *kind as u64);
                wr::array(&mut out, 0);
            }
        }
        out
    }

    fn decode(rd: &mut Rd, len: usize, epoch: u32) -> Result<Self, StreamError> {
        if len < 5 {
            return malformed("event of too few fields");
        }
        let tick = rd.uint()? as u32;
        let kind = rd.uint()? as u8;
        let kind = match kind {
            EVENT_LAP_TIMING => {
                let n = rd.array()?;
                if n < 8 {
                    return malformed("lap timing of too few fields");
                }
                let t = LapTimingData {
                    car_index: rd.uint()? as u8,
                    lap: rd.uint()? as u16,
                    sector: rd.uint()? as u8,
                    sector_time_ms: rd.uint()? as u32,
                    lap_time_ms: rd.uint()? as u32,
                    is_lap_end: rd.bool()?,
                    valid: rd.bool()?,
                    flags: rd.uint()? as u8,
                };
                rd.skip_rest(n, 8)?;
                EventKind::LapTiming(t)
            }
            EVENT_TRACK_SECTORS => {
                let n = rd.array()?;
                if n < 2 {
                    return malformed("track sectors of too few fields");
                }
                let track_length_m = rd.f32()?;
                let count = rd.array()?;
                let boundaries_m = (0..count)
                    .map(|_| rd.f32())
                    .collect::<Result<Vec<_>, _>>()?;
                rd.skip_rest(n, 2)?;
                EventKind::TrackSectors {
                    track_length_m,
                    boundaries_m,
                }
            }
            EVENT_SESSION_STATE => {
                let n = rd.array()?;
                if n < 1 {
                    return malformed("session state of no fields");
                }
                let state = match rd.uint()? {
                    1 => SessionState::Countdown,
                    2 => SessionState::Racing,
                    3 => SessionState::Finished,
                    _ => SessionState::Lobby,
                };
                rd.skip_rest(n, 1)?;
                EventKind::SessionState(state)
            }
            EVENT_FINISH | EVENT_RETIRED | EVENT_PIT_STOP => {
                let n = rd.array()?;
                if n < 2 {
                    return malformed("event of too few fields");
                }
                let car_index = rd.uint()? as u8;
                let value = rd.uint()? as u8;
                rd.skip_rest(n, 2)?;
                match kind {
                    EVENT_FINISH => EventKind::Finish {
                        car_index,
                        position: value,
                    },
                    EVENT_RETIRED => EventKind::Retired {
                        car_index,
                        reason: value,
                    },
                    _ => EventKind::PitStop {
                        car_index,
                        phase: value,
                    },
                }
            }
            EVENT_CONTACT => {
                let n = rd.array()?;
                if n < 3 {
                    return malformed("contact of too few fields");
                }
                let event = EventKind::Contact {
                    car_index: rd.uint()? as u8,
                    other: rd.uint()? as u8,
                    speed_cms: rd.uint()? as u16,
                };
                rd.skip_rest(n, 3)?;
                event
            }
            other => {
                skip_value(&mut rd.0)?;
                EventKind::Unknown(other)
            }
        };
        rd.skip_rest(len, 5)?;
        Ok(Self { epoch, tick, kind })
    }
}

// --- Path ---------------------------------------------------------------------

/// The track's centerline every [`PATH_SPACING_M`]: where a broadcast
/// camera stands, for a viewer with no lobby to ask (a file played offline).
#[derive(Debug, Clone, PartialEq)]
pub struct StreamPath {
    pub epoch: u32,
    pub spacing_m: f32,
    /// `[x, y]` in metres, server frame.
    pub points: Vec<[f32; 2]>,
}

impl StreamPath {
    pub fn from_centerline(centerline: &[TrackPoint], epoch: u32) -> Self {
        let mut points = Vec::new();
        let mut last = f32::NEG_INFINITY;
        for p in centerline {
            if p.distance_from_start_m - last >= PATH_SPACING_M {
                points.push([p.x, p.y]);
                last = p.distance_from_start_m;
            }
        }
        Self {
            epoch,
            spacing_m: PATH_SPACING_M,
            points,
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(self.points.len() * 8);
        for [x, y] in &self.points {
            bytes.extend_from_slice(&metres_to_mm(*x).to_le_bytes());
            bytes.extend_from_slice(&metres_to_mm(*y).to_le_bytes());
        }
        let mut out = Vec::with_capacity(bytes.len() + 16);
        begin(&mut out, 4, RECORD_PATH, self.epoch);
        wr::f32(&mut out, self.spacing_m);
        wr::bin(&mut out, &bytes);
        out
    }

    fn decode(rd: &mut Rd, len: usize, epoch: u32) -> Result<Self, StreamError> {
        if len < 4 {
            return malformed("path of too few fields");
        }
        let spacing_m = rd.f32()?;
        let points = rd
            .bin()?
            .as_chunks::<8>()
            .0
            .iter()
            .map(|c| {
                [
                    i32::from_le_bytes([c[0], c[1], c[2], c[3]]) as f32 / 1000.0,
                    i32::from_le_bytes([c[4], c[5], c[6], c[7]]) as f32 / 1000.0,
                ]
            })
            .collect();
        rd.skip_rest(len, 4)?;
        Ok(Self {
            epoch,
            spacing_m,
            points,
        })
    }
}

// --- File-only records ------------------------------------------------------------

/// About a second of framed records, zlib-compressed.
#[derive(Debug, Clone, PartialEq)]
pub struct Block {
    pub first_tick: u32,
    pub last_tick: u32,
    pub records: u32,
    /// Bytes once inflated.
    pub raw_len: u32,
    pub zlib: Vec<u8>,
}

impl Block {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.zlib.len() + 24);
        wr::array(&mut out, 6);
        wr::uint(&mut out, RECORD_BLOCK as u64);
        wr::uint(&mut out, self.first_tick as u64);
        wr::uint(&mut out, self.last_tick as u64);
        wr::uint(&mut out, self.records as u64);
        wr::uint(&mut out, self.raw_len as u64);
        wr::bin(&mut out, &self.zlib);
        out
    }

    fn decode(rd: &mut Rd, len: usize) -> Result<Self, StreamError> {
        if len < 6 {
            return malformed("block of too few fields");
        }
        let block = Self {
            first_tick: rd.uint()? as u32,
            last_tick: rd.uint()? as u32,
            records: rd.uint()? as u32,
            raw_len: rd.uint()? as u32,
            zlib: rd.bin()?.to_vec(),
        };
        rd.skip_rest(len, 6)?;
        Ok(block)
    }

    /// The record bodies inside, in order.
    pub fn bodies(&self) -> Result<Vec<Vec<u8>>, StreamError> {
        // A corrupt length must not allocate the moon.
        let cap = (self.raw_len as usize).min(64 << 20);
        let mut raw = Vec::with_capacity(cap);
        flate2::read::ZlibDecoder::new(&self.zlib[..])
            .take(64 << 20)
            .read_to_end(&mut raw)?;
        split_framed(&raw)
    }
}

/// Split a run of `[u32 length][body]` records.
pub fn split_framed(mut raw: &[u8]) -> Result<Vec<Vec<u8>>, StreamError> {
    let mut bodies = Vec::new();
    while !raw.is_empty() {
        if raw.len() < 4 {
            return malformed("truncated record length");
        }
        let len = u32::from_be_bytes([raw[0], raw[1], raw[2], raw[3]]) as usize;
        if raw.len() < 4 + len {
            return malformed("truncated record");
        }
        bodies.push(raw[4..4 + len].to_vec());
        raw = &raw[4 + len..];
    }
    Ok(bodies)
}

/// Where one block starts, so a reader can seek to a tick.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct IndexEntry {
    pub first_tick: u32,
    /// Byte offset of the block's length prefix from the start of the file.
    pub offset: u64,
    pub records: u32,
}

/// The file's last record: one entry per block. Every frame is a keyframe
/// in version 1, so a block's first tick is all a seek needs.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Index {
    pub blocks: Vec<IndexEntry>,
}

impl Index {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(8 + self.blocks.len() * 16);
        wr::array(&mut out, 2);
        wr::uint(&mut out, RECORD_INDEX as u64);
        wr::array(&mut out, self.blocks.len());
        for b in &self.blocks {
            wr::array(&mut out, 3);
            wr::uint(&mut out, b.first_tick as u64);
            wr::uint(&mut out, b.offset);
            wr::uint(&mut out, b.records as u64);
        }
        out
    }

    fn decode(rd: &mut Rd, len: usize) -> Result<Self, StreamError> {
        if len < 2 {
            return malformed("index of too few fields");
        }
        let count = rd.array()?;
        let mut blocks = Vec::with_capacity(count.min(1 << 16));
        for _ in 0..count {
            let n = rd.array()?;
            if n < 3 {
                return malformed("index entry of too few fields");
            }
            blocks.push(IndexEntry {
                first_tick: rd.uint()? as u32,
                offset: rd.uint()?,
                records: rd.uint()? as u32,
            });
            rd.skip_rest(n, 3)?;
        }
        rd.skip_rest(len, 2)?;
        Ok(Self { blocks })
    }
}

// --- Any record ---------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub enum Record {
    Header(StreamHeader),
    Roster(StreamRoster),
    Frame(StreamFrame),
    Event(StreamEvent),
    Block(Block),
    Index(Index),
    Path(StreamPath),
    /// A record type this build does not know.
    Unknown(u8),
}

impl Record {
    pub fn decode(body: &[u8]) -> Result<Record, StreamError> {
        let mut rd = Rd(body);
        let len = rd.array()?;
        if len < 2 {
            return malformed("record of too few fields");
        }
        let kind = rd.uint()? as u8;
        match kind {
            RECORD_BLOCK => Ok(Record::Block(Block::decode(&mut rd, len)?)),
            RECORD_INDEX => Ok(Record::Index(Index::decode(&mut rd, len)?)),
            RECORD_HEADER | RECORD_ROSTER | RECORD_FRAME | RECORD_EVENT | RECORD_PATH => {
                let epoch = rd.uint()? as u32;
                Ok(match kind {
                    RECORD_HEADER => Record::Header(StreamHeader::decode(&mut rd, len, epoch)?),
                    RECORD_ROSTER => Record::Roster(StreamRoster::decode(&mut rd, len, epoch)?),
                    RECORD_FRAME => Record::Frame(StreamFrame::decode(&mut rd, len, epoch)?),
                    RECORD_EVENT => Record::Event(StreamEvent::decode(&mut rd, len, epoch)?),
                    _ => Record::Path(StreamPath::decode(&mut rd, len, epoch)?),
                })
            }
            other => Ok(Record::Unknown(other)),
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        match self {
            Record::Header(r) => r.encode(),
            Record::Roster(r) => r.encode(),
            Record::Frame(r) => r.encode(),
            Record::Event(r) => r.encode(),
            Record::Block(r) => r.encode(),
            Record::Index(r) => r.encode(),
            Record::Path(r) => r.encode(),
            Record::Unknown(kind) => {
                let mut out = Vec::new();
                wr::array(&mut out, 2);
                wr::uint(&mut out, *kind as u64);
                wr::nil(&mut out);
                out
            }
        }
    }
}

// --- The file -------------------------------------------------------------------

/// A whole stream, ready to be written as a file: the plain preamble and
/// the timed records after it.
#[derive(Debug, Clone)]
pub struct StreamContent {
    pub header: StreamHeader,
    pub roster: StreamRoster,
    pub path: Option<StreamPath>,
    /// Events a viewer needs before the first frame (`TrackSectors`).
    pub preamble: Vec<StreamEvent>,
    /// `(tick, body)` of every frame, event and roster change, in order.
    pub records: Vec<(u32, Vec<u8>)>,
}

impl StreamContent {
    /// The file's bytes:
    ///
    /// ```text
    /// "APXS", u16 file version (big-endian)
    /// Header, Roster, [Path], preamble events     (plain records)
    /// Block, Block, ...                           (zlib, ~1 s each)
    /// Index                                       (plain)
    /// u64 offset of the Index record (big-endian)
    /// ```
    ///
    /// The same content gives the same bytes: nothing here reads a clock.
    pub fn to_bytes(&self) -> Result<Vec<u8>, StreamError> {
        let mut out = Vec::new();
        out.extend_from_slice(FILE_MAGIC);
        out.extend_from_slice(&FILE_VERSION.to_be_bytes());
        out.extend_from_slice(&framed(&self.header.encode()));
        out.extend_from_slice(&framed(&self.roster.encode()));
        if let Some(path) = &self.path {
            out.extend_from_slice(&framed(&path.encode()));
        }
        for event in &self.preamble {
            out.extend_from_slice(&framed(&event.encode()));
        }

        let block_ticks = self.header.tick_rate.max(1) as u32;
        let mut index = Index::default();
        let mut i = 0;
        while i < self.records.len() {
            let first_tick = self.records[i].0;
            let mut raw = Vec::new();
            let mut count = 0u32;
            let mut last_tick = first_tick;
            while i < self.records.len() && self.records[i].0 < first_tick + block_ticks {
                raw.extend_from_slice(&framed(&self.records[i].1));
                last_tick = self.records[i].0;
                count += 1;
                i += 1;
            }
            let mut encoder =
                flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(6));
            encoder.write_all(&raw)?;
            let block = Block {
                first_tick,
                last_tick,
                records: count,
                raw_len: raw.len() as u32,
                zlib: encoder.finish()?,
            };
            index.blocks.push(IndexEntry {
                first_tick,
                offset: out.len() as u64,
                records: count,
            });
            out.extend_from_slice(&framed(&block.encode()));
        }
        let index_offset = out.len() as u64;
        out.extend_from_slice(&framed(&index.encode()));
        out.extend_from_slice(&index_offset.to_be_bytes());
        Ok(out)
    }

    pub fn write_file(&self, path: &std::path::Path) -> Result<(), StreamError> {
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::write(path, self.to_bytes()?)?;
        Ok(())
    }
}

/// An `.apxs` file held in memory: the preamble decoded, the blocks still
/// compressed until asked for.
#[derive(Debug, Clone)]
pub struct StreamFile {
    pub header: StreamHeader,
    pub roster: StreamRoster,
    pub path: Option<StreamPath>,
    pub preamble: Vec<StreamEvent>,
    pub index: Index,
    bytes: Vec<u8>,
}

impl StreamFile {
    pub fn open(path: &std::path::Path) -> Result<Self, StreamError> {
        Self::from_bytes(std::fs::read(path)?)
    }

    /// Whether a file starts like a stream (the magic), without reading it.
    pub fn is_stream_file(path: &std::path::Path) -> bool {
        let mut magic = [0u8; 4];
        std::fs::File::open(path)
            .and_then(|mut f| f.read_exact(&mut magic))
            .is_ok()
            && &magic == FILE_MAGIC
    }

    pub fn from_bytes(bytes: Vec<u8>) -> Result<Self, StreamError> {
        if bytes.len() < 6 + 8 || &bytes[..4] != FILE_MAGIC {
            return malformed("no APXS magic");
        }
        let version = u16::from_be_bytes([bytes[4], bytes[5]]);
        if version != FILE_VERSION {
            return Err(StreamError::Version {
                what: "file",
                found: version as u32,
                supported: FILE_VERSION as u32,
            });
        }
        let trailer = bytes.len() - 8;
        let index_offset = u64::from_be_bytes(bytes[trailer..].try_into().unwrap()) as usize;
        let read_at = |offset: usize| -> Result<(&[u8], usize), StreamError> {
            if offset + 4 > trailer {
                return malformed("record past the end of the file");
            }
            let len = u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap()) as usize;
            if offset + 4 + len > trailer {
                return malformed("record past the end of the file");
            }
            Ok((&bytes[offset + 4..offset + 4 + len], offset + 4 + len))
        };

        let Record::Index(index) = Record::decode(read_at(index_offset)?.0)? else {
            return malformed("the trailer does not point at the index");
        };

        let mut header = None;
        let mut roster = None;
        let mut path = None;
        let mut preamble = Vec::new();
        let mut offset = 6;
        while offset < index_offset {
            let (body, next) = read_at(offset)?;
            if record_type(body) == Some(RECORD_BLOCK) {
                break;
            }
            match Record::decode(body)? {
                Record::Header(h) => header = Some(h),
                Record::Roster(r) => roster = Some(r),
                Record::Path(p) => path = Some(p),
                Record::Event(e) => preamble.push(e),
                _ => {}
            }
            offset = next;
        }
        let header = header.ok_or_else(|| StreamError::Malformed("no header".into()))?;
        let roster = roster.ok_or_else(|| StreamError::Malformed("no roster".into()))?;
        Ok(Self {
            header,
            roster,
            path,
            preamble,
            index,
            bytes,
        })
    }

    pub fn block_count(&self) -> usize {
        self.index.blocks.len()
    }

    /// The record bodies of one block.
    pub fn block(&self, i: usize) -> Result<Vec<Vec<u8>>, StreamError> {
        let entry = self
            .index
            .blocks
            .get(i)
            .ok_or_else(|| StreamError::Malformed(format!("no block {i}")))?;
        let offset = entry.offset as usize;
        if offset + 4 > self.bytes.len() {
            return malformed("block past the end of the file");
        }
        let len = u32::from_be_bytes(self.bytes[offset..offset + 4].try_into().unwrap()) as usize;
        if offset + 4 + len > self.bytes.len() {
            return malformed("block past the end of the file");
        }
        match Record::decode(&self.bytes[offset + 4..offset + 4 + len])? {
            Record::Block(block) => block.bodies(),
            _ => malformed("the index does not point at a block"),
        }
    }

    /// The block holding `tick` (the last whose first tick is not after it).
    pub fn block_for_tick(&self, tick: u32) -> usize {
        self.index
            .blocks
            .partition_point(|b| b.first_tick <= tick)
            .saturating_sub(1)
    }

    /// Every timed record body, in order: the whole file inflated.
    pub fn records(&self) -> Result<Vec<Vec<u8>>, StreamError> {
        let mut all = Vec::new();
        for i in 0..self.block_count() {
            all.extend(self.block(i)?);
        }
        Ok(all)
    }

    pub fn file_len(&self) -> usize {
        self.bytes.len()
    }
}

// --- The encoder ------------------------------------------------------------------

/// What the encoder remembers of a car between two looks at it, to tell an
/// event from a state.
#[derive(Debug, Clone, Copy, Default)]
struct CarEdge {
    finished: bool,
    retired: bool,
    in_lane: bool,
    servicing: bool,
    colliding: bool,
}

/// Session to records: the one encoder the render tool writes files with
/// and a live session will be streamed through.
///
/// Call [`BroadcastEncoder::observe`] once per simulated tick (it turns
/// state changes into events) and [`BroadcastEncoder::frame`] on the ticks
/// a frame is due.
#[derive(Debug, Clone)]
pub struct BroadcastEncoder {
    pub epoch: u32,
    pub roster_revision: u16,
    state: Option<SessionState>,
    cars: std::collections::BTreeMap<PlayerId, CarEdge>,
}

impl BroadcastEncoder {
    pub fn new(epoch: u32) -> Self {
        Self {
            epoch,
            roster_revision: 0,
            state: None,
            cars: std::collections::BTreeMap::new(),
        }
    }

    /// The header of a session's stream. The content's ticks, the frame
    /// rate and how it was rendered are the caller's to fill in.
    pub fn header(&self, session: &GameSession, track_stem: &str) -> StreamHeader {
        StreamHeader {
            epoch: self.epoch,
            version: FORMAT_VERSION,
            stream_id: session.session.id,
            tick_rate: session.tick_rate_hz(),
            frame_rate: session.tick_rate_hz(),
            row_size: ROW_SIZE as u8,
            track: StreamTrack {
                track_id: session.track_config.id,
                stem: track_stem.to_string(),
                display_name: session.track_config.name.clone(),
                source_crc: session.track_config.content_crc,
                length_m: session.track_length_m(),
            },
            conditions: session.session.conditions,
            session_kind: session.session.session_kind,
            game_mode: session.session.game_mode,
            lap_limit: session.session.lap_limit,
            race_start_tick: session.session.race_start_tick,
            start_tick: session.session.current_tick,
            end_tick: session.session.current_tick,
            render: StreamRender::default(),
        }
    }

    pub fn roster(&self, session: &GameSession) -> StreamRoster {
        StreamRoster::from_session(session, self.epoch, self.roster_revision)
    }

    pub fn path(&self, session: &GameSession) -> StreamPath {
        StreamPath::from_centerline(&session.track_config.centerline, self.epoch)
    }

    pub fn sectors(&self, session: &GameSession) -> StreamEvent {
        StreamEvent {
            epoch: self.epoch,
            tick: session.session.current_tick,
            kind: EventKind::TrackSectors {
                track_length_m: session.track_length_m(),
                boundaries_m: session.sector_boundaries_m(),
            },
        }
    }

    /// What changed since the last call, as events; `lap_events` are the
    /// timing lines the session crossed this tick (drained by the caller).
    pub fn observe(
        &mut self,
        session: &GameSession,
        lap_events: &[crate::game_session::SessionLapEvent],
    ) -> Vec<StreamEvent> {
        let tick = session.session.current_tick;
        let epoch = self.epoch;
        let mut out = Vec::new();
        let mut push = |kind: EventKind| out.push(StreamEvent { epoch, tick, kind });

        if self.state != Some(session.session.state) {
            if self.state.is_some() {
                push(EventKind::SessionState(session.session.state));
            }
            self.state = Some(session.session.state);
        }

        for lap in lap_events {
            let Some(car_index) = session.car_index_of(&lap.player_id) else {
                continue;
            };
            let mut flags = 0u8;
            if lap.event.personal_best_lap {
                flags |= LapTimingData::FLAG_PERSONAL_BEST_LAP;
            }
            if lap.session_best_lap {
                flags |= LapTimingData::FLAG_SESSION_BEST_LAP;
            }
            if lap.event.personal_best_sector {
                flags |= LapTimingData::FLAG_PERSONAL_BEST_SECTOR;
            }
            if lap.session_best_sector {
                flags |= LapTimingData::FLAG_SESSION_BEST_SECTOR;
            }
            push(EventKind::LapTiming(LapTimingData {
                car_index,
                lap: lap.event.lap,
                sector: lap.event.sector,
                sector_time_ms: lap.event.sector_time_ms,
                lap_time_ms: lap.event.lap_time_ms.unwrap_or(0),
                is_lap_end: lap.event.lap_time_ms.is_some(),
                valid: lap.event.valid,
                flags,
            }));
        }

        for (index, (player_id, car)) in session.session.participants.iter().enumerate() {
            let car_index = index as u8;
            let now = CarEdge {
                finished: car.finish_position.is_some(),
                retired: !car.damage.is_drivable,
                in_lane: car.pit.in_lane,
                servicing: car.pit.servicing,
                colliding: car.is_colliding,
            };
            // A car first seen starts from what it is, not from nothing: a
            // stream that begins mid-race has no events to invent.
            let was = self.cars.insert(*player_id, now).unwrap_or(now);
            if now.finished && !was.finished {
                push(EventKind::Finish {
                    car_index,
                    position: car.finish_position.unwrap_or(0),
                });
            }
            if now.retired && !was.retired {
                push(EventKind::Retired {
                    car_index,
                    reason: 1,
                });
            }
            if now.in_lane && !was.in_lane {
                push(EventKind::PitStop {
                    car_index,
                    phase: PIT_ENTERED,
                });
            }
            if was.servicing && !now.servicing {
                push(EventKind::PitStop {
                    car_index,
                    phase: PIT_SERVICED,
                });
            }
            if was.in_lane && !now.in_lane {
                push(EventKind::PitStop {
                    car_index,
                    phase: PIT_LEFT,
                });
            }
            if now.colliding && !was.colliding {
                push(EventKind::Contact {
                    car_index,
                    other: CONTACT_UNKNOWN,
                    speed_cms: (car.speed_mps.abs() * 100.0).round().min(u16::MAX as f32) as u16,
                });
            }
        }
        out
    }

    /// Every car's row, in car-index order.
    pub fn rows(session: &GameSession) -> Vec<CarRow> {
        session
            .session
            .participants
            .values()
            .enumerate()
            .map(|(i, state)| CarRow::from_car_state(state, i as u8))
            .collect()
    }

    /// This tick's frame, as one record body per part.
    pub fn frame(&self, session: &GameSession) -> Vec<Vec<u8>> {
        self.frame_of(session, &Self::rows(session))
    }

    /// [`Self::frame`] from rows already taken.
    pub fn frame_of(&self, session: &GameSession, rows: &[CarRow]) -> Vec<Vec<u8>> {
        let countdown_ms = session
            .session
            .countdown_ticks_remaining
            .map(|ticks| ((ticks as f32 / session.tick_rate_hz() as f32) * 1000.0) as u16);
        encode_frames(
            self.epoch,
            session.session.current_tick,
            self.roster_revision,
            session.session.state,
            countdown_ms,
            rows,
        )
    }
}

// --- Scoring a rendered race ---------------------------------------------------------

/// How good a backdrop a race makes: what the AI survey and `find` measure,
/// folded into one number so the best of several seeds can be kept.
#[derive(Debug, Clone, Copy, PartialEq, Default, serde::Serialize)]
pub struct RaceScore {
    pub retirements: u32,
    /// Car-seconds in contact with a car or a wall.
    pub contact_s: f32,
    /// Car-seconds off the track.
    pub off_road_s: f32,
    /// Car-seconds spent within a second of the car ahead on the road.
    pub close_s: f32,
}

impl RaceScore {
    /// Higher is better: racing close counts for, everything else against.
    pub fn score(&self) -> f32 {
        self.close_s - 2.0 * self.contact_s - self.off_road_s - 60.0 * self.retirements as f32
    }
}

/// Accumulates a [`RaceScore`] from the frames of a race.
#[derive(Debug, Clone, Default)]
pub struct ScoreKeeper {
    score: RaceScore,
    retired: std::collections::BTreeSet<u8>,
}

impl ScoreKeeper {
    /// One frame's rows, `dt_s` after the one before, on a lap of `lap_m`.
    pub fn frame(&mut self, rows: &[CarRow], racing: bool, dt_s: f32, lap_m: f32) {
        for row in rows {
            if row.status & STATUS_RETIRED != 0 {
                self.retired.insert(row.car_index);
            }
        }
        if !racing {
            return;
        }
        let running: Vec<&CarRow> = rows
            .iter()
            .filter(|r| r.status & (STATUS_RETIRED | STATUS_FINISHED | STATUS_IN_GARAGE) == 0)
            .collect();
        for row in &running {
            if row.status & STATUS_COLLIDING != 0 {
                self.score.contact_s += dt_s;
            }
            if row.status & STATUS_ON_TRACK == 0 {
                self.score.off_road_s += dt_s;
            }
            // The nearest car ahead on the road, whatever its lap: what a
            // camera sees as two cars together.
            let speed = row.speed_mps().max(10.0);
            let close = running.iter().any(|other| {
                if other.car_index == row.car_index {
                    return false;
                }
                let mut gap = other.station_m() - row.station_m();
                if lap_m > 0.0 {
                    gap = gap.rem_euclid(lap_m);
                }
                gap > 0.0 && gap / speed < 1.0
            });
            if close {
                self.score.close_s += dt_s;
            }
        }
    }

    pub fn finish(mut self) -> RaceScore {
        self.score.retirements = self.retired.len() as u32;
        self.score
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(bytes: &[u8]) -> String {
        bytes
            .chunks(16)
            .map(|line| {
                line.iter()
                    .map(|b| format!("0x{:02X}", b))
                    .collect::<Vec<_>>()
                    .join(", ")
            })
            .collect::<Vec<_>>()
            .join(",\n\t\t")
    }

    fn sample_header() -> StreamHeader {
        StreamHeader {
            epoch: 3,
            version: FORMAT_VERSION,
            stream_id: uuid::Uuid::parse_str("01234567-89ab-cdef-0123-456789abcdef").unwrap(),
            tick_rate: 240,
            frame_rate: 30,
            row_size: ROW_SIZE as u8,
            track: StreamTrack {
                track_id: uuid::Uuid::parse_str("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").unwrap(),
                stem: "Zandvoort".into(),
                display_name: "Zandervoort".into(),
                source_crc: 0xCBF4_3926,
                length_m: 4259.0,
            },
            conditions: SessionConditions {
                weather: Weather::LightRain,
                time_of_day_minutes: 21 * 60 + 30,
                air_temp_c: Some(-3),
                humidity_pct: Some(85),
                wind_kph: Some(22),
                wind_from_deg: Some(270),
                time_scale: None,
                changeable: None,
                track_rubber_pct: None,
            },
            session_kind: SessionKind::Multiplayer,
            game_mode: GameMode::Race,
            lap_limit: 2,
            race_start_tick: Some(1920),
            start_tick: 8,
            end_tick: 52_800,
            render: StreamRender {
                seed: Some(7),
                score: Some(123.5),
            },
        }
    }

    fn sample_roster() -> StreamRoster {
        StreamRoster {
            epoch: 3,
            revision: 1,
            entries: vec![
                StreamRosterEntry {
                    car_index: 0,
                    car_config_id: uuid::Uuid::parse_str("11111111-2222-3333-4444-555555555555")
                        .unwrap(),
                    content_crc: 0x1234_5678,
                    livery: 2,
                    name: "A. Driver".into(),
                    is_ai: true,
                },
                StreamRosterEntry {
                    car_index: 1,
                    car_config_id: uuid::Uuid::parse_str("66666666-7777-8888-9999-aaaaaaaaaaaa")
                        .unwrap(),
                    content_crc: 0,
                    livery: 0,
                    name: "Player".into(),
                    is_ai: false,
                },
            ],
        }
    }

    fn sample_row(car_index: u8) -> CarRow {
        CarRow {
            car_index,
            status: STATUS_ON_TRACK | STATUS_COLLIDING,
            x_mm: 100_500,
            y_mm: -20_250,
            z_mm: 500,
            yaw: yaw_to_u16(1.5),
            pitch: angle_to_i16(0.02),
            roll: angle_to_i16(-0.25),
            speed_cms: 4200,
            steering: -64,
            throttle: 255,
            brake: 0,
            gear: 4,
            engine_rpm: 11_000,
            lap: 3,
            station_cm: 123_456,
            finish_position: 0,
            lap_flags: 0x29,
            pit_flags: 5,
            compound: 1,
            damage: [12, 0, 3, 0, 40],
            ers_flags: 0x05,
            tyre_wear: [14, 16, 9, 11],
            tyre_c: [88, 91, 95, 97],
        }
    }

    #[test]
    fn rows_round_trip_at_their_offsets() {
        let row = sample_row(7);
        let mut bytes = Vec::new();
        row.write(&mut bytes);
        assert_eq!(bytes.len(), ROW_SIZE);
        assert_eq!(CarRow::read(&bytes), Some(row));
        // The layout the documentation promises.
        assert_eq!(bytes[0], 7);
        assert_eq!(i32::from_le_bytes(bytes[2..6].try_into().unwrap()), 100_500);
        assert_eq!(u16::from_le_bytes(bytes[20..22].try_into().unwrap()), 4200);
        assert_eq!(bytes[25], 4);
        assert_eq!(
            u32::from_le_bytes(bytes[30..34].try_into().unwrap()),
            123_456
        );
        assert_eq!(&bytes[38..43], &[12, 0, 3, 0, 40]);
        assert_eq!(bytes[43], 0x05);
        assert_eq!(&bytes[44..48], &[14, 16, 9, 11]);
        assert_eq!(&bytes[48..52], &[88, 91, 95, 97]);
        // A longer row from a newer writer still reads.
        bytes.extend_from_slice(&[9, 9, 9, 9]);
        assert_eq!(CarRow::read(&bytes), Some(row));
        assert!(CarRow::read(&bytes[..40]).is_none());
    }

    #[test]
    fn a_version_1_row_reads_with_its_tyres_unknown() {
        let row = sample_row(3);
        let mut bytes = Vec::new();
        row.write(&mut bytes);
        let old = CarRow::read(&bytes[..ROW_SIZE_V1]).unwrap();
        assert_eq!(old.tyre_wear, [TYRE_WEAR_UNKNOWN; 4]);
        assert_eq!(old.tyre_c, [0; 4]);
        assert_eq!(
            CarRow {
                tyre_wear: row.tyre_wear,
                tyre_c: row.tyre_c,
                ..old
            },
            row
        );
        // A v1 stream's frame: its header says 44, and every row reads.
        let mut rows = Vec::new();
        for i in 0..3u8 {
            let mut one = Vec::new();
            sample_row(i).write(&mut one);
            rows.extend_from_slice(&one[..ROW_SIZE_V1]);
        }
        let frame = StreamFrame {
            epoch: 0,
            tick: 8,
            roster_revision: 0,
            state: SessionState::Racing,
            countdown_ms: NO_COUNTDOWN,
            part: 0,
            parts: 1,
            rows,
        };
        let read: Vec<u8> = frame.cars(ROW_SIZE_V1).map(|r| r.car_index).collect();
        assert_eq!(read, vec![0, 1, 2]);
    }

    #[test]
    fn angles_wrap_and_clamp() {
        assert_eq!(yaw_to_u16(0.0), 0);
        assert_eq!(yaw_to_u16(std::f32::consts::PI), 32768);
        assert_eq!(yaw_to_u16(-std::f32::consts::FRAC_PI_2), 49152);
        // A hair under a full turn rounds onto zero, not past the end.
        assert_eq!(yaw_to_u16(std::f32::consts::TAU - 1e-7), 0);
        assert_eq!(angle_to_i16(std::f32::consts::PI), i16::MAX);
        assert_eq!(angle_to_i16(-std::f32::consts::PI), i16::MIN);
        let row = CarRow {
            yaw: yaw_to_u16(-3.0),
            ..sample_row(0)
        };
        let back = row.yaw_rad() - std::f32::consts::TAU;
        assert!((back + 3.0).abs() < 1e-3, "{back}");
    }

    #[test]
    fn every_record_round_trips() {
        let header = sample_header();
        assert_eq!(
            Record::decode(&header.encode()).unwrap(),
            Record::Header(header.clone())
        );
        let bare = StreamHeader {
            conditions: SessionConditions::DEFAULT,
            race_start_tick: None,
            render: StreamRender::default(),
            ..header
        };
        assert_eq!(
            Record::decode(&bare.encode()).unwrap(),
            Record::Header(bare)
        );

        let roster = sample_roster();
        assert_eq!(
            Record::decode(&roster.encode()).unwrap(),
            Record::Roster(roster)
        );

        let frames = encode_frames(
            3,
            4808,
            1,
            SessionState::Racing,
            None,
            &[sample_row(0), sample_row(1)],
        );
        assert_eq!(frames.len(), 1);
        let Record::Frame(frame) = Record::decode(&frames[0]).unwrap() else {
            panic!("not a frame");
        };
        assert_eq!((frame.tick, frame.epoch, frame.parts), (4808, 3, 1));
        assert_eq!(frame.countdown_ms, NO_COUNTDOWN);
        assert_eq!(
            frame.cars(ROW_SIZE).collect::<Vec<_>>(),
            vec![sample_row(0), sample_row(1)]
        );

        for kind in [
            EventKind::LapTiming(LapTimingData {
                car_index: 2,
                lap: 3,
                sector: 2,
                sector_time_ms: 28_114,
                lap_time_ms: 82_615,
                is_lap_end: true,
                valid: true,
                flags: 5,
            }),
            EventKind::TrackSectors {
                track_length_m: 4259.0,
                boundaries_m: vec![1400.0, 2900.0],
            },
            EventKind::SessionState(SessionState::Racing),
            EventKind::Finish {
                car_index: 4,
                position: 1,
            },
            EventKind::Retired {
                car_index: 5,
                reason: 1,
            },
            EventKind::PitStop {
                car_index: 6,
                phase: PIT_SERVICED,
            },
            EventKind::Contact {
                car_index: 7,
                other: CONTACT_UNKNOWN,
                speed_cms: 5100,
            },
        ] {
            let event = StreamEvent {
                epoch: 3,
                tick: 9000,
                kind,
            };
            assert_eq!(
                Record::decode(&event.encode()).unwrap(),
                Record::Event(event)
            );
        }

        let path = StreamPath {
            epoch: 3,
            spacing_m: 10.0,
            points: vec![[0.0, 0.0], [10.0, 0.5], [-19.75, 3.25]],
        };
        assert_eq!(Record::decode(&path.encode()).unwrap(), Record::Path(path));
    }

    #[test]
    fn the_epoch_is_patched_in_place() {
        let mut frame = encode_frames(0, 4808, 0, SessionState::Racing, Some(1500), &[])
            .pop()
            .unwrap();
        assert_eq!(record_type(&frame), Some(RECORD_FRAME));
        assert_eq!(record_epoch(&frame), Some(0));
        assert_eq!(record_tick(&frame), Some(4808));
        assert!(patch_epoch(&mut frame, 0xDEAD_BEEF));
        let Record::Frame(decoded) = Record::decode(&frame).unwrap() else {
            panic!("not a frame");
        };
        assert_eq!(decoded.epoch, 0xDEAD_BEEF);
        assert_eq!(decoded.tick, 4808);
        assert_eq!(decoded.countdown_ms, 1500);

        // Every viewer-facing record keeps its epoch at the same place.
        for mut body in [
            sample_header().encode(),
            sample_roster().encode(),
            StreamEvent {
                epoch: 0,
                tick: 1,
                kind: EventKind::SessionState(SessionState::Finished),
            }
            .encode(),
            StreamPath {
                epoch: 0,
                spacing_m: 10.0,
                points: vec![],
            }
            .encode(),
        ] {
            assert!(patch_epoch(&mut body, 77));
            assert_eq!(record_epoch(&body), Some(77));
        }
        // A block has none.
        let mut block = Block {
            first_tick: 0,
            last_tick: 0,
            records: 0,
            raw_len: 0,
            zlib: vec![],
        }
        .encode();
        assert!(!patch_epoch(&mut block, 1));
    }

    #[test]
    fn a_big_field_is_sent_in_parts() {
        let rows: Vec<CarRow> = (0..40).map(sample_row).collect();
        let parts = encode_frames(1, 240, 0, SessionState::Racing, None, &rows);
        assert_eq!(parts.len(), 2);
        let mut seen = Vec::new();
        for (i, body) in parts.iter().enumerate() {
            assert!(body.len() <= 1400, "{} bytes", body.len());
            let Record::Frame(frame) = Record::decode(body).unwrap() else {
                panic!("not a frame");
            };
            assert_eq!((frame.part as usize, frame.parts), (i, 2));
            assert_eq!(frame.tick, 240);
            seen.extend(frame.cars(ROW_SIZE).map(|r| r.car_index));
        }
        assert_eq!(seen, (0..40).collect::<Vec<u8>>());
        // Twenty cars: one datagram of about 1 060 bytes.
        let twenty = encode_frames(1, 240, 0, SessionState::Racing, None, &rows[..20]);
        assert_eq!(twenty.len(), 1);
        assert!(
            twenty[0].len() < 1080 && twenty[0].len() > 1040,
            "{}",
            twenty[0].len()
        );
    }

    fn sample_content() -> StreamContent {
        let mut records = Vec::new();
        for i in 0..90u32 {
            let tick = 8 + i * 8;
            for body in encode_frames(
                0,
                tick,
                0,
                SessionState::Racing,
                None,
                &[sample_row(0), sample_row(1)],
            ) {
                records.push((tick, body));
            }
            if i == 40 {
                records.push((
                    tick,
                    StreamEvent {
                        epoch: 0,
                        tick,
                        kind: EventKind::Finish {
                            car_index: 1,
                            position: 1,
                        },
                    }
                    .encode(),
                ));
            }
        }
        StreamContent {
            header: StreamHeader {
                epoch: 0,
                start_tick: 8,
                end_tick: 8 + 89 * 8,
                ..sample_header()
            },
            roster: StreamRoster {
                epoch: 0,
                // The frames above carry revision 0: a player drops a frame
                // whose revision is not the roster's.
                revision: 0,
                ..sample_roster()
            },
            path: Some(StreamPath {
                epoch: 0,
                spacing_m: 10.0,
                points: vec![[0.0, 0.0], [10.0, 0.0]],
            }),
            preamble: vec![StreamEvent {
                epoch: 0,
                tick: 8,
                kind: EventKind::TrackSectors {
                    track_length_m: 4259.0,
                    boundaries_m: vec![1400.0, 2900.0],
                },
            }],
            records,
        }
    }

    #[test]
    fn a_file_round_trips_and_seeks() {
        let content = sample_content();
        let bytes = content.to_bytes().unwrap();
        assert_eq!(&bytes[..4], b"APXS");
        // Nothing in the writer reads a clock or a hash map.
        assert_eq!(bytes, content.to_bytes().unwrap());

        let file = StreamFile::from_bytes(bytes).unwrap();
        assert_eq!(file.header, content.header);
        assert_eq!(file.roster, content.roster);
        assert_eq!(file.path, content.path);
        assert_eq!(file.preamble, content.preamble);
        // 90 frames at 30 Hz of a 240 Hz clock: three blocks of a second.
        assert_eq!(file.block_count(), 3);
        assert_eq!(file.index.blocks[0].first_tick, 8);
        assert_eq!(file.index.blocks[1].first_tick, 248);
        let all = file.records().unwrap();
        assert_eq!(all.len(), content.records.len());
        assert!(all
            .iter()
            .zip(&content.records)
            .all(|(read, (_, written))| read == written));
        // Seeking: the block that holds a tick starts at or before it.
        assert_eq!(file.block_for_tick(0), 0);
        assert_eq!(file.block_for_tick(250), 1);
        assert_eq!(file.block_for_tick(100_000), 2);
        let second = file.block(1).unwrap();
        assert_eq!(record_tick(&second[0]), Some(248));
        assert!(second
            .iter()
            .any(|body| record_type(body) == Some(RECORD_EVENT)));

        assert!(StreamFile::from_bytes(b"APXS".to_vec()).is_err());
        assert!(StreamFile::from_bytes(b"nope, not one at all".to_vec()).is_err());
    }

    #[test]
    fn a_newer_writers_extra_fields_are_skipped() {
        // A frame with a tenth element, rows four bytes longer, and an
        // event kind nobody has heard of.
        let mut rows = Vec::new();
        sample_row(0).write(&mut rows);
        rows.extend_from_slice(&[1, 2, 3, 4]);
        let mut body = Vec::new();
        wr::array(&mut body, 10);
        wr::uint(&mut body, RECORD_FRAME as u64);
        wr::u32_fixed(&mut body, 3);
        wr::u32_fixed(&mut body, 16);
        wr::uint(&mut body, 0);
        wr::uint(&mut body, 2);
        wr::uint(&mut body, NO_COUNTDOWN as u64);
        wr::uint(&mut body, 0);
        wr::uint(&mut body, 1);
        wr::bin(&mut body, &rows);
        wr::str(&mut body, "from the future");
        let Record::Frame(frame) = Record::decode(&body).unwrap() else {
            panic!("not a frame");
        };
        assert_eq!(
            frame.cars(ROW_SIZE + 4).collect::<Vec<_>>(),
            vec![sample_row(0)]
        );

        let mut event = Vec::new();
        begin(&mut event, 5, RECORD_EVENT, 3);
        wr::u32_fixed(&mut event, 16);
        wr::uint(&mut event, 99);
        wr::array(&mut event, 2);
        wr::str(&mut event, "x");
        wr::uint(&mut event, 1);
        assert_eq!(
            Record::decode(&event).unwrap(),
            Record::Event(StreamEvent {
                epoch: 3,
                tick: 16,
                kind: EventKind::Unknown(99)
            })
        );

        let mut unknown = Vec::new();
        wr::array(&mut unknown, 2);
        wr::uint(&mut unknown, 42);
        wr::nil(&mut unknown);
        assert_eq!(Record::decode(&unknown).unwrap(), Record::Unknown(42));
    }

    #[test]
    fn close_racing_scores_and_trouble_costs() {
        let mut keeper = ScoreKeeper::default();
        let ahead = CarRow {
            station_cm: 50_000,
            status: STATUS_ON_TRACK,
            ..sample_row(0)
        };
        let behind = CarRow {
            station_cm: 48_000,
            status: STATUS_ON_TRACK,
            ..sample_row(1)
        };
        // 20 m apart at 42 m/s: under a second, for the one behind.
        for _ in 0..30 {
            keeper.frame(&[ahead, behind], true, 1.0 / 30.0, 4000.0);
        }
        let clean = keeper.clone().finish();
        assert!((clean.close_s - 1.0).abs() < 1e-3, "{clean:?}");
        assert_eq!(clean.contact_s, 0.0);
        assert!(clean.score() > 0.0);

        let off = CarRow {
            status: 0,
            ..behind
        };
        let wrecked = CarRow {
            status: STATUS_ON_TRACK | STATUS_RETIRED,
            ..ahead
        };
        for _ in 0..30 {
            keeper.frame(&[wrecked, off], true, 1.0 / 30.0, 4000.0);
        }
        // A countdown frame counts for nothing.
        keeper.frame(&[ahead, behind], false, 10.0, 4000.0);
        let messy = keeper.finish();
        assert_eq!(messy.retirements, 1);
        assert!((messy.off_road_s - 1.0).abs() < 1e-3);
        assert!(messy.score() < clean.score());
    }

    /// The golden bytes the client's codec is tested against:
    /// `cargo test spectator_wire_format -- --nocapture`, pasted into
    /// `ApexSpectatorGoldenBlobs.h`.
    #[test]
    fn spectator_wire_format() {
        let header = sample_header().encode();
        let roster = sample_roster().encode();
        let frame = encode_frames(
            3,
            4808,
            1,
            SessionState::Racing,
            Some(1500),
            &[sample_row(0), sample_row(1)],
        )
        .pop()
        .unwrap();
        let lap = StreamEvent {
            epoch: 3,
            tick: 9000,
            kind: EventKind::LapTiming(LapTimingData {
                car_index: 2,
                lap: 3,
                sector: 2,
                sector_time_ms: 28_114,
                lap_time_ms: 82_615,
                is_lap_end: true,
                valid: true,
                flags: 5,
            }),
        }
        .encode();
        let sectors = StreamEvent {
            epoch: 3,
            tick: 8,
            kind: EventKind::TrackSectors {
                track_length_m: 4259.0,
                boundaries_m: vec![1400.0, 2900.0],
            },
        }
        .encode();
        let contact = StreamEvent {
            epoch: 3,
            tick: 9100,
            kind: EventKind::Contact {
                car_index: 7,
                other: CONTACT_UNKNOWN,
                speed_cms: 5100,
            },
        }
        .encode();
        let path = StreamPath {
            epoch: 3,
            spacing_m: 10.0,
            points: vec![[0.0, 0.0], [10.0, 0.5], [-19.75, 3.25]],
        }
        .encode();
        let file = sample_content().to_bytes().unwrap();

        println!(
            "\tinline constexpr uint8 Header[] = {{\n\t\t{}\n\t}};\n",
            hex(&header)
        );
        println!(
            "\tinline constexpr uint8 Roster[] = {{\n\t\t{}\n\t}};\n",
            hex(&roster)
        );
        println!(
            "\tinline constexpr uint8 Frame[] = {{\n\t\t{}\n\t}};\n",
            hex(&frame)
        );
        println!(
            "\tinline constexpr uint8 EventLapTiming[] = {{\n\t\t{}\n\t}};\n",
            hex(&lap)
        );
        println!(
            "\tinline constexpr uint8 EventTrackSectors[] = {{\n\t\t{}\n\t}};\n",
            hex(&sectors)
        );
        println!(
            "\tinline constexpr uint8 EventContact[] = {{\n\t\t{}\n\t}};\n",
            hex(&contact)
        );
        println!(
            "\tinline constexpr uint8 Path[] = {{\n\t\t{}\n\t}};\n",
            hex(&path)
        );
        println!(
            "\tinline constexpr uint8 File[] = {{\n\t\t{}\n\t}};\n",
            hex(&file)
        );

        // The shapes the client's reader leans on.
        assert_eq!(header[0], 0x9E, "a 14-element fixarray");
        assert_eq!(&header[1..7], &[RECORD_HEADER, 0xCE, 0, 0, 0, 3]);
        assert_eq!(
            &frame[..12],
            &[0x99, 3, 0xCE, 0, 0, 0, 3, 0xCE, 0, 0, 0x12, 0xC8]
        );
        assert_eq!(frame.len(), 12 + 7 + 2 + 2 * ROW_SIZE);
        assert_eq!(&lap[..2], &[0x95, RECORD_EVENT]);
        assert_eq!(&roster[..2], &[0x94, RECORD_ROSTER]);
        assert_eq!(&path[..2], &[0x94, RECORD_PATH]);
    }
}
