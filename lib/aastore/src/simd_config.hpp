#pragma once

// Standalone source builds retain the existing runtime CPU dispatch by default.
#ifndef AASTORE_MAX_SIMD_BITS
#define AASTORE_MAX_SIMD_BITS 512
#endif

#if AASTORE_MAX_SIMD_BITS != 0 && AASTORE_MAX_SIMD_BITS != 256 && AASTORE_MAX_SIMD_BITS != 512
#error "AASTORE_MAX_SIMD_BITS must be 0, 256, or 512"
#endif
