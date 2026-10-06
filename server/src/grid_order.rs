//! The start order of a race: which car sits on which grid slot.
//!
//! The host can ask for an order on the create screen (dragged by hand or
//! loaded from a saved qualifying result), and a session that ran
//! qualifying grids from it. Both are lists of **driver references**, since
//! the cars are not seated yet when the host asks (the AI are, the other
//! humans join later):
//!
//! * `@host` — the session's host;
//! * `@ai:N` — the Nth AI car of the session (1-based, in seating order);
//! * anything else — a driver by name, human or AI.
//!
//! A reference that matches nobody is skipped (a driver who did not show up
//! closes the grid up), and every driver the list leaves out starts behind
//! the listed ones, in the order they were seated.

use crate::data::PlayerId;

/// The reference for the host's own car.
pub const HOST_REF: &str = "@host";
/// The longest order a request may carry, and the longest reference.
pub const MAX_GRID_ENTRIES: usize = 64;
pub const MAX_REF_LEN: usize = 64;

/// The reference for the `n`th AI car (1-based).
pub fn ai_ref(n: usize) -> String {
    format!("@ai:{n}")
}

/// A seated driver as the ordering sees them.
#[derive(Debug, Clone)]
pub struct Seated {
    pub id: PlayerId,
    pub name: String,
    /// The slot the driver holds now; breaks ties and orders the unlisted.
    pub grid_position: u8,
}

/// Clean a requested order on the way in: bounded, no blank references.
pub fn sanitize(order: Vec<String>) -> Vec<String> {
    order
        .into_iter()
        .map(|r| r.trim().chars().take(MAX_REF_LEN).collect::<String>())
        .filter(|r| !r.is_empty())
        .take(MAX_GRID_ENTRIES)
        .collect()
}

/// Every seated driver in start order for `refs`: the ones a reference
/// names, in the list's order (a driver is claimed once), then the rest by
/// the slot they hold.
pub fn resolve(
    drivers: &[Seated],
    host: PlayerId,
    ai_ids: &[PlayerId],
    refs: &[String],
) -> Vec<PlayerId> {
    let mut by_slot: Vec<&Seated> = drivers.iter().collect();
    by_slot.sort_by_key(|d| (d.grid_position, d.id));
    let mut taken: Vec<PlayerId> = Vec::with_capacity(drivers.len());
    for reference in refs {
        let found = if reference == HOST_REF {
            Some(host)
        } else if let Some(n) = reference.strip_prefix("@ai:") {
            n.parse::<usize>()
                .ok()
                .and_then(|n| n.checked_sub(1))
                .and_then(|i| ai_ids.get(i).copied())
        } else {
            by_slot
                .iter()
                .find(|d| d.name == *reference && !taken.contains(&d.id))
                .map(|d| d.id)
        };
        if let Some(id) = found {
            if !taken.contains(&id) && drivers.iter().any(|d| d.id == id) {
                taken.push(id);
            }
        }
    }
    complete(&by_slot, taken)
}

/// `first` followed by every other driver in `by_slot` order.
pub fn complete(by_slot: &[&Seated], mut first: Vec<PlayerId>) -> Vec<PlayerId> {
    for d in by_slot {
        if !first.contains(&d.id) {
            first.push(d.id);
        }
    }
    first
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn seat(name: &str, slot: u8) -> Seated {
        Seated {
            id: Uuid::new_v4(),
            name: name.to_string(),
            grid_position: slot,
        }
    }

    #[test]
    fn listed_drivers_lead_in_the_lists_order_and_the_rest_follow() {
        let (a, b, c, d) = (seat("Ann", 1), seat("Bo", 2), seat("Cy", 3), seat("Di", 4));
        let all = vec![a.clone(), b.clone(), c.clone(), d.clone()];
        let got = resolve(&all, a.id, &[], &["Cy".into(), "Di".into()]);
        assert_eq!(got, vec![c.id, d.id, a.id, b.id]);
    }

    #[test]
    fn host_and_ai_references_resolve_by_seating() {
        let (h, x, y) = (seat("Host", 1), seat("Max", 2), seat("Max", 3));
        let all = vec![h.clone(), x.clone(), y.clone()];
        let got = resolve(&all, h.id, &[x.id, y.id], &["@ai:2".into(), "@host".into()]);
        assert_eq!(got, vec![y.id, h.id, x.id]);
    }

    #[test]
    fn a_name_matches_each_driver_once_and_a_missing_one_closes_up() {
        let (x, y) = (seat("Max", 1), seat("Max", 2));
        let all = vec![x.clone(), y.clone()];
        let got = resolve(
            &all,
            x.id,
            &[],
            &["Max".into(), "Ghost".into(), "Max".into()],
        );
        assert_eq!(got, vec![x.id, y.id]);
        let got = resolve(&all, x.id, &[], &["Max".into(), "Max".into()]);
        assert_eq!(got, vec![x.id, y.id]);
    }

    #[test]
    fn an_out_of_range_ai_reference_is_ignored() {
        let (h, x) = (seat("Host", 1), seat("A", 2));
        let all = vec![h.clone(), x.clone()];
        let got = resolve(
            &all,
            h.id,
            &[x.id],
            &["@ai:0".into(), "@ai:9".into(), "@ai:x".into()],
        );
        assert_eq!(got, vec![h.id, x.id]);
    }

    #[test]
    fn sanitize_bounds_the_list() {
        let long = "x".repeat(200);
        let got = sanitize(vec!["  ".into(), " a ".into(), long]);
        assert_eq!(got.len(), 2);
        assert_eq!(got[0], "a");
        assert_eq!(got[1].len(), MAX_REF_LEN);
    }
}
