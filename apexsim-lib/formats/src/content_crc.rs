//! The content checksum a client compares against the server's.
//!
//! The Unreal client renders a track from a level baked out of the track
//! YAML and shows a car from a mesh imported beside its `car.toml`, while the
//! server simulates from the YAML and the TOML themselves. Every source file
//! therefore gets one number, and the two sides compare it when the client
//! loads the content:
//!
//! - the server computes it as the file is loaded (`CarConfig::content_crc`,
//!   `TrackConfig::content_crc`, sent in the lobby summaries as `ContentCrc`);
//! - `ats-export` writes it into the scene export as `source_crc`;
//! - `apexsim-content/tools/build_track_catalog.py` writes it into the track
//!   catalog manifest (`source_crc`) and from there onto the
//!   `DT_TrackCatalog` row;
//! - the `ApexCarImport` commandlet writes it onto the `DT_CarCatalog` row.
//!
//! The number is the CRC-32 used by zlib, PNG and Ethernet (polynomial
//! `0xEDB88320` reflected, initial and final XOR `0xFFFFFFFF`) over the file's
//! bytes with every carriage return (`0x0D`) dropped first, so a checkout
//! with `core.autocrlf` on and one without agree. The C++ twin is
//! `ApexContentCrc.h` and the Python one is in `build_track_catalog.py`; the
//! reference vector is `"123456789"` -> `0xCBF43926`.

/// CRC-32 (IEEE) over `bytes` with carriage returns removed.
pub fn content_crc(bytes: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in bytes {
        if b == b'\r' {
            continue;
        }
        crc = CRC_TABLE[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8);
    }
    !crc
}

/// The same CRC-32 over every byte, carriage returns included: for binary
/// files (a track's baked sidecars), where a `0x0D` is data.
pub fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in bytes {
        crc = CRC_TABLE[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8);
    }
    !crc
}

static CRC_TABLE: [u32; 256] = crc_table();

const fn crc_table() -> [u32; 256] {
    let mut table = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 {
                0xEDB8_8320 ^ (c >> 1)
            } else {
                c >> 1
            };
            k += 1;
        }
        table[i] = c;
        i += 1;
    }
    table
}

#[cfg(test)]
mod tests {
    use super::{content_crc, crc32};

    #[test]
    fn matches_the_standard_check_vector() {
        assert_eq!(content_crc(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn empty_input_is_zero() {
        assert_eq!(content_crc(b""), 0);
    }

    #[test]
    fn line_endings_do_not_change_the_checksum() {
        let lf = b"name: Monza\ntrack_id: abc\n";
        let crlf = b"name: Monza\r\ntrack_id: abc\r\n";
        assert_eq!(content_crc(lf), content_crc(crlf));
        assert_eq!(
            content_crc(b"name: Mini\r\nclosed_loop: true\r\n"),
            content_crc(b"name: Mini\nclosed_loop: true\n")
        );
    }

    #[test]
    fn the_binary_crc_keeps_carriage_returns() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_ne!(crc32(b"a\r\nb"), crc32(b"a\nb"));
        assert_eq!(crc32(b"a\nb"), content_crc(b"a\nb"));
    }

    #[test]
    fn a_changed_byte_changes_the_checksum() {
        assert_ne!(content_crc(b"width_m: 12"), content_crc(b"width_m: 13"));
    }
}
