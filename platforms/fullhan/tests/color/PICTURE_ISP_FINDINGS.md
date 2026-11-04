# Live ISP picture controls

`picture_isp.[ch]` supplies a pure validated ten-write plan plus publication
through the existing `fh_linux_io` mapping. It performs no opens, allocation,
VMM operations, producer restart or gamma-table update. The caller holds the
same illumination lock as other picture stages. A publication failure is
sticky and prevents later writes; readback checks only owned field bits.

## Supported fields

| Control | Range/default | Recovered register contract |
| --- | --- | --- |
| Saturation | raw0..1020/default320 |5d0 low10bits; stock manual byte<<2 |
| Contrast |0..255/default60 |5c8 low10bits; stock byte input |
| Luma offset |−128..127/default0 |5c8 bits16..23, signed byte |
| Hue |−180..180degrees/default0 |5cc signed9-bit sine/cosine fields |
| Cb chroma gain |0..200/default54 |4dc high12 and both4e0 coefficient fields |
| Cr chroma gain |0..200/default50 |4e4 coefficient fields and4e8 low12 |
| Temporal NR |off/on/defaultoff |24 bit23 is bypass when set |
| Detail strength |0..200percent/default100 |4f8 high16 and4fc two16-bit coefficients |

The first seven masked writes reproduce the original112a44 fixed-manual
colour stage. Masks preserve the Y/luma low12bits of4dc used by AWB and
preserve other stage bits. Cb/Cr coefficients use the original arithmetic
right-shift rounding for negative values. They do not modify the RGB CCM.

Hue uses the exact360-entry table at original ELFVA3d4ba0, not a floating-point
sine approximation. Its asymmetric peaks are+127 and−128. The original packed
manual API exposes only−64..63degrees; the native UI can use the full bounded
table range. Full-range hue is a native range extension, not original input-ABI
parity. Stock-range hues have exact ARM parity.

The detail fields are observed in112d08 as three profile coefficients scaled
by a strength/64. Native `detail_percent` scales only the known startup
coefficients20/16/16, with nearest-integer percent rounding. Default100 retains
the original working startup words00140000/00100010. This deliberately leaves
the existing detail tables and gating unchanged. It does not replay the full
detail-profile trial that lost RTSP, and does not claim a complete original
adaptive sharpening controller or a physical lens-focus control.

Temporal NR toggles only the existing bypass bit. The caller must retain the
queried mode1 backing/resources already used by the production pipeline.
Gamma belongs to the separate652-byte driver packet owner; this plan never
touches register28 or LUT SRAM.

## Integration

`fh_picture_isp_defaults`, `fh_picture_isp_valid`,
`fh_picture_isp_make_plan`, `fh_picture_isp_publish` are the exposed API.
Night mode must publish a copy with saturation0. The controller must reapply
the current Day saturation after physical Night→Day transitions, even if the
saved picture version did not change: the illumination controller otherwise
restores the startup Day saturation. Public APIs must expose typed settings,
not arbitrary offsets or masks.

No single-frame atomic commit is claimed. Planning/validation completes before
publication, and every register operation uses the retained owner's checked
mapping. When a read/write/readback fails after prior writes, callers report
the error and retain all media resources rather than attempting warm teardown.

## Evidence and tests

Matching original dgiot SHA256:
bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714.

-112a44:ev_f98dfecda7c100f5701bfa9ec88217460ddaa8318d526bdc7064276c1e215e34.
-112d08:ev_996f922d53cf66ff104c8b06d0ff32a89c7246382763634d95a535f17e87bf48.
-Complete evidence is retained in docs/evidence/mode1-tone.rea.json.gz.
-Known startup coefficient words are in isp_fixed_plan.inc at ARM105d34/105d38.

`test_stock_picture_isp.py` executes the matching original ARM routines in
Unicorn synthetic RAM:628 complete112a44 MMIO comparisons, including every
stock hue and500random parameter cases;9exact-quarter detail coefficient
scalings also match112d08. No camera or vendor process runs during the test.

`test_picture_isp.py` passes ASan/UBSan over all361hues×201detail multipliers,
default/range/output-preservation checks, unrelated-bit preservation, initial
lease denial and every10write/20read/10readback failure position. Strict
static ARMv6 soft-float compilation passes. These establish the field contracts
and planning/publication behavior, not physical image-quality acceptance.
