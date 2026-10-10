# Vendored web-design-guidelines

This directory contains a repository-local copy of Vercel's skill as ordinary
files. It needs no global or home-directory installation.

## Source

- Repository: https://github.com/vercel-labs/agent-skills
- Original directory: `skills/web-design-guidelines/`
- Source commit: `063bee94c3f4df8453406c830b0a7df0f2860278`
- Pinned source: https://github.com/vercel-labs/agent-skills/tree/063bee94c3f4df8453406c830b0a7df0f2860278/skills/web-design-guidelines
- Copied on: 2026-10-10

The complete upstream directory at this commit contains only `SKILL.md`.
That file is preserved byte for byte, including its YAML metadata and original
instructions. This README is local provenance documentation, not an upstream
file. The source Git blob ID of `SKILL.md` is
`ceae92ab319216a68274168fba9b63b998b65997`.

## Current guidelines and network access

The skill intentionally fetches fresh rules before each review from:

https://raw.githubusercontent.com/vercel-labs/web-interface-guidelines/main/command.md

This URL returned HTTP 200 with a nonempty Markdown response during integration
on 2026-10-10. This was an availability check, not an audit of the application's
UI. The external rules are not frozen by the skill's source commit and must be
fetched again for future reviews.

Reviews require HTTPS access to `raw.githubusercontent.com`. Updating this copy
also requires `github.com` for Git access, or `api.github.com` when using the
GitHub API. If access is restricted, report the blocked host and do not describe
the latest guidelines as verified.

## Reproducible updates

1. Obtain `vercel-labs/agent-skills` in a temporary checkout outside this
   repository. Resolve the desired revision to its full commit SHA and check
   out that SHA, rather than copying from a moving `main` reference.
2. Inspect the entire `skills/web-design-guidelines/` tree at that commit,
   including hidden files, metadata, and referenced helper files. Review the
   changes before replacing the local copy. Reject symlinks; keep all required
   upstream files as ordinary files with their original contents.
3. Copy the complete skill directory here. Remove an old upstream file only
   when the new upstream tree confirms its removal. Preserve this provenance
   README; if upstream adds a file with this name, move local provenance to a
   separate filename instead of overwriting either file.
4. Update the commit, pinned source URL, date, upstream file inventory, and
   recorded blob IDs in this README. Check `SKILL.md` YAML metadata and compare
   every copied file with the pinned source. For the original revision,
   `git hash-object .agents/skills/web-design-guidelines/SKILL.md` must return
   the blob ID recorded above.
5. Fetch the live guidelines URL again, record whether the request succeeded,
   and review the Git diff. Commit the skill update and provenance together in
   a PR without mixing in application changes.
