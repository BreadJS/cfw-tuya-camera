# MIS2006 mode1 white-balance target

The matching day profile SHA256
`467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba`
has0x6c=1 (mode1),0x6e=1,0x74=32 and0x75..77=`ff ff ff`.
The primary executable SHA256 is
`bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714`.
Complete original pseudocode/evidence is retained in
`../../docs/evidence/mode1-tone.rea.json.gz`; the matching executable is the
reproducible oracle.

`0x10c424` normalizes each nine-cell R/G/B SUM by512/reference and copies
COUNT. There is no square-root transform. References512 make direct SUMs
correct; caller must retain that invariant and the recovered hardware
chromaticity filter. Generic full-image consensus cannot substitute for the
profile's filtered statistics/estimator.

`0x10b160` accepts count>=11 with nonzero denominators, aggregates valid SUMs,
and computes per-cell R/B inQ12. `0x10aa8c` exchange-sorts all nine values
(invalid=0) with strict comparisons, preserving the exact resulting original
indices. The chosen original index is the upper-middle valid cell. The
R/B spread and profile thresholds select a blend weight0..8 between that
cell's inverse RGB gains and aggregate inverse RGB gains. Weight5 is absent.
The low-R/B conditions only select weights8/7 when spread also exceeds the
corresponding threshold. With profile thresholds all255 (32640Q12), typical
small-spread scenes use aggregate gains, not the median cell.

`isp_awb_mode1.c` reconstructs this pure steady-state target. `pre_luma`
contains the exact blended inverse gains. Returned logical gains normalize
G to512 and clampR/B256..2048 for the existing shared color writer; this
normalization has integer rounding and does not claim stock writer bit parity.
Stock luma/BLC normalization and CCM selection remain outside the estimator.
The common color writer's norm*gain/1024 matches the original0x10b160 luma
stage; it is not an extra factor-of-two brightness reduction.

Explicit native policy retained: at least3 valid cells, configurable dark and
clipping guards, fresh-generation rejection, and bounded slow smoothing.
No original startup retry counter, unsafe assertion, filter MMIO, or global
controller state is copied. Dark/unreliable input holds current gains.
No hardware validation or image-quality outcome is claimed by these tests.

Validation (host synthetic RAM only):

- `uv run --with unicorn python tests/color/test_stock_awb_mode1.py` executes
  the exact shipped ARM mode1 and ranking functions against2028 synthetic
  cell/config vectors: exact pre-luma RGB target and original selected index
  PASS; every stock blend weight0,1,2,3,4,6,7,8 covered. Test hooks only replace
  division and skip external filter publication; execution stops before the
  common hardware writer. Matching profile plus two synthetic threshold sets.
- Strict hostASAN/UBSAN test `test_awb_mode1.c`:10000 mixed/adversarial samples,
  dark/clipped hold, stale generations, neutral target, bounds and smoothing
  PASS.
- Strict static ARMv6 soft-float compilation PASS (same module+test).
