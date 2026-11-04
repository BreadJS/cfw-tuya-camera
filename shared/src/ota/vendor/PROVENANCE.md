# Cryptographic dependency

Unmodified Monocypher 4.0.2 sources from upstream commit
`0d85f98c9d9b0227e42cf795cb527dff372b40a4`:

- https://github.com/LoupVaillant/Monocypher/tree/0d85f98c9d9b0227e42cf795cb527dff372b40a4
- https://github.com/LoupVaillant/Monocypher/releases/tag/4.0.2

The optional Ed25519 implementation supplies standard Ed25519/SHA-512,
interoperable with Python cryptography/OpenSSL. The default Monocypher
EdDSA/BLAKE2b signature construction is not used. The main implementation
supplies the separate BLAKE2b-512 payload digest.

Upstream's complete dual BSD-2-Clause/CC0 terms are in `LICENCE.md` and source
headers. This project keeps those notices. SHA-256 of the downloaded bytes:

| File | SHA-256 |
|---|---|
| `LICENCE.md` | `a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0` |
| `monocypher.c` | `02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d` |
| `monocypher.h` | `fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3` |
| `monocypher-ed25519.c` | `97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b` |
| `monocypher-ed25519.h` | `3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce` |
