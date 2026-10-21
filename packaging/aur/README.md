# AUR packaging

This directory contains the AUR package recipes for PipeDuck:

| Directory | Package | Source |
| --- | --- | --- |
| `pipeduck/` | `pipeduck` | Latest tagged release |
| `pipeduck-git/` | `pipeduck-git` | `main` branch |

Each directory is laid out as a standalone AUR repository: `PKGBUILD` plus the
generated `.SRCINFO` belong at the root of the AUR Git repository.

These recipes are optional, community-maintained packaging. The project CI
builds and tests the source tree, but it does not build, validate or publish
distribution packages.

## Build and install locally

```bash
cd packaging/aur/pipeduck
makepkg -si
```

Or, for the development snapshot:

```bash
cd packaging/aur/pipeduck-git
makepkg -si
```

`pipeduck` and `pipeduck-git` provide the same package, so install only one of
them. The `-git` package declares both `provides` and `conflicts` for
`pipeduck`.

## Publish to the AUR

1. Create an AUR account and upload an SSH public key at
   <https://aur.archlinux.org/account>.
2. Clone the (possibly empty) AUR repository:

   ```bash
   git clone ssh://aur@aur.archlinux.org/pipeduck.git
   ```

3. Copy `PKGBUILD`, `.SRCINFO` and any patch files (for `pipeduck`, that is
   `fix-installed-icon-path.patch`) from `packaging/aur/pipeduck/` into the
   clone, then commit and push:

   ```bash
   git add PKGBUILD .SRCINFO fix-installed-icon-path.patch
   git commit -m "Initial import: pipeduck 1.1.0"
   git push
   ```

4. Repeat with `pipeduck-git` if the development package should be published
   as well.

## Updating

For a new release:

1. Update `pkgver` in `packaging/aur/pipeduck/PKGBUILD` and reset `pkgrel` to
   `1`.
2. Download the new tag archive and update `sha256sums`:

   ```bash
   cd packaging/aur/pipeduck
   updpkgsums
   ```

3. Validate and regenerate `.SRCINFO`:

   ```bash
   makepkg -f
   makepkg --printsrcinfo > .SRCINFO
   ```

4. Copy the updated `PKGBUILD`, `.SRCINFO` and patch files to the AUR clone and
   push.

For `pipeduck-git`, only `.SRCINFO` needs to be regenerated when the recipe
changes; `pkgver()` derives the version from the latest commit at build time.

`fix-installed-icon-path.patch` makes `PIPEDUCK_ASSETS_DIR` configurable so the
release binary does not embed the makepkg build root. Drop the patch on the
next upstream release that already contains the change (1.1.1 or later), then
regenerate `sha256sums` and `.SRCINFO`.

## Checks

```bash
namcap PKGBUILD
namcap pipeduck-*.pkg.tar.zst
```

CI does not publish packages automatically. Publishing is a deliberate manual
step performed from a machine with AUR SSH access.
