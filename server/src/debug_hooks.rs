//! Debug-only hooks for looking at states a short race seldom reaches: a
//! pit stop, a puncture, a retired car towed to its box. Off unless the
//! server's `[debug]` table (or `APEXSIM_DEBUG_*`) switches them on, and
//! never on in a session the server did not create from that config, so no
//! test, render or survey is touched by them.
//!
//! - **Stand-in driver** (`stand_in_driver = true`): every human's car is
//!   driven by an AI profile instead of the client's input, pit stops
//!   planned like any AI's. The client still sees its own car, so the
//!   driver's HUD shows a car racing, stopping and boosting unattended.
//! - **Events** (`events = "30:host:wear=75;45:3:damage=front:100"`): at
//!   so many seconds of green, do something to a car. The target is `host`
//!   (every human car), `all`, or a car index as the telemetry carries it.
//!   Actions: `wear=P` (every tyre P% worn), `puncture=W` / `leak=W:KPA`
//!   (W is FL FR RL RR), `flatspot=W:S` (0..1), `damage=ZONE:P` (front rear
//!   left right engine; 100 retires the car), `brakewear=P`, `pit` (the
//!   next pass of the lane is a stop), `boost=S` (a stand-in holds the
//!   overtake button for S seconds), `aids=off` (ABS and traction control
//!   off: a stand-in then locks and spins its wheels).

use crate::data::{CarState, TireData};

/// What the hooks are asked to do, as the config holds it.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct DebugHooks {
    pub stand_in_driver: bool,
    pub events: Vec<DebugEvent>,
}

/// Which cars an event is for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Target {
    /// Every human's car.
    Host,
    /// Every car.
    All,
    /// The car with this telemetry index.
    Car(u8),
}

/// A damage zone.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Zone {
    Front,
    Rear,
    Left,
    Right,
    Engine,
}

/// What happens to the car.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Action {
    Wear(f32),
    Puncture(usize),
    Leak(usize, f32),
    FlatSpot(usize, f32),
    Damage(Zone, f32),
    BrakeWear(f32),
    Pit,
    /// Seconds the stand-in holds the overtake button; applied by the
    /// session (`GameSession::stand_in_input`), not to the car.
    Boost(f32),
    /// ABS and traction control off.
    AidsOff,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DebugEvent {
    /// Seconds of green.
    pub at_s: f32,
    pub target: Target,
    pub action: Action,
}

impl DebugHooks {
    pub fn is_active(&self) -> bool {
        self.stand_in_driver || !self.events.is_empty()
    }

    /// Hooks from the config's fields; an event that does not parse is
    /// logged and left out.
    pub fn from_settings(stand_in_driver: bool, events: &str) -> Self {
        let mut parsed = Vec::new();
        for item in events.split(';').map(str::trim).filter(|s| !s.is_empty()) {
            match parse_event(item) {
                Some(event) => parsed.push(event),
                None => tracing::warn!("debug.events: ignoring '{}'", item),
            }
        }
        parsed.sort_by(|a, b| a.at_s.total_cmp(&b.at_s));
        Self {
            stand_in_driver,
            events: parsed,
        }
    }
}

fn wheel(name: &str) -> Option<usize> {
    match name.trim().to_ascii_uppercase().as_str() {
        "FL" => Some(0),
        "FR" => Some(1),
        "RL" => Some(2),
        "RR" => Some(3),
        _ => None,
    }
}

fn zone(name: &str) -> Option<Zone> {
    match name.trim().to_ascii_lowercase().as_str() {
        "front" => Some(Zone::Front),
        "rear" => Some(Zone::Rear),
        "left" => Some(Zone::Left),
        "right" => Some(Zone::Right),
        "engine" => Some(Zone::Engine),
        _ => None,
    }
}

/// `<seconds>:<target>:<action>[=<args>]`.
pub fn parse_event(text: &str) -> Option<DebugEvent> {
    let mut parts = text.splitn(3, ':');
    let at_s: f32 = parts.next()?.trim().parse().ok()?;
    let target = match parts.next()?.trim().to_ascii_lowercase().as_str() {
        "host" => Target::Host,
        "all" => Target::All,
        n => Target::Car(n.parse().ok()?),
    };
    let rest = parts.next()?.trim();
    let (verb, args) = rest.split_once('=').unwrap_or((rest, ""));
    let mut args = args.split(':');
    let mut next = || args.next().unwrap_or("").trim().to_string();
    let action = match verb.trim().to_ascii_lowercase().as_str() {
        "wear" => Action::Wear(next().parse().ok()?),
        "puncture" => Action::Puncture(wheel(&next())?),
        "leak" => {
            let w = wheel(&next())?;
            Action::Leak(w, next().parse().ok()?)
        }
        "flatspot" => {
            let w = wheel(&next())?;
            Action::FlatSpot(w, next().parse().ok()?)
        }
        "damage" => {
            let z = zone(&next())?;
            Action::Damage(z, next().parse().ok()?)
        }
        "brakewear" => Action::BrakeWear(next().parse().ok()?),
        "pit" => Action::Pit,
        "boost" => Action::Boost(next().parse().ok()?),
        "aids" if next().eq_ignore_ascii_case("off") => Action::AidsOff,
        _ => return None,
    };
    if !at_s.is_finite() || at_s < 0.0 {
        return None;
    }
    Some(DebugEvent {
        at_s,
        target,
        action,
    })
}

fn tyre(state: &mut CarState, w: usize) -> &mut TireData {
    let [fl, fr, rl, rr] = state.tires.each_mut();
    match w {
        0 => fl,
        1 => fr,
        2 => rl,
        _ => rr,
    }
}

/// Do `action` to one car.
pub fn apply(action: Action, state: &mut CarState) {
    match action {
        Action::Wear(p) => {
            for t in state.tires.each_mut() {
                t.wear_percent = p.clamp(0.0, 100.0);
            }
        }
        Action::Puncture(w) => crate::tyre_thermal::puncture(tyre(state, w), None),
        Action::Leak(w, kpa) => crate::tyre_thermal::puncture(tyre(state, w), Some(kpa)),
        Action::FlatSpot(w, s) => tyre(state, w).flat_spot = s.clamp(0.0, 1.0),
        Action::Damage(z, p) => {
            let p = p.clamp(0.0, 100.0);
            let d = &mut state.damage;
            match z {
                Zone::Front => d.front_damage_percent = p,
                Zone::Rear => d.rear_damage_percent = p,
                Zone::Left => d.left_damage_percent = p,
                Zone::Right => d.right_damage_percent = p,
                Zone::Engine => d.engine_damage_percent = p,
            }
            d.refresh();
        }
        Action::BrakeWear(p) => state.brake_wear_pct = [p.clamp(0.0, 100.0); 4],
        Action::Pit => {
            state.pit.wants_stop = true;
            state.pit.next_compound = state.tyre_compound;
        }
        Action::Boost(_) => {}
        Action::AidsOff => {
            state.abs = Some(false);
            state.traction_control = Some(crate::data::TractionControl::Off);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn events_parse_and_sort() {
        let hooks = DebugHooks::from_settings(
            false,
            "45:3:damage=front:100; 30:host:wear=75;10:all:pit;bad;20:2:leak=RL:3;50:host:boost=4;1:host:aids=off",
        );
        assert_eq!(hooks.events.len(), 6);
        assert_eq!(hooks.events[0].action, Action::AidsOff);
        assert_eq!(hooks.events[5].action, Action::Boost(4.0));
        assert_eq!(hooks.events[1].action, Action::Pit);
        assert_eq!(hooks.events[1].target, Target::All);
        assert_eq!(hooks.events[2].action, Action::Leak(2, 3.0));
        assert_eq!(hooks.events[3].target, Target::Host);
        assert_eq!(hooks.events[3].action, Action::Wear(75.0));
        assert_eq!(hooks.events[4].target, Target::Car(3));
        assert_eq!(hooks.events[4].action, Action::Damage(Zone::Front, 100.0));
        assert!(hooks.is_active());
        assert!(!DebugHooks::from_settings(false, "").is_active());
    }

    #[test]
    fn full_damage_retires_and_a_puncture_flattens() {
        let slot = crate::data::GridSlot {
            position: 0,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        let mut state = CarState::new(uuid::Uuid::from_u128(1), uuid::Uuid::from_u128(2), &slot);
        apply(Action::Damage(Zone::Engine, 100.0), &mut state);
        assert!(!state.damage.is_drivable);
        apply(Action::Puncture(1), &mut state);
        assert!(state.tires.front_right.punctured);
        apply(Action::Wear(80.0), &mut state);
        assert!(state.tires.each().iter().all(|t| t.wear_percent == 80.0));
    }
}
