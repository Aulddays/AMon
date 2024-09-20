#pragma once
#include <stdint.h>
#include <algorithm>
#include <assert.h>

// 16bit approximate unsigned int representation
// AUint<11>: 11bit base + 5bit exponent, MAX: ((1<<11) - 1) << (32 - 11) ==  4,292,870,144, precision 1/2048 ~= 0.049%
// AUint<12>: 12bit base + 4bit exponent, MAX: ((1<<12) - 2) << ((1<<4) - 1) == 134,152,192, precision 1/4096 ~= 0.024%
// AUint<13>: 13bit base + 3bit exponent, MAX: ((1<<13) - 2) << ((1<<3) - 1) ==   1,048,320, precision 1/8192 ~= 0.012%
template<uint8_t precision = 12>
class AUint
{
public:
	// number of bits in the base part
	static const uint32_t AU_BASE = precision;
	// number of bits in the exponent part
	static const uint32_t AU_EXP = 16 - AU_BASE;
	// the max uint32 value representable by AUint
	// for EXP < 5, the exponent part may get to its largest value (all bits of 1), and in this case, the base part may not
	// take all bits of 1, because all 1's is reserved by NAN. So max of base is all but lowest bit of 1, or (1 << BASE) - 2
	// for EXP >= 5, the exponent part may not get to all 1's (even with EXP == 5, exponent part of 31 will overrun uint32).
	// So the base part may get to all 1's ((1 << BASE) - 1) and exponent part 32-BASE
	static const uint32_t AU_MAX = ((1 << AU_BASE) - (AU_EXP < 5 ? 2 : 1)) << (AU_EXP < 5 ? ((1 << AU_EXP) - 1) : (32 - AU_BASE));
	// value reserved to represent NaN
	static const uint16_t AU_NAN = ~(uint16_t)0;

	AUint(): data(AU_NAN) {}
	~AUint() {}

	AUint(const uint32_t &r) { set(r); }
	AUint &operator =(const AUint &r) { data = r.data; return *this; }
	AUint &operator =(uint32_t r) { return set(r); }

	AUint &set(uint32_t r) { data = fromint(r); return *this; }
	uint32_t get() const { return toint(data); }
	uint16_t getraw() const { return data; }
	bool isnan() { return isnan(data); }

	static uint16_t fromnan() { return AU_NAN; }
	static bool isnan(uint16_t r) { return r == AU_NAN; }
	static uint16_t fromint(uint32_t r)
	{
		uint16_t data = AU_NAN;
		uint32_t baseth = 1u << AU_BASE;
		if (r > AU_MAX)
			data = (AU_NAN - 1);
		else if (r < (1u << AU_BASE))
			data = r;
		else
		{
			// look for the highest bit 1. There may be direct instruction on certain CPU
			for (int exp = std::min((1u << AU_EXP) - 1, 31u - AU_BASE); exp > 0; --exp)
			{
				uint32_t limit = (1 << (AU_BASE - 1)) << exp;
				if ((r & limit) == 0)
					continue;
				// highest bit found
				uint32_t sig = (((1 << (AU_BASE + 1)) - 1) << (exp - 1)) & r;
				sig += 1 << (exp - 1);
				if (sig & (limit << 1))
					data = ((exp + 1) << AU_BASE) | (1 << (AU_BASE - 1));
				else
					data = (exp << AU_BASE) | (sig >> exp);
				return data;
			}
			assert(false);
		}
		return data;
	}
	static uint32_t toint(uint16_t rawau) { return rawau == AU_NAN ? 0 : (rawau & ((1 << AU_BASE) - 1)) << (rawau >> AU_BASE); }


private:
	uint16_t data;
};

