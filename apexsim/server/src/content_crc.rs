//! The content checksum a client compares against the server's.
//!
//! The server computes it as a car or track file is loaded
//! (`CarConfig::content_crc`, `TrackConfig::content_crc`) and sends it in the
//! lobby summaries as `ContentCrc`; the client compares it with the
//! `SourceCrc` its catalog row carries. The rule (CRC-32 over the file's
//! bytes with carriage returns dropped) is the formats crate's
//! (`apexsim-lib/formats/src/content_crc.rs`), with its C++ twin in
//! `ApexContentCrc.h`.

pub use apexsim_formats::content_crc::{content_crc, crc32};
