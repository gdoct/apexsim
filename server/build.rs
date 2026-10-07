// Stamps the Windows executables with the release version. build_release.ps1
// sets APEXSIM_VERSION; a plain `cargo build` falls back to the crate version.
fn main() {
    println!("cargo:rerun-if-env-changed=APEXSIM_VERSION");
    println!("cargo:rerun-if-changed=build.rs");
    let version = std::env::var("APEXSIM_VERSION")
        .ok()
        .filter(|v| !v.trim().is_empty())
        .unwrap_or_else(|| std::env::var("CARGO_PKG_VERSION").unwrap());
    println!("cargo:rustc-env=APEXSIM_BUILD_VERSION={version}");

    #[cfg(windows)]
    {
        if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
            let mut res = winresource::WindowsResource::new();
            res.set("ProductName", "ApexSim Server");
            res.set("FileDescription", "ApexSim server");
            res.set("ProductVersion", &version);
            res.set("FileVersion", &version);
            // winresource reads Cargo's version for the numeric fields.
            let mut nums = [0u64; 3];
            for (slot, part) in nums.iter_mut().zip(version.split(['.', '-'])) {
                *slot = part.parse().unwrap_or(0);
            }
            let packed = (nums[0] << 48) | (nums[1] << 32) | (nums[2] << 16);
            res.set_version_info(winresource::VersionInfo::FILEVERSION, packed);
            res.set_version_info(winresource::VersionInfo::PRODUCTVERSION, packed);
            if let Err(e) = res.compile() {
                println!("cargo:warning=could not embed version resource: {e}");
            }
        }
    }
}
