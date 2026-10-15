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
//! A driver a reference names who is not here (a name nobody seated has,
//! the host gone) leaves their slot empty, as a car that does not take the
//! start does: the cars behind keep their places. A reference to no car
//! at all (`@ai:N` past the session's AI) is skipped. Every driver the
//! list leaves out starts behind the listed ones, in the order they were
//! seated. An order with more slots than the track has closes its gaps
//! up from the back until it fits ([`fit`]).

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

/// The start order for `refs`, slot by slot from pole: the drivers a
/// reference names, in the list's order (a driver is claimed once), an
/// empty slot (`None`) for each one who is not here, then the rest by the
/// slot they hold.
pub fn resolve(
    drivers: &[Seated],
    host: PlayerId,
    ai_ids: &[PlayerId],
    refs: &[String],
) -> Vec<Option<PlayerId>> {
    let mut by_slot: Vec<&Seated> = drivers.iter().collect();
    by_slot.sort_by_key(|d| (d.grid_position, d.id));
    let seated = |id: &PlayerId| drivers.iter().any(|d| d.id == *id);
    let mut slots: Vec<Option<PlayerId>> = Vec::with_capacity(drivers.len());
    for reference in refs {
        let claimed = |id: &PlayerId| slots.contains(&Some(*id));
        // The host and an AI car are one driver each: named twice, the
        // second is skipped. Not here, their slot stays empty.
        let one = |id: PlayerId| (!claimed(&id)).then_some(Some(id).filter(seated));
        let slot = if reference == HOST_REF {
            one(host)
        } else if let Some(n) = reference.strip_prefix("@ai:") {
            // A reference to no car of this session names nobody.
            n.parse::<usize>()
                .ok()
                .and_then(|n| n.checked_sub(1))
                .and_then(|i| ai_ids.get(i).copied())
                .and_then(one)
        } else {
            Some(
                by_slot
                    .iter()
                    .find(|d| d.name == *reference && !claimed(&d.id))
                    .map(|d| d.id),
            )
        };
        if let Some(slot) = slot {
            slots.push(slot);
        }
    }
    complete(&by_slot, slots)
}

/// `first` followed by every other driver in `by_slot` order, with no
/// empty slot at the back.
pub fn complete(by_slot: &[&Seated], mut first: Vec<Option<PlayerId>>) -> Vec<Option<PlayerId>> {
    for d in by_slot {
        if !first.contains(&Some(d.id)) {
            first.push(Some(d.id));
        }
    }
    while first.last() == Some(&None) {
        first.pop();
    }
    first
}

/// `order` held to a grid of `slots`: its empty slots closed up from the
/// back until it fits, or `None` when the drivers alone do not.
pub fn fit(mut order: Vec<Option<PlayerId>>, slots: usize) -> Option<Vec<Option<PlayerId>>> {
    while order.len() > slots {
        let gap = order.iter().rposition(|s| s.is_none())?;
        order.remove(gap);
    }
    Some(order)
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn ids(list: &[PlayerId]) -> Vec<Option<PlayerId>> {
        list.iter().copied().map(Some).collect()
    }

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
        assert_eq!(got, ids(&[c.id, d.id, a.id, b.id]));
    }

    #[test]
    fn host_and_ai_references_resolve_by_seating() {
        let (h, x, y) = (seat("Host", 1), seat("Max", 2), seat("Max", 3));
        let all = vec![h.clone(), x.clone(), y.clone()];
        let got = resolve(&all, h.id, &[x.id, y.id], &["@ai:2".into(), "@host".into()]);
        assert_eq!(got, ids(&[y.id, h.id, x.id]));
    }

    #[test]
    fn a_name_matches_each_driver_once_and_a_no_show_leaves_the_slot_empty() {
        let (x, y) = (seat("Max", 1), seat("Max", 2));
        let all = vec![x.clone(), y.clone()];
        let got = resolve(
            &all,
            x.id,
            &[],
            &["Max".into(), "Ghost".into(), "Max".into()],
        );
        assert_eq!(got, vec![Some(x.id), None, Some(y.id)]);
        let got = resolve(&all, x.id, &[], &["Max".into(), "Max".into()]);
        assert_eq!(got, ids(&[x.id, y.id]));
        // A third Max did not come: nobody is behind the slot, so it goes.
        let got = resolve(&all, x.id, &[], &["Max".into(), "Max".into(), "Max".into()]);
        assert_eq!(got, ids(&[x.id, y.id]));
    }

    #[test]
    fn the_unlisted_start_behind_an_empty_slot() {
        let (h, b) = (seat("Host", 1), seat("Bo", 2));
        let all = vec![h.clone(), b.clone()];
        let got = resolve(&all, h.id, &[], &["@host".into(), "Ghost".into()]);
        assert_eq!(got, vec![Some(h.id), None, Some(b.id)]);
        // The host gone, their pole stays empty.
        let gone = vec![b.clone()];
        let got = resolve(&gone, h.id, &[], &["@host".into(), "Bo".into()]);
        assert_eq!(got, vec![None, Some(b.id)]);
    }

    #[test]
    fn an_order_too_long_for_the_grid_closes_its_gaps_from_the_back() {
        let (a, b, c) = (Uuid::from_u128(1), Uuid::from_u128(2), Uuid::from_u128(3));
        let order = vec![Some(a), None, Some(b), None, Some(c)];
        assert_eq!(fit(order.clone(), 5), Some(order.clone()));
        assert_eq!(
            fit(order.clone(), 4),
            Some(vec![Some(a), None, Some(b), Some(c)])
        );
        assert_eq!(fit(order.clone(), 3), Some(ids(&[a, b, c])));
        assert_eq!(fit(order, 2), None);
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
        assert_eq!(got, ids(&[h.id, x.id]));
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
