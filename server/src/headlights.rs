//! A car's headlights: on, off, or flashed.
//!
//! The lights are the server's to say because everyone sees them: the car
//! ahead needs to see the one behind flash. Each tick, before the mode's
//! own work, every car takes its driver's switch from `PlayerInputData`
//! (`Some(on)`), or — when the switch has not been touched, for the AI and
//! for a client from before the field — the session's conditions: on as
//! the sun goes and in the rain ([`SessionConditions::headlights_needed`],
//! the client sky model's rule). The flash button is held, and puts the
//! lights on at full beam for as long as it is.
//!
//! Telemetry carries the two as bits 5 and 6 of `lap_flags`; nothing in the
//! sim reads them.

use std::collections::{BTreeMap, HashMap};

use crate::data::{CarState, PlayerId, PlayerInputData, SessionConditions};

/// `lap_flags` bit 5: the headlights are on.
pub const LAP_FLAG_HEADLIGHTS: u8 = 32;
/// `lap_flags` bit 6: the driver is flashing them.
pub const LAP_FLAG_HEADLIGHT_FLASH: u8 = 64;

/// Sets every car's lights from its driver's input for this tick.
pub fn update(
    participants: &mut BTreeMap<PlayerId, CarState>,
    inputs: &HashMap<PlayerId, PlayerInputData>,
    conditions: SessionConditions,
) {
    let auto = conditions.headlights_needed();
    for state in participants.values_mut() {
        let input = inputs.get(&state.player_id);
        state.headlights = input.and_then(|i| i.headlights).unwrap_or(auto);
        state.headlight_flash = input.is_some_and(|i| i.flash);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::{GridSlot, Weather};
    use uuid::Uuid;

    fn field() -> (BTreeMap<PlayerId, CarState>, PlayerId) {
        let id = Uuid::new_v4();
        let slot = GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        let mut field = BTreeMap::new();
        field.insert(id, CarState::new(id, Uuid::new_v4(), &slot));
        (field, id)
    }

    fn at(weather: Weather, hh: u16, mm: u16) -> SessionConditions {
        SessionConditions {
            weather,
            time_of_day_minutes: hh * 60 + mm,
        }
    }

    #[test]
    fn untouched_lights_follow_the_sky() {
        assert!(!at(Weather::Sunny, 13, 0).headlights_needed());
        assert!(!at(Weather::Overcast, 13, 0).headlights_needed());
        assert!(at(Weather::LightRain, 13, 0).headlights_needed());
        assert!(at(Weather::Sunny, 22, 15).headlights_needed());
        assert!(at(Weather::Sunny, 4, 0).headlights_needed());
        // The client's dusk case (`ApexSim.Sky`): the sun under 6° at 20:45.
        assert!(at(Weather::Sunny, 20, 45).headlights_needed());
        assert!(!at(Weather::Sunny, 18, 0).headlights_needed());
        let noon = at(Weather::Sunny, 13, 0).sun_elevation_deg();
        assert!(
            (noon - 60.0).abs() < 0.01,
            "50° N, +20° declination: {noon}"
        );

        let (mut cars, id) = field();
        update(&mut cars, &HashMap::new(), at(Weather::Sunny, 22, 0));
        assert!(cars[&id].headlights, "no input yet: the conditions decide");
        assert!(!cars[&id].headlight_flash);
    }

    #[test]
    fn the_switch_beats_the_sky_and_the_flash_is_held() {
        let (mut cars, id) = field();
        let mut inputs = HashMap::new();
        inputs.insert(
            id,
            PlayerInputData {
                headlights: Some(false),
                ..Default::default()
            },
        );
        update(&mut cars, &inputs, at(Weather::HeavyRain, 23, 0));
        assert!(!cars[&id].headlights, "switched off at night");

        inputs.get_mut(&id).unwrap().headlights = Some(true);
        update(&mut cars, &inputs, SessionConditions::DEFAULT);
        assert!(cars[&id].headlights, "switched on by day");

        let input = inputs.get_mut(&id).unwrap();
        input.headlights = None;
        input.flash = true;
        update(&mut cars, &inputs, SessionConditions::DEFAULT);
        assert!(!cars[&id].headlights);
        assert!(cars[&id].headlight_flash);
        let flags = crate::network::lap_flags_of(&cars[&id]);
        assert_eq!(
            flags & (LAP_FLAG_HEADLIGHTS | LAP_FLAG_HEADLIGHT_FLASH),
            LAP_FLAG_HEADLIGHT_FLASH
        );

        inputs.get_mut(&id).unwrap().flash = false;
        update(&mut cars, &inputs, SessionConditions::DEFAULT);
        assert!(!cars[&id].headlight_flash, "let go, the flash ends");
    }
}
