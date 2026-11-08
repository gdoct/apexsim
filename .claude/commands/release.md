---
description: Build, smoke-test and publish a new ApexSim release on GitHub
---

Make a new release and publish it on GitHub. Every path here is under
`apexsim/` (the code root): run the commands from there. Read
`docs/building.md`, section "Release package" and "Publishing a release",
first. The arguments, if any, are the version (`0.4.5`) or `minor` / `patch`
(default `patch`), and `draft` to publish a draft release.

Publishing pushes `main` and a tag to `origin` and creates a public release:
do it only when the person asked for the release to be published in this
conversation. Otherwise stop after step 5 and say it is ready to publish.

1. **Ready to build.** `git status`: every change that belongs in the
   release is committed. Changes that are not yours to commit (the
   person's own edits, a site refresh waiting for review) are left as they
   are and kept out of every commit; they do not go into the package, and
   step 6 stashes them around the publish. No editor or server may be
   running from the folders the build writes: the build compiles
   `server/server/target/release` (a running server there locks its exe)
   and runs commandlets (the editor must be closed). Some 60 GB free on
   the drive.
2. **The version.** `python scripts/publish_release.py next` (or `next
   --minor`) gives the version after the latest GitHub release; the
   person's version wins. `python scripts/publish_release.py bump <v>`
   writes it into `DefaultGame.ini` and commits it, so the package's
   `release.json` commit is the release's own.
3. **Build.** `powershell -ExecutionPolicy Bypass -File
   scripts/build_release.ps1` in the background (it takes an hour or
   more: the server, props, every track's bake, a Shipping client, the
   content pack, the zip), its output to `out/release-<v>.log`. If a stage
   fails, fix the cause, commit the fix, and run it again with the earlier
   stages skipped (`-SkipServer`, `-SkipProps`, `-SkipTracks`,
   `-SkipClient`) only when the fix did not touch them; then `bump` is not
   needed again, but the package must end up built from HEAD (step 6
   checks `release.json`'s commit).
4. **Smoke test.** `python scripts/smoke_release.py <v>`: the layout, the
   content pack installing dry, the packaged server on ports 9100-9104,
   and a short auto race in the packaged game with two grabs in
   `out/release-smoke/<v>/`. Look at the grabs: a race on screen with the
   HUD, not a menu, a black frame or an error. A failure stops the
   release: fix, rebuild, test again.
5. **Notes.** `python scripts/publish_release.py since` prints the
   features-by-date days and the commits since the last release (add the
   missing days to `docs/features-by-date.md` first, and commit them).
   Write `artifacts/release/RELEASE_NOTES-<v>.md` for players, in the
   voice and shape of the last release's notes (`gh release view` shows
   them): `# ApexSim <v>`, "Changes since <previous>.", a Downloads
   section naming the zip and the content pack as before, then sections
   such as New ways to play, Race rules, Driving, Watching and sharing,
   HUD and menus, Graphics, Hosting, Fixes, Experimental. Plain sentences,
   what a player sees or can do; no commit hashes, item ids, file paths
   or code names, no superlatives. Circuits and cars by their display
   names only, never a real circuit, series or car name (`naming.md`).
   Say what is off by default and how to turn it on. Leave out what does
   not reach a player (tests, docs, tooling, verification runs) and what
   is deferred (docs/simulation-gaps.md, docs/follow-ups.md). Check each
   claim against the code or its doc.
6. **Publish.** Stash what is not the release's (`git stash push -- <paths>`),
   then `python scripts/publish_release.py check <v>` must print "ready to
   publish". `python scripts/publish_release.py publish <v>` (add
   `--draft` when asked) pushes `main`, tags `v<v>` at HEAD, pushes the tag
   and creates the release with the zip, the content pack and the notes,
   then checks each upload's size; the upload takes a while (some 1.3 GB).
   `git stash pop` afterwards.
7. **After.** Open the release's URL (`gh release view v<v>`) and check its
   assets are there; add a "Release" line for the version to the day's
   section of `docs/features-by-date.md` and commit it (it goes out with
   the next push). Report the URL, the version, the asset sizes, what the
   smoke test showed and anything left out of the notes on purpose.
