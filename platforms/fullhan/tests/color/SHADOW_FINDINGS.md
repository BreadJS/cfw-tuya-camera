# Pure adaptive shadow reconstruction

Evidence: `docs/evidence/mode1-tone.rea.json.gz`, original functions1111a4,
1112ac and111a88. Optional oracle `test_stock_shadow.py` executes the matching
shipped ARM executable in synthetic RAM, never a camera or original process.
It asserts executable/profile SHA256 before execution.

## Inputs and ownership

-1111a4 consumes Bayer statistics rooted at107ae0 (completed bank+0x5c8).
  Four planes are0x1000 apart, records16bytes. It uses each planeSUM and the
  first planeCOUNT. Luminance=`((sumFourPlanes/count)>>2)<<4`, count0 becomes1.
  Tile count=`(rows+1)*(cols+1)-1` fromISP1cc's two5bit grid fields, so255
  tiles for15/15. It excludes the last tile. Minimum starts4095 despite this
  luminance scaling; retain that original behavior. Extrema use original
  piecewise-log2Q8, not square-root statistics.
-1112ac consumes three words from107c48 (completed bank+0x21250), plus those
  extrema. Keep their generation coherent with the existing completed-bank
  copy and its verification. Histogram semantics are left as raw words;
  stock math itself normalizesH0 by(area>>6),H1 by(area>>2),H2 directly.
- Area is the product ofISP30 low/high11bit dimensions. Native planner inputs
  must match those registers, not assume arbitrary sensor/full-frame values.
- Exact day profile config:66=106,265=10,268=64,269=64,26a=255. AE target uses
  `max(1,106*106>>4)<<4 =11232`, consistent with new AE target11235.
- Input original register words23c/240/244/248/24c/250 are required to preserve
  unrelated bits. In particular24c low10bits are the fixed knee ordinate.

## Controller and output contract

`isp_shadow.[ch]` is a pure planner: no MMIO, allocation, sensor access,
profile integration or enable-bit changes. State starts zero, as stock BSS.
It retains previous min/max, smoothed min/max/hist endpoints and integrated
mean mass. Min/max first average current and previous measurements, then use
`(previousSmooth +4*new)/5`. Histogram endpoints use the same1/5 and4/5
smoothing and are clamped to original0x1100/0xa100 lower limits. Mean mass
also preserves the original integer truncation between frames.

The target log, scales268..26a and smoothed extrema determine low/middle/high
abscissae and two slopes. The23c shadow-strength field derives from:

`clamp(round(1024*(2.4e-7*x^3 +5.9e-6*x^2 -0.00073*x +0.024 +(265&15)/10)),0,1023)`

where `x=(H1/(area>>2)+8)>>4`. Original double constants and operation order
are retained. With default265=10, this field saturates frequently; endpoint
and slope adaptation is a distinct behavior from changing that field.

A plan updates six register words, preserving original masks. It does not
supply the static tone curves or111a88 profile/gain interpolation. Caller
must establish correct hardware enable/bypass state separately; a correct
plan cannot improve a bypassed block. Publish through the owner's coherent
path and commit the proposed state only after all writes succeed.

Native guard differences: dimensions<=2047 witharea>=64, logs<=8192,
H0/area-normalized<=65535 andH2<=65535, fresh generation, and initialized
state bounds. Invalid/stale input leaves state/output unchanged. These are
explicit caller contract checks rather than stock overflow/assert behavior.
They must be checked against retained actual statistics before integration.

## Validation

- `uv run --with unicorn python tests/color/test_stock_shadow.py`:750
  consecutive plans over3 profile parameter sets and3 frame sizes match all
  six output words and all seven persistent state values, exactly. Original
  reducer1111a4 also matches50x255 synthetic tile vectors. Division and soft
  double ABI calls are replaced with equivalent host operations in synthetic
  execution; completed-bank fetches are substituted with synthetic RAM.
- `test_shadow.c`:10000 bounded plans under ASAN/UBSAN pass, including masks,
  freshness, geometry, invalid input/state preservation and extrema bounds.
- Strict static ARMv6 soft-float compilation passes; no ARM execution on
  hardware, target installation or visual quality outcome is claimed.
