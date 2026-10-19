//! The safety car on the road (`RaceRules::safety_car` `Full`).
//!
//! A car stranded on the road (`race_control::FlagCar::stranded`, the same
//! trigger as the virtual safety car) brings out a safety car. It is not a
//! participant: no roster slot, no classification, no lap record. It waits
//! at the pit exit for the leader, comes out ahead of it, and drives the
//! racing line at [`SC_LINE_SHARE`] of the line's planned speed, at most
//! [`SC_SPEED_MPS`].
//!
//! - **The queue.** Every car on the road is held (`CarState::speed_cap_mps`)
//!   no faster than the car ahead of it on the road (the safety car for the
//!   first) allows at [`SC_GAP_M`] behind it, and no faster than the
//!   race-control pace coming down to [`SC_CATCH_MPS`]: the field catches
//!   up and runs single file, no overtaking (a pass is one under a yellow,
//!   passing the safety car too).
//! - **Formed.** With every car within [`SC_QUEUED_M`] of the one ahead (or
//!   [`SC_FORM_LAPS`] laps of the safety car gone by) the queue has formed:
//!   the pit lane opens, and a lapped car (one with a car behind it in the
//!   queue a lap or more ahead of it) is let by: held only to the catch pace,
//!   free to pass the queue and the safety car, it goes round to the back.
//! - **In this lap.** With the queue formed, nobody being let by, the road
//!   clear (`race_control::RaceControl::road_clear`) and the queue closed up
//!   again (or [`SC_FORM_LAPS`] more laps gone), the lights go out: the
//!   safety car goes into the pit lane at its entry (off the road at the
//!   line where there is none).
//! - **The restart.** The leader sets the pace, no faster than
//!   [`SC_SPEED_MPS`] until [`SC_RESTART_ZONE_M`] before the line, free from
//!   there; the rest keep station behind it. Green as the leader crosses the
//!   line.
//!
//! Everything here is worked out from the tick's car states and the track,
//! with no chance and no clock, so a seeded race runs the same every time.

use crate::data::{CarConfig, TrackConfig};
use crate::race_start::follow_cap_mps;

/// The safety car's pace on a straight, m/s (162 km/h).
pub const SC_SPEED_MPS: f32 = 45.0;
/// Its share of the racing line's planned speed: the corners at a pace a
/// queue can follow (and the braking at its square, half the car's).
pub const SC_LINE_SHARE: f32 = 0.7;
/// How hard it pulls away, m/s².
pub const SC_ACCEL_MPS2: f32 = 4.0;
/// The pace a car not yet in the queue (and a lapped car let by) is held
/// to, m/s: faster than the safety car on a straight, so the field catches
/// it up and a lapped car gets by.
pub const SC_CATCH_MPS: f32 = 55.0;
/// Each car is held this far behind the car (or safety car) ahead, m,
/// middle to middle.
pub const SC_GAP_M: f32 = 24.0;
/// A car within this of the car ahead is in the queue, m (ten car lengths).
pub const SC_QUEUED_M: f32 = 50.0;
/// The safety car leaves the pit exit when the leader is this close behind
/// it on the road, m.
pub const SC_PICKUP_M: f32 = 200.0;
/// The queue counts as formed after this many laps of the safety car, and
/// the lights go out this many laps after it formed, whether every car has
/// closed up or not (one off the road, one slow out of the pits).
pub const SC_FORM_LAPS: u32 = 2;
/// The leader is free to set its own pace this far before the line, m.
pub const SC_RESTART_ZONE_M: f32 = 250.0;
/// A car this close behind the safety car one tick and as close ahead the
/// next has passed it, m.
pub const SC_PASS_WINDOW_M: f32 = 60.0;
/// A car ahead slower than this is no pace for the queue behind it (stopped
/// or crawling: the cars behind go round it), m/s.
pub const SC_PACE_SETTER_MPS: f32 = 3.0;

/// Where the safety car is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScStage {
    /// At the pit exit, waiting for the leader.
    Waiting,
    /// On the road, leading the queue.
    OnTrack,
    /// Going into the pit lane, the lights out.
    InLane,
    /// Off the road: the restart.
    Gone,
}

/// What a tick of the safety car decided for the race as a whole.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScEvent {
    /// The lights are out: in this lap.
    InThisLap,
    /// A car stranded again before it went in: lights back on.
    BackOut,
    /// The leader crossed the line: green.
    Green,
}

/// What the safety car reads of each car.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct QueueCar {
    /// Laps run, the lap in progress as a share.
    pub laps: f32,
    /// Where along the lap, m.
    pub progress_m: f32,
    pub speed_mps: f32,
    /// On the road and racing: not in the pit lane between its limit lines,
    /// its garage, towed, finished or broken down. (A car on the lane's
    /// taper beside the road is on it.)
    pub running: bool,
    /// On the road and moving: a pace for the car behind it.
    pub moving: bool,
}

/// What the safety car tells each car this tick, in the order of the cars
/// it was given.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct QueueOrders {
    /// Each car's pace, m/s (0: none).
    pub caps: Vec<f32>,
    /// Let by: free to pass, and passed by no one.
    pub let_by: Vec<bool>,
    /// Went past the safety car this tick, not let by.
    pub passed_sc: Vec<usize>,
}

/// The safety car's pose, for telemetry.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct ScPose {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    pub speed_mps: f32,
}

/// The line the safety car drives: a closed polyline with its planned
/// speed and each point's place on the lap.
#[derive(Debug, Clone, Default)]
struct ScLine {
    points: Vec<[f32; 3]>,
    /// Distance along the line to each point, m; `length_m` closes it.
    along_m: Vec<f32>,
    length_m: f32,
    /// The speed the safety car may make at each point, m/s.
    pace_mps: Vec<f32>,
    /// Each point's station on the lap, m.
    stations: Vec<f32>,
}

impl ScLine {
    /// The racing line planned for `car` on `track` (the centerline at
    /// [`SC_SPEED_MPS`] when no line can be planned).
    fn plan(track: &TrackConfig, car: Option<&CarConfig>) -> ScLine {
        let total = crate::laps::track_length_m(track);
        let profile = car.and_then(|c| crate::racing_line::build(track, c));
        let (points, pace_mps): (Vec<[f32; 3]>, Vec<f32>) = match profile {
            Some(p) if p.points.len() >= 3 && p.points.len() == p.speed_mps.len() => (
                p.points.clone(),
                p.speed_mps
                    .iter()
                    .map(|v| (v * SC_LINE_SHARE).min(SC_SPEED_MPS))
                    .collect(),
            ),
            _ => (
                track.centerline.iter().map(|p| [p.x, p.y, p.z]).collect(),
                vec![SC_SPEED_MPS; track.centerline.len()],
            ),
        };
        let mut along_m = Vec::with_capacity(points.len());
        let mut at = 0.0f32;
        for (i, p) in points.iter().enumerate() {
            along_m.push(at);
            let q = points[(i + 1) % points.len()];
            at += ((q[0] - p[0]).powi(2) + (q[1] - p[1]).powi(2)).sqrt();
        }
        let stations = stations_on_lap(&points, track, total);
        ScLine {
            points,
            along_m,
            length_m: at.max(1.0),
            pace_mps,
            stations,
        }
    }

    /// The segment `along` lies on, and how far along it.
    fn segment(&self, along: f32) -> (usize, usize, f32) {
        let s = along.rem_euclid(self.length_m);
        let i = self
            .along_m
            .partition_point(|&v| v <= s)
            .saturating_sub(1)
            .min(self.points.len().saturating_sub(1));
        let j = (i + 1) % self.points.len();
        let end = if j == 0 {
            self.length_m
        } else {
            self.along_m[j]
        };
        let t = ((s - self.along_m[i]) / (end - self.along_m[i]).max(1e-6)).clamp(0.0, 1.0);
        (i, j, t)
    }

    fn pace_at(&self, along: f32) -> f32 {
        let (i, j, t) = self.segment(along);
        self.pace_mps[i] + (self.pace_mps[j] - self.pace_mps[i]) * t
    }

    fn station_at(&self, along: f32, total: f32) -> f32 {
        let (i, j, t) = self.segment(along);
        let (a, mut b) = (self.stations[i], self.stations[j]);
        if b < a - 0.5 * total {
            b += total;
        }
        (a + (b - a) * t).rem_euclid(total.max(1.0))
    }

    fn pose_at(&self, along: f32) -> (f32, f32, f32, f32) {
        let (i, j, t) = self.segment(along);
        let (a, b) = (self.points[i], self.points[j]);
        (
            a[0] + (b[0] - a[0]) * t,
            a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t,
            (b[1] - a[1]).atan2(b[0] - a[0]),
        )
    }

    /// The distance along the line of the point nearest (x, y).
    fn nearest_along(&self, x: f32, y: f32) -> f32 {
        let mut best = (f32::MAX, 0usize);
        for (i, p) in self.points.iter().enumerate() {
            let d = (p[0] - x).powi(2) + (p[1] - y).powi(2);
            if d < best.0 {
                best = (d, i);
            }
        }
        self.along_m.get(best.1).copied().unwrap_or(0.0)
    }
}

/// Each point's station on the lap: the nearest centerline point, found by
/// walking forward from the last one (both run the same way round), and
/// the point's projection on the road's heading there.
fn stations_on_lap(points: &[[f32; 3]], track: &TrackConfig, total: f32) -> Vec<f32> {
    let cl = &track.centerline;
    if cl.is_empty() || total <= 0.0 {
        return vec![0.0; points.len()];
    }
    let d2 = |i: usize, p: &[f32; 3]| (cl[i].x - p[0]).powi(2) + (cl[i].y - p[1]).powi(2);
    let mut ci = (0..cl.len())
        .min_by(|a, b| d2(*a, &points[0]).total_cmp(&d2(*b, &points[0])))
        .unwrap_or(0);
    points
        .iter()
        .map(|p| {
            for _ in 0..cl.len() {
                let next = (ci + 1) % cl.len();
                if d2(next, p) < d2(ci, p) {
                    ci = next;
                } else {
                    break;
                }
            }
            let c = &cl[ci];
            let along = (p[0] - c.x) * c.heading_rad.cos() + (p[1] - c.y) * c.heading_rad.sin();
            (c.distance_from_start_m + along).rem_euclid(total)
        })
        .collect()
}

/// The safety car of one race, from the moment it is called out until the
/// green.
#[derive(Debug, Clone)]
pub struct SafetyCar {
    pub stage: ScStage,
    line: ScLine,
    /// Where it joins the road from the pit exit, along the line, and that
    /// place's station on the lap.
    join_along_m: f32,
    join_station_m: f32,
    /// Where it leaves the road: the pit entry's station (the line's where
    /// there is no lane).
    entry_station_m: f32,
    /// Along the line on the road, m; along the pit lane in it, m.
    along_m: f32,
    lane_m: f32,
    pub speed_mps: f32,
    /// Its station on the lap, m.
    pub progress_m: f32,
    pub pose: ScPose,
    /// The lights are out: it goes in at the end of this lap.
    pub in_this_lap: bool,
    /// The queue has formed (the pit lane opens).
    pub formed: bool,
    pub pit_open: bool,
    /// Its laps on the road (line crossings), and the count when the
    /// queue formed.
    pub laps: u32,
    formed_at_laps: u32,
    /// The leader's laps that end the restart: green as it reaches them.
    restart_laps: Option<f32>,
    /// Each car's distance behind the safety car last tick, m.
    last_behind_m: Vec<f32>,
    /// The cars being let by, in the order of the cars it is given, and
    /// the ones that were and have gone past it.
    pub let_by: Vec<bool>,
    went_round: Vec<bool>,
}

impl SafetyCar {
    /// Called out: waiting at the pit exit (on the line, at the start, on a
    /// circuit without a pit lane), the pit lane closed. Drives the racing
    /// line planned for `car`.
    pub fn deploy(track: &TrackConfig, car: Option<&CarConfig>) -> SafetyCar {
        let line = ScLine::plan(track, car);
        let total = crate::laps::track_length_m(track);
        let lane = track.pit_lane.as_ref();
        let (wait, entry) = match lane.and_then(|l| l.nodes.last().map(|n| (l, *n))) {
            Some((l, end)) => {
                let n = l.nodes.len();
                let before = l.nodes[n.saturating_sub(2)];
                let yaw = (end[1] - before[1]).atan2(end[0] - before[0]);
                (
                    ScPose {
                        x: end[0],
                        y: end[1],
                        z: end[2],
                        yaw_rad: yaw,
                        speed_mps: 0.0,
                    },
                    l.entry_station_m,
                )
            }
            None => {
                let start = track.centerline.first();
                let (x, y) = start.map_or((0.0, 0.0), |p| (p.x, p.y));
                let along = line.nearest_along(x, y);
                let (px, py, pz, yaw) = line.pose_at(along);
                (
                    ScPose {
                        x: px,
                        y: py,
                        z: pz,
                        yaw_rad: yaw,
                        speed_mps: 0.0,
                    },
                    0.0,
                )
            }
        };
        let join_along_m = line.nearest_along(wait.x, wait.y);
        let join_station_m = line.station_at(join_along_m, total);
        SafetyCar {
            stage: ScStage::Waiting,
            join_along_m,
            join_station_m,
            entry_station_m: entry,
            along_m: join_along_m,
            lane_m: 0.0,
            speed_mps: 0.0,
            progress_m: join_station_m,
            pose: wait,
            in_this_lap: false,
            formed: false,
            pit_open: false,
            laps: 0,
            formed_at_laps: 0,
            restart_laps: None,
            last_behind_m: Vec::new(),
            let_by: Vec::new(),
            went_round: Vec::new(),
            line,
        }
    }

    /// Its pose for telemetry: `None` once it is off the road.
    pub fn wire_pose(&self) -> Option<ScPose> {
        (self.stage != ScStage::Gone).then_some(self.pose)
    }

    /// One tick: the safety car moves, and each car is told its pace.
    /// `ramp_mps` is race control's pace coming down to [`SC_CATCH_MPS`]
    /// since it was called out; `road_clear` no car stranded for a while;
    /// `stranded_long` a car stranded long enough to call it out.
    #[allow(clippy::too_many_arguments)]
    pub fn tick(
        &mut self,
        cars: &[QueueCar],
        track: &TrackConfig,
        dt: f32,
        ramp_mps: f32,
        road_clear: bool,
        stranded_long: bool,
    ) -> (QueueOrders, Option<ScEvent>) {
        let total = crate::laps::track_length_m(track).max(1.0);
        let leader = cars
            .iter()
            .enumerate()
            .filter(|(_, c)| c.running)
            .max_by(|(ia, a), (ib, b)| a.laps.total_cmp(&b.laps).then(ib.cmp(ia)))
            .map(|(i, _)| i);
        let mut event = None;
        if self.in_this_lap && stranded_long && self.stage == ScStage::OnTrack {
            self.in_this_lap = false;
            event = Some(ScEvent::BackOut);
        }
        self.drive(cars, leader, track, total, dt);
        if matches!(self.stage, ScStage::InLane | ScStage::Gone) && self.restart_laps.is_none() {
            self.restart_laps = leader.map(|l| cars[l].laps.floor() + 1.0);
        }

        let mut orders = QueueOrders {
            caps: vec![0.0; cars.len()],
            let_by: vec![false; cars.len()],
            passed_sc: Vec::new(),
        };
        match self.stage {
            ScStage::Waiting => {
                for (cap, car) in orders.caps.iter_mut().zip(cars) {
                    if car.running {
                        *cap = ramp_mps;
                    }
                }
            }
            ScStage::OnTrack => {
                // A lapped car is seen from the tick after the queue formed:
                // the lights stay on until it has been let by.
                let was_formed = self.formed;
                let queued = self.queue(cars, total, ramp_mps, &mut orders);
                if !self.formed && (queued || self.laps >= SC_FORM_LAPS) {
                    self.formed = true;
                    self.pit_open = true;
                    self.formed_at_laps = self.laps;
                }
                let letting_by = orders.let_by.iter().any(|b| *b);
                if was_formed
                    && !self.in_this_lap
                    && road_clear
                    && !letting_by
                    && !stranded_long
                    && (queued || self.laps >= self.formed_at_laps + SC_FORM_LAPS)
                {
                    self.in_this_lap = true;
                    event = Some(ScEvent::InThisLap);
                }
            }
            ScStage::InLane | ScStage::Gone => {
                self.restart(cars, leader, total, ramp_mps, &mut orders);
                let green = match (leader, self.restart_laps) {
                    (Some(l), Some(at)) => cars[l].laps >= at,
                    (None, _) => true,
                    _ => false,
                };
                if green {
                    event = Some(ScEvent::Green);
                }
            }
        }
        (orders, event)
    }

    /// Move the safety car: out of the pit exit as the leader comes up to
    /// it, round the line at its pace, into the pit lane once in this lap.
    fn drive(
        &mut self,
        cars: &[QueueCar],
        leader: Option<usize>,
        track: &TrackConfig,
        total: f32,
        dt: f32,
    ) {
        match self.stage {
            ScStage::Waiting => {
                let coming = leader.is_some_and(|l| {
                    (self.join_station_m - cars[l].progress_m).rem_euclid(total) <= SC_PICKUP_M
                });
                if coming {
                    self.stage = ScStage::OnTrack;
                    self.along_m = self.join_along_m;
                    self.speed_mps = track
                        .pit_lane
                        .as_ref()
                        .map_or(SC_ACCEL_MPS2, |l| l.speed_limit_mps);
                    self.place_on_line(total);
                }
            }
            ScStage::OnTrack => {
                let target = self.line.pace_at(self.along_m);
                self.speed_mps = (self.speed_mps + SC_ACCEL_MPS2 * dt).min(target);
                let before = self.progress_m;
                self.along_m = (self.along_m + self.speed_mps * dt).rem_euclid(self.line.length_m);
                self.place_on_line(total);
                if self.progress_m < before - 0.5 * total {
                    self.laps += 1;
                }
                if self.in_this_lap {
                    let to = |p: f32| (self.entry_station_m - p).rem_euclid(total);
                    if to(self.progress_m) > to(before) + 0.5 * total {
                        match track.pit_lane.as_ref() {
                            Some(_) => {
                                self.stage = ScStage::InLane;
                                self.lane_m = 0.0;
                            }
                            None => self.stage = ScStage::Gone,
                        }
                    }
                }
            }
            ScStage::InLane => {
                let Some(lane) = track.pit_lane.as_ref() else {
                    self.stage = ScStage::Gone;
                    return;
                };
                let limit = lane.speed_limit_mps.max(1.0);
                self.speed_mps = if self.speed_mps > limit {
                    (self.speed_mps - SC_ACCEL_MPS2 * dt).max(limit)
                } else {
                    limit
                };
                self.lane_m += self.speed_mps * dt;
                if self.lane_m >= lane.limit_start_m.min(lane.length_m) {
                    self.stage = ScStage::Gone;
                    self.speed_mps = 0.0;
                    return;
                }
                self.pose = lane_pose(lane, self.lane_m, self.speed_mps);
            }
            ScStage::Gone => {}
        }
    }

    fn place_on_line(&mut self, total: f32) {
        let (x, y, z, yaw) = self.line.pose_at(self.along_m);
        self.pose = ScPose {
            x,
            y,
            z,
            yaw_rad: yaw,
            speed_mps: self.speed_mps,
        };
        self.progress_m = self.line.station_at(self.along_m, total);
    }

    /// The queue behind the safety car on the road: each car's pace, the
    /// lapped cars let by once it has formed, and the cars that went past
    /// it. Returns whether every car is in the queue.
    fn queue(
        &mut self,
        cars: &[QueueCar],
        total: f32,
        ramp_mps: f32,
        orders: &mut QueueOrders,
    ) -> bool {
        let behind: Vec<f32> = cars
            .iter()
            .map(|c| (self.progress_m - c.progress_m).rem_euclid(total))
            .collect();
        // Past the safety car: close behind it last tick, close ahead now.
        if self.last_behind_m.len() == cars.len() {
            for (i, car) in cars.iter().enumerate() {
                let was = self.last_behind_m[i];
                if car.running && was < SC_PASS_WINDOW_M && behind[i] > total - SC_PASS_WINDOW_M {
                    orders.passed_sc.push(i);
                }
            }
        }
        self.last_behind_m = behind.clone();

        // The cars on the road, nearest the safety car first.
        let mut order: Vec<usize> = (0..cars.len()).filter(|i| cars[*i].running).collect();
        order.sort_by(|a, b| behind[*a].total_cmp(&behind[*b]).then(a.cmp(b)));
        // A lapped car: one with a car behind it in the queue a lap or more
        // up on it. Let by once the queue has formed, until the lights go,
        // and let by until it has gone round to the back: well past the
        // safety car, nobody behind it a lap up.
        if self.let_by.len() != cars.len() {
            self.let_by = vec![false; cars.len()];
            self.went_round = vec![false; cars.len()];
        }
        let mut most_behind = f32::MIN;
        let mut lapped = vec![false; cars.len()];
        for i in order.iter().rev() {
            lapped[*i] = most_behind > cars[*i].laps + 0.5;
            most_behind = most_behind.max(cars[*i].laps);
        }
        if self.formed && !self.in_this_lap {
            for (let_by, lapped) in self.let_by.iter_mut().zip(&lapped) {
                *let_by |= *lapped;
            }
        }
        // Passing the safety car and the queue at the catch pace is no
        // offence.
        orders.passed_sc.retain(|p| !self.let_by[*p]);
        for (i, let_by) in self.let_by.iter_mut().enumerate() {
            let round = behind[i] > 0.5 * total && behind[i] < total - SC_PASS_WINDOW_M;
            if *let_by && cars[i].running && round && !lapped[i] {
                self.went_round[i] = true;
            }
            if !cars[i].running || (round && !lapped[i]) {
                *let_by = false;
            }
        }
        orders.let_by.clone_from(&self.let_by);
        for (i, let_by) in self.let_by.iter().enumerate() {
            if *let_by {
                orders.caps[i] = ramp_mps.min(SC_CATCH_MPS);
            }
        }
        // Single file: each car held behind the car ahead (the safety car
        // for the first), a stopped car no pace for the ones behind it.
        let mut front = (0.0f32, self.speed_mps);
        let mut queued = true;
        for i in order.into_iter().filter(|i| !orders.let_by[*i]) {
            let gap = behind[i] - front.0;
            // A car let by catches the queue up on its own: the lights do
            // not wait for it.
            queued &= gap <= SC_QUEUED_M || self.went_round[i];
            orders.caps[i] = ramp_mps
                .min(follow_cap_mps(front.1, gap, SC_GAP_M))
                .max(0.5);
            if cars[i].moving && cars[i].speed_mps >= SC_PACE_SETTER_MPS {
                front = (behind[i], cars[i].speed_mps);
            }
        }
        queued
    }

    /// The restart: the leader sets the pace, free in the restart zone; the
    /// rest keep station behind it.
    fn restart(
        &mut self,
        cars: &[QueueCar],
        leader: Option<usize>,
        total: f32,
        ramp_mps: f32,
        orders: &mut QueueOrders,
    ) {
        let Some(l) = leader else {
            return;
        };
        let to_line = total - cars[l].progress_m;
        let free = to_line <= SC_RESTART_ZONE_M;
        orders.caps[l] = if free { 0.0 } else { SC_SPEED_MPS };
        let behind: Vec<f32> = cars
            .iter()
            .map(|c| (cars[l].progress_m - c.progress_m).rem_euclid(total))
            .collect();
        let mut order: Vec<usize> = (0..cars.len())
            .filter(|i| cars[*i].running && *i != l)
            .collect();
        order.sort_by(|a, b| behind[*a].total_cmp(&behind[*b]).then(a.cmp(b)));
        let mut front = (0.0f32, cars[l].speed_mps);
        for i in order {
            let gap = behind[i] - front.0;
            let follow = follow_cap_mps(front.1, gap, SC_GAP_M);
            orders.caps[i] = if free { follow } else { follow.min(ramp_mps) }.max(0.5);
            if cars[i].moving && cars[i].speed_mps >= SC_PACE_SETTER_MPS {
                front = (behind[i], cars[i].speed_mps);
            }
        }
    }
}

/// A pose `along_m` down the pit lane.
fn lane_pose(lane: &crate::pit::PitLane, along_m: f32, speed_mps: f32) -> ScPose {
    let s = along_m.clamp(0.0, *lane.stations.last().unwrap_or(&0.0));
    let i = lane
        .stations
        .partition_point(|&v| v <= s)
        .clamp(1, lane.nodes.len().saturating_sub(1).max(1));
    let (a, b) = (lane.nodes[i - 1], lane.nodes[i.min(lane.nodes.len() - 1)]);
    let span = (lane.stations[i.min(lane.stations.len() - 1)] - lane.stations[i - 1]).max(1e-6);
    let t = ((s - lane.stations[i - 1]) / span).clamp(0.0, 1.0);
    ScPose {
        x: a[0] + (b[0] - a[0]) * t,
        y: a[1] + (b[1] - a[1]) * t,
        z: a[2] + (b[2] - a[2]) * t,
        yaw_rad: (b[1] - a[1]).atan2(b[0] - a[0]),
        speed_mps,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::TrackPoint;

    /// A round track of `total` m, its centerline every 5 m.
    fn ring(total: f32) -> TrackConfig {
        let n = (total / 5.0) as usize;
        let r = total / std::f32::consts::TAU;
        let centerline = (0..=n)
            .map(|i| {
                let a = i as f32 / n as f32 * std::f32::consts::TAU;
                TrackPoint {
                    x: r * a.cos(),
                    y: r * a.sin(),
                    distance_from_start_m: i as f32 * total / n as f32,
                    heading_rad: a + std::f32::consts::FRAC_PI_2,
                    ..Default::default()
                }
            })
            .collect();
        TrackConfig {
            centerline,
            ..Default::default()
        }
    }

    fn car(laps: f32, total: f32, speed: f32) -> QueueCar {
        QueueCar {
            laps,
            progress_m: laps.fract() * total,
            speed_mps: speed,
            running: true,
            moving: true,
        }
    }

    #[test]
    fn it_waits_for_the_leader_then_leads_the_queue() {
        let total = 3000.0;
        let track = ring(total);
        let mut sc = SafetyCar::deploy(&track, None);
        assert_eq!(sc.stage, ScStage::Waiting);
        assert!(!sc.pit_open, "the lane is shut while the field forms up");
        // The leader half a lap from the line: the safety car waits.
        let cars = [car(2.5, total, 40.0), car(2.45, total, 40.0)];
        let (orders, _) = sc.tick(&cars, &track, 0.01, 50.0, false, false);
        assert_eq!(sc.stage, ScStage::Waiting);
        assert_eq!(orders.caps, vec![50.0, 50.0]);
        // Coming up to it: out ahead of the leader.
        let cars = [car(2.96, total, 40.0), car(2.9, total, 40.0)];
        let (orders, _) = sc.tick(&cars, &track, 0.01, 50.0, false, false);
        assert_eq!(sc.stage, ScStage::OnTrack);
        assert!(orders.caps[0] < 50.0, "the leader held behind it");
        assert!(sc.wire_pose().is_some());
    }

    #[test]
    fn a_lapped_car_is_let_by_once_the_queue_has_formed() {
        let total = 3000.0;
        let track = ring(total);
        let mut sc = SafetyCar::deploy(&track, None);
        sc.stage = ScStage::OnTrack;
        sc.along_m = 1000.0;
        sc.speed_mps = 40.0;
        sc.place_on_line(total);
        let sc_at = sc.progress_m;
        let at = |behind: f32, laps: u32| {
            let p = (sc_at - behind).rem_euclid(total);
            QueueCar {
                laps: laps as f32 + p / total,
                progress_m: p,
                speed_mps: 40.0,
                running: true,
                moving: true,
            }
        };
        // Leader, a lapped car, then the second car on the lead lap.
        let cars = [at(24.0, 5), at(48.0, 4), at(72.0, 5)];
        let (orders, _) = sc.tick(&cars, &track, 0.0, 55.0, true, false);
        assert!(sc.formed && sc.pit_open);
        assert_eq!(orders.let_by, vec![false, false, false], "formed this tick");
        let (orders, event) = sc.tick(&cars, &track, 0.0, 55.0, true, false);
        assert_eq!(orders.let_by, vec![false, true, false]);
        assert_eq!(orders.caps[1], SC_CATCH_MPS);
        assert_eq!(event, None, "not in this lap while a car is let by");
        // The third car keeps station on the leader, not the lapped car.
        assert!(orders.caps[2] > 40.0, "{:?}", orders.caps);
        // Past the safety car and going round to the back: in this lap.
        let cars = [at(24.0, 5), at(total - 200.0, 4), at(48.0, 5)];
        let (_, event) = sc.tick(&cars, &track, 0.0, 55.0, true, false);
        assert_eq!(event, Some(ScEvent::InThisLap));
    }

    #[test]
    fn a_car_going_past_it_is_seen() {
        let total = 3000.0;
        let track = ring(total);
        let mut sc = SafetyCar::deploy(&track, None);
        sc.stage = ScStage::OnTrack;
        sc.along_m = 1000.0;
        sc.place_on_line(total);
        let p = sc.progress_m;
        let mut c = car(1.0, total, 40.0);
        c.progress_m = p - 5.0;
        sc.tick(&[c], &track, 0.0, 55.0, false, false);
        c.progress_m = p + 5.0;
        let (orders, _) = sc.tick(&[c], &track, 0.0, 55.0, false, false);
        assert_eq!(orders.passed_sc, vec![0]);
    }

    #[test]
    fn the_leader_restarts_the_race_at_the_line() {
        let total = 3000.0;
        let track = ring(total);
        let mut sc = SafetyCar::deploy(&track, None);
        sc.stage = ScStage::OnTrack;
        sc.formed = true;
        sc.in_this_lap = true;
        sc.along_m = total - 30.0;
        sc.speed_mps = 40.0;
        sc.place_on_line(total);
        let cars = [car(3.98, total, 40.0), car(3.97, total, 40.0)];
        // No pit lane: off the road at the line.
        let mut ticks = 0;
        while sc.stage != ScStage::Gone && ticks < 1000 {
            sc.tick(&cars, &track, 0.01, 55.0, true, false);
            ticks += 1;
        }
        assert_eq!(sc.stage, ScStage::Gone);
        assert!(sc.wire_pose().is_none());
        let (orders, event) = sc.tick(&cars, &track, 0.01, 55.0, true, false);
        assert_eq!(event, None);
        assert_eq!(orders.caps[0], 0.0, "free in the restart zone");
        assert!(orders.caps[1] > 0.0, "the rest keep station");
        let crossed = [car(4.001, total, 50.0), car(3.99, total, 50.0)];
        let (_, event) = sc.tick(&crossed, &track, 0.01, 55.0, true, false);
        assert_eq!(event, Some(ScEvent::Green));
    }
}
