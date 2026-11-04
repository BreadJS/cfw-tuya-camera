# Anyka ABI references

`src/anyka/sdk_abi.h` and `sdk_api.[ch]` are C declarations/bindings written for
this project. They contain no copied vendor implementation. Layout comparison
uses the MIT project [medevil84/ipcd](https://github.com/medevil84/ipcd), notably
`src/ipcd/src/cap_pipeline.c`; its copyright and permission notice are retained
in `ipcd-LICENSE.txt` in this directory.

The owner's exact executable confirms the four pure version getter strings and
the encoder's 60-byte configuration copy. Its decompiled encoder entry takes a
configuration pointer plus an output handle pointer. The VI56/channel20 and
stream24 layouts are comparison contracts requiring target review/runtime proof;
library version matches alone do not prove every field's meaning.

The binder resolves symbols and invokes only constant version getters. It never
initializes memory or opens devices. `version_match` is a static compatibility
check; `runtime_proven` remains zero. Audio SDK calls are intentionally absent
until their exact target layouts have been independently checked.

The shipped `ak_mem_init` has TWO arguments. Target decompilation stores the
second argument into allocator state; the original SDK init call loads both
arguments. Read-only inspection of the running original allocator state confirms
the stock pair(0,0). The one-argument declaration in the comparison project is
not used by this binding.

`anyka_video_stream.data_address` is an ARM32 address rather than a host pointer.
The source timestamp's units must be measured against a monotonic clock before
RTSP use. Owned stream bytes must be copied before release. Media operations
require a separate, exclusive-owner trial and validated watchdog handling.
