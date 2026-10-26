//! Drivers sharing a car (`data::RaceRules::drivers_per_car`): who is in
//! the car, how long each has driven, and who takes over at a stop.
//!
//! A car's drivers change only in its box, during the service: the change
//! takes [`DRIVER_CHANGE_S`] and runs alongside the tyres, the fuel and
//! the repairs, so a stop that changes the driver lasts at least that
//! long. Without the rule (one driver) nothing here is kept and a car is
//! one driver to the flag.

use crate::data::RaceRules;
use crate::pit::PitPlan;

/// The most drivers a car may share.
pub const MAX_DRIVERS: u8 = 4;
/// Climbing out, the next driver in, the belts and the radio, s.
pub const DRIVER_CHANGE_S: f32 = 10.0;
/// A driver whose spell in the car would run past this, s, at the end of
/// another stint as long as the last hands over at the stop: past it a
/// driver starts to tire (`racecraft::MistakeRisk::fatigue`).
pub const REST_AFTER_S: f32 = crate::racecraft::FATIGUE_FROM_S;

/// One car's drivers through a race.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Crew {
    /// The driver in the car (0-based); the one taking over while a
    /// change is under way.
    pub driver: u8,
    /// Each driver's time in the car in the spells already over, s.
    pub driven_s: [f32; MAX_DRIVERS as usize],
    /// The tick the driver in the car got in (the green light, or the end
    /// of the stop that put them in); `None` while a change is under way.
    pub in_since_tick: Option<u32>,
}

impl Crew {
    /// The first driver in at `tick` (the green light), nobody's time yet.
    pub fn starting(tick: u32) -> Self {
        Self {
            driver: 0,
            driven_s: [0.0; MAX_DRIVERS as usize],
            in_since_tick: Some(tick),
        }
    }

    /// The spell the driver in the car is on, s.
    pub fn spell_s(&self, tick: u32, rate_hz: f32) -> f32 {
        self.in_since_tick.map_or(0.0, |from| {
            tick.saturating_sub(from) as f32 / rate_hz.max(1.0)
        })
    }

    /// `driver`'s time in the car over the race, the spell under way
    /// included, s.
    pub fn total_s(&self, driver: u8, tick: u32, rate_hz: f32) -> f32 {
        let done = self.driven_s.get(driver as usize).copied().unwrap_or(0.0);
        if driver == self.driver {
            done + self.spell_s(tick, rate_hz)
        } else {
            done
        }
    }

    /// The time `driver` may still drive under `rules`' most per driver,
    /// s; `None` without that rule.
    pub fn left_s(&self, rules: &RaceRules, driver: u8, tick: u32, rate_hz: f32) -> Option<f32> {
        (rules.max_driver_minutes > 0)
            .then(|| rules.max_driver_minutes as f32 * 60.0 - self.total_s(driver, tick, rate_hz))
    }

    /// Whether a driver other than the one in the car has more time left
    /// than them: a stop can then hand the car on.
    pub fn can_hand_on(&self, rules: &RaceRules, tick: u32, rate_hz: f32) -> bool {
        let Some(own) = self.left_s(rules, self.driver, tick, rate_hz) else {
            return rules.shares_cars();
        };
        (0..rules.drivers())
            .filter(|d| *d != self.driver)
            .any(|d| self.left_s(rules, d, tick, rate_hz).unwrap_or(f32::MAX) > own)
    }

    /// The driver in the car gets out at `tick` (their spell ends there)
    /// and `next` is getting in.
    pub fn begin_change(&mut self, next: u8, tick: u32, rate_hz: f32) {
        let spell = self.spell_s(tick, rate_hz);
        if let Some(t) = self.driven_s.get_mut(self.driver as usize) {
            *t += spell;
        }
        self.driver = next;
        self.in_since_tick = None;
    }

    /// The change is done: the new driver's spell starts at `tick`.
    pub fn end_change(&mut self, tick: u32) {
        if self.in_since_tick.is_none() {
            self.in_since_tick = Some(tick);
        }
    }
}

/// Who drives on from a stop at `tick`: `Some(next)` to change, `None` to
/// keep the driver in the car. The driver's order (`PitPlan::change_driver`)
/// is followed where another driver is there to take over; unasked, the
/// crew hands over when the driver in the car would, after another stint
/// as long as the one just run (`next_stint_s`), be past
/// [`REST_AFTER_S`] in this spell or past `rules`' most per driver, to the
/// next driver in turn who can drive that stint (or, if none can, to the
/// one with the most time left when that is more than the driver in the
/// car has).
pub fn change_at_stop(
    crew: &Crew,
    rules: &RaceRules,
    plan: &PitPlan,
    next_stint_s: f32,
    tick: u32,
    rate_hz: f32,
) -> Option<u8> {
    if !rules.shares_cars() || plan.change_driver == Some(false) {
        return None;
    }
    let n = rules.drivers();
    let left = |d: u8| crew.left_s(rules, d, tick, rate_hz).unwrap_or(f32::MAX);
    let ordered = plan.change_driver == Some(true);
    if !ordered {
        let spell = crew.spell_s(tick, rate_hz);
        let tired = spell + next_stint_s > REST_AFTER_S;
        let out_of_time = left(crew.driver) < next_stint_s;
        if !tired && !out_of_time {
            return None;
        }
    }
    let turn = (1..n).map(|k| (crew.driver + k) % n);
    if let Some(next) = turn.clone().find(|d| left(*d) >= next_stint_s) {
        return Some(next);
    }
    // Nobody can drive a whole stint more: the one with the most time.
    let best = turn.fold(None, |best: Option<u8>, d| match best {
        Some(b) if left(b) >= left(d) => Some(b),
        _ => Some(d),
    })?;
    (ordered || left(best) > left(crew.driver)).then_some(best)
}

#[cfg(test)]
mod tests {
    use super::*;

    const RATE: f32 = 100.0;

    fn rules(drivers: u8, max_minutes: u16) -> RaceRules {
        RaceRules {
            drivers_per_car: drivers,
            max_driver_minutes: max_minutes,
            ..Default::default()
        }
    }

    fn at(s: f32) -> u32 {
        (s * RATE) as u32
    }

    #[test]
    fn one_driver_never_changes() {
        let crew = Crew::starting(0);
        let order = PitPlan {
            change_driver: Some(true),
            ..Default::default()
        };
        for drivers in [0, 1] {
            let r = rules(drivers, 0);
            assert_eq!(
                change_at_stop(&crew, &r, &order, 3000.0, at(3000.0), RATE),
                None
            );
        }
    }

    #[test]
    fn the_crew_hands_over_before_a_spell_tires_the_driver() {
        let r = rules(2, 0);
        let crew = Crew::starting(0);
        let unasked = PitPlan::default();
        // 25 min in, another 25 min stint: still fresh.
        assert_eq!(
            change_at_stop(&crew, &r, &unasked, 1500.0, at(1500.0), RATE),
            None
        );
        // 40 min in, another 40: past the hour, so the second driver.
        assert_eq!(
            change_at_stop(&crew, &r, &unasked, 2400.0, at(2400.0), RATE),
            Some(1)
        );
        // Ordered either way, the order stands.
        let stay = PitPlan {
            change_driver: Some(false),
            ..Default::default()
        };
        assert_eq!(
            change_at_stop(&crew, &r, &stay, 2400.0, at(2400.0), RATE),
            None
        );
        let swap = PitPlan {
            change_driver: Some(true),
            ..Default::default()
        };
        assert_eq!(
            change_at_stop(&crew, &r, &swap, 600.0, at(600.0), RATE),
            Some(1)
        );
    }

    #[test]
    fn the_most_per_driver_picks_who_takes_over() {
        // Three drivers, 60 minutes each at most.
        let r = rules(3, 60);
        let mut crew = Crew::starting(0);
        // Driver 0 did 50 min, then driver 1 50 min: driver 1 is in.
        crew.driven_s = [3000.0, 0.0, 0.0, 0.0];
        crew.driver = 1;
        crew.in_since_tick = Some(at(3000.0));
        let now = at(6000.0);
        assert!((crew.total_s(1, now, RATE) - 3000.0).abs() < 0.1);
        assert!((crew.left_s(&r, 1, now, RATE).unwrap() - 600.0).abs() < 0.1);
        // Another 20 min stint: driver 2, the next in turn with the time.
        assert_eq!(
            change_at_stop(&crew, &r, &PitPlan::default(), 1200.0, now, RATE),
            Some(2)
        );
        // With driver 2 used up too, nobody has the whole stint: the one
        // with the most time left, as it is more than driver 1 has.
        crew.driven_s[0] = 2700.0;
        crew.driven_s[2] = 3500.0;
        assert_eq!(
            change_at_stop(&crew, &r, &PitPlan::default(), 1200.0, now, RATE),
            Some(0)
        );
        assert!(crew.can_hand_on(&r, now, RATE));
        // With nobody better off than the driver in the car, stay in.
        crew.driven_s[0] = 3200.0;
        assert_eq!(
            change_at_stop(&crew, &r, &PitPlan::default(), 1200.0, now, RATE),
            None
        );
        assert!(!crew.can_hand_on(&r, now, RATE));
    }

    #[test]
    fn a_change_closes_the_spell_and_the_new_one_starts_at_its_end() {
        let mut crew = Crew::starting(at(10.0));
        crew.begin_change(1, at(1810.0), RATE);
        assert!((crew.driven_s[0] - 1800.0).abs() < 0.1);
        assert_eq!(crew.spell_s(at(1815.0), RATE), 0.0);
        crew.end_change(at(1820.0));
        assert!((crew.spell_s(at(1880.0), RATE) - 60.0).abs() < 0.1);
        assert!((crew.total_s(0, at(1880.0), RATE) - 1800.0).abs() < 0.1);
    }
}
