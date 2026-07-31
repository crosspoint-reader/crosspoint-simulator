-- deploy.applescript -- drive Terminal.app to run deploy.sh
--
-- Why this exists: codesign needs the login keychain, and only a GUI Terminal
-- session under the logged-in user has it. Firing deploy.sh from a sandboxed
-- agent shell (Claude Code's Bash tool, or a plain SSH session) fails with
-- errSecInternalComponent partway through signing. Handing the command to
-- Terminal.app sidesteps that, which is what lets an agent on the Mac deploy
-- unattended. This mirrors the crds-ios scripts/mac/deploy.applescript pattern.
--
-- Usage, from any shell ON the Mac (local or SSH):
--   osascript packaging/macos/deploy.applescript
--   osascript packaging/macos/deploy.applescript \
--       "MARKETING_VERSION=0.1.0" "BUILD_NUMBER=2" "SKIP_UPLOAD=1"
--
-- Every argument is a KEY=VALUE assignment, passed through `env` so it
-- survives into the login shell Terminal spawns. Environment variables set on
-- the osascript caller's shell are LOST otherwise -- a recurring deploy
-- failure in the crds-ios pipeline.
--
-- One-time setup:
--   System Settings > Privacy & Security > Automation: allow ssh/osascript to
--   control Terminal. macOS prompts on the Mac's screen the first time; if
--   nobody is at the screen to approve it, the script fails silently.
--   The Mac must be logged in and unlocked, or the keychain stays locked.
--
-- Limitation: AppleScript returns as soon as Terminal has been told to start.
-- It cannot wait for the deploy or report its exit status. Watch the Terminal
-- window, or poll App Store Connect for the build.
--
-- Set REPO_PATH below if this checkout lives somewhere else. It must point at
-- the firmware project that builds the simulator, not at the simulator library.

on run argv
    set repoPath to "$HOME/src/crosspoint"
    set deployScript to "./packaging/macos/deploy.sh"

    set envPrefix to ""
    repeat with arg in argv
        set envPrefix to envPrefix & (quoted form of (arg as text)) & " "
    end repeat

    set scriptCmd to "cd " & repoPath & " && "
    if envPrefix is not "" then
        set scriptCmd to scriptCmd & "env " & envPrefix
    end if
    set scriptCmd to scriptCmd & deployScript

    tell application "Terminal"
        activate
        do script scriptCmd
    end tell
end run
