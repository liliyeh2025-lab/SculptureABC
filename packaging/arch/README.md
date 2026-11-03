# Arch Linux (AUR)

`PKGBUILD` builds SculptForge from a release tag against Arch's own SDL3 and curl, fully offline
(Dear ImGui and GLM are pinned sources with checksums).

## Publishing to the AUR (maintainer, once per release)

1. Create an AUR account at https://aur.archlinux.org and add your SSH public key to it.
2. First time only: `git clone ssh://aur@aur.archlinux.org/sculptforge.git aur-sculptforge`
3. Copy this `PKGBUILD` in, set `pkgver` to the release and update the first checksum:
   `updpkgsums` (from `pacman-contrib`), then `makepkg -si` to test the build.
4. `makepkg --printsrcinfo > .SRCINFO`
5. `git add PKGBUILD .SRCINFO && git commit -m "Update to <version>" && git push`

Users then install with `yay -S sculptforge` (or any AUR helper).
