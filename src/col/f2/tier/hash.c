/*
 * col/f2/tier/hash.c - SHA-256, HMAC-SHA-256 and a CRC32C that continues, in plain C: the signer of the object store and the digest of an object link no crypto library.
 */
#include "core/host.h"

#include "col/f2/tier/tier.h"

static const uint32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))

static void
block(SiftSha256 *c, const uint8 *p)
{
	uint32		w[64],
				v[8];

	for (int i = 0; i < 16; i++)
		w[i] = (uint32) p[4 * i] << 24 | (uint32) p[4 * i + 1] << 16 | (uint32) p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++)
	{
		uint32		s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3),
					s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(v, c->h, sizeof(v));
	for (int i = 0; i < 64; i++)
	{
		uint32		t1 = v[7] + (ROR(v[4], 6) ^ ROR(v[4], 11) ^ ROR(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i],
					t2 = (ROR(v[0], 2) ^ ROR(v[0], 13) ^ ROR(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));

		memmove(v + 1, v, 7 * sizeof(uint32));
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++)
		c->h[i] += v[i];
}

void
sift_sha256_init(SiftSha256 *c)
{
	static const uint32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

	memcpy(c->h, iv, sizeof(iv));
	c->len = 0;
	c->nbuf = 0;
}

void
sift_sha256_update(SiftSha256 *c, const void *data, size_t len)
{
	const uint8 *p = data;

	c->len += len;
	if (c->nbuf > 0)
	{
		size_t		n = Min(len, 64 - c->nbuf);

		memcpy(c->buf + c->nbuf, p, n);
		c->nbuf += n;
		p += n;
		len -= n;
		if (c->nbuf < 64)
			return;
		block(c, c->buf);
		c->nbuf = 0;
	}
	for (; len >= 64; p += 64, len -= 64)
		block(c, p);
	memcpy(c->buf, p, len);
	c->nbuf = len;
}

void
sift_sha256_final(SiftSha256 *c, uint8 out[SIFT_SHA256_LEN])
{
	uint64		bits = c->len * 8;
	uint8		pad[72] = {0x80};
	size_t		padlen = (c->nbuf < 56 ? 56 : 120) - c->nbuf;

	for (int i = 0; i < 8; i++)
		pad[padlen + i] = bits >> (56 - 8 * i);
	sift_sha256_update(c, pad, padlen + 8);
	for (int i = 0; i < 8; i++)
	{
		out[4 * i] = c->h[i] >> 24;
		out[4 * i + 1] = c->h[i] >> 16;
		out[4 * i + 2] = c->h[i] >> 8;
		out[4 * i + 3] = c->h[i];
	}
}

void
sift_hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, uint8 out[SIFT_SHA256_LEN])
{
	uint8		k[64] = {0},
				ipad[64],
				opad[64],
				inner[SIFT_SHA256_LEN];
	SiftSha256	c;

	if (klen > 64)
	{
		sift_sha256_init(&c);
		sift_sha256_update(&c, key, klen);
		sift_sha256_final(&c, k);
	}
	else
		memcpy(k, key, klen);
	for (int i = 0; i < 64; i++)
	{
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sift_sha256_init(&c);
	sift_sha256_update(&c, ipad, 64);
	sift_sha256_update(&c, msg, mlen);
	sift_sha256_final(&c, inner);
	sift_sha256_init(&c);
	sift_sha256_update(&c, opad, 64);
	sift_sha256_update(&c, inner, SIFT_SHA256_LEN);
	sift_sha256_final(&c, out);
}

void
sift_hex(const uint8 *in, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++)
	{
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static const uint32 crc_nibble[16] = {
	0x00000000, 0x105ec76f, 0x20bd8ede, 0x30e349b1,
	0x417b1dbc, 0x5125dad3, 0x61c69362, 0x7198540d,
	0x82f63b78, 0x92a8fc17, 0xa24bb5a6, 0xb21572c9,
	0xc38d26c4, 0xd3d3e1ab, 0xe330a81a, 0xf36e6f75
};

uint32
sift_tier_crc32c_extend(uint32 crc, const void *data, size_t len)
{
	const uint8 *p = data;

	crc = ~crc;
	for (size_t i = 0; i < len; i++)
	{
		crc ^= p[i];
		crc = crc_nibble[crc & 15] ^ (crc >> 4);
		crc = crc_nibble[crc & 15] ^ (crc >> 4);
	}
	return ~crc;
}
