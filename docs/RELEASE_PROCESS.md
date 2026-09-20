# Release process

`dev` is the only firmware release source. Feature and repair branches merge
into `dev`; `main` and unmerged branches must never be tagged for OTA.

## Stable release

1. Merge the intended change into `dev` and wait for all CI jobs to pass.
2. Set `[crosspoint] version` in `platformio.ini` to the exact `vMAJOR.MINOR.PATCH`
   tag and add `docs/releases/<tag>.md`.
3. Test the production build. Hardware-specific changes require the matching
   device; missing hardware must be stated in the release notes and must not be
   presented as hardware-validated.
4. Tag the tested commit already present on `origin/dev` and push the tag.
5. Wait for `Compile Release`, then download the published assets and verify
   `SHA256SUMS`, image size, ESP image checksum and embedded app version.
6. Confirm that `releases/latest` resolves to the new stable release before
   announcing it or offering it over OTA.

The release workflow fails before building when the tagged commit is absent
from `origin/dev`, the tag differs from `platformio.ini`, or release notes are
missing. Releases run one at a time.

## Incident response

If a release causes field regressions, mark it as a prerelease and explicitly
set the last known-good release as latest. Keep the withdrawn tag and assets for
diagnosis. Devices already on the withdrawn version need a newer recovery
version because the OTA updater does not offer semantic downgrades.
