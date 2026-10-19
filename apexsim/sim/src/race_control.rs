//! Race control: the flags a race shows its drivers.
//!
//! - **Yellow**, per marshal sector: the lap cut into sectors of
//!   [`MARSHAL_SECTOR_M`] from the line. A car stopped or crawling, spinning
//!   off and rejoining, backing up, or in contact is an incident: its own
//!   sector shows a double yellow when the car is on the road (in the way),
//!   a single one when it is off it, and the sector before it a single
//!   yellow, the warning. A sector's flag is held [`YELLOW_HOLD_S`] after
//!   its last cause has gone. A flag warns the drivers coming up on the
//!   incident: one raised by one car alone is not shown to that car (a car
//!   running wide is not slowed by its own yellow), and a car already past
//!   the rearmost incident in its sector no longer sees that sector's. A car passing
//!   another in a yellow sector is recorded ([`RaceControl::take_yellow_passes`]), for the penalties.
//! - **Blue**, per car: a car a lap or more up the road is within
//!   [`BLUE_GAP_S`] behind it.
//!
//! - **Virtual safety car** (`RaceRules::safety_car`, [`Course`]): a car
//!   stopped on the road for [`VSC_STOPPED_S`] neutralises the race. Every
//!   car is held to a pace coming down to [`VSC_SPEED_MPS`]
//!   ([`RaceControl::vsc_pace_mps`]), a pass anywhere counts as one under a
//!   yellow, the pit lane stays open. With the road clear for
//!   [`VSC_CLEAR_S`] it is ending for [`VSC_ENDING_S`], then green.
//! - **Safety car** (`SafetyCar::Full`, [`Course::Sc`]): the same trigger
//!   brings out a safety car on the road (`crate::safety_car`), which this
//!   holds from the call until the green; the pace before it reaches the
//!   field comes down to `safety_car::SC_CATCH_MPS` ([`RaceControl::sc_ramp_mps`]).
//! - **Penalties** ([`Penalty`]), for the offences the session's rules
//!   name (`data::RaceRules`): a jump start, a pass under a yellow, leaving
//!   a manual pit lane past its red light. Each is a time penalty added to
//!   the race time, a drive-through or a stop-go served in the pit lane
//!   within [`SERVE_WITHIN_LAPS`] laps, or else added as time
//!   ([`Penalty::convert`]).
//!
//! Everything here is worked out from the tick's car states alone (lap
//! positions, speeds, contact), with no chance and no clock, so a seeded
//! race shows the same flags every time it is run.

/// The length of a marshal sector, m.
pub const MARSHAL_SECTOR_M: f32 = 250.0;
/// Below this a car on the lap is crawling: an incident, m/s.
pub const INCIDENT_SPEED_MPS: f32 = 8.0;
/// A car is an incident only once it has been in trouble (crawling, off
/// the road, backing up or in contact) this long without a break, s: a
/// quick step over the white line or a brush raises no yellow, and a
/// real one is out a moment after it happens, as a marshal's would be.
pub const INCIDENT_CONFIRM_S: f32 = 1.5;
/// A sector's yellow stays out this long after its last cause left, s.
pub const YELLOW_HOLD_S: f32 = 5.0;
/// A car on a later lap within this many seconds behind (at its speed)
/// brings out the blue flag, s.
pub const BLUE_GAP_S: f32 = 1.5;
/// ... or within this many metres, whatever its speed.
pub const BLUE_GAP_MIN_M: f32 = 30.0;
/// A car this far past the rearmost incident in its sector has gone by
/// it: alongside is still at it, m.
pub const PAST_INCIDENT_M: f32 = 10.0;
/// The grid getting away is slow everywhere: for this long after the green
/// a slow car is no incident, s.
pub const START_GRACE_S: f32 = 10.0;
/// A car passed under a yellow that is going slower than this share of
/// the passer's speed is in trouble (the incident, or slowed by it): going
/// by it is no offence.
pub const YELLOW_PASS_SLOWER_SHARE: f32 = 0.7;
/// A pass under a yellow: a time penalty of this, s.
pub const YELLOW_PASS_PENALTY_S: u16 = 5;
/// Leaving a manual pit lane past its red light: a stop-go of this, s.
pub const PIT_EXIT_STOP_GO_S: u16 = 10;
/// A drive-through or stop-go must be served within this many laps of the
/// lap it was given on, or it is added as time.
pub const SERVE_WITHIN_LAPS: u16 = 3;
/// What a drive-through not served (or given too late to serve, or where
/// there is no pit lane) adds to the race time, s; a stop-go adds this and
/// its hold.
pub const DRIVE_THROUGH_TIME_S: u16 = 20;
/// A car that passed another under a yellow has this long to hand the
/// place back: still ahead of it after this, it is penalised; behind it
/// again, the pass is forgiven. The order swapping back and forth within
/// it (side by side) is one pass, s.
pub const YELLOW_PASS_HANDBACK_S: f32 = 10.0;

/// A car stopped on the road this long brings out the virtual safety car,
/// s.
pub const VSC_STOPPED_S: f32 = 8.0;
/// The pace every car is held to under the virtual safety car, m/s.
pub const VSC_SPEED_MPS: f32 = 35.0;
/// The pace comes down to it from this, m/s, at [`VSC_SLOW_MPS2`]: the
/// field slows over some seconds rather than braking as one.
pub const VSC_FROM_MPS: f32 = 90.0;
pub const VSC_SLOW_MPS2: f32 = 5.0;
/// With no car stopped on the road for this long the virtual safety car
/// is ending, s; and green after this, s.
pub const VSC_CLEAR_S: f32 = 5.0;
pub const VSC_ENDING_S: f32 = 5.0;

/// The race as a whole.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Course {
    #[default]
    Green,
    /// The virtual safety car is out.
    Vsc,
    /// It is ending: still held to the pace, green in [`VSC_ENDING_S`].
    VscEnding,
    /// The safety car is out (`crate::safety_car`): at the pit exit or
    /// leading the queue.
    Sc,
    /// Safety car in this lap: its lights out, it goes in, the leader
    /// restarts the race at the line.
    ScInThisLap,
}

/// What a car did wrong.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Offence {
    /// Moved off its slot before the lights went out (`race_start`).
    JumpStart = 0,
    /// Passed a car on its lap in a yellow sector.
    YellowPass = 1,
    /// Left a manual pit lane past its red exit light.
    PitExitRed = 2,
}

/// How a penalty is paid.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PenaltyKind {
    /// Seconds added to the race time.
    Time = 0,
    /// A drive down the pit lane at its limit, no stop.
    DriveThrough = 1,
    /// A stop in the car's box, held for the penalty's seconds, no work.
    StopGo = 2,
}

/// Where a penalty stands.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PenaltyStatus {
    /// A drive-through or stop-go still to serve.
    Owed = 0,
    /// Served in the pit lane.
    Served = 1,
    /// Counted in the race time: a time penalty, or one not served in time.
    Added = 2,
}

/// One penalty given to a car.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Penalty {
    pub offence: Offence,
    pub kind: PenaltyKind,
    /// A time penalty's seconds, a stop-go's hold.
    pub seconds: u16,
    /// The lap the car was on when it was given.
    pub lap: u16,
    pub tick: u32,
    pub status: PenaltyStatus,
}

impl Penalty {
    /// The penalty for `offence`, given on `lap`: a drive-through for a
    /// jump start, a time penalty for a pass under a yellow, a stop-go for
    /// a red-light pit exit. One that must be served where it cannot be
    /// (`can_serve`: no pit lane, or fewer than [`SERVE_WITHIN_LAPS`] laps
    /// left) is added as time at once.
    pub fn issue(offence: Offence, lap: u16, tick: u32, can_serve: bool) -> Penalty {
        let (kind, seconds) = match offence {
            Offence::JumpStart => (PenaltyKind::DriveThrough, 0),
            Offence::YellowPass => (PenaltyKind::Time, YELLOW_PASS_PENALTY_S),
            Offence::PitExitRed => (PenaltyKind::StopGo, PIT_EXIT_STOP_GO_S),
        };
        let mut penalty = Penalty {
            offence,
            kind,
            seconds,
            lap,
            tick,
            status: if kind == PenaltyKind::Time {
                PenaltyStatus::Added
            } else {
                PenaltyStatus::Owed
            },
        };
        if !can_serve {
            penalty.convert();
        }
        penalty
    }

    /// A drive-through or stop-go not served becomes time: what it would
    /// have cost in the lane ([`DRIVE_THROUGH_TIME_S`], and a stop-go's
    /// hold).
    pub fn convert(&mut self) {
        if self.status != PenaltyStatus::Owed {
            return;
        }
        self.seconds = match self.kind {
            PenaltyKind::Time => self.seconds,
            PenaltyKind::DriveThrough => DRIVE_THROUGH_TIME_S,
            PenaltyKind::StopGo => DRIVE_THROUGH_TIME_S + self.seconds,
        };
        self.kind = PenaltyKind::Time;
        self.status = PenaltyStatus::Added;
    }

    /// Whether it is still to serve, and by when it is too late: past the
    /// lap [`SERVE_WITHIN_LAPS`] after it was given.
    pub fn overdue(&self, lap: u16) -> bool {
        self.status == PenaltyStatus::Owed && lap > self.lap.saturating_add(SERVE_WITHIN_LAPS)
    }

    /// The seconds it adds to the race time.
    pub fn added_s(&self) -> u32 {
        if self.status == PenaltyStatus::Added {
            self.seconds as u32
        } else {
            0
        }
    }
}

/// The first penalty of `penalties` still to serve in the pit lane.
pub fn owed(penalties: &[Penalty]) -> Option<&Penalty> {
    penalties.iter().find(|p| p.status == PenaltyStatus::Owed)
}

/// A sector's flag.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
pub enum Yellow {
    #[default]
    None = 0,
    /// Take care, no overtaking: an incident ahead, or off the road here.
    Single = 1,
    /// Slow down, be ready to stop: a car on the road in this sector.
    Double = 2,
}

/// What race control reads of each car.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FlagCar {
    /// Laps run, the lap in progress as a share (`laps_run`).
    pub laps: f32,
    /// Stopped or crawling on the road (or broken down on it, still
    /// rolling), out of the pit lane and its garage, not yet towed: what
    /// brings out the virtual safety car.
    pub stranded: bool,
    /// Where along the lap, m.
    pub progress_m: f32,
    pub speed_mps: f32,
    pub on_road: bool,
    pub colliding: bool,
    pub reversing: bool,
    /// In the pit lane, finished, towed or in its garage: not racing here.
    pub out_of_it: bool,
    /// Let by under the safety car (`crate::safety_car`): it may pass and
    /// be passed.
    pub free: bool,
}

/// One car passing another under a yellow: the tick, the sector and both
/// cars, by their place in the slice the tick was given.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct YellowPass {
    pub tick: u32,
    pub sector: u16,
    pub passer: usize,
    pub passed: usize,
}

/// The flags of one race.
#[derive(Debug, Clone, Default)]
pub struct RaceControl {
    /// Each sector's flag now and how long it is held, s.
    yellow: Vec<Yellow>,
    hold_s: Vec<f32>,
    /// The one car (by its place in the update's cars) every cause of each
    /// sector's flag came from, or `None` when more than one did.
    cause_car: Vec<Option<usize>>,
    /// Where in each sector its rearmost cause is, m from the sector's
    /// start (0 for the warning before an incident: all of it).
    from_m: Vec<f32>,
    /// The car count of the last update: places mean other cars after a
    /// change.
    car_count: usize,
    /// How long each car has been in trouble without a break, s
    /// ([`INCIDENT_CONFIRM_S`]).
    trouble_s: Vec<f32>,
    /// Each car's blue flag, in the order of the last update's cars.
    blue: Vec<bool>,
    /// Race order (laps run) of each car at the last update, for passes.
    last_laps: Vec<f32>,
    pub yellow_passes: Vec<YellowPass>,
    /// The virtual safety car: out, ending or not, and how long in that
    /// state, s.
    pub course: Course,
    course_s: f32,
    /// Under it, how long no car has been stranded, s.
    clear_s: f32,
    /// How long each car has been stranded, s.
    stranded_s: Vec<f32>,
    /// The safety car on the road, from its call to the green
    /// (`crate::safety_car`).
    pub safety_car: Option<crate::safety_car::SafetyCar>,
}

impl RaceControl {
    /// The sector of a lap position, on a lap of `total_m`.
    pub fn sector_of(progress_m: f32, total_m: f32) -> usize {
        let count = sector_count(total_m);
        ((progress_m.rem_euclid(total_m.max(1.0)) / MARSHAL_SECTOR_M) as usize).min(count - 1)
    }

    /// Every sector showing a yellow, as (sector, flag).
    pub fn yellows(&self) -> impl Iterator<Item = (usize, Yellow)> + '_ {
        self.yellow
            .iter()
            .enumerate()
            .filter(|(_, y)| **y != Yellow::None)
            .map(|(i, y)| (i, *y))
    }

    /// The flag of sector `sector`.
    pub fn yellow_at(&self, sector: usize) -> Yellow {
        self.yellow.get(sector).copied().unwrap_or_default()
    }

    /// The worst yellow a car at `progress_m` sees: its own sector's and
    /// the next one's.
    pub fn yellow_ahead(&self, progress_m: f32, total_m: f32) -> Yellow {
        if self.yellow.is_empty() {
            return Yellow::None;
        }
        let here = Self::sector_of(progress_m, total_m);
        let next = (here + 1) % self.yellow.len();
        self.yellow_at(here).max(self.yellow_at(next))
    }

    /// [`Self::yellow_ahead`] as the `index`th car of the last update sees
    /// it: without a flag that car alone raised, nor its own sector's once
    /// past the incident there.
    pub fn yellow_ahead_for(&self, index: usize, progress_m: f32, total_m: f32) -> Yellow {
        if self.yellow.is_empty() {
            return Yellow::None;
        }
        let here = Self::sector_of(progress_m, total_m);
        let next = (here + 1) % self.yellow.len();
        let seen = |sector: usize| {
            if self.cause_car.get(sector).copied().flatten() == Some(index) {
                Yellow::None
            } else {
                self.yellow_at(sector)
            }
        };
        let in_sector = progress_m.rem_euclid(total_m.max(1.0)) - here as f32 * MARSHAL_SECTOR_M;
        let past = self
            .from_m
            .get(here)
            .is_some_and(|from| in_sector > from + PAST_INCIDENT_M);
        let own = if past { Yellow::None } else { seen(here) };
        own.max(seen(next))
    }

    /// Whether the `index`th car of the last update is shown the blue flag.
    pub fn blue(&self, index: usize) -> bool {
        self.blue.get(index).copied().unwrap_or(false)
    }

    /// No flags at all (before the start, after a reset).
    pub fn clear(&mut self) {
        *self = Self::default();
    }

    /// The race as a whole, this tick: whether a stranded car brings out
    /// the virtual safety car or the safety car (`rule`, the session's),
    /// how long it has been out, the virtual one's end. The safety car's
    /// own course ([`Course::ScInThisLap`], the green) is the session's to
    /// set from `crate::safety_car::SafetyCar::tick`.
    pub fn update_course(&mut self, cars: &[FlagCar], dt: f32, rule: crate::data::SafetyCar) {
        use crate::data::SafetyCar;
        if self.stranded_s.len() != cars.len() {
            self.stranded_s = vec![0.0; cars.len()];
        }
        for (held, car) in self.stranded_s.iter_mut().zip(cars) {
            *held = if car.stranded { *held + dt } else { 0.0 };
        }
        let any = cars.iter().any(|c| c.stranded);
        let long = self.stranded_s.iter().any(|s| *s >= VSC_STOPPED_S);
        self.course_s += dt;
        match self.course {
            Course::Green => {
                let out = match rule {
                    SafetyCar::Off => None,
                    SafetyCar::Virtual => Some(Course::Vsc),
                    SafetyCar::Full => Some(Course::Sc),
                };
                if let Some(out) = out.filter(|_| long) {
                    self.course = out;
                    self.course_s = 0.0;
                    self.clear_s = 0.0;
                }
            }
            Course::Sc | Course::ScInThisLap => {
                self.clear_s = if any { 0.0 } else { self.clear_s + dt };
            }
            Course::Vsc => {
                self.clear_s = if any { 0.0 } else { self.clear_s + dt };
                if self.clear_s >= VSC_CLEAR_S {
                    self.course = Course::VscEnding;
                    self.course_s = 0.0;
                }
            }
            Course::VscEnding => {
                if long {
                    self.course = Course::Vsc;
                    self.clear_s = 0.0;
                } else if self.course_s >= VSC_ENDING_S {
                    self.course = Course::Green;
                    self.course_s = 0.0;
                }
            }
        }
    }

    /// The pace the virtual safety car holds every car to, m/s: coming down
    /// from [`VSC_FROM_MPS`] to [`VSC_SPEED_MPS`] since it came out; 0
    /// (none) on a green course or under the safety car.
    pub fn vsc_pace_mps(&self) -> f32 {
        match self.course {
            Course::Green | Course::Sc | Course::ScInThisLap => 0.0,
            Course::Vsc => (VSC_FROM_MPS - VSC_SLOW_MPS2 * self.course_s).max(VSC_SPEED_MPS),
            Course::VscEnding => VSC_SPEED_MPS,
        }
    }

    /// Under the safety car, the pace a car not yet in its queue is held
    /// to, m/s: coming down from [`VSC_FROM_MPS`] at [`VSC_SLOW_MPS2`] to
    /// `safety_car::SC_CATCH_MPS` since the call; 0 under no safety car.
    pub fn sc_ramp_mps(&self) -> f32 {
        use crate::safety_car::SC_CATCH_MPS;
        match self.course {
            Course::Sc => (VSC_FROM_MPS - VSC_SLOW_MPS2 * self.course_s).max(SC_CATCH_MPS),
            Course::ScInThisLap => SC_CATCH_MPS,
            _ => 0.0,
        }
    }

    /// No car stranded on the road for [`VSC_CLEAR_S`].
    pub fn road_clear(&self) -> bool {
        self.clear_s >= VSC_CLEAR_S
    }

    /// A car stranded long enough to call a safety car out.
    pub fn stranded_long(&self) -> bool {
        self.stranded_s.iter().any(|s| *s >= VSC_STOPPED_S)
    }

    /// The pit lane is closed: the safety car is out and its queue has not
    /// formed yet.
    pub fn pit_closed(&self) -> bool {
        self.safety_car.as_ref().is_some_and(|sc| !sc.pit_open)
    }

    /// Set the course from the safety car's tick.
    pub fn set_course(&mut self, course: Course) {
        if self.course != course {
            self.course = course;
            self.course_s = 0.0;
        }
    }

    /// The passes under a yellow recorded since the last call.
    pub fn take_yellow_passes(&mut self) -> Vec<YellowPass> {
        std::mem::take(&mut self.yellow_passes)
    }

    /// Advance the flags by `dt` over this tick's `cars` on a lap of
    /// `total_m`. While `launching` (the first seconds after the green) a
    /// car is not an incident for being slow: the whole grid is.
    pub fn update(&mut self, cars: &[FlagCar], total_m: f32, dt: f32, tick: u32, launching: bool) {
        if total_m <= 0.0 {
            return;
        }
        let count = sector_count(total_m);
        if self.yellow.len() != count {
            self.yellow = vec![Yellow::None; count];
            self.hold_s = vec![0.0; count];
            self.cause_car = vec![None; count];
            self.from_m = vec![0.0; count];
        }
        if self.car_count != cars.len() {
            // Places now name other cars: every flag is everyone's.
            self.cause_car.iter_mut().for_each(|c| *c = None);
            self.car_count = cars.len();
        }
        if self.trouble_s.len() != cars.len() {
            self.trouble_s = vec![0.0; cars.len()];
        }
        // This tick's causes, and the one car behind each (`None` if more).
        let mut cause = vec![Yellow::None; count];
        let mut by: Vec<Option<usize>> = vec![None; count];
        let mut at = vec![f32::MAX; count];
        let mut add = |sector: usize, level: Yellow, car: usize, from_m: f32| {
            at[sector] = at[sector].min(from_m);
            by[sector] = if cause[sector] == Yellow::None || by[sector] == Some(car) {
                Some(car)
            } else {
                None
            };
            cause[sector] = cause[sector].max(level);
        };
        for (i, car) in cars.iter().enumerate() {
            let trouble = !car.out_of_it
                && ((car.speed_mps < INCIDENT_SPEED_MPS && !launching)
                    || !car.on_road
                    || car.reversing
                    || car.colliding);
            self.trouble_s[i] = if trouble { self.trouble_s[i] + dt } else { 0.0 };
            if self.trouble_s[i] < INCIDENT_CONFIRM_S {
                continue;
            }
            let here = Self::sector_of(car.progress_m, total_m);
            let level = if car.on_road {
                Yellow::Double
            } else {
                Yellow::Single
            };
            let in_sector = car.progress_m.rem_euclid(total_m) - here as f32 * MARSHAL_SECTOR_M;
            add(here, level, i, in_sector);
            add((here + count - 1) % count, Yellow::Single, i, 0.0);
        }
        for ((((cause, by), at), flag), ((hold, cause_car), from)) in cause
            .iter()
            .zip(by)
            .zip(at)
            .zip(self.yellow.iter_mut())
            .zip(
                self.hold_s
                    .iter_mut()
                    .zip(self.cause_car.iter_mut())
                    .zip(self.from_m.iter_mut()),
            )
        {
            if *cause != Yellow::None {
                // The rearmost incident while the flag is out.
                *from = if *flag == Yellow::None {
                    at
                } else {
                    from.min(at)
                };
                // One car's flag while it alone has caused it.
                *cause_car = match (*flag, *cause_car) {
                    (Yellow::None, _) => by,
                    (_, held) if held == by => by,
                    _ => None,
                };
                // A new cause raises the flag at once; a lesser one holds it.
                *flag = (*flag).max(*cause);
                if cause == flag {
                    *hold = YELLOW_HOLD_S;
                }
            } else if *flag != Yellow::None {
                *hold -= dt;
                if *hold <= 0.0 {
                    *flag = Yellow::None;
                    *cause_car = None;
                }
            }
        }

        // Blue: a car a lap or more up within reach behind.
        self.blue = cars
            .iter()
            .map(|me| {
                !me.out_of_it
                    && cars.iter().any(|o| {
                        if o.out_of_it || o.laps < me.laps + 0.5 {
                            return false;
                        }
                        let behind = (me.progress_m - o.progress_m).rem_euclid(total_m);
                        behind > 0.0 && behind < (o.speed_mps * BLUE_GAP_S).max(BLUE_GAP_MIN_M)
                    })
            })
            .collect();

        // Passes under a yellow: two cars that swapped order on the road
        // this tick, the passer in a yellow sector, neither out of it nor
        // a lapping car going by (that is what the blue is for).
        if self.last_laps.len() == cars.len() {
            for (a, ca) in cars.iter().enumerate() {
                if ca.out_of_it || ca.free {
                    continue;
                }
                let sector = Self::sector_of(ca.progress_m, total_m);
                if self.yellow[sector] == Yellow::None && self.course == Course::Green {
                    continue;
                }
                for (b, cb) in cars.iter().enumerate() {
                    if a == b || cb.out_of_it || cb.free || (ca.laps - cb.laps).abs() > 0.5 {
                        continue;
                    }
                    // Going by the incident, or a car slowed by it, is
                    // what the yellow is there for.
                    let in_trouble = cb.speed_mps < INCIDENT_SPEED_MPS
                        || !cb.on_road
                        || cb.reversing
                        || cb.speed_mps < YELLOW_PASS_SLOWER_SHARE * ca.speed_mps;
                    if in_trouble {
                        continue;
                    }
                    if self.last_laps[a] < self.last_laps[b] && ca.laps > cb.laps {
                        self.yellow_passes.push(YellowPass {
                            tick,
                            sector: sector as u16,
                            passer: a,
                            passed: b,
                        });
                    }
                }
            }
        }
        self.last_laps = cars.iter().map(|c| c.laps).collect();
    }
}

fn sector_count(total_m: f32) -> usize {
    ((total_m / MARSHAL_SECTOR_M).ceil() as usize).max(1)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::SafetyCar;

    const LAP: f32 = 5000.0;

    fn racing(laps: f32, speed: f32) -> FlagCar {
        FlagCar {
            laps,
            stranded: false,
            progress_m: laps.fract() * LAP,
            speed_mps: speed,
            on_road: true,
            colliding: false,
            reversing: false,
            out_of_it: false,
            free: false,
        }
    }

    #[test]
    fn a_car_stopped_on_the_road_brings_out_a_double_yellow_and_a_warning() {
        let mut rc = RaceControl::default();
        let stopped = racing(1.21, 0.0); // 1050 m: sector 4
        rc.update(
            &[racing(1.1, 60.0), stopped],
            LAP,
            INCIDENT_CONFIRM_S,
            1,
            false,
        );
        assert_eq!(rc.yellow_at(4), Yellow::Double);
        assert_eq!(rc.yellow_at(3), Yellow::Single);
        assert_eq!(rc.yellow_at(5), Yellow::None);
        assert_eq!(rc.yellow_ahead(600.0, LAP), Yellow::Single);
        assert_eq!(
            rc.yellow_ahead(800.0, LAP),
            Yellow::Double,
            "the next sector's"
        );
        assert_eq!(rc.yellow_ahead(1000.0, LAP), Yellow::Double);
        assert_eq!(rc.yellows().count(), 2);
    }

    #[test]
    fn off_the_road_is_a_single_yellow_held_after_the_car_has_gone() {
        let mut rc = RaceControl::default();
        let mut off = racing(1.21, 30.0);
        off.on_road = false;
        rc.update(&[off], LAP, INCIDENT_CONFIRM_S, 1, false);
        assert_eq!(rc.yellow_at(4), Yellow::Single);
        let back = racing(1.21, 30.0);
        for t in 0..((YELLOW_HOLD_S / 0.1) as u32 - 1) {
            rc.update(&[back], LAP, 0.1, t + 2, false);
        }
        assert_eq!(rc.yellow_at(4), Yellow::Single, "held");
        rc.update(&[back], LAP, 0.1, 100, false);
        rc.update(&[back], LAP, 0.1, 101, false);
        assert_eq!(rc.yellow_at(4), Yellow::None, "cleared");
    }

    #[test]
    fn a_car_is_not_shown_the_yellow_it_alone_raised() {
        let mut rc = RaceControl::default();
        let mut wide = racing(1.21, 30.0); // 1050 m: sector 4
        wide.on_road = false;
        let behind = racing(1.19, 40.0); // 950 m: sector 3
        rc.update(&[behind, wide], LAP, INCIDENT_CONFIRM_S, 1, false);
        assert_eq!(rc.yellow_ahead_for(1, wide.progress_m, LAP), Yellow::None);
        assert_eq!(
            rc.yellow_ahead_for(0, behind.progress_m, LAP),
            Yellow::Single,
            "the car coming up on it is warned"
        );
        // Back on the road, the flag held: still its own.
        let back = racing(1.2105, 30.0);
        rc.update(&[behind, back], LAP, 0.1, 2, false);
        assert_eq!(rc.yellow_ahead_for(1, back.progress_m, LAP), Yellow::None);
        // A second car off in the same sector: the flag is everyone's.
        let mut also = racing(1.2112, 30.0); // beside it
        also.on_road = false;
        rc.update(&[also, back], LAP, INCIDENT_CONFIRM_S, 3, false);
        assert_eq!(rc.yellow_ahead_for(1, back.progress_m, LAP), Yellow::Single);
        assert_eq!(rc.yellow_ahead_for(0, also.progress_m, LAP), Yellow::Single);
    }

    #[test]
    fn a_car_past_the_incident_no_longer_sees_its_sector_yellow() {
        let mut rc = RaceControl::default();
        let stopped = racing(1.21, 0.0); // 1050 m: 50 m into sector 4
        rc.update(&[stopped], LAP, INCIDENT_CONFIRM_S, 1, false);
        assert_eq!(rc.yellow_ahead_for(1, 1040.0, LAP), Yellow::Double);
        assert_eq!(
            rc.yellow_ahead_for(1, 1055.0, LAP),
            Yellow::Double,
            "alongside"
        );
        assert_eq!(rc.yellow_ahead_for(1, 1100.0, LAP), Yellow::None, "gone by");
        assert_eq!(rc.yellow_ahead_for(1, 900.0, LAP), Yellow::Double, "warned");
        // The flag held after the car has gone stays where it was.
        let gone = racing(1.3, 50.0);
        rc.update(&[gone], LAP, 0.1, 2, false);
        assert_eq!(rc.yellow_ahead_for(1, 1040.0, LAP), Yellow::Double);
        assert_eq!(rc.yellow_ahead_for(1, 1100.0, LAP), Yellow::None);
    }

    #[test]
    fn a_grid_getting_away_is_no_incident() {
        let mut rc = RaceControl::default();
        rc.update(&[racing(0.99, 2.0), racing(0.98, 1.0)], LAP, 0.01, 1, true);
        assert_eq!(rc.yellows().count(), 0);
    }

    #[test]
    fn a_car_in_the_pits_or_finished_brings_out_nothing() {
        let mut rc = RaceControl::default();
        let mut pits = racing(1.21, 0.0);
        pits.out_of_it = true;
        rc.update(&[pits], LAP, 0.1, 1, false);
        assert_eq!(rc.yellows().count(), 0);
    }

    #[test]
    fn a_leader_closing_shows_the_backmarker_blue() {
        let mut rc = RaceControl::default();
        // The backmarker at 2000 m on lap 1, the leader 40 m behind it a lap up.
        let backmarker = racing(1.4, 50.0);
        let leader = racing(2.392, 60.0);
        let rival = racing(1.395, 50.0); // 15 m ahead of the leader
        rc.update(&[backmarker, leader, rival], LAP, 0.01, 1, false);
        assert!(rc.blue(0), "lapped car warned");
        assert!(!rc.blue(1), "the leader is not");
        assert!(rc.blue(2), "a car racing it behind is warned as well");
        let far = racing(2.3, 60.0); // 500 m behind
        rc.update(&[backmarker, far], LAP, 0.01, 2, false);
        assert!(!rc.blue(0), "too far back");
        let same_lap = racing(1.392, 60.0);
        rc.update(&[backmarker, same_lap], LAP, 0.01, 3, false);
        assert!(!rc.blue(0), "racing for position");
    }

    #[test]
    fn a_pass_in_a_yellow_sector_is_recorded() {
        let mut rc = RaceControl::default();
        let stopped = racing(1.25, 0.0); // 1250 m: sector 5, 4 warned
        rc.update(
            &[racing(1.19, 50.0), racing(1.2, 45.0), stopped],
            LAP,
            INCIDENT_CONFIRM_S,
            1,
            false,
        );
        assert_eq!(rc.yellow_at(4), Yellow::Single);
        rc.update(
            &[racing(1.201, 50.0), racing(1.2005, 45.0), stopped],
            LAP,
            0.01,
            2,
            false,
        );
        assert_eq!(rc.yellow_passes.len(), 1);
        let pass = rc.yellow_passes[0];
        assert_eq!(
            (pass.passer, pass.passed, pass.sector, pass.tick),
            (0, 1, 4, 2)
        );
    }

    #[test]
    fn going_by_the_incident_under_a_yellow_is_no_pass() {
        let mut rc = RaceControl::default();
        // The car passed is the stopped one: no offence.
        let stopped = racing(1.2, 0.0);
        rc.update(&[racing(1.19, 50.0), stopped], LAP, 0.01, 1, false);
        rc.update(&[racing(1.201, 50.0), stopped], LAP, 0.01, 2, false);
        assert!(rc.take_yellow_passes().is_empty());
        // Nor one crawling away from it at a third of the passer's speed.
        let mut rc = RaceControl::default();
        let incident = racing(1.25, 0.0);
        rc.update(
            &[racing(1.19, 50.0), racing(1.2, 15.0), incident],
            LAP,
            0.01,
            1,
            false,
        );
        rc.update(
            &[racing(1.201, 50.0), racing(1.2003, 15.0), incident],
            LAP,
            0.01,
            2,
            false,
        );
        assert!(rc.take_yellow_passes().is_empty());
    }

    #[test]
    fn a_penalty_is_served_or_added_as_time() {
        let pass = Penalty::issue(Offence::YellowPass, 4, 10, true);
        assert_eq!(
            (pass.kind, pass.status, pass.added_s()),
            (
                PenaltyKind::Time,
                PenaltyStatus::Added,
                YELLOW_PASS_PENALTY_S as u32
            )
        );
        let mut jump = Penalty::issue(Offence::JumpStart, 1, 10, true);
        assert_eq!(
            (jump.kind, jump.status),
            (PenaltyKind::DriveThrough, PenaltyStatus::Owed)
        );
        assert_eq!(jump.added_s(), 0);
        assert!(!jump.overdue(1 + SERVE_WITHIN_LAPS));
        assert!(jump.overdue(2 + SERVE_WITHIN_LAPS));
        jump.convert();
        assert_eq!(jump.added_s(), DRIVE_THROUGH_TIME_S as u32);
        let late = Penalty::issue(Offence::PitExitRed, 9, 10, false);
        assert_eq!(
            late.added_s(),
            (DRIVE_THROUGH_TIME_S + PIT_EXIT_STOP_GO_S) as u32,
            "a stop-go too late to serve"
        );
        let list = [pass, Penalty::issue(Offence::PitExitRed, 5, 11, true)];
        assert_eq!(owed(&list).map(|p| p.kind), Some(PenaltyKind::StopGo));
    }

    #[test]
    fn a_stranded_car_brings_out_the_virtual_safety_car_until_the_road_is_clear() {
        let mut rc = RaceControl::default();
        let mut stuck = racing(1.2, 0.0);
        stuck.stranded = true;
        let dt = 0.1;
        let steps = |s: f32| (s / dt).round() as usize;
        for _ in 0..steps(VSC_STOPPED_S) - 1 {
            rc.update_course(&[racing(1.1, 50.0), stuck], dt, SafetyCar::Virtual);
        }
        assert_eq!(rc.course, Course::Green);
        rc.update_course(&[racing(1.1, 50.0), stuck], dt, SafetyCar::Virtual);
        rc.update_course(&[racing(1.1, 50.0), stuck], dt, SafetyCar::Virtual);
        assert_eq!(rc.course, Course::Vsc);
        assert!(rc.vsc_pace_mps() > VSC_SPEED_MPS, "coming down");
        for _ in 0..steps(20.0) {
            rc.update_course(&[racing(1.1, 50.0), stuck], dt, SafetyCar::Virtual);
        }
        assert_eq!(rc.vsc_pace_mps(), VSC_SPEED_MPS);
        // Towed away: clear, ending, green.
        let gone = racing(1.2, 0.0);
        for _ in 0..steps(VSC_CLEAR_S) + 1 {
            rc.update_course(&[racing(1.1, 50.0), gone], dt, SafetyCar::Virtual);
        }
        assert_eq!(rc.course, Course::VscEnding);
        assert_eq!(rc.vsc_pace_mps(), VSC_SPEED_MPS);
        for _ in 0..steps(VSC_ENDING_S) + 1 {
            rc.update_course(&[racing(1.1, 50.0), gone], dt, SafetyCar::Virtual);
        }
        assert_eq!(rc.course, Course::Green);
        assert_eq!(rc.vsc_pace_mps(), 0.0);
        // Without the rule a stranded car brings out yellows only.
        let mut rc = RaceControl::default();
        for _ in 0..steps(30.0) {
            rc.update_course(&[stuck], dt, SafetyCar::Off);
        }
        assert_eq!(rc.course, Course::Green);
    }

    #[test]
    fn a_pass_anywhere_under_the_vsc_is_a_pass_under_a_yellow() {
        let mut rc = RaceControl {
            course: Course::Vsc,
            ..Default::default()
        };
        rc.update(
            &[racing(1.59, 35.0), racing(1.6, 30.0)],
            LAP,
            0.01,
            1,
            false,
        );
        rc.update(
            &[racing(1.601, 35.0), racing(1.6005, 30.0)],
            LAP,
            0.01,
            2,
            false,
        );
        assert_eq!(rc.take_yellow_passes().len(), 1);
    }

    /// A quick step over the white line, or a brush, is no incident: the
    /// trouble has to last `INCIDENT_CONFIRM_S` without a break.
    #[test]
    fn a_quick_step_off_the_road_raises_no_yellow() {
        let mut rc = RaceControl::default();
        let mut off = racing(1.21, 45.0);
        off.on_road = false;
        let back = racing(1.212, 45.0);
        let dt = 0.1;
        // A second off, then back on: nothing.
        for t in 0..10 {
            rc.update(&[off], LAP, dt, t, false);
        }
        rc.update(&[back], LAP, dt, 10, false);
        assert_eq!(rc.yellows().count(), 0, "a wheel off for a second");
        // Off again: the count starts over, and the flag is out once it
        // has lasted.
        let steps = (INCIDENT_CONFIRM_S / dt).ceil() as u32;
        for t in 0..steps - 1 {
            rc.update(&[off], LAP, dt, 11 + t, false);
        }
        assert_eq!(rc.yellows().count(), 0, "not yet");
        rc.update(&[off], LAP, dt, 11 + steps, false);
        assert_eq!(rc.yellow_at(4), Yellow::Single, "off long enough");
    }

    #[test]
    fn the_last_sector_warns_across_the_line() {
        let mut rc = RaceControl::default();
        rc.update(&[racing(2.01, 0.0)], LAP, INCIDENT_CONFIRM_S, 1, false);
        assert_eq!(rc.yellow_at(0), Yellow::Double);
        assert_eq!(rc.yellow_at(19), Yellow::Single);
        assert_eq!(RaceControl::sector_of(4999.0, LAP), 19);
        assert_eq!(RaceControl::sector_of(5000.0, LAP), 0);
    }
}
