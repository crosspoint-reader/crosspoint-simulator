-- deploy.applescript — fire the iOS TestFlight deploy in Terminal.app.
--
-- Why: codesign and xcodebuild archive signing need the login keychain, which
-- only a GUI Terminal session under the logged-in user has. An SSH shell (or a
-- sandboxed agent shell) fails with errSecInternalComponent. Handing the
-- command to Terminal.app sidesteps that — the same trick as
-- crds-ios/scripts/mac/deploy.applescript, which this mirrors.
--
-- From the phone (Terminus/Blink, or a Shortcuts "Run Script Over SSH" action):
--   ssh <mac> 'osascript ~/src/crosspoint-simulator/ios/deploy.applescript'
--
-- Arguments are KEY=VALUE pairs passed through `env` into the Terminal
-- subshell (env vars on the osascript caller's shell are LOST otherwise):
--   osascript ios/deploy.applescript "CROSSPOINT_MARKETING_VERSION=0.1.1"
--
-- Requirements (one-time): allow osascript/sshd-launched processes to control
-- Terminal under Privacy & Security > Automation, and the Mac must be logged
-- in and unlocked. AppleScript returns once Terminal starts the command; the
-- result arrives as the testflight.sh ntfy on your phone.

on run argv
    set wrapperPath to "$HOME/src/crosspoint-simulator/ios/deploy-from-repo.sh"

    set envPrefix to ""
    repeat with arg in argv
        set envPrefix to envPrefix & (quoted form of (arg as text)) & " "
    end repeat

    set scriptCmd to ""
    if envPrefix is not "" then
        set scriptCmd to "env " & envPrefix
    end if
    set scriptCmd to scriptCmd & wrapperPath

    tell application "Terminal"
        activate
        do script scriptCmd
    end tell
end run
