# UI audit remediation

Reviewed with the repository's `web-design-guidelines` skill. The current
rules were fetched successfully on 2026-10-10 from
<https://raw.githubusercontent.com/vercel-labs/web-interface-guidelines/main/command.md>.
Baseline: `9676d016d962e0d8756b3eddc207fc27058d3a9b`.

## Confirmed findings and fixes

P1 denotes a risk of losing user changes; P2 a substantial usability or
accessibility issue; P3 a smaller accessibility or localization issue.

| Priority | Location | Confirmed issue | Correction |
| --- | --- | --- | --- |
| P1 | `frontend/app.js`, `openTorrent` | Reopening loses an unsaved buffer, staged edits or preview | Ask before invoking native opening; cancellation retains edits. Also guard `openJobResult`. |
| P1 | `frontend/app.js`, `deleteProfile` | Saved profiles are deleted immediately | Name the profile in a confirmation, default to cancellation. |
| P2 | `frontend/app.js`, `newDraft` | Reset clears the draft without a visible warning | Confirm before reset, including unsaved editor changes. Flush typed draft fields before accepted reset. |
| P2 | `frontend/metadata-editor.js`, field selection | Async busy rendering loses keyboard focus | Give field buttons stable IDs, focus the loaded value unless the user moved elsewhere. |
| P2 | `frontend/metadata-editor.js`, add controls | Known-field selector unnamed; extension key labelled only by placeholder | Add translated accessible names. |
| P2 | `frontend/metadata-editor.js`, value input | Boolean `false` is skipped by the DOM builder, leaving spellchecking enabled | Use the enumerated HTML attribute `spellcheck="false"`. |
| P2 | `frontend/views.js`, fixed date | Date control has no accessible name | Add its own translated UTC label. |
| P2 | `frontend/dialogs.js`, batch rows | Per-task policy selects have no accessible name | Name each task's select in both paged and table views. |
| P2 | `frontend/dialogs.js`, profile preview | Long unbroken values overflow the modal | Constrain table columns and wrap long values. |
| P2 | `frontend/app.css`, error banner/toast | White on the dark theme's light red gives 2.77:1 contrast | Separate error background from inline error text; white on `#b91c1c` exceeds 4.5:1. |
| P2 | `frontend/virtual-list.js`, row insertion | Scrolling backward appends early rows after later rows in DOM | Insert after the preceding index; expose absolute position and total count to assistive technology. |
| P3 | `frontend/format.js` | Byte sizes ignore the selected UI locale | Use `Intl.NumberFormat` and retain exact `BigInt` formatting. |
| P3 | `frontend/views.js`, text input factory | Technical inputs lack name/autocomplete | Add meaningful field names and `autocomplete="off"`. |
| P3 | `frontend/index.html`, chrome | No shortcut to bypass commands | Add a translated first-focus skip link; focus main without changing the native host URL. |
| P3 | `frontend/app.css`, dialogs | Modal scroll can chain into its parent | Set `overscroll-behavior: contain` on dialog and its scrollable body. |

The existing C++ bridge operations, CSP, host navigation restrictions,
installer, updater, dependencies and stored data format are unchanged. No
unconfirmed unused code or user files were removed.

## Validation

Before changes: all 57 existing Chromium UI tests and 183 Linux CTest tests
passed. The initial UI launch required selecting the already installed system
Chromium with `TC_CHROMIUM=/usr/bin/chromium`; the launch failure was an
environment prerequisite, not an application test failure.

`tests/ui/specs/ui-guidelines.spec.js` adds targeted checks for cancellation and
acceptance, the three editor states, reset of a preview, async keyboard focus
and focus retention, accessible names, spellchecking, UTC input behavior,
both batch layouts, long values, WCAG AA error contrast in both themes,
virtual DOM order/positions, Russian formatting of exact large byte counts,
and the skip link's focus transfer. Existing native self-tests exercise the
same confirmation dialogs and check profile deletion/cancellation against
settings reloaded from disk; the Windows flow also checks draft reset cancellation.

Commands:

```sh
TC_CHROMIUM=/usr/bin/chromium npm --prefix tests/ui test -- --workers=2
cmake --build build/linux-release -j 2
ctest --test-dir build/linux-release --output-on-failure -j 2
```

After fixes: all 76 Chromium UI tests (57 existing and 19 new), the Linux
build and all 183 CTest tests passed. `git diff --check` passed.

The project has no separate frontend lint or static type-check command.
Its CI additionally builds and tests Windows x64, packaged Windows 11 ARM64
with x64 emulation, sanitizers, installers and the existing bounded fuzz smoke
campaigns. CI results are recorded in the remediation PR and task report.

## Remaining manual coverage

Chromium UI tests use a mocked native bridge. They do not establish behavior
of physical keyboard focus, NVDA/JAWS, actual display DPI changes or hybrid
Windows touch gestures. Previous native UI Automation checks confirmed the
three profile/reset/date findings, while foreground keyboard access was
blocked by Windows error 5. Keep these limits separate from passing DOM or
native self-tests. High contrast flag readback alone is not a visual review.
