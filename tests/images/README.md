# Test images

Images here are generated, not committed (see `.gitignore`).

Two sources:

1. **Linux golden images** (`make_golden_images.sh` on a Linux machine):
   `empty-default.img`, `empty-minimal.img`, `empty-bigtime.img`,
   `empty-nrext64.img`, `empty-1g.img`, `populated.img`.
   The populated image contains the DoD test set: small files, a large file,
   a sparse file, deep directories, a 5000-entry directory, Unicode names,
   symlinks, hardlinks, and a file with many extents (extent B+tree).
2. **Windows-created images** (`xfswin mkimage` + `xfswin format`) used by
   `verify_windows_image_linux.sh` on Linux to prove the formatter creates a
   standard filesystem.

Recommended workflow (CI):

```
Windows job : mkimage + format + xfswin check + xfswin tests
artifact    : windows-created.img
Linux job   : verify_windows_image_linux.sh windows-created.img
              make_golden_images.sh + upload populated.img
Windows job : mount/read the Linux images with xfswin and compare against
              xfs_db output captured on Linux
```

Never treat "Explorer lists a directory" as proof of correctness. Compare
inode numbers, sizes, modes, timestamps and extent lists with `xfs_db`.
