//! The inside of a [`super::ColumnBlock`] (layout [`super::COLUMN_LAYOUT_DELTA`]):
//! a block's records rearranged so zlib finds what repeats
//! (docs/game/spectator.md, "The file").
//!
//! A frame is self-contained on the wire: every car's absolute pose,
//! interleaved car after car. On disk the same bytes are laid out by field
//! instead, each car's values one after the other, most of them as the
//! difference from the car's previous frame in the block, and every
//! multi-byte column split into byte planes (all low bytes, then the next).
//! Nothing is lost: unpacking gives back the record bodies byte for byte.
//!
//! ```text
//! u32 records, u32 frames, u32 rows                      (little-endian)
//! u8 per record: 1 a frame laid out below, 0 a record kept as it was
//! per frame: epoch, tick, revision, countdown (zigzag deltas from the frame
//!   before), state, part, parts, rows (bytes); each a column of byte planes
//! per row, in frame order: the car index
//! per field of the row (FIELDS), the rows sorted by car then frame: the
//!   values, a delta field as the zigzag difference from the car's row before
//!   (the first row of a car from 0), byte-planed
//! the other records, framed: [u32 big-endian length][body]
//! ```
//!
//! Only a frame whose rows are [`ROW_SIZE`] each and whose body this build
//! encodes to the same bytes ([`StreamFrame::encode`]) is laid out; anything
//! else (an older row layout, a frame another writer spelled differently)
//! is kept as it was, so the round trip holds for any block.

use super::{
    framed, malformed, split_framed, Record, SessionState, StreamError, StreamFrame, ROW_SIZE,
};

/// One field of a [`super::CarRow`] as written: its offset, its width in
/// bytes, and whether it is stored as the difference from the car's row
/// before. What changes a little every frame is a delta; flags, pedals and
/// the noisy wheel bytes are stored as they are.
const FIELDS: &[(usize, usize, bool)] = &[
    (1, 1, false), // status
    (2, 4, true),  // x
    (6, 4, true),  // y
    (10, 4, true), // z
    (14, 2, true), // yaw (wraps)
    (16, 2, true), // pitch
    (18, 2, true), // roll
    (20, 2, true), // speed
    (22, 1, false),
    (23, 1, false),
    (24, 1, false),
    (25, 1, false),
    (26, 2, true), // rpm
    (28, 2, true), // lap
    (30, 4, true), // station
];
/// Bytes from here to the end of the row are single-byte columns, as they are.
const BYTES_FROM: usize = 34;

fn mask(width: usize) -> u64 {
    if width >= 8 {
        u64::MAX
    } else {
        (1u64 << (8 * width)) - 1
    }
}

/// `v - prev` in `width` bytes, as a zigzag: small either way, small number.
fn zigzag(v: u64, prev: u64, width: usize) -> u64 {
    let bits = 8 * width as u32;
    let d = v.wrapping_sub(prev) & mask(width);
    // Sign-extend from `bits`.
    let signed = ((d << (64 - bits)) as i64) >> (64 - bits);
    (((signed << 1) ^ (signed >> 63)) as u64) & mask(width)
}

fn unzigzag(z: u64, prev: u64, width: usize) -> u64 {
    let signed = ((z >> 1) as i64) ^ -((z & 1) as i64);
    prev.wrapping_add(signed as u64) & mask(width)
}

fn put_planes(out: &mut Vec<u8>, values: &[u64], width: usize) {
    for plane in 0..width {
        out.extend(values.iter().map(|v| (v >> (8 * plane)) as u8));
    }
}

struct Cursor<'a>(&'a [u8]);

impl<'a> Cursor<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], StreamError> {
        if self.0.len() < n {
            return malformed("column block cut short");
        }
        let (head, tail) = self.0.split_at(n);
        self.0 = tail;
        Ok(head)
    }
    fn u32(&mut self) -> Result<u32, StreamError> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    fn planes(&mut self, n: usize, width: usize) -> Result<Vec<u64>, StreamError> {
        let bytes = self.take(n.checked_mul(width).ok_or_else(too_big)?)?;
        let mut values = vec![0u64; n];
        for plane in 0..width {
            for (v, b) in values.iter_mut().zip(&bytes[plane * n..(plane + 1) * n]) {
                *v |= (*b as u64) << (8 * plane);
            }
        }
        Ok(values)
    }
}

fn too_big() -> StreamError {
    StreamError::Malformed("column block counts overflow".into())
}

/// A frame this layout can carry: its rows whole, and its bytes what this
/// build would write for it.
fn columnar(body: &[u8]) -> Option<StreamFrame> {
    let Ok(Record::Frame(frame)) = Record::decode(body) else {
        return None;
    };
    let rows = frame.rows.len() / ROW_SIZE;
    (frame.rows.len() % ROW_SIZE == 0 && rows <= u8::MAX as usize && frame.encode() == body)
        .then_some(frame)
}

/// Rows, as `(car_index, row number)` order: by car, then by frame.
fn by_car(cars: &[u8]) -> Vec<usize> {
    let mut order: Vec<usize> = (0..cars.len()).collect();
    order.sort_by_key(|&i| (cars[i], i));
    order
}

/// The payload (before zlib) of a block of `bodies`.
pub(super) fn pack(bodies: &[&[u8]]) -> Vec<u8> {
    let frames: Vec<Option<StreamFrame>> = bodies.iter().map(|b| columnar(b)).collect();
    let laid: Vec<&StreamFrame> = frames.iter().flatten().collect();
    let rows: Vec<&[u8; ROW_SIZE]> = laid
        .iter()
        .flat_map(|f| f.rows.as_chunks::<ROW_SIZE>().0)
        .collect();

    let mut out = Vec::with_capacity(bodies.iter().map(|b| b.len()).sum::<usize>() + 64);
    out.extend_from_slice(&(bodies.len() as u32).to_le_bytes());
    out.extend_from_slice(&(laid.len() as u32).to_le_bytes());
    out.extend_from_slice(&(rows.len() as u32).to_le_bytes());
    out.extend(frames.iter().map(|f| f.is_some() as u8));

    // The frame headers, each a column.
    let delta_column = |get: &dyn Fn(&StreamFrame) -> u64, width: usize, out: &mut Vec<u8>| {
        let mut prev = 0;
        let values: Vec<u64> = laid
            .iter()
            .map(|f| {
                let v = get(f);
                let z = zigzag(v, prev, width);
                prev = v;
                z
            })
            .collect();
        put_planes(out, &values, width);
    };
    delta_column(&|f| f.epoch as u64, 4, &mut out);
    delta_column(&|f| f.tick as u64, 4, &mut out);
    delta_column(&|f| f.roster_revision as u64, 2, &mut out);
    delta_column(&|f| f.countdown_ms as u64, 2, &mut out);
    out.extend(laid.iter().map(|f| f.state as u8));
    out.extend(laid.iter().map(|f| f.part));
    out.extend(laid.iter().map(|f| f.parts));
    out.extend(laid.iter().map(|f| (f.rows.len() / ROW_SIZE) as u8));

    // Which car each row is.
    let cars: Vec<u8> = rows.iter().map(|r| r[0]).collect();
    out.extend_from_slice(&cars);

    // The fields, car after car.
    let order = by_car(&cars);
    let field = |offset: usize, width: usize, delta: bool, out: &mut Vec<u8>| {
        let read = |r: &[u8]| (0..width).fold(0u64, |a, b| a | ((r[offset + b] as u64) << (8 * b)));
        let mut values = Vec::with_capacity(order.len());
        let mut prev = 0u64;
        let mut car = None;
        for &i in &order {
            if car != Some(cars[i]) {
                car = Some(cars[i]);
                prev = 0;
            }
            let v = read(rows[i]);
            values.push(if delta { zigzag(v, prev, width) } else { v });
            prev = v;
        }
        put_planes(out, &values, width);
    };
    for &(offset, width, delta) in FIELDS {
        field(offset, width, delta, &mut out);
    }
    for offset in BYTES_FROM..ROW_SIZE {
        field(offset, 1, false, &mut out);
    }

    for (body, frame) in bodies.iter().zip(&frames) {
        if frame.is_none() {
            out.extend_from_slice(&framed(body));
        }
    }
    out
}

/// The record bodies of a payload [`pack`] wrote, in their order.
pub(super) fn unpack(payload: &[u8]) -> Result<Vec<Vec<u8>>, StreamError> {
    let mut c = Cursor(payload);
    let records = c.u32()? as usize;
    let frames = c.u32()? as usize;
    let rows = c.u32()? as usize;
    // Every count costs at least a byte of the payload: a corrupt one fails
    // on the bytes, not on an allocation.
    if records > payload.len() || frames > records || rows > payload.len() {
        return malformed("column block counts past its size");
    }
    let kinds = c.take(records)?;
    if kinds.iter().filter(|k| **k == 1).count() != frames {
        return malformed("column block frame count");
    }

    let delta_column = |c: &mut Cursor, width: usize| -> Result<Vec<u64>, StreamError> {
        let mut prev = 0;
        Ok(c.planes(frames, width)?
            .into_iter()
            .map(|z| {
                prev = unzigzag(z, prev, width);
                prev
            })
            .collect())
    };
    let epochs = delta_column(&mut c, 4)?;
    let ticks = delta_column(&mut c, 4)?;
    let revisions = delta_column(&mut c, 2)?;
    let countdowns = delta_column(&mut c, 2)?;
    let states = c.take(frames)?;
    let parts = c.take(frames)?;
    let part_counts = c.take(frames)?;
    let row_counts = c.take(frames)?;
    if row_counts.iter().map(|n| *n as usize).sum::<usize>() != rows {
        return malformed("column block row count");
    }
    let cars = c.take(rows)?;

    let order = by_car(cars);
    let mut row_bytes = vec![0u8; rows * ROW_SIZE];
    for (i, &car) in cars.iter().enumerate() {
        row_bytes[i * ROW_SIZE] = car;
    }
    let mut field = |c: &mut Cursor, offset: usize, width: usize, delta: bool| {
        let values = c.planes(rows, width)?;
        let mut prev = 0u64;
        let mut car = None;
        for (&i, z) in order.iter().zip(values) {
            if car != Some(cars[i]) {
                car = Some(cars[i]);
                prev = 0;
            }
            let v = if delta { unzigzag(z, prev, width) } else { z };
            prev = v;
            for b in 0..width {
                row_bytes[i * ROW_SIZE + offset + b] = (v >> (8 * b)) as u8;
            }
        }
        Ok::<(), StreamError>(())
    };
    for &(offset, width, delta) in FIELDS {
        field(&mut c, offset, width, delta)?;
    }
    for offset in BYTES_FROM..ROW_SIZE {
        field(&mut c, offset, 1, false)?;
    }

    let mut others = split_framed(c.0)?.into_iter();
    let mut bodies = Vec::with_capacity(records);
    let mut frame = 0;
    let mut row = 0;
    for &kind in kinds {
        if kind == 0 {
            bodies.push(
                others
                    .next()
                    .ok_or_else(|| StreamError::Malformed("column block lost a record".into()))?,
            );
            continue;
        }
        let n = row_counts[frame] as usize;
        let state = match states[frame] {
            0 => SessionState::Lobby,
            1 => SessionState::Countdown,
            2 => SessionState::Racing,
            3 => SessionState::Finished,
            other => return malformed(format!("session state {other}")),
        };
        bodies.push(
            StreamFrame {
                epoch: epochs[frame] as u32,
                tick: ticks[frame] as u32,
                roster_revision: revisions[frame] as u16,
                state,
                countdown_ms: countdowns[frame] as u16,
                part: parts[frame],
                parts: part_counts[frame],
                rows: row_bytes[row * ROW_SIZE..(row + n) * ROW_SIZE].to_vec(),
            }
            .encode(),
        );
        frame += 1;
        row += n;
    }
    if others.next().is_some() {
        return malformed("column block has records left over");
    }
    Ok(bodies)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zigzag_round_trips_at_every_width() {
        for width in [1usize, 2, 4] {
            let m = mask(width);
            for (v, prev) in [(0, 0), (5, 3), (3, 5), (m, 0), (0, m), (m / 2, m / 2 + 1)] {
                let z = zigzag(v, prev, width);
                assert!(z <= m);
                assert_eq!(unzigzag(z, prev, width), v, "{width} {v} {prev}");
            }
        }
        // Small either way stays small.
        assert_eq!(zigzag(9, 10, 4), 1);
        assert_eq!(zigzag(11, 10, 4), 2);
        // A yaw wrapping past a turn is a small step.
        assert_eq!(zigzag(1, 0xFFFF, 2), 4);
    }
}
