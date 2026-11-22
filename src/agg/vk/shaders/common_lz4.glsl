/*
 * common_lz4.glsl - an LZ4 block decompressor run by one workgroup of LZ_WG invocations (the sequences are read one after the other by
 * invocation 0, the literals and the match of each are copied by all of them). Called with uniform control flow by the whole workgroup;
 * src, dst are byte offsets of the Work buffer (w8), the block is slen bytes and must give exactly dlen.
 */
#define LZ_WG 64u

shared uint lz_ip, lz_op, lz_lit, lz_litsrc, lz_off, lz_mlen, lz_state;

bool lz4_block(uint src, uint slen, uint dst, uint dlen)
{
	uint tid = gl_LocalInvocationID.x;

	if (tid == 0u)
	{
		lz_ip = 0u;
		lz_op = 0u;
		lz_state = 0u;
	}
	barrier();
	for (;;)
	{
		if (tid == 0u)
		{
			uint ip = lz_ip, op = lz_op, st = 0u, lit = 0u, mlen = 0u, moff = 0u, litsrc = 0u;

			if (ip >= slen)
				st = (op == dlen) ? 3u : 2u;
			else
			{
				uint tok = uint(w8[src + ip]);

				ip++;
				lit = tok >> 4;
				mlen = tok & 15u;
				if (lit == 15u)
					for (;;)
					{
						if (ip >= slen) { st = 2u; break; }
						uint b = uint(w8[src + ip]);
						ip++;
						lit += b;
						if (b != 255u)
							break;
					}
				litsrc = ip;
				ip += lit;
				if (st == 0u && (ip > slen || op + lit > dlen))
					st = 2u;
				if (st == 0u)
				{
					if (ip == slen)
					{
						mlen = 0u;
						st = 1u;
					}
					else if (ip + 2u > slen)
						st = 2u;
					else
					{
						moff = uint(w8[src + ip]) | (uint(w8[src + ip + 1u]) << 8);
						ip += 2u;
						if (mlen == 15u)
							for (;;)
							{
								if (ip >= slen) { st = 2u; break; }
								uint b = uint(w8[src + ip]);
								ip++;
								mlen += b;
								if (b != 255u)
									break;
							}
						mlen += 4u;
						if (st == 0u && (moff == 0u || moff > op + lit || op + lit + mlen > dlen))
							st = 2u;
					}
				}
			}
			lz_ip = ip;
			lz_lit = lit;
			lz_litsrc = litsrc;
			lz_off = moff;
			lz_mlen = mlen;
			lz_state = st;
		}
		barrier();
		uint st = lz_state;

		if (st >= 2u)
		{
			barrier();
			return st == 3u;
		}
		uint op = lz_op, lit = lz_lit, litsrc = lz_litsrc, moff = lz_off, mlen = lz_mlen;

		for (uint i = tid; i < lit; i += LZ_WG)
			w8[dst + op + i] = w8[src + litsrc + i];
		memoryBarrierBuffer();
		barrier();
		for (uint j = tid; j < mlen; j += LZ_WG)
			w8[dst + op + lit + j] = w8[dst + op + lit - moff + (j % moff)];
		memoryBarrierBuffer();
		barrier();
		if (tid == 0u)
			lz_op = op + lit + mlen;
		barrier();
		if (st == 1u)
			return lz_op == dlen;
	}
	return false;
}
