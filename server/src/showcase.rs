//! Showcase channels: rendered races (`.apxs`, `crate::spectator`) played
//! in a loop to any number of viewers.
//!
//! The menu of every client plays an AI race behind its screens. Simulating
//! one per client (`SessionKind::Demo`) is a 420 Hz race nobody drives for
//! each player sitting in the menu; a showcase is that race rendered once
//! (`apexsim-replay render`) and *played back*: a file read and a fan-out,
//! no physics, and the frames forwarded as the bytes the file holds.
//!
//! A **channel** is one file on one clock, shared by everyone watching it:
//! a viewer joins mid-race, which is what a broadcast is. The game loop
//! advances every channel that has viewers once per tick
//! ([`ShowcaseState::advance`]), which hands back the records whose tick
//! has come; the loop sends frames over UDP and everything else over TCP.
//! A channel nobody watches stops and drops its inflated records.
//!
//! Each time a channel starts or loops it takes a fresh **epoch** from one
//! server-wide counter and stamps it into the records
//! (`spectator::patch_epoch`), so a client can tell a frame of the stream
//! it is watching from a stale one of any other.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};

use bytes::Bytes;
use tracing::{info, warn};

use crate::config::{ShowcaseMode, ShowcaseSettings};
use crate::data::*;
use crate::network::{ShowcaseSummary, SpectatorJoinedData, SpectatorKind};
use crate::spectator::{self, StreamFile, StreamHeader, StreamRoster, RECORD_EVENT, RECORD_FRAME};

/// One rendered race the server can play, read from its file's first few
/// kilobytes at startup.
#[derive(Debug, Clone)]
pub struct ShowcaseEntry {
    /// The file's name without `.apxs`: `Zandvoort.gt3.day`.
    pub id: String,
    pub path: PathBuf,
    pub header: StreamHeader,
    pub roster: StreamRoster,
    /// The class of the field (its first car's), as car.toml spells it.
    pub class: String,
}

/// A file inflated for playing: every timed record in order.
pub struct LoadedStream {
    /// Header, roster, path and the preamble's events.
    preamble: Vec<Vec<u8>>,
    records: Vec<TimedRecord>,
}

struct TimedRecord {
    tick: u32,
    kind: u8,
    body: Vec<u8>,
}

impl LoadedStream {
    /// Read and inflate a whole file. Slow enough (a few megabytes of zlib)
    /// to be kept off the game loop.
    pub fn load(path: &Path) -> Result<Self, String> {
        let file = StreamFile::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let mut preamble = vec![file.header.encode(), file.roster.encode()];
        if let Some(path) = &file.path {
            preamble.push(path.encode());
        }
        preamble.extend(file.preamble.iter().map(|e| e.encode()));
        let mut records = Vec::new();
        let mut last_tick = file.header.start_tick;
        for body in file
            .records()
            .map_err(|e| format!("{}: {e}", path.display()))?
        {
            let Some(kind) = spectator::record_type(&body) else {
                continue;
            };
            // A roster change carries no tick of its own: it goes out with
            // whatever it follows.
            let tick = spectator::record_tick(&body).unwrap_or(last_tick);
            last_tick = tick;
            records.push(TimedRecord { tick, kind, body });
        }
        Ok(Self { preamble, records })
    }

    fn stamp(&mut self, epoch: u32) {
        for body in &mut self.preamble {
            spectator::patch_epoch(body, epoch);
        }
        for record in &mut self.records {
            spectator::patch_epoch(&mut record.body, epoch);
        }
    }
}

/// What one channel wants sent this tick.
#[derive(Debug, Default)]
pub struct ChannelOutput {
    /// Everyone watching.
    pub viewers: Vec<PlayerId>,
    /// Record bodies for all of them, reliably and in order.
    pub reliable: Vec<Bytes>,
    /// Frame bodies for all of them, as datagrams.
    pub frames: Vec<Bytes>,
    /// Record bodies for viewers who have just joined (or the whole channel
    /// after a loop): the preamble and the timing so far.
    pub catch_up: Vec<(PlayerId, Vec<Bytes>)>,
}

struct Channel {
    /// The channel's name: the id of the file it started on.
    id: String,
    /// Index into the library of the file now playing.
    playing: usize,
    viewers: BTreeSet<PlayerId>,
    /// Joined, and not yet sent the preamble.
    newcomers: BTreeSet<PlayerId>,
    loaded: Option<LoadedStream>,
    /// A load is under way off the loop.
    loading: bool,
    epoch: u32,
    /// The content's clock, in the file's ticks.
    clock: f64,
    cursor: usize,
    /// Frame ticks sent, for `stream_divisor`.
    frame_ticks: u64,
    last_frame_tick: Option<u32>,
}

/// A request to inflate a channel's file, for the loop to run off itself.
#[derive(Debug, Clone, PartialEq)]
pub struct LoadRequest {
    pub channel: usize,
    pub entry: usize,
    pub path: PathBuf,
}

/// Every showcase the server lists and the channels playing them.
pub struct ShowcaseState {
    entries: Vec<ShowcaseEntry>,
    channels: Vec<Channel>,
    watching: HashMap<PlayerId, usize>,
    mode: ShowcaseMode,
    stream_divisor: u64,
    next_epoch: u32,
}

impl ShowcaseState {
    /// No showcases: what a server without a `showcase` folder has.
    pub fn empty() -> Self {
        Self::from_entries(Vec::new(), ShowcaseMode::Loop, 1)
    }

    pub fn from_entries(entries: Vec<ShowcaseEntry>, mode: ShowcaseMode, divisor: u16) -> Self {
        let channels = entries
            .iter()
            .enumerate()
            .map(|(i, entry)| Channel {
                id: entry.id.clone(),
                playing: i,
                viewers: BTreeSet::new(),
                newcomers: BTreeSet::new(),
                loaded: None,
                loading: false,
                epoch: 0,
                clock: 0.0,
                cursor: 0,
                frame_ticks: 0,
                last_frame_tick: None,
            })
            .collect();
        Self {
            entries,
            channels,
            watching: HashMap::new(),
            mode,
            stream_divisor: divisor.max(1) as u64,
            next_epoch: 1,
        }
    }

    /// Read the playlist's files from `settings.dir`: each one's header and
    /// roster, dropping (with a log line) a file whose track or cars this
    /// server does not have or whose checksums disagree with its content,
    /// because cars would drive a road that has moved.
    pub fn load(
        settings: &ShowcaseSettings,
        cars: &HashMap<CarConfigId, CarConfig>,
        tracks: &HashMap<TrackConfigId, TrackConfig>,
    ) -> Self {
        if !settings.enabled {
            return Self::empty();
        }
        let dir = Path::new(&settings.dir);
        let mut found: BTreeMap<String, PathBuf> = BTreeMap::new();
        if let Ok(read) = std::fs::read_dir(dir) {
            for path in read.filter_map(|e| e.ok()).map(|e| e.path()) {
                if path
                    .extension()
                    .is_some_and(|e| e.eq_ignore_ascii_case("apxs"))
                {
                    if let Some(stem) = path.file_stem() {
                        found.insert(stem.to_string_lossy().into_owned(), path);
                    }
                }
            }
        }
        // The playlist's order, `*` standing for every file not named.
        let mut order: Vec<String> = Vec::new();
        for name in &settings.playlist {
            if name == "*" {
                for id in found.keys() {
                    if !order.contains(id) && !settings.playlist.contains(id) {
                        order.push(id.clone());
                    }
                }
            } else if found.contains_key(name) {
                if !order.contains(name) {
                    order.push(name.clone());
                }
            } else {
                warn!(
                    "Showcase '{}' is in the playlist but not in {}",
                    name,
                    dir.display()
                );
            }
        }

        let mut entries = Vec::new();
        for id in order {
            let path = found[&id].clone();
            match read_entry(&id, &path, cars, tracks) {
                Ok(entry) => entries.push(entry),
                Err(why) => warn!("Showcase {} left out: {}", path.display(), why),
            }
        }
        if !entries.is_empty() {
            info!(
                "Showcase: {} race(s) from {}: {}",
                entries.len(),
                dir.display(),
                entries
                    .iter()
                    .map(|e| e.id.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
        Self::from_entries(entries, settings.mode, settings.stream_divisor)
    }

    pub fn is_available(&self) -> bool {
        !self.channels.is_empty()
    }

    /// Somebody is watching something: the loop has channels to advance.
    pub fn has_viewers(&self) -> bool {
        !self.watching.is_empty()
    }

    pub fn entries(&self) -> &[ShowcaseEntry] {
        &self.entries
    }

    /// The channels, as `Showcases` lists them: what each is playing now.
    pub fn summaries(&self) -> Vec<ShowcaseSummary> {
        self.channels
            .iter()
            .map(|channel| {
                let entry = &self.entries[channel.playing];
                ShowcaseSummary {
                    id: channel.id.clone(),
                    track_id: entry.header.track.track_id,
                    track_name: entry.header.track.display_name.clone(),
                    class: entry.class.clone(),
                    conditions: entry.header.conditions,
                    duration_s: entry.header.duration_s(),
                    cars: entry.roster.entries.len().min(255) as u8,
                    viewers: channel.viewers.len() as u32,
                }
            })
            .collect()
    }

    /// The channel a player watches, if any.
    pub fn watching(&self, player: &PlayerId) -> Option<&str> {
        self.watching
            .get(player)
            .map(|&i| self.channels[i].id.as_str())
    }

    /// Put a player on a channel (`id`, or the first when `None`), leaving
    /// whichever they watched. `None` when there is no such channel.
    /// The second value asks for the channel's file to be loaded.
    pub fn join(
        &mut self,
        player: PlayerId,
        id: Option<&str>,
    ) -> Option<(SpectatorJoinedData, Option<LoadRequest>)> {
        let index = match id {
            Some(id) => self
                .channels
                .iter()
                .position(|c| c.id.eq_ignore_ascii_case(id))?,
            None if self.channels.is_empty() => return None,
            None => 0,
        };
        self.leave(&player);
        self.watching.insert(player, index);
        let channel = &mut self.channels[index];
        channel.viewers.insert(player);
        channel.newcomers.insert(player);
        let entry = &self.entries[channel.playing];
        let joined = SpectatorJoinedData {
            stream_id: entry.header.stream_id,
            kind: SpectatorKind::Showcase,
            showcase_id: channel.id.clone(),
        };
        let load = (channel.loaded.is_none() && !channel.loading).then(|| {
            channel.loading = true;
            LoadRequest {
                channel: index,
                entry: channel.playing,
                path: entry.path.clone(),
            }
        });
        Some((joined, load))
    }

    /// Take a player off their channel. True when they were on one.
    pub fn leave(&mut self, player: &PlayerId) -> bool {
        let Some(index) = self.watching.remove(player) else {
            return false;
        };
        let channel = &mut self.channels[index];
        channel.viewers.remove(player);
        channel.newcomers.remove(player);
        if channel.viewers.is_empty() {
            // Nobody watching: stop, and give the memory back. The next
            // viewer starts it from the beginning.
            channel.loaded = None;
        }
        true
    }

    /// A load finished (or failed). The channel starts from the beginning of
    /// its content under a fresh epoch.
    pub fn install(&mut self, request: &LoadRequest, loaded: Result<LoadedStream, String>) {
        let Some(channel) = self.channels.get_mut(request.channel) else {
            return;
        };
        channel.loading = false;
        if channel.playing != request.entry {
            return;
        }
        match loaded {
            Ok(stream) => {
                if channel.viewers.is_empty() {
                    return;
                }
                channel.loaded = Some(stream);
                start(channel, &self.entries, &mut self.next_epoch);
            }
            Err(why) => {
                warn!("Showcase {} cannot be played: {}", channel.id, why);
                // Its viewers get nothing; they time out on their own.
            }
        }
    }

    /// Advance every watched channel by one server tick of `tick_rate` and
    /// return what each wants sent, plus any file that now needs loading
    /// (a `rotate` channel moving on to the next).
    pub fn advance(&mut self, tick_rate: u16) -> (Vec<ChannelOutput>, Vec<LoadRequest>) {
        let mut outputs = Vec::new();
        let mut loads = Vec::new();
        let count = self.entries.len();
        for (index, channel) in self.channels.iter_mut().enumerate() {
            if channel.viewers.is_empty() {
                continue;
            }
            if channel.loaded.is_none() {
                continue;
            }
            let entry = &self.entries[channel.playing];
            let mut out = ChannelOutput {
                viewers: channel.viewers.iter().copied().collect(),
                ..Default::default()
            };

            // The end of the content: loop, or move on to the next file.
            if channel.clock > entry.header.end_tick as f64 {
                if self.mode == ShowcaseMode::Rotate && count > 1 {
                    channel.playing = (channel.playing + 1) % count;
                    channel.loaded = None;
                    channel.loading = true;
                    channel.newcomers = channel.viewers.clone();
                    loads.push(LoadRequest {
                        channel: index,
                        entry: channel.playing,
                        path: self.entries[channel.playing].path.clone(),
                    });
                    continue;
                }
                start(channel, &self.entries, &mut self.next_epoch);
            }

            let loaded = channel.loaded.as_ref().expect("checked above");
            // Whoever has not had the preamble gets it, with the timing
            // lines crossed so far, before anything else of this epoch.
            if !channel.newcomers.is_empty() {
                let mut catch_up: Vec<Bytes> = loaded
                    .preamble
                    .iter()
                    .map(|body| Bytes::copy_from_slice(body))
                    .collect();
                catch_up.extend(
                    loaded.records[..channel.cursor]
                        .iter()
                        .filter(|r| r.kind == RECORD_EVENT && is_lap_timing(&r.body))
                        .map(|r| Bytes::copy_from_slice(&r.body)),
                );
                for player in std::mem::take(&mut channel.newcomers) {
                    out.catch_up.push((player, catch_up.clone()));
                }
            }

            while let Some(record) = loaded.records.get(channel.cursor) {
                if record.tick as f64 > channel.clock {
                    break;
                }
                channel.cursor += 1;
                if record.kind == RECORD_FRAME {
                    if channel.last_frame_tick != Some(record.tick) {
                        channel.last_frame_tick = Some(record.tick);
                        channel.frame_ticks += 1;
                    }
                    if (channel.frame_ticks - 1).is_multiple_of(self.stream_divisor) {
                        out.frames.push(Bytes::copy_from_slice(&record.body));
                    }
                } else {
                    out.reliable.push(Bytes::copy_from_slice(&record.body));
                }
            }
            channel.clock += entry.header.tick_rate.max(1) as f64 / tick_rate.max(1) as f64;
            outputs.push(out);
        }
        (outputs, loads)
    }
}

/// (Re)start a loaded channel at the beginning of its content.
fn start(channel: &mut Channel, entries: &[ShowcaseEntry], next_epoch: &mut u32) {
    channel.epoch = *next_epoch;
    // Zero is what a file holds; a live epoch never is.
    *next_epoch = next_epoch.checked_add(1).unwrap_or(1);
    if let Some(loaded) = &mut channel.loaded {
        loaded.stamp(channel.epoch);
    }
    channel.clock = entries[channel.playing].header.start_tick as f64;
    channel.cursor = 0;
    channel.frame_ticks = 0;
    channel.last_frame_tick = None;
    channel.newcomers = channel.viewers.clone();
}

fn is_lap_timing(body: &[u8]) -> bool {
    // [4, epoch (5 bytes), tick (5 bytes), kind, payload]
    body.get(12) == Some(&spectator::EVENT_LAP_TIMING)
}

/// A file's catalog entry, or why this server cannot play it.
fn read_entry(
    id: &str,
    path: &Path,
    cars: &HashMap<CarConfigId, CarConfig>,
    tracks: &HashMap<TrackConfigId, TrackConfig>,
) -> Result<ShowcaseEntry, String> {
    let file = StreamFile::open(path).map_err(|e| e.to_string())?;
    let header = file.header;
    let roster = file.roster;
    let track = tracks
        .get(&header.track.track_id)
        .ok_or_else(|| format!("this server has no track {}", header.track.stem))?;
    if header.track.source_crc != 0
        && track.content_crc != 0
        && header.track.source_crc != track.content_crc
    {
        return Err(format!(
            "it was raced on another version of {} ({:08x}, the track here is {:08x}); render it again",
            header.track.stem, header.track.source_crc, track.content_crc
        ));
    }
    for entry in &roster.entries {
        let car = cars
            .get(&entry.car_config_id)
            .ok_or_else(|| format!("this server has no car {}", entry.car_config_id))?;
        if entry.content_crc != 0 && car.content_crc != 0 && entry.content_crc != car.content_crc {
            return Err(format!(
                "it was raced on another version of {} ({:08x}, the car here is {:08x}); render it again",
                car.name, entry.content_crc, car.content_crc
            ));
        }
    }
    let class = roster
        .entries
        .first()
        .and_then(|e| cars.get(&e.car_config_id))
        .map(|c| c.class.clone())
        .unwrap_or_default();
    Ok(ShowcaseEntry {
        id: id.to_string(),
        path: path.to_path_buf(),
        header,
        roster,
        class,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::spectator::{
        encode_frames, CarRow, EventKind, Record, StreamContent, StreamEvent, StreamRender,
        StreamRosterEntry, StreamTrack,
    };

    fn row(car_index: u8, station_cm: u32) -> CarRow {
        CarRow {
            car_index,
            status: spectator::STATUS_ON_TRACK,
            x_mm: 0,
            y_mm: 0,
            z_mm: 0,
            yaw: 0,
            pitch: 0,
            roll: 0,
            speed_cms: 5000,
            steering: 0,
            throttle: 255,
            brake: 0,
            gear: 3,
            engine_rpm: 7000,
            lap: 1,
            station_cm,
            finish_position: 0,
            lap_flags: 0,
            pit_flags: 0,
            compound: 1,
            damage: [0; 5],
            ers_flags: 0,
            tyre_wear: [5; 4],
            tyre_c: [85; 4],
        }
    }

    /// A two-second stream at 30 Hz of a 240 Hz clock, with a lap-timing
    /// event one second in.
    pub(crate) fn tiny_stream(
        stem: &str,
        track_id: TrackConfigId,
        car: CarConfigId,
    ) -> StreamContent {
        let mut records = Vec::new();
        for i in 0..60u32 {
            let tick = 8 + i * 8;
            if i == 30 {
                records.push((
                    tick,
                    StreamEvent {
                        epoch: 0,
                        tick,
                        kind: EventKind::LapTiming(crate::network::LapTimingData {
                            car_index: 0,
                            lap: 1,
                            sector: 0,
                            sector_time_ms: 1000,
                            lap_time_ms: 0,
                            is_lap_end: false,
                            valid: true,
                            flags: 0,
                        }),
                    }
                    .encode(),
                ));
            }
            for body in encode_frames(
                0,
                tick,
                0,
                SessionState::Racing,
                None,
                &[row(0, i * 100), row(1, i * 90)],
            ) {
                records.push((tick, body));
            }
        }
        StreamContent {
            header: StreamHeader {
                epoch: 0,
                version: spectator::FORMAT_VERSION,
                stream_id: uuid::Uuid::from_u128(0xABCD),
                tick_rate: 240,
                frame_rate: 30,
                row_size: spectator::ROW_SIZE as u8,
                track: StreamTrack {
                    track_id,
                    stem: stem.into(),
                    display_name: stem.into(),
                    source_crc: 0,
                    length_m: 1000.0,
                },
                conditions: SessionConditions::DEFAULT,
                session_kind: SessionKind::Multiplayer,
                game_mode: GameMode::Race,
                lap_limit: 2,
                race_start_tick: Some(8),
                start_tick: 8,
                end_tick: 8 + 59 * 8,
                render: StreamRender::default(),
            },
            roster: StreamRoster {
                epoch: 0,
                revision: 0,
                entries: (0..2)
                    .map(|i| StreamRosterEntry {
                        car_index: i,
                        car_config_id: car,
                        content_crc: 0,
                        livery: 0,
                        name: format!("AI {i}"),
                        is_ai: true,
                    })
                    .collect(),
            },
            path: None,
            preamble: vec![StreamEvent {
                epoch: 0,
                tick: 8,
                kind: EventKind::TrackSectors {
                    track_length_m: 1000.0,
                    boundaries_m: vec![300.0, 600.0],
                },
            }],
            records,
        }
    }

    fn state_with(dir: &Path, names: &[&str], mode: ShowcaseMode, divisor: u16) -> ShowcaseState {
        let track = TrackConfig::default();
        let car = CarConfig::default();
        for name in names {
            tiny_stream(name, track.id, car.id)
                .write_file(&dir.join(format!("{name}.apxs")))
                .unwrap();
        }
        let settings = ShowcaseSettings {
            enabled: true,
            dir: dir.to_string_lossy().into_owned(),
            playlist: vec!["*".into()],
            mode,
            stream_divisor: divisor,
        };
        ShowcaseState::load(
            &settings,
            &HashMap::from([(car.id, car)]),
            &HashMap::from([(track.id, track)]),
        )
    }

    fn load_now(state: &mut ShowcaseState, request: Option<LoadRequest>) {
        if let Some(request) = request {
            let loaded = LoadedStream::load(&request.path);
            state.install(&request, loaded);
        }
    }

    fn epoch_of(body: &Bytes) -> u32 {
        spectator::record_epoch(body).unwrap()
    }

    #[test]
    fn a_channel_plays_on_one_clock_and_loops_under_a_new_epoch() {
        let dir = tempfile::TempDir::new().unwrap();
        let mut state = state_with(dir.path(), &["Ring"], ShowcaseMode::Loop, 1);
        assert!(state.is_available());
        let (first, second) = (uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2));

        let (joined, load) = state.join(first, None).unwrap();
        assert_eq!(joined.showcase_id, "Ring");
        assert_eq!(joined.stream_id, uuid::Uuid::from_u128(0xABCD));
        // Nothing plays until the file is in.
        assert!(state.advance(240).0.is_empty());
        load_now(&mut state, load);

        // The first tick: the preamble to the newcomer, then the first frame.
        let (out, _) = state.advance(240);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].viewers, vec![first]);
        assert_eq!(out[0].catch_up.len(), 1);
        let preamble = &out[0].catch_up[0].1;
        assert!(matches!(
            Record::decode(&preamble[0]).unwrap(),
            Record::Header(h) if h.epoch == 1
        ));
        assert!(matches!(Record::decode(&preamble[1]).unwrap(), Record::Roster(r) if r.epoch == 1));
        assert!(matches!(
            Record::decode(&preamble[2]).unwrap(),
            Record::Event(e) if matches!(e.kind, EventKind::TrackSectors { .. })
        ));
        assert_eq!(out[0].frames.len(), 1);
        assert_eq!(epoch_of(&out[0].frames[0]), 1);

        // A second and a half on, a late joiner: the same clock, the header
        // first, and the lap timing it missed.
        let mut frames = 1;
        for _ in 0..360 {
            let (out, _) = state.advance(240);
            frames += out[0].frames.len();
        }
        assert_eq!(frames, 46, "30 frames a second of a 240 Hz clock");
        assert_eq!(state.summaries()[0].viewers, 1);
        let (_, load) = state.join(second, Some("ring")).unwrap();
        assert!(load.is_none(), "the channel is already playing");
        let (out, _) = state.advance(240);
        assert_eq!(out[0].viewers, vec![first, second]);
        assert_eq!(out[0].catch_up.len(), 1);
        let (who, catch_up) = &out[0].catch_up[0];
        assert_eq!(*who, second);
        assert!(matches!(
            Record::decode(&catch_up[0]).unwrap(),
            Record::Header(_)
        ));
        assert!(matches!(
            Record::decode(catch_up.last().unwrap()).unwrap(),
            Record::Event(e) if matches!(e.kind, EventKind::LapTiming(_))
        ));

        // Past the end the channel starts over: a new epoch, and the header
        // again for everyone.
        let mut looped = None;
        for _ in 0..200 {
            let (out, _) = state.advance(240);
            if !out[0].catch_up.is_empty() {
                looped = Some(out);
                break;
            }
        }
        let out = looped.expect("the channel loops");
        assert_eq!(out[0].catch_up.len(), 2);
        assert!(matches!(
            Record::decode(&out[0].catch_up[0].1[0]).unwrap(),
            Record::Header(h) if h.epoch == 2
        ));
        // Nothing of the lap before is replayed to them.
        assert_eq!(out[0].catch_up[0].1.len(), 3);
        assert_eq!(epoch_of(&out[0].frames[0]), 2);
        assert_eq!(spectator::record_tick(&out[0].frames[0]), Some(8));

        // The last viewer leaving stops it; the next starts from the top.
        assert!(state.leave(&first));
        assert!(state.leave(&second));
        assert!(!state.leave(&second));
        assert!(state.advance(240).0.is_empty());
        let (_, load) = state.join(first, Some("Ring")).unwrap();
        assert!(load.is_some(), "a stopped channel dropped its records");
        load_now(&mut state, load);
        let (out, _) = state.advance(240);
        assert_eq!(epoch_of(&out[0].frames[0]), 3);
        assert!(state.join(first, Some("Nope")).is_none());
    }

    #[test]
    fn the_divisor_thins_frames_and_rotate_moves_on() {
        let dir = tempfile::TempDir::new().unwrap();
        let mut state = state_with(dir.path(), &["Alpha", "Beta"], ShowcaseMode::Rotate, 2);
        assert_eq!(
            state
                .summaries()
                .iter()
                .map(|s| s.id.as_str())
                .collect::<Vec<_>>(),
            vec!["Alpha", "Beta"]
        );
        let viewer = uuid::Uuid::from_u128(9);
        let (_, load) = state.join(viewer, Some("Alpha")).unwrap();
        load_now(&mut state, load);
        let mut frames = 0;
        let mut next = None;
        for _ in 0..600 {
            let (out, loads) = state.advance(240);
            frames += out.iter().map(|o| o.frames.len()).sum::<usize>();
            if let Some(load) = loads.into_iter().next() {
                next = Some(load);
                break;
            }
        }
        assert_eq!(frames, 30, "every second frame of 60");
        let next = next.expect("the channel moves on at the end");
        assert_eq!((next.channel, next.entry), (0, 1));
        assert_eq!(state.summaries()[0].track_name, "Beta");
        load_now(&mut state, Some(next));
        let (out, _) = state.advance(240);
        // The next file's header, for the viewer who stayed.
        assert!(matches!(
            Record::decode(&out[0].catch_up[0].1[0]).unwrap(),
            Record::Header(h) if h.track.stem == "Beta" && h.epoch == 2
        ));
    }

    #[test]
    fn a_file_that_does_not_match_the_content_is_left_out() {
        let dir = tempfile::TempDir::new().unwrap();
        let track = TrackConfig {
            content_crc: 0x1111,
            ..TrackConfig::default()
        };
        let car = CarConfig::default();
        let mut good = tiny_stream("Good", track.id, car.id);
        good.header.track.source_crc = 0x1111;
        good.write_file(&dir.path().join("Good.apxs")).unwrap();
        let mut stale = tiny_stream("Stale", track.id, car.id);
        stale.header.track.source_crc = 0x2222;
        stale.write_file(&dir.path().join("Stale.apxs")).unwrap();
        tiny_stream("Elsewhere", uuid::Uuid::from_u128(77), car.id)
            .write_file(&dir.path().join("Elsewhere.apxs"))
            .unwrap();
        tiny_stream("OtherCar", track.id, uuid::Uuid::from_u128(78))
            .write_file(&dir.path().join("OtherCar.apxs"))
            .unwrap();
        std::fs::write(dir.path().join("Broken.apxs"), b"not a stream").unwrap();

        let settings = ShowcaseSettings {
            enabled: true,
            dir: dir.path().to_string_lossy().into_owned(),
            playlist: vec!["Stale".into(), "Missing".into(), "*".into()],
            mode: ShowcaseMode::Loop,
            stream_divisor: 1,
        };
        let cars = HashMap::from([(car.id, car)]);
        let tracks = HashMap::from([(track.id, track)]);
        let state = ShowcaseState::load(&settings, &cars, &tracks);
        assert_eq!(
            state
                .entries()
                .iter()
                .map(|e| e.id.as_str())
                .collect::<Vec<_>>(),
            vec!["Good"]
        );
        let off = ShowcaseSettings {
            enabled: false,
            ..settings
        };
        assert!(!ShowcaseState::load(&off, &cars, &tracks).is_available());
    }
}
