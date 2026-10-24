//! A player's own livery as it travels between clients (N11): the
//! `livery.toml` fields of one `[[livery]]` table and the images it names,
//! packed as one MessagePack map (`LiveryPackage`, named keys). The client
//! uploads it in `UploadLivery` chunks, the server checks it
//! (`LiveryPackage::check`) and keeps it by the SHA-256 of its bytes, the
//! roster names that hash on the car that wears it (`RosterEntry.OwnLivery`),
//! and another client fetches it in `LiveryChunk`s (`RequestLivery`).
//!
//! The server never looks inside an image beyond its header: PNG or JPEG
//! only, sides within `MAX_IMAGE_SIDE`, the whole package within
//! `MAX_LIVERY_BYTES`. What the images show is the player's own; no check
//! for trademarks is possible.

use crate::own_car::RecordBytes;
use serde::{Deserialize, Serialize};

/// A package's bytes at most (all its images and fields): 1 MiB.
pub const MAX_LIVERY_BYTES: usize = 1 << 20;
/// The bytes of one `UploadLivery` or `LiveryChunk`: 256 KiB, so a package
/// is four at most.
pub const LIVERY_CHUNK_BYTES: usize = 256 * 1024;
/// The most chunks a package comes in.
pub const MAX_LIVERY_CHUNKS: u32 = MAX_LIVERY_BYTES.div_ceil(LIVERY_CHUNK_BYTES) as u32;
/// An image's width and height at most, pixels.
pub const MAX_IMAGE_SIDE: u32 = 4096;
/// The named texture slots a package may carry at most.
pub const MAX_LIVERY_TEXTURES: usize = 8;
/// A livery's name, characters at most.
pub const MAX_LIVERY_NAME_CHARS: usize = 40;

/// One `[[livery]]` table of a player's own `livery.toml`, its images
/// inlined. Colours are linear RGB in 0..1, as in car.toml.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct LiveryPackage {
    /// The car it is painted for, its `CarConfigId` as a string: the
    /// server takes it only from a driver of that car.
    pub car: String,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub paint: Option<[f32; 3]>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub accent: Option<[f32; 3]>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub metallic: Option<f32>,
    /// The `car_logo` texture (PNG or JPEG).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub logo: Option<RecordBytes>,
    /// Every `car_skin*` slot's texture.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub skin: Option<RecordBytes>,
    /// Named slots' textures (`textures = ["SLOT=file", ...]`).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub textures: Vec<LiveryTexture>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct LiveryTexture {
    pub slot: String,
    pub image: RecordBytes,
}

/// Why a package is refused.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum LiveryError {
    #[error("the livery is {0} bytes, over the {MAX_LIVERY_BYTES} allowed")]
    TooLarge(usize),
    #[error("the livery does not read: {0}")]
    Unreadable(String),
    #[error("the livery's car is not a car id")]
    BadCar,
    #[error("the livery needs a name of 1 to {MAX_LIVERY_NAME_CHARS} characters")]
    BadName,
    #[error("the livery's colours must be within 0 to 1")]
    BadColour,
    #[error("the livery needs a paint, a skin or textures")]
    Empty,
    #[error("the livery's {0} is not a PNG or JPEG within {MAX_IMAGE_SIDE} pixels a side")]
    BadImage(String),
    #[error("the livery's texture slots must be 1 to {MAX_LIVERY_TEXTURES} distinct names of letters, digits and _")]
    BadSlots,
}

impl LiveryPackage {
    /// The package in `bytes`, checked: its size, its fields, each image's
    /// header.
    pub fn check(bytes: &[u8]) -> Result<Self, LiveryError> {
        if bytes.len() > MAX_LIVERY_BYTES {
            return Err(LiveryError::TooLarge(bytes.len()));
        }
        let package: LiveryPackage =
            rmp_serde::from_slice(bytes).map_err(|e| LiveryError::Unreadable(e.to_string()))?;
        if uuid::Uuid::parse_str(&package.car).is_err() {
            return Err(LiveryError::BadCar);
        }
        let name = package.name.trim();
        let chars = name.chars().count();
        if chars == 0 || chars > MAX_LIVERY_NAME_CHARS || name.chars().any(char::is_control) {
            return Err(LiveryError::BadName);
        }
        let unit = |v: f32| v.is_finite() && (0.0..=1.0).contains(&v);
        for colour in [package.paint, package.accent].into_iter().flatten() {
            if !colour.iter().all(|&c| unit(c)) {
                return Err(LiveryError::BadColour);
            }
        }
        if package.metallic.is_some_and(|m| !unit(m)) {
            return Err(LiveryError::BadColour);
        }
        if package.paint.is_none() && package.skin.is_none() && package.textures.is_empty() {
            return Err(LiveryError::Empty);
        }
        if package.textures.len() > MAX_LIVERY_TEXTURES {
            return Err(LiveryError::BadSlots);
        }
        let mut slots = std::collections::BTreeSet::new();
        for texture in &package.textures {
            let slot = texture.slot.as_str();
            if slot.is_empty()
                || slot.len() > 64
                || !slot.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
                || !slots.insert(slot.to_ascii_lowercase())
            {
                return Err(LiveryError::BadSlots);
            }
        }
        let images = package
            .logo
            .iter()
            .map(|i| ("logo", i))
            .chain(package.skin.iter().map(|i| ("skin", i)))
            .chain(package.textures.iter().map(|t| (t.slot.as_str(), &t.image)));
        for (what, image) in images {
            if image_size(&image.0).is_none() {
                return Err(LiveryError::BadImage(what.to_string()));
            }
        }
        Ok(package)
    }
}

/// An image's width and height when it is a PNG or a JPEG within
/// `MAX_IMAGE_SIDE` a side (read from its header: PNG's IHDR, JPEG's first
/// start-of-frame), else `None`.
pub fn image_size(bytes: &[u8]) -> Option<(u32, u32)> {
    let within = |w: u32, h: u32| {
        (w > 0 && h > 0 && w <= MAX_IMAGE_SIDE && h <= MAX_IMAGE_SIDE).then_some((w, h))
    };
    const PNG: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
    if bytes.len() >= 24 && bytes[..8] == PNG && &bytes[12..16] == b"IHDR" {
        let w = u32::from_be_bytes(bytes[16..20].try_into().ok()?);
        let h = u32::from_be_bytes(bytes[20..24].try_into().ok()?);
        return within(w, h);
    }
    if bytes.len() >= 4 && bytes[..3] == [0xFF, 0xD8, 0xFF] {
        // Walk the markers to the first SOFn (C0-CF but C4, C8, CC).
        let mut at = 2;
        while at + 4 <= bytes.len() {
            if bytes[at] != 0xFF {
                return None;
            }
            let marker = bytes[at + 1];
            if marker == 0xFF {
                at += 1;
                continue;
            }
            let len = u16::from_be_bytes([bytes[at + 2], bytes[at + 3]]) as usize;
            if len < 2 {
                return None;
            }
            let sof = (0xC0..=0xCF).contains(&marker) && ![0xC4, 0xC8, 0xCC].contains(&marker);
            if sof {
                if at + 9 > bytes.len() {
                    return None;
                }
                let h = u16::from_be_bytes([bytes[at + 5], bytes[at + 6]]) as u32;
                let w = u16::from_be_bytes([bytes[at + 7], bytes[at + 8]]) as u32;
                return within(w, h);
            }
            at += 2 + len;
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The smallest PNG header that reads as `w` x `h`.
    pub fn png(w: u32, h: u32) -> Vec<u8> {
        let mut b = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13];
        b.extend_from_slice(b"IHDR");
        b.extend_from_slice(&w.to_be_bytes());
        b.extend_from_slice(&h.to_be_bytes());
        b.extend_from_slice(&[8, 6, 0, 0, 0]);
        b
    }

    fn jpeg(w: u16, h: u16) -> Vec<u8> {
        let mut b = vec![0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46];
        b.extend_from_slice(&[0xFF, 0xC0, 0x00, 0x11, 0x08]);
        b.extend_from_slice(&h.to_be_bytes());
        b.extend_from_slice(&w.to_be_bytes());
        b.extend_from_slice(&[3; 10]);
        b
    }

    fn package() -> LiveryPackage {
        LiveryPackage {
            car: "99999999-8888-7777-6666-555555555555".into(),
            name: "Mine".into(),
            paint: Some([0.6, 0.05, 0.04]),
            accent: Some([1.0, 1.0, 1.0]),
            metallic: None,
            logo: Some(RecordBytes(png(512, 128))),
            skin: None,
            textures: Vec::new(),
        }
    }

    fn bytes(p: &LiveryPackage) -> Vec<u8> {
        rmp_serde::to_vec_named(p).unwrap()
    }

    #[test]
    fn a_package_reads_back_as_itself() {
        let p = package();
        assert_eq!(LiveryPackage::check(&bytes(&p)), Ok(p));
    }

    #[test]
    fn image_headers_give_their_size() {
        assert_eq!(image_size(&png(512, 128)), Some((512, 128)));
        assert_eq!(image_size(&jpeg(640, 480)), Some((640, 480)));
        assert_eq!(image_size(&png(8192, 16)), None, "over the side");
        assert_eq!(image_size(&png(0, 16)), None, "empty");
        assert_eq!(
            image_size(b"GIF89a.................."),
            None,
            "not PNG or JPEG"
        );
        assert_eq!(image_size(&[0xFF, 0xD8, 0xFF]), None, "a cut JPEG");
    }

    #[test]
    fn bad_packages_are_refused() {
        let refused = |edit: &dyn Fn(&mut LiveryPackage)| {
            let mut p = package();
            edit(&mut p);
            LiveryPackage::check(&bytes(&p)).unwrap_err()
        };
        assert_eq!(refused(&|p| p.car = "a car".into()), LiveryError::BadCar);
        assert_eq!(refused(&|p| p.name = "  ".into()), LiveryError::BadName);
        assert_eq!(refused(&|p| p.name = "x".repeat(41)), LiveryError::BadName);
        assert_eq!(
            refused(&|p| p.name = "a\u{7}b".into()),
            LiveryError::BadName
        );
        assert_eq!(
            refused(&|p| p.paint = Some([1.5, 0.0, 0.0])),
            LiveryError::BadColour
        );
        assert_eq!(
            refused(&|p| p.accent = Some([f32::NAN, 0.0, 0.0])),
            LiveryError::BadColour
        );
        assert_eq!(
            refused(&|p| p.metallic = Some(-0.1)),
            LiveryError::BadColour
        );
        assert_eq!(refused(&|p| p.paint = None), LiveryError::Empty);
        assert_eq!(
            refused(&|p| p.logo = Some(RecordBytes(b"<svg/>".to_vec()))),
            LiveryError::BadImage("logo".into())
        );
        assert_eq!(
            refused(&|p| p.textures = vec![LiveryTexture {
                slot: "EXT RIM".into(),
                image: RecordBytes(png(4, 4)),
            }]),
            LiveryError::BadSlots
        );
        let twice = |p: &mut LiveryPackage| {
            p.textures = ["Rim", "RIM"]
                .map(|slot| LiveryTexture {
                    slot: slot.into(),
                    image: RecordBytes(png(4, 4)),
                })
                .to_vec()
        };
        assert_eq!(refused(&twice), LiveryError::BadSlots);
        assert!(matches!(
            LiveryPackage::check(b"not msgpack at all"),
            Err(LiveryError::Unreadable(_))
        ));
        let mut big = png(64, 64);
        big.resize(MAX_LIVERY_BYTES, 0);
        let mut p = package();
        p.skin = Some(RecordBytes(big));
        assert!(matches!(
            LiveryPackage::check(&bytes(&p)),
            Err(LiveryError::TooLarge(_))
        ));
    }

    /// A package as the client packs it, every field set: `cargo test
    /// livery_package_wire_format -- --nocapture` -> `ApexGolden::C_LiveryPackage`.
    #[test]
    fn livery_package_wire_format() {
        let p = LiveryPackage {
            car: "99999999-8888-7777-6666-555555555555".into(),
            name: "Mine".into(),
            paint: Some([0.5, 0.25, 0.0]),
            accent: Some([1.0, 1.0, 1.0]),
            metallic: Some(0.5),
            logo: Some(RecordBytes(png(2, 1))),
            skin: Some(RecordBytes(jpeg(2, 1))),
            textures: vec![LiveryTexture {
                slot: "EXT_RIM".into(),
                image: RecordBytes(png(1, 1)),
            }],
        };
        let b = bytes(&p);
        println!(
            "C_LiveryPackage: {}",
            b.iter()
                .map(|x| format!("0x{:02X}", x))
                .collect::<Vec<_>>()
                .join(", ")
        );
        assert_eq!(LiveryPackage::check(&b), Ok(p));
    }

    #[test]
    fn a_package_comes_in_four_chunks_at_most() {
        assert_eq!(MAX_LIVERY_CHUNKS, 4);
    }
}
