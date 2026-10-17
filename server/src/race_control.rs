//! Race control: the flags a race shows its drivers.
//!
//! - **Yellow**, per marshal sector: the lap cut into sectors of
//!   [`MARSHAL_SECTOR_M`] from the line. A car stopped or crawling, spinning
//!   off and rejoining, backing up, or in contact is an incident: its own
//!   sector shows a double yellow when the car is on the road (in the way),
//!   a single one when it is off it, and the sector before it a single
//!   yellow, the warning. A sector's flag is held [`YELLOW_HOLD_S`] after
//!   its last cause has gone. A car passing another in a yellow sector is
//!   recorded ([`RaceControl::yellow_passes`]), for penalties to come.
//! - **Blue**, per car: a car a lap or more up the road is within
//!   [`BLUE_GAP_S`] behind it.
//!
//! Everything here is worked out from the tick's car states alone (lap
//! positions, speeds, contact), with no chance and no clock, so a seeded
//! race shows the same flags every time it is run.

/// The length of a marshal sector, m.
pub const MARSHAL_SECTOR_M: f32 = 250.0;
/// Below this a car on the lap is crawling: an incident, m/s.
pub const INCIDENT_SPEED_MPS: f32 = 8.0;
/// A sector's yellow stays out this long after its last cause left, s.
pub const YELLOW_HOLD_S: f32 = 5.0;
/// A car on a later lap within this many seconds behind (at its speed)
/// brings out the blue flag, s.
pub const BLUE_GAP_S: f32 = 1.5;
/// ... or within this many metres, whatever its speed.
pub const BLUE_GAP_MIN_M: f32 = 30.0;
/// The grid getting away is slow everywhere: for this long after the green
/// a slow car is no incident, s.
pub const START_GRACE_S: f32 = 10.0;

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
    /// Where along the lap, m.
    pub progress_m: f32,
    pub speed_mps: f32,
    pub on_road: bool,
    pub colliding: bool,
    pub reversing: bool,
    /// In the pit lane, finished, towed or in its garage: not racing here.
    pub out_of_it: bool,
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
    /// Each car's blue flag, in the order of the last update's cars.
    blue: Vec<bool>,
    /// Race order (laps run) of each car at the last update, for passes.
    last_laps: Vec<f32>,
    pub yellow_passes: Vec<YellowPass>,
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

    /// Whether the `index`th car of the last update is shown the blue flag.
    pub fn blue(&self, index: usize) -> bool {
        self.blue.get(index).copied().unwrap_or(false)
    }

    /// No flags at all (before the start, after a reset).
    pub fn clear(&mut self) {
        *self = Self::default();
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
        }
        // This tick's causes.
        let mut cause = vec![Yellow::None; count];
        for car in cars.iter().filter(|c| !c.out_of_it) {
            let incident = (car.speed_mps < INCIDENT_SPEED_MPS && !launching)
                || !car.on_road
                || car.reversing
                || car.colliding;
            if !incident {
                continue;
            }
            let here = Self::sector_of(car.progress_m, total_m);
            let level = if car.on_road {
                Yellow::Double
            } else {
                Yellow::Single
            };
            cause[here] = cause[here].max(level);
            let before = (here + count - 1) % count;
            cause[before] = cause[before].max(Yellow::Single);
        }
        for ((cause, flag), hold) in cause
            .iter()
            .zip(self.yellow.iter_mut())
            .zip(self.hold_s.iter_mut())
        {
            if *cause != Yellow::None {
                // A new cause raises the flag at once; a lesser one holds it.
                *flag = (*flag).max(*cause);
                if cause == flag {
                    *hold = YELLOW_HOLD_S;
                }
            } else if *flag != Yellow::None {
                *hold -= dt;
                if *hold <= 0.0 {
                    *flag = Yellow::None;
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
                if ca.out_of_it {
                    continue;
                }
                let sector = Self::sector_of(ca.progress_m, total_m);
                if self.yellow[sector] == Yellow::None {
                    continue;
                }
                for (b, cb) in cars.iter().enumerate() {
                    if a == b || cb.out_of_it || (ca.laps - cb.laps).abs() > 0.5 {
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

    const LAP: f32 = 5000.0;

    fn racing(laps: f32, speed: f32) -> FlagCar {
        FlagCar {
            laps,
            progress_m: laps.fract() * LAP,
            speed_mps: speed,
            on_road: true,
            colliding: false,
            reversing: false,
            out_of_it: false,
        }
    }

    #[test]
    fn a_car_stopped_on_the_road_brings_out_a_double_yellow_and_a_warning() {
        let mut rc = RaceControl::default();
        let stopped = racing(1.21, 0.0); // 1050 m: sector 4
        rc.update(&[racing(1.1, 60.0), stopped], LAP, 0.01, 1, false);
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
        rc.update(&[off], LAP, 0.1, 1, false);
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
            0.01,
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
    fn the_last_sector_warns_across_the_line() {
        let mut rc = RaceControl::default();
        rc.update(&[racing(2.01, 0.0)], LAP, 0.1, 1, false);
        assert_eq!(rc.yellow_at(0), Yellow::Double);
        assert_eq!(rc.yellow_at(19), Yellow::Single);
        assert_eq!(RaceControl::sector_of(4999.0, LAP), 19);
        assert_eq!(RaceControl::sector_of(5000.0, LAP), 0);
    }
}
