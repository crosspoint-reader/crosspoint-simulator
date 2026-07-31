# Dispatch: ship a CrossPoint X3 TestFlight build

Self-contained prompt for a Claude session running **on the Mac** (the deploy
trigger in this project, per the crds-ios pattern). Paste the block below into
that session verbatim. It carries everything a fresh session needs — no prior
context assumed.

---

Ship the next CrossPoint X3 iOS TestFlight build.

Repo: `~/src/crosspoint-simulator`, branch `feat/ios-x3-port`.

Run:

```bash
osascript ~/src/crosspoint-simulator/ios/deploy.applescript
```

That opens Terminal.app (required: codesign needs the login keychain, which
only a GUI Terminal session has — running the deploy directly from your shell
fails with `errSecInternalComponent`). Terminal runs
`ios/deploy-from-repo.sh`, which pulls the branch `--ff-only` and executes
`ios/testflight.sh`: desktop canary build, Xcode archive, IPA export,
purpose-string verification inside the IPA, `altool` upload, `build-N` tag
push, and an ntfy to the operator's phone with the result.

The AppleScript returns immediately; the deploy runs on in the Terminal tab.
Follow it there (or `tail -f` the log path testflight.sh prints). Success =
the 🚀 ntfy fires and `git tag` shows a new `build-N`.

Failure playbook (testflight.sh prints these too):
- **90382 / upload limit**: per-marketing-version daily cap. Re-fire with
  `osascript ios/deploy.applescript "CROSSPOINT_MARKETING_VERSION=<x.y.z+1>"`.
- **rc 19 / "Cannot determine the Apple ID"**: usually an expired App Store
  Connect agreement, not a key problem — check Agreements, Tax, and Banking.
- **ITMS-90683**: a purpose string regressed. Stop and report; do not strip
  the verify gate.
- **`errSecInternalComponent`**: the Mac is locked or the command escaped
  Terminal. Unlock the Mac; re-fire via the AppleScript, never directly.
- **git pull refuses (non-ff)**: the Mac checkout diverged. Reconcile it
  (stash or reset to origin) before re-firing; do not force-push.

Preconditions (normally already true on this Mac): logged in and unlocked;
Automation permission for Terminal control granted; ASC key at
`~/.appstoreconnect/private_keys/`.
