# Bundled cross compilers

These are the original pinned archives, checked into this repository as ordinary
Git files. No Git LFS download or external release is needed. Keep them in source
exports so a fresh checkout can install both compilers offline:

```sh
python3 tools/toolchain.py --board lsc-smart-indoor
python3 tools/toolchain.py --board lsc-smart-rotatable
```

The installer reads only the repository archive named by each `soc/*.json` pin,
checks its byte count and SHA-256 before extraction, and records the compiler in
ignored `local/toolchains.json`. Missing or corrupt archives are errors; there is
no network fallback. `--cache` changes the extraction directory only.

| Archive | Target | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `toolchain.hisilicon-hi3516cv100.tgz` | Fullhan / musl | 102606142 | `466683e215a493144ba87b3d57a733864b6db318aad847bf0fd0616dda39729e` |
| `x-tools.tar.bz2` | Anyka / uClibc | 46300143 | `a49310e4fd15c9df25a4feedf17d770e187dac4e45e3edc9c8f9c8cb8e4b0970` |

For provenance, the Fullhan-compatible SDK came from the OpenIPC firmware
`toolchain` release (`toolchain.hisilicon-hi3516cv100.tgz`). The Anyka SDK came
from `medevil84/ipcd`, commit `7b77c193c631b1e77c3d00459441e8d14b0d88d3`,
file `src/x-tools.tar.bz2`. These are historical origins, not installation inputs.

The archives retain their upstream component notices under `share/doc`,
`share/licenses`, and related SDK directories. The project's MIT license does
not replace those component licenses. See [input and licensing documentation](../docs/INPUTS.md).
