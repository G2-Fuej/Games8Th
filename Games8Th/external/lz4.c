/* Minimal LZ4 block-codec, source-compatible with the official lz4.h API
 *
 * Decompressor: full LZ4 block format (matches + literals + 255-extensions),
 * so files written by the official lib (existing configs) decode correctly.
 * Compressor: emits valid LZ4 blocks as all-literal runs (no match search).
 * Round-trips identically through any correct LZ4 decoder; compression ratio
 * is ~1:1 (literals-only) which is fine for small config blobs.
 *
 * Only the symbols consumed by this project are provided:
 *   LZ4_compressBound / LZ4_compress_default / LZ4_decompress_safe
 *
 * Pure C (the project compiles .c files as C).
 */
#include "lz4.h"

#include <stdint.h>
#include <string.h>

#define LZ4_ERROR(x) (-(x))

int LZ4_compressBound(int inputSize)
{
	if (inputSize < 0)
		return 0;
	/* Worst case: literals-only, 1/255 escaping overhead + token/end margin. */
	return inputSize + inputSize / 255 + 16;
}

int LZ4_compress_default(const char* src, char* dst, int srcSize, int dstCapacity)
{
	const uint8_t* ip;
	uint8_t* op;
	uint8_t* const oend = (uint8_t*)dst + dstCapacity;
	uint32_t lit;
	uint32_t t;

	if (!src || !dst || srcSize <= 0 || dstCapacity <= 0)
		return 0;
	if (dstCapacity < LZ4_compressBound(srcSize))
		return 0;

	ip = (const uint8_t*)src;
	op = (uint8_t*)dst;

	/* All-literal block: token(litlen nibble) + 255-extensions + literals. */
	lit = (uint32_t)srcSize;
	if ((uint32_t)(op - (uint8_t*)dst) + 1 + (lit / 255) > (uint32_t)dstCapacity)
		return 0;

	t = lit < 15 ? lit : 15;
	*op++ = (uint8_t)((t << 4) | 0x00);
	lit -= t;
	while (lit >= 255) {
		*op++ = 0xFF;
		lit -= 255;
	}
	if (lit)
		*op++ = (uint8_t)lit;

	if ((size_t)(op - (uint8_t*)dst) + (size_t)srcSize > (size_t)dstCapacity)
		return 0;
	memcpy(op, ip, (size_t)srcSize);
	(void)oend;
	op += srcSize;

	return (int)(op - (uint8_t*)dst);
}

int LZ4_decompress_safe(const char* src, char* dst, int compressedSize, int dstCapacity)
{
	const uint8_t* ip;
	const uint8_t* const iend = (const uint8_t*)src + compressedSize;
	uint8_t* op;
	uint8_t* const ostart = (uint8_t*)dst;
	uint8_t* const oend = ostart + dstCapacity;

	if (!src || !dst || compressedSize <= 0 || dstCapacity <= 0)
		return LZ4_ERROR(1);

	ip = (const uint8_t*)src;
	op = ostart;

	while (ip < iend) {
		/* --- literals --- */
		const uint8_t token = *ip++;
		uint32_t litLen = token >> 4;
		uint32_t b;

		if (litLen == 15) {
			if (ip >= iend)
				return LZ4_ERROR(2);
			do {
				if (ip >= iend)
					return LZ4_ERROR(3);
				b = *ip++;
				litLen += b;
			} while (b == 255);
		}
		if ((size_t)(iend - ip) < litLen || (size_t)(oend - op) < litLen)
			return LZ4_ERROR(4);
		memcpy(op, ip, litLen);
		op += litLen;
		ip += litLen;

		if (ip >= iend)
			break; /* End of block after final literals. */

		/* --- match --- */
		{
			const uint32_t offset = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
			uint32_t matchLen;
			const uint8_t* match;

			if ((size_t)(iend - ip) < 2)
				return LZ4_ERROR(5);
			ip += 2;
			if (offset == 0)
				return LZ4_ERROR(6);

			matchLen = token & 0x0F;
			if (matchLen == 15) {
				if (ip >= iend)
					return LZ4_ERROR(7);
				do {
					if (ip >= iend)
						return LZ4_ERROR(8);
					b = *ip++;
					matchLen += b;
				} while (b == 255);
			}
			matchLen += 4;

			if ((size_t)(oend - op) < matchLen)
				return LZ4_ERROR(9);
			match = op - offset;
			if (match < ostart)
				return LZ4_ERROR(10);
			if (offset >= matchLen) {
				memcpy(op, match, matchLen);
			} else {
				uint32_t i;
				for (i = 0; i < matchLen; ++i)
					op[i] = match[i];
			}
			op += matchLen;
		}
	}

	return (int)(op - ostart);
}