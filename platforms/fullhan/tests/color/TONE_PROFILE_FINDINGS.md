# Exact gain-dependent static tone plan

`isp_tone_profile.[ch]` reconstructs the static part of original111a88 using
matching MIS2006 day profile260..2a7. Adaptive1111a4/1112ac are deliberately
separate. Profile/executable SHA256 are asserted by the extraction and oracle
scripts, matching the0.1.7 color evidence.

API: `fh_tone_profile_make_plan(gain_q6,&plan)`, supporting gain64..1024.
Each of44 unique entries merges `(old & keep_mask) | value`:

- ISP24 bit18 follows original profile260 bit1 (cleared in matching day).
  The owner must account for its current bypass policy rather than silently
  assuming this control is unchanged.
- ISP254 combines original low/high and compression controls; ISP258 updates
  onlybits20..27. ISP23c updates onlylow8bits, preserving adaptive strength.
-40 words contain the two original preset curves and their tail controls.
- Adaptive240/244/248/24c/250 are never included.

The five gain lookups follow original107520/1074d0: power-of-two Q6 buckets,
byte or packed-nibble values, signed Q16 slope and original signed rounding.
Every supported gain reaches only curve16..20 and secondary0. Accordingly,
only those five41word curves, one37word secondary curve and72 profile bytes
are retained asuint16/uint8 data (556bytes total). No stock loader, table
pointer or arbitrary profile overread exists. The reproducible extractor is
`tests/color/extract_tone_tables.py` and rejects mismatching source hashes.

Validation:

- `uv run --with unicorn python tests/color/test_stock_tone_profile.py`:
  original111a88 executed against synthetic RAM with only the two adaptive
  calls substituted. All961 integer gains64..1024 match the complete MMIO
  image exactly after applying the44 native writes, including preservation
  of randomized unrelated bits. No camera access occurs.
- `test_tone_profile.c`: same full gain range underASAN/UBSAN passes, checking
  unique bounded offsets, masks, untouched adaptive fields, and invalid-input
  nonmutation.
- Strict static ARMv6 soft-float build passes. No target execution, physical
  image-quality parity or frame-atomic publication claim follows from these
  pure tests.

Integration: recompute when actualQ6 gain changes. Serialize masked writes
with adaptive-shadow/color publication under the owner's existing lock.
Sticky publication failure and proposed-state commit remain the owner's
responsibility. This component contains no driver/MMIO calls or persistent
configuration edits.
