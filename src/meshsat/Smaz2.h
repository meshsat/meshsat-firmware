#pragma once

#include <cstddef>
#include <cstdint>

namespace meshsat
{
// The Hub's SMAZ2 variant (meshsat-hub internal/compress/smaz2.go): bigram codes 0x80-0xFF,
// verbatim runs 1-5, word escapes 6 (word), 7 (word + space), 8 (space + word), every other
// byte a literal. Plain text passes through unchanged, so it is safe on every MT payload.
// Returns the decompressed length, or 0 when the input is malformed or does not fit.
size_t smaz2Decompress(const uint8_t *in, size_t length, uint8_t *out, size_t capacity);
} // namespace meshsat
