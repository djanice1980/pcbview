# winget packaging

Manifests for submission to [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs).
Kept here so each release's manifest is versioned alongside the code that
produced it.

`manifests/d/DavidJanice/pcbview/<version>/` mirrors the layout winget-pkgs
expects, so the folder can be copied straight into a fork.

## What is verified before submitting

winget supplies the Inno silent switches itself — `/VERYSILENT
/SUPPRESSMSGBOXES /NORESTART /SP-` — so the manifest declares no custom ones.
Each release is checked against that exact set rather than assumed:

- install with those switches returns exit code 0
- the version resource matches the manifest's `PackageVersion`
- the registry key created matches the manifest's `ProductCode`
  (`{7E1F7A2C-9B7D-4A63-B7B1-52D1C0B4D6E1}_is1`)
- `InstallerSha256` matches the file **as served by GitHub**, not the local
  build — they can differ if the asset was rebuilt after publishing
- silent uninstall removes files, shortcut and registry entry
- upgrading over a *running* instance returns 0 rather than failing on a
  locked file

The manifest gives `ProductCode` as the only detection hint. An `UpgradeCode`
is an MSI concept Inno never writes, and the Add/Remove `DisplayName` differs
between scopes (`pcbview` per-machine, `pcbview (Current user)` per-user), so
asserting either would be publishing something unverified.

## Validating and submitting

```powershell
# schema check (same validation the winget-pkgs pipeline runs)
winget validate --manifest packaging\winget\manifests\d\DavidJanice\pcbview\1.17.1

# optional end-to-end test; needs an ELEVATED shell for the first command
winget settings --enable LocalManifestFiles
winget install --manifest packaging\winget\manifests\d\DavidJanice\pcbview\1.17.1

# submit (opens a PR on microsoft/winget-pkgs under your GitHub account)
winget install wingetcreate
wingetcreate submit --token <github-pat> packaging\winget\manifests\d\DavidJanice\pcbview\1.17.1
```

`wingetcreate update DavidJanice.pcbview --version <new> --urls <installer-url>
--submit` regenerates and submits for a later release, carrying the metadata
forward.
