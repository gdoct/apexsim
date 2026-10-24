//! Installing loose content packs (`apexsim-server content install
//! <file>.acp`): the cars, tracks and HUD components of a hand-made zip
//! ([`apexsim_formats::pack::acp`]) written into the `custom/` folders the
//! server and the game already read, in a release install or a checkout.
//!
//! ```text
//!                 release (<root>)                      checkout (apexsim/, apexsim-content/)
//! car             Game/Cars/custom/<name>/              apexsim-content/cars/custom/<name>/
//!                 Server/content/cars/custom/<name>/car.toml
//! track           Server/content/tracks/custom/<Stem>/  apexsim-content/tracks/custom/<Stem>/
//!                 Game/Tracks/<Stem>.*                  apexsim-content/build/tracks/<Stem>.*
//! HUD component   Game/Hud/custom/<id>/                 apexsim/hud/custom/<id>/
//! ```
//!
//! An item replaces the player's own item of its name (its folders are
//! emptied first) and never a shipped one: a track named as a shipped track,
//! or a car or track whose id a shipped or custom item already has, is
//! refused before anything is written, since both sides would ignore it.
//! A HUD component named as a shipped one replaces it on screen, as a
//! `custom/` component is meant to.

use apexsim_formats::pack::acp::{self, Acp, AcpItem, AcpKind};
use serde::Serialize;
use std::io::Read;
use std::path::{Path, PathBuf};

/// Where an install keeps its content.
#[derive(Debug, Clone)]
pub enum Layout {
    /// A release install: the folder holding `Server` and `Game`.
    Release(PathBuf),
    /// A source checkout: the code root (`apexsim/`) and the content root
    /// beside it (`apexsim-content/`).
    Checkout { code: PathBuf, content: PathBuf },
}

impl Layout {
    /// The checkout whose code root is `code`, with `apexsim-content` beside it.
    pub fn checkout(code: &Path) -> Result<Layout, String> {
        let content = code
            .parent()
            .map(|p| p.join("apexsim-content"))
            .unwrap_or_default();
        if !code.join("hud").is_dir() || !content.join("cars").is_dir() {
            return Err(format!(
                "{} is not a checkout's code root (with hud/ and ../apexsim-content/cars)",
                code.display()
            ));
        }
        Ok(Layout::Checkout {
            code: code.to_path_buf(),
            content,
        })
    }

    /// The server's cars and tracks folders (each holding `default/` and `custom/`).
    fn server_dir(&self, kind: &str) -> PathBuf {
        match self {
            Layout::Release(root) => root.join("Server").join("content").join(kind),
            Layout::Checkout { content, .. } => content.join(kind),
        }
    }

    /// The folder the game reads track exports from.
    fn export_dir(&self) -> PathBuf {
        match self {
            Layout::Release(root) => root.join("Game").join("Tracks"),
            Layout::Checkout { content, .. } => content.join("build").join("tracks"),
        }
    }

    fn hud_dir(&self, sub: &str) -> PathBuf {
        match self {
            Layout::Release(root) => root.join("Game").join("Hud").join(sub),
            Layout::Checkout { code, .. } => code.join("hud").join(sub),
        }
    }

    /// The folders a car of `name` is written to: the game's whole car and,
    /// in a release, the server's copy of its `car.toml`. One in a checkout,
    /// where both sides read `apexsim-content/cars`.
    fn car_dirs(&self, name: &str) -> (PathBuf, Option<PathBuf>) {
        match self {
            Layout::Release(root) => (
                root.join("Game").join("Cars").join("custom").join(name),
                Some(self.server_dir("cars").join("custom").join(name)),
            ),
            Layout::Checkout { content, .. } => {
                (content.join("cars").join("custom").join(name), None)
            }
        }
    }
}

/// One file to write: where, and which bytes of the pack.
struct Write<'a> {
    path: PathBuf,
    bytes: &'a [u8],
}

/// What installing one item does.
struct Plan<'a> {
    /// Folders emptied first (they belong to the item whole).
    clear_dirs: Vec<PathBuf>,
    /// Single files removed first (a track's export beside the others).
    clear_files: Vec<PathBuf>,
    writes: Vec<Write<'a>>,
    /// The folders written to, for the report.
    to: Vec<PathBuf>,
}

/// One item as the report gives it. `id`, `label`, `note` and `refused`
/// are always strings, so a reader without a JSON parser can line them up.
#[derive(Debug, Clone, Serialize)]
pub struct ItemReport {
    /// `<kind>:<name>` (`car:my-gt3`), what `--only` takes.
    pub id: String,
    pub kind: &'static str,
    pub name: String,
    /// `Car my-gt3`, for a person to read.
    pub label: String,
    /// What else to know: what it replaces, its warnings; empty when nothing.
    pub note: String,
    /// Why it cannot be installed; empty when it can.
    pub refused: String,
    /// Picked for this install (every item without `--only`).
    pub selected: bool,
    pub warnings: Vec<String>,
    pub files: usize,
    /// The player's own item of this name was there and is replaced.
    pub replaces: bool,
    /// Replaces a shipped HUD component on screen (a `custom/` override).
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    pub overrides_default: bool,
    /// The folders written to.
    pub to: Vec<String>,
}

/// What an install did (or, with `dry_run`, would do).
#[derive(Debug, Clone, Serialize)]
pub struct Report {
    pub installed: bool,
    /// Every item of the pack, picked or not.
    pub items: Vec<ItemReport>,
    pub ignored: Vec<String>,
    /// The picked items, one per line, for a person to read.
    pub summary: String,
}

/// The `id = "..."` of a `car.toml` or the `track_id:` of a track YAML:
/// the first such line at the top level. Only the head of the file is read.
fn item_id(bytes: &[u8], key: &str) -> Option<String> {
    let text = String::from_utf8_lossy(&bytes[..bytes.len().min(16 * 1024)]);
    for line in text.lines() {
        if line.starts_with('[') {
            return None;
        }
        let Some(rest) = line.strip_prefix(key) else {
            continue;
        };
        let rest = rest.trim_start();
        let Some(value) = rest.strip_prefix('=').or_else(|| rest.strip_prefix(':')) else {
            continue;
        };
        let value = value
            .split('#')
            .next()
            .unwrap_or("")
            .trim()
            .trim_matches(['"', '\'']);
        if !value.is_empty() {
            return Some(value.to_ascii_lowercase());
        }
    }
    None
}

fn read_head(path: &Path) -> Vec<u8> {
    let mut head = Vec::new();
    if let Ok(file) = std::fs::File::open(path) {
        let _ = file.take(16 * 1024).read_to_end(&mut head);
    }
    head
}

/// Every car's (`<cars>/<default|custom>/<folder>/car.toml`) or track's
/// (`<tracks>/<default|custom>/**/<Stem>.yaml`) id with the folder it is in,
/// as `<default|custom>/<folder>`.
fn installed_ids(dir: &Path, car: bool) -> Vec<(String, String)> {
    fn walk(dir: &Path, depth: usize, out: &mut Vec<PathBuf>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                if depth < 3 {
                    walk(&path, depth + 1, out);
                }
            } else if path.extension().is_some_and(|e| e == "yaml") {
                out.push(path);
            }
        }
    }
    let mut out = Vec::new();
    for sub in ["default", "custom"] {
        let base = dir.join(sub);
        let files: Vec<PathBuf> = if car {
            std::fs::read_dir(&base)
                .into_iter()
                .flatten()
                .flatten()
                .map(|e| e.path().join("car.toml"))
                .filter(|p| p.is_file())
                .collect()
        } else {
            let mut v = Vec::new();
            walk(&base, 0, &mut v);
            v
        };
        for file in files {
            let key = if car { "id" } else { "track_id" };
            let Some(id) = item_id(&read_head(&file), key) else {
                continue;
            };
            // The item's folder under default/ or custom/ (a loose track
            // file directly in custom/ is its own stem).
            let rel = file.strip_prefix(&base).unwrap_or(&file);
            let folder = match rel.components().count() {
                1 => rel.file_stem(),
                _ => rel.components().next().map(|c| c.as_os_str()),
            }
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
            out.push((id, format!("{sub}/{folder}")));
        }
    }
    out
}

/// The folders and single files that are an item of `kind` and `name` in
/// `layout`: what installing it empties first and uninstalling it removes.
/// A track's export files sit beside the others in one folder.
fn item_paths(layout: &Layout, kind: AcpKind, name: &str) -> (Vec<PathBuf>, Vec<PathBuf>) {
    match kind {
        AcpKind::Car => {
            let (game, server) = layout.car_dirs(name);
            (std::iter::once(game).chain(server).collect(), Vec::new())
        }
        AcpKind::Track => {
            let export = layout.export_dir();
            (
                vec![
                    layout.server_dir("tracks").join("custom").join(name),
                    export.join(format!("{name}.textures")),
                ],
                ["uescene.json", "uemesh", "png"]
                    .iter()
                    .map(|ext| export.join(format!("{name}.{ext}")))
                    .collect(),
            )
        }
        AcpKind::Hud => (vec![layout.hud_dir("custom").join(name)], Vec::new()),
    }
}

fn plan_item<'a>(layout: &Layout, item: &'a AcpItem) -> Plan<'a> {
    let (clear_dirs, clear_files) = item_paths(layout, item.kind, &item.name);
    let mut plan = Plan {
        clear_dirs,
        clear_files,
        writes: Vec::new(),
        to: Vec::new(),
    };
    let put = |plan: &mut Plan<'a>, dir: &Path, files: &'a [(String, Vec<u8>)]| {
        for (rel, bytes) in files {
            plan.writes.push(Write {
                path: dir.join(rel),
                bytes,
            });
        }
    };
    match item.kind {
        AcpKind::Car => {
            let (game, server) = layout.car_dirs(&item.name);
            put(&mut plan, &game, &item.files);
            plan.to.push(game);
            if let Some(server) = server {
                // The server reads only car.toml.
                for (rel, bytes) in &item.files {
                    if rel.eq_ignore_ascii_case("car.toml") {
                        plan.writes.push(Write {
                            path: server.join("car.toml"),
                            bytes,
                        });
                    }
                }
                plan.to.push(server);
            }
        }
        AcpKind::Track => {
            let server = layout.server_dir("tracks").join("custom").join(&item.name);
            put(&mut plan, &server, &item.files);
            let export = layout.export_dir();
            put(&mut plan, &export, &item.export);
            plan.to.extend([server, export]);
        }
        AcpKind::Hud => {
            let dir = layout.hud_dir("custom").join(&item.name);
            put(&mut plan, &dir, &item.files);
            plan.to.push(dir);
        }
    }
    plan
}

/// Why `item` may not go into `layout`, or `None`.
fn refusal(layout: &Layout, item: &AcpItem) -> Option<String> {
    let (key, car, kind) = match item.kind {
        AcpKind::Car => ("id", true, "cars"),
        AcpKind::Track => ("track_id", false, "tracks"),
        AcpKind::Hud => return None,
    };
    let dir = layout.server_dir(kind);
    if item.kind == AcpKind::Track && dir.join("default").join(&item.name).exists() {
        return Some(format!(
            "a shipped track is named {}: give the track another stem",
            item.name
        ));
    }
    let own = if car {
        "car.toml".to_string()
    } else {
        format!("{}.yaml", item.name)
    };
    let id = item
        .files
        .iter()
        .find(|(rel, _)| rel.eq_ignore_ascii_case(&own))
        .and_then(|(_, bytes)| item_id(bytes, key));
    let Some(id) = id else {
        return Some(format!(
            "the {} {} has no {key} in {own}",
            item.kind.label(),
            item.name
        ));
    };
    let mine = format!("custom/{}", item.name);
    installed_ids(&dir, car)
        .into_iter()
        .find(|(other, folder)| *other == id && *folder != mine)
        .map(|(_, folder)| {
            format!(
                "the {} {} has the {key} of {folder} ({id}): give it an {key} of its own",
                item.kind.label(),
                item.name
            )
        })
}

fn exists_any(plan: &Plan) -> bool {
    plan.clear_dirs.iter().any(|d| d.exists()) || plan.clear_files.iter().any(|f| f.exists())
}

fn write_file(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, bytes)
}

fn display(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

/// Install the loose pack at `pack_path` into `layout`: the items `only`
/// names (`car:my-gt3`, any case), or every item when it is empty. With
/// `dry_run`, only say what that would do, refusals included. Everything
/// is checked before anything is written; a picked item that cannot be
/// installed stops the install, one left out does not.
pub fn install(
    layout: &Layout,
    pack_path: &Path,
    dry_run: bool,
    only: &[String],
) -> Result<Report, String> {
    let Acp { items, ignored } = acp::read(pack_path).map_err(|e| e.to_string())?;
    let id_of = |item: &AcpItem| format!("{}:{}", item.kind.label(), item.name);
    for wanted in only {
        if !items.iter().any(|i| id_of(i).eq_ignore_ascii_case(wanted)) {
            return Err(format!("the pack holds no {wanted}"));
        }
    }
    let selected: Vec<bool> = items
        .iter()
        .map(|i| only.is_empty() || only.iter().any(|w| id_of(i).eq_ignore_ascii_case(w)))
        .collect();
    let plans: Vec<Plan> = items.iter().map(|item| plan_item(layout, item)).collect();

    let mut rows = Vec::new();
    let mut summary = Vec::new();
    for ((item, plan), &picked) in items.iter().zip(&plans).zip(&selected) {
        let refused = item
            .problem
            .clone()
            .or_else(|| refusal(layout, item))
            .unwrap_or_default();
        if picked && !refused.is_empty() && !dry_run {
            return Err(format!("{} {}: {refused}", item.kind.label(), item.name));
        }
        let replaces = exists_any(plan);
        let overrides_default =
            item.kind == AcpKind::Hud && layout.hud_dir("default").join(&item.name).is_dir();
        let what = match item.kind {
            AcpKind::Car => "Car",
            AcpKind::Track => "Track",
            AcpKind::Hud => "HUD component",
        };
        let label = format!("{what} {}", item.name);
        let mut notes: Vec<String> = Vec::new();
        if replaces {
            notes.push("replaces your own".into());
        } else if overrides_default {
            notes.push("replaces the shipped one".into());
        }
        notes.extend(item.warnings.iter().cloned());
        let note = notes.join("; ");
        if picked {
            summary.push(if note.is_empty() {
                label.clone()
            } else {
                format!("{label} ({note})")
            });
        }
        rows.push(ItemReport {
            id: id_of(item),
            kind: item.kind.label(),
            name: item.name.clone(),
            label,
            note,
            refused,
            selected: picked,
            warnings: item.warnings.clone(),
            files: item.files.len() + item.export.len(),
            replaces,
            overrides_default,
            to: plan.to.iter().map(|d| display(d)).collect(),
        });
    }

    if !dry_run {
        for (plan, _) in plans.iter().zip(&selected).filter(|(_, &picked)| picked) {
            for dir in &plan.clear_dirs {
                if dir.is_dir() {
                    std::fs::remove_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
                }
            }
            for file in &plan.clear_files {
                if file.is_file() {
                    std::fs::remove_file(file).map_err(|e| format!("{}: {e}", file.display()))?;
                }
            }
            for w in &plan.writes {
                write_file(&w.path, w.bytes).map_err(|e| format!("{}: {e}", w.path.display()))?;
            }
        }
    }
    Ok(Report {
        installed: !dry_run,
        items: rows,
        ignored,
        summary: summary.join("\n"),
    })
}

/// What uninstalling one item removed.
#[derive(Debug, Clone, Serialize)]
pub struct Removed {
    /// `<kind>:<name>`, as given.
    pub id: String,
    /// The folders and files removed.
    pub removed: Vec<String>,
    /// A shipped HUD component of this id is on screen again.
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    pub restores_default: bool,
}

/// Remove the player's own items `ids` names (`car:<folder>`,
/// `track:<Stem>`, `hud:<id>`, any case) from the `custom/` folders of
/// `layout`, whether a pack installed them or the player put them there.
/// Every id is checked before anything is removed: a malformed one or one
/// with nothing installed stops it. Shipped content is never touched: a
/// track export whose stem a shipped track has stays.
pub fn uninstall(layout: &Layout, ids: &[String]) -> Result<Vec<Removed>, String> {
    if ids.is_empty() {
        return Err("name the items to uninstall (car:<folder>, track:<Stem>, hud:<id>)".into());
    }
    let mut plans = Vec::new();
    for id in ids {
        let (kind, name) = id
            .split_once(':')
            .ok_or_else(|| format!("{id} is not <kind>:<name>"))?;
        let kind = match kind.to_ascii_lowercase().as_str() {
            "car" => AcpKind::Car,
            "track" => AcpKind::Track,
            "hud" => AcpKind::Hud,
            _ => return Err(format!("{id}: the kind is car, track or hud")),
        };
        if !acp::valid_name(name) {
            return Err(format!(
                "{id}: not a folder name of letters, digits, - and _"
            ));
        }
        let (mut dirs, mut files) = item_paths(layout, kind, name);
        if kind == AcpKind::Track
            && layout
                .server_dir("tracks")
                .join("default")
                .join(name)
                .exists()
        {
            // The export is the shipped track's: only the server folder is ours.
            dirs.truncate(1);
            files.clear();
        }
        dirs.retain(|d| d.is_dir());
        files.retain(|f| f.is_file());
        if dirs.is_empty() && files.is_empty() {
            return Err(format!("{id}: none of your own is installed"));
        }
        let restores_default =
            kind == AcpKind::Hud && layout.hud_dir("default").join(name).is_dir();
        plans.push((id, dirs, files, restores_default));
    }
    let mut out = Vec::new();
    for (id, dirs, files, restores_default) in plans {
        for dir in &dirs {
            std::fs::remove_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        }
        for file in &files {
            std::fs::remove_file(file).map_err(|e| format!("{}: {e}", file.display()))?;
        }
        out.push(Removed {
            id: id.clone(),
            removed: dirs.iter().chain(&files).map(|p| display(p)).collect(),
            restores_default,
        });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write as _;

    fn release() -> PathBuf {
        let root = std::env::temp_dir().join(format!("apexsim-acp-{}", uuid::Uuid::new_v4()));
        for dir in [
            "Server/content/cars/default/shipped",
            "Server/content/tracks/default/Shipped",
            "Game/Cars/default/shipped",
            "Game/Hud/default/standings",
            "Game/Tracks",
        ] {
            std::fs::create_dir_all(root.join(dir)).unwrap();
        }
        std::fs::write(
            root.join("Server/content/cars/default/shipped/car.toml"),
            "id = \"aaaa\"\nname = \"Shipped\"\n",
        )
        .unwrap();
        std::fs::write(
            root.join("Server/content/tracks/default/Shipped/Shipped.yaml"),
            "name: Shipped\ntrack_id: bbbb\n",
        )
        .unwrap();
        root
    }

    type Files<'a> = [(&'a str, &'a str)];

    fn pack(dir: &Path, name: &str, files: &Files) -> PathBuf {
        let path = dir.join(name);
        let mut zip = zip::ZipWriter::new(std::fs::File::create(&path).unwrap());
        let options = zip::write::SimpleFileOptions::default();
        for (name, text) in files {
            zip.start_file(*name, options).unwrap();
            zip.write_all(text.as_bytes()).unwrap();
        }
        zip.finish().unwrap();
        path
    }

    const RING: [(&str, &str); 5] = [
        ("Ring/Ring.yaml", "name: Ring\ntrack_id: cccc\n"),
        ("Ring/Ring.road.msgpack", "road"),
        ("Ring.uescene.json", "{}"),
        ("Ring.uemesh", "mesh"),
        ("Ring.textures/road.png", "png"),
    ];

    #[test]
    fn a_pack_of_each_kind_lands_where_the_server_and_the_game_read_it() {
        let root = release();
        let mut files = vec![
            ("my-car/car.toml", "id = \"dddd\"\nname = \"Mine\"\n"),
            ("my-car/body.glb", "glb"),
            ("hud/standings/component.json", "{}"),
            ("hud/my_gauge/component.json", "{}"),
        ];
        files.extend(RING);
        let path = pack(&root, "stuff.acp", &files);
        let layout = Layout::Release(root.clone());

        let dry = install(&layout, &path, true, &[]).unwrap();
        assert!(!dry.installed);
        assert!(
            !root.join("Game/Cars/custom/my-car").exists(),
            "a dry run writes nothing"
        );
        assert_eq!(
            dry.summary,
            "Car my-car\n\
             Track Ring (no Ring.ground.msgpack; no Ring.curbs.msgpack; no Ring.walls.msgpack; no Ring.pit.msgpack)\n\
             HUD component my_gauge\n\
             HUD component standings (replaces the shipped one)"
        );

        let done = install(&layout, &path, false, &[]).unwrap();
        assert!(done.installed);
        assert_eq!(
            std::fs::read(root.join("Game/Cars/custom/my-car/body.glb")).unwrap(),
            b"glb"
        );
        assert!(root
            .join("Server/content/cars/custom/my-car/car.toml")
            .is_file());
        assert!(!root
            .join("Server/content/cars/custom/my-car/body.glb")
            .exists());
        assert!(root
            .join("Server/content/tracks/custom/Ring/Ring.yaml")
            .is_file());
        assert!(root
            .join("Server/content/tracks/custom/Ring/Ring.road.msgpack")
            .is_file());
        assert!(root.join("Game/Tracks/Ring.uemesh").is_file());
        assert!(root.join("Game/Tracks/Ring.textures/road.png").is_file());
        assert!(root
            .join("Game/Hud/custom/my_gauge/component.json")
            .is_file());
        assert!(root
            .join("Game/Hud/custom/standings/component.json")
            .is_file());
        let ring = done.items.iter().find(|i| i.id == "track:Ring").unwrap();
        assert_eq!(ring.warnings.len(), 4, "{:?}", ring.warnings);

        // Installed again: the player's own items are replaced, a file the
        // new version dropped does not linger.
        std::fs::write(root.join("Game/Cars/custom/my-car/old.png"), "x").unwrap();
        let again = install(&layout, &path, false, &[]).unwrap();
        assert!(again.items.iter().all(|i| i.replaces));
        assert!(!root.join("Game/Cars/custom/my-car/old.png").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_shipped_name_or_id_is_refused_before_anything_is_written() {
        let root = release();
        let layout = Layout::Release(root.clone());
        let cases = [
            (
                "stem.acp",
                vec![
                    ("Shipped/Shipped.yaml", "track_id: eeee\n"),
                    ("Shipped.uescene.json", "{}"),
                    ("Shipped.uemesh", "m"),
                ],
                "a shipped track is named Shipped",
            ),
            (
                "car.acp",
                vec![
                    ("copy/car.toml", "id = \"AAAA\" # copied\n"),
                    ("copy/x.glb", "x"),
                ],
                "the id of default/shipped",
            ),
            (
                "track.acp",
                vec![
                    ("Ring/Ring.yaml", "track_id: bbbb\n"),
                    ("Ring.uescene.json", "{}"),
                    ("Ring.uemesh", "m"),
                ],
                "the track_id of default/Shipped",
            ),
        ];
        for (name, files, expected) in cases {
            let err = install(&layout, &pack(&root, name, &files), false, &[]).unwrap_err();
            assert!(err.contains(expected), "{name}: {err}");
        }
        assert!(!root.join("Game/Cars/custom").exists());
        assert!(!root.join("Server/content/tracks/custom").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn only_the_picked_items_install_and_one_left_out_may_be_refused() {
        let root = release();
        let layout = Layout::Release(root.clone());
        let path = pack(
            &root,
            "mixed.acp",
            &[
                ("mine/car.toml", "id = \"dddd\"\n"),
                ("copy/car.toml", "id = \"aaaa\"\n"),
                ("hud/gauge/component.json", "{}"),
            ],
        );
        let dry = install(&layout, &path, true, &[]).unwrap();
        let refused: Vec<(&str, bool)> = dry
            .items
            .iter()
            .map(|i| (i.id.as_str(), i.refused.is_empty()))
            .collect();
        assert_eq!(
            refused,
            [("car:copy", false), ("car:mine", true), ("hud:gauge", true)]
        );
        assert!(
            install(&layout, &path, false, &[]).is_err(),
            "all of it: copy is refused"
        );

        let only = ["CAR:mine".to_string(), "hud:gauge".to_string()];
        let done = install(&layout, &path, false, &only).unwrap();
        assert_eq!(done.summary, "Car mine\nHUD component gauge");
        assert!(root.join("Game/Cars/custom/mine/car.toml").is_file());
        assert!(!root.join("Game/Cars/custom/copy").exists());
        assert!(root.join("Game/Hud/custom/gauge/component.json").is_file());

        let err = install(&layout, &path, true, &["track:nope".to_string()]).unwrap_err();
        assert!(err.contains("holds no track:nope"), "{err}");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_checkout_takes_a_car_once_and_the_export_into_build_tracks() {
        let base = std::env::temp_dir().join(format!("apexsim-acp-{}", uuid::Uuid::new_v4()));
        let code = base.join("apexsim");
        std::fs::create_dir_all(code.join("hud/default")).unwrap();
        std::fs::create_dir_all(base.join("apexsim-content/cars/default")).unwrap();
        let layout = Layout::checkout(&code).unwrap();
        let mut files = vec![("car.toml", "id = \"ffff\"\n")];
        files.extend(RING);
        install(&layout, &pack(&base, "Their Car.acp", &files), false, &[]).unwrap();
        let content = base.join("apexsim-content");
        assert!(content.join("cars/custom/Their-Car/car.toml").is_file());
        assert!(content.join("tracks/custom/Ring/Ring.yaml").is_file());
        assert!(content.join("build/tracks/Ring.uescene.json").is_file());
        assert!(Layout::checkout(&base).is_err());
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn uninstall_removes_both_sides_and_leaves_shipped_content() {
        let root = release();
        let layout = Layout::Release(root.clone());
        let mut files = vec![
            (
                "my-car/car.toml",
                "id = \"dddd\"
",
            ),
            ("hud/standings/component.json", "{}"),
        ];
        files.extend(RING);
        install(&layout, &pack(&root, "stuff.acp", &files), false, &[]).unwrap();
        // A hand-made track named as the shipped one: its export is the shipped track's.
        std::fs::create_dir_all(root.join("Server/content/tracks/custom/Shipped")).unwrap();
        std::fs::write(root.join("Game/Tracks/Shipped.uescene.json"), "{}").unwrap();

        let err = uninstall(&layout, &["car:my-car".into(), "car:nope".into()]).unwrap_err();
        assert!(err.contains("car:nope"), "{err}");
        assert!(
            root.join("Game/Cars/custom/my-car").is_dir(),
            "nothing removed on an error"
        );
        assert!(uninstall(&layout, &["car:../default".into()]).is_err());

        let ids: Vec<String> = ["CAR:my-car", "track:Ring", "hud:standings", "track:Shipped"]
            .map(String::from)
            .into();
        let done = uninstall(&layout, &ids).unwrap();
        assert!(done[2].restores_default);
        for gone in [
            "Game/Cars/custom/my-car",
            "Server/content/cars/custom/my-car",
            "Server/content/tracks/custom/Ring",
            "Game/Tracks/Ring.uemesh",
            "Game/Tracks/Ring.textures",
            "Game/Hud/custom/standings",
            "Server/content/tracks/custom/Shipped",
        ] {
            assert!(!root.join(gone).exists(), "{gone}");
        }
        assert!(root.join("Game/Tracks/Shipped.uescene.json").is_file());
        assert!(root.join("Game/Hud/default/standings").is_dir());
        assert!(root
            .join("Server/content/cars/default/shipped/car.toml")
            .is_file());
        let _ = std::fs::remove_dir_all(&root);
    }
}
