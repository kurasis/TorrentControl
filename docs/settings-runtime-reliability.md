# Settings persistence and WebView2 compatibility

Settings updates, custom-profile creation/deletion and profile application save
the candidate settings before changing live state. A failed folder creation,
write, flush, close or replacement returns retryable `SETTINGS_WRITE_FAILED`.
The previous preferences, profiles, draft and saved file remain available;
the user can retry after restoring disk access. Failed preference controls
return to their saved values. This does not simulate full disks or promise
crash durability on every filesystem.

An explicitly memory-only service reports `persistence: "memory"`. The UI shows
a persistent session-only warning, including when the Windows host cannot
load its settings and starts with isolated defaults. That mode still allows
work, but does not claim that preferences or profiles survive a restart.

The Windows host checks **113.0.1774.30** before constructing the web UI.
This is the API floor for the DOM File / WebMessage `AdditionalObjects` API
used by Explorer drops. Microsoft introduced it in SDK 1.0.1774.30 and lists
that Runtime as the compatibility minimum in its
[SDK release notes](https://learn.microsoft.com/microsoft-edge/webview2/release-notes/archive?tabs=dotnetcsharp#10177430).
Use the current Evergreen Runtime; the API floor is not a recommendation to
install an old version. Missing, malformed and older versions show a native
explanation with the official installation/update link and exit with code 3.

Local verification: 158 Linux CTest cases and 37 Playwright cases passed.
Tests exercise blocked parent directories and failed file replacement, verify
unchanged live/disk state through service and bridge APIs, then restore access
and retry. Numeric Runtime comparisons cover boundary and malformed versions.
Windows CI also checks the real installed Runtime against a forced higher
test-only floor, before any web UI is opened. This checks the rejection path;
it does not replace installing the actual minimum Runtime on a clean machine.
Clean Windows 10/11 installation and offline deployment remain M4 gates.
