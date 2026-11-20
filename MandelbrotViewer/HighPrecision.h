#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>
#include <sstream>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace Mandelbrot
{
    // 1024-bit significand floating-point number (16 x 64-bit limbs in base 2^64)
    // Provides ~308 decimal digits of precision with zero heap allocations.
    // Value = (-1)^sign * sum_{i=0}^{W-1} (limbs[i] * B^{-(i+1)}) * B^exp, where B = 2^64.
    class BigFloat
    {
    public:
        // N retains the 32-bit limb count (32 * 32 = 1024 bits) for activeLimbs API compatibility;
        // W is the internal 64-bit word count (17 * 64 = 1088 bits, >= 1025 bits even with 63 leading zeros).
        static constexpr int N = 32;
        static constexpr int W = 17;

        bool sign{ false };
        bool isZero{ true };
        int64_t exp{ 0 };
        uint64_t limbs[W]{};

        constexpr BigFloat() = default;

        explicit BigFloat(double val)
        {
            FromDouble(val);
        }

        explicit BigFloat(std::string_view str)
        {
            FromString(str);
        }

        constexpr bool IsZero() const noexcept
        {
            return isZero;
        }

        void SetZero() noexcept
        {
            sign = false;
            isZero = true;
            exp = 0;
            std::memset(limbs, 0, sizeof(limbs));
        }

        void Normalize(int wLimit = W) noexcept
        {
            const int limit = std::clamp(wLimit, 1, W);
            int firstNonZero = -1;
            for (int i = 0; i < limit; ++i)
            {
                if (limbs[i] != 0)
                {
                    firstNonZero = i;
                    break;
                }
            }

            if (firstNonZero < 0)
            {
                SetZero();
                return;
            }

            isZero = false;
            if (firstNonZero > 0)
            {
                exp -= firstNonZero;
                for (int i = 0; i < limit - firstNonZero; ++i)
                {
                    limbs[i] = limbs[i + firstNonZero];
                }
                for (int i = limit - firstNonZero; i < W; ++i)
                {
                    limbs[i] = 0;
                }
            }
        }

        void FromDouble(double val) noexcept
        {
            SetZero();
            if (val == 0.0 || std::isnan(val) || std::isinf(val))
            {
                return;
            }

            if (val < 0.0)
            {
                sign = true;
                val = -val;
            }

            int bitExp = 0;
            const double m = std::frexp(val, &bitExp); // val = m * 2^bitExp, m in [0.5, 1.0)
            const uint64_t sig = static_cast<uint64_t>(std::ldexp(m, 53)); // exact 53-bit integer in [2^52, 2^53)

            // Floor division so that 64*(exp - 1) < bitExp <= 64*exp
            const int64_t bExp = static_cast<int64_t>(bitExp) - 1;
            const int64_t q = (bExp >= 0) ? (bExp / 64) : ((bExp - 63) / 64);
            exp = q + 1;

            // Bit offset of sig's LSB relative to limbs[0]'s LSB is in [-52, 11]
            const int s = static_cast<int>(bitExp - 64 * exp) + 11;
            if (s >= 0)
            {
                limbs[0] = sig << s;
                limbs[1] = 0;
            }
            else
            {
                limbs[0] = sig >> (-s);
                limbs[1] = sig << (64 + s);
            }

            isZero = false;
        }

        double ToDouble() const noexcept
        {
            if (isZero) return 0.0;

            constexpr double INV_BASE1 = 5.42101086242752217003726400434970855712890625e-20; // 2^-64

            const double mant = static_cast<double>(limbs[0])
                              + static_cast<double>(limbs[1]) * INV_BASE1;

            int64_t bitShift = 64 * (exp - 1);
            if (bitShift > 1024) bitShift = 1024;
            if (bitShift < -1139) return 0.0;

            const double res = std::ldexp(mant, static_cast<int>(bitShift));
            return sign ? -res : res;
        }

        BigFloat Abs() const noexcept
        {
            BigFloat res = *this;
            if (!res.isZero) res.sign = false;
            return res;
        }

        BigFloat operator-() const noexcept
        {
            BigFloat res = *this;
            if (!res.isZero) res.sign = !res.sign;
            return res;
        }

        static int CompareMagnitude(const BigFloat& a, const BigFloat& b, int activeLimbs = N) noexcept
        {
            if (a.isZero && b.isZero) return 0;
            if (a.isZero) return -1;
            if (b.isZero) return 1;
            if (a.exp != b.exp) return (a.exp > b.exp) ? 1 : -1;
            const int wLimit = WordsForLimbs(activeLimbs);
            for (int i = 0; i < wLimit; ++i)
            {
                if (a.limbs[i] != b.limbs[i])
                {
                    return (a.limbs[i] > b.limbs[i]) ? 1 : -1;
                }
            }
            return 0;
        }

        bool operator==(const BigFloat& rhs) const noexcept
        {
            if (isZero && rhs.isZero) return true;
            if (isZero != rhs.isZero || sign != rhs.sign || exp != rhs.exp) return false;
            return std::memcmp(limbs, rhs.limbs, sizeof(limbs)) == 0;
        }

        bool operator!=(const BigFloat& rhs) const noexcept
        {
            return !(*this == rhs);
        }

        static BigFloat AddMagnitude(const BigFloat& a, const BigFloat& b, bool resultSign, int activeLimbs = N) noexcept
        {
            if (a.isZero)
            {
                BigFloat r = b;
                if (!r.isZero) r.sign = resultSign;
                return r;
            }
            if (b.isZero)
            {
                BigFloat r = a;
                if (!r.isZero) r.sign = resultSign;
                return r;
            }

            if (a.exp < b.exp)
            {
                return AddMagnitude(b, a, resultSign, activeLimbs);
            }

            const int wLimit = WordsForLimbs(activeLimbs);
            const int64_t diff = a.exp - b.exp;
            if (diff >= wLimit)
            {
                BigFloat r = a;
                r.sign = resultSign;
                return r;
            }

            BigFloat res;
            res.sign = resultSign;
            res.isZero = false;

            uint64_t carry = 0;
            uint64_t temp[W]{};
            for (int i = wLimit - 1; i >= 0; --i)
            {
                uint64_t sum = a.limbs[i] + carry;
                uint64_t c = (sum < carry) ? 1ULL : 0ULL;
                if (i >= diff)
                {
                    const uint64_t bv = b.limbs[i - diff];
                    sum += bv;
                    c += (sum < bv) ? 1ULL : 0ULL;
                }
                temp[i] = sum;
                carry = c;
            }

            if (carry > 0)
            {
                res.exp = a.exp + 1;
                res.limbs[0] = carry;
                for (int i = 1; i < wLimit; ++i)
                {
                    res.limbs[i] = temp[i - 1];
                }
            }
            else
            {
                res.exp = a.exp;
                for (int i = 0; i < wLimit; ++i)
                {
                    res.limbs[i] = temp[i];
                }
            }

            return res;
        }

        // Assumes |a| >= |b|
        static BigFloat SubMagnitude(const BigFloat& a, const BigFloat& b, bool resultSign, int activeLimbs = N) noexcept
        {
            if (b.isZero)
            {
                BigFloat r = a;
                if (!r.isZero) r.sign = resultSign;
                return r;
            }

            const int wLimit = WordsForLimbs(activeLimbs);
            const int64_t diff = a.exp - b.exp;
            if (diff > wLimit)
            {
                BigFloat r = a;
                r.sign = resultSign;
                return r;
            }

            uint64_t borrow = 0;
            uint64_t temp[W + 1]{};
            for (int i = wLimit; i >= 0; --i)
            {
                const uint64_t av = (i < wLimit) ? a.limbs[i] : 0ULL;
                const uint64_t bv = (i >= diff && (i - diff) < wLimit) ? b.limbs[i - diff] : 0ULL;
                const uint64_t d1 = av - bv;
                const uint64_t b1 = (av < bv) ? 1ULL : 0ULL;
                const uint64_t d2 = d1 - borrow;
                const uint64_t b2 = (d1 < borrow) ? 1ULL : 0ULL;
                temp[i] = d2;
                borrow = b1 + b2;
            }

            int firstNonZero = -1;
            for (int i = 0; i <= wLimit; ++i)
            {
                if (temp[i] != 0)
                {
                    firstNonZero = i;
                    break;
                }
            }

            BigFloat res;
            if (firstNonZero < 0)
            {
                return res; // Zero
            }

            res.sign = resultSign;
            res.isZero = false;
            res.exp = a.exp - firstNonZero;
            for (int i = 0; i < wLimit; ++i)
            {
                const int srcIdx = firstNonZero + i;
                res.limbs[i] = (srcIdx <= wLimit) ? temp[srcIdx] : 0ULL;
            }
            return res;
        }

        BigFloat Add(const BigFloat& rhs, int activeLimbs = N) const noexcept
        {
            if (isZero) return rhs;
            if (rhs.isZero) return *this;

            if (sign == rhs.sign)
            {
                return AddMagnitude(*this, rhs, sign, activeLimbs);
            }

            const int cmp = CompareMagnitude(*this, rhs, activeLimbs);
            if (cmp == 0)
            {
                return BigFloat(); // Zero
            }
            if (cmp > 0)
            {
                return SubMagnitude(*this, rhs, sign, activeLimbs);
            }
            return SubMagnitude(rhs, *this, rhs.sign, activeLimbs);
        }

        BigFloat Sub(const BigFloat& rhs, int activeLimbs = N) const noexcept
        {
            if (rhs.isZero) return *this;
            if (isZero) return -rhs;

            if (sign != rhs.sign)
            {
                return AddMagnitude(*this, rhs, sign, activeLimbs);
            }

            const int cmp = CompareMagnitude(*this, rhs, activeLimbs);
            if (cmp == 0)
            {
                return BigFloat(); // Zero
            }
            if (cmp > 0)
            {
                return SubMagnitude(*this, rhs, sign, activeLimbs);
            }
            return SubMagnitude(rhs, *this, !rhs.sign, activeLimbs);
        }

        BigFloat operator+(const BigFloat& rhs) const noexcept
        {
            return Add(rhs, N);
        }

        BigFloat operator-(const BigFloat& rhs) const noexcept
        {
            return Sub(rhs, N);
        }

        BigFloat& operator+=(const BigFloat& rhs) noexcept
        {
            *this = Add(rhs, N);
            return *this;
        }

        BigFloat& operator-=(const BigFloat& rhs) noexcept
        {
            *this = Sub(rhs, N);
            return *this;
        }

        BigFloat Mul(const BigFloat& rhs, int activeLimbs = N) const noexcept
        {
            if (isZero || rhs.isZero) return BigFloat();

            const int wLimit = WordsForLimbs(activeLimbs);
            const int prodLen = wLimit + 3;
            uint64_t prod[W + 3]{};
            uint64_t overflow[W + 3]{};

            for (int i = 0; i < wLimit; ++i)
            {
                const uint64_t ai = limbs[i];
                if (ai == 0) continue;
                const int maxJ = std::min(wLimit - 1, wLimit + 1 - i);
                for (int j = 0; j <= maxJ; ++j)
                {
                    const uint64_t bj = rhs.limbs[j];
                    if (bj == 0) continue;
                    uint64_t lo = 0, hi = 0;
                    Mul64x64(ai, bj, lo, hi);

                    const int loIdx = i + j + 1;
                    const uint64_t prevLo = prod[loIdx];
                    prod[loIdx] = prevLo + lo;
                    if (prod[loIdx] < prevLo) ++overflow[loIdx];

                    const int hiIdx = i + j;
                    const uint64_t prevHi = prod[hiIdx];
                    prod[hiIdx] = prevHi + hi;
                    if (prod[hiIdx] < prevHi) ++overflow[hiIdx];
                }
            }

            uint64_t carry = 0;
            for (int k = prodLen - 1; k >= 1; --k)
            {
                const uint64_t v = prod[k] + carry;
                const uint64_t c1 = (v < prod[k]) ? 1ULL : 0ULL;
                prod[k] = v;
                carry = overflow[k] + c1;
            }
            prod[0] += carry;

            BigFloat res;
            res.sign = (sign != rhs.sign);
            res.isZero = false;

            if (prod[0] != 0)
            {
                res.exp = exp + rhs.exp;
                for (int i = 0; i < wLimit; ++i)
                {
                    res.limbs[i] = prod[i];
                }
            }
            else
            {
                res.exp = exp + rhs.exp - 1;
                for (int i = 0; i < wLimit; ++i)
                {
                    res.limbs[i] = prod[i + 1];
                }
            }

            res.Normalize(wLimit);
            return res;
        }

        BigFloat operator*(const BigFloat& rhs) const noexcept
        {
            return Mul(rhs, N);
        }

        BigFloat Sqr(int activeLimbs = N) const noexcept
        {
            if (isZero) return BigFloat();

            const int wLimit = WordsForLimbs(activeLimbs);
            const int prodLen = wLimit + 3;
            uint64_t prod[W + 3]{};
            uint64_t overflow[W + 3]{};

            // 1. Accumulate off-diagonal products (i < j) once
            for (int i = 0; i < wLimit; ++i)
            {
                const uint64_t ai = limbs[i];
                if (ai == 0) continue;
                const int maxJ = std::min(wLimit - 1, wLimit + 1 - i);
                for (int j = i + 1; j <= maxJ; ++j)
                {
                    const uint64_t aj = limbs[j];
                    if (aj == 0) continue;
                    uint64_t lo = 0, hi = 0;
                    Mul64x64(ai, aj, lo, hi);

                    const int loIdx = i + j + 1;
                    const uint64_t prevLo = prod[loIdx];
                    prod[loIdx] = prevLo + lo;
                    if (prod[loIdx] < prevLo) ++overflow[loIdx];

                    const int hiIdx = i + j;
                    const uint64_t prevHi = prod[hiIdx];
                    prod[hiIdx] = prevHi + hi;
                    if (prod[hiIdx] < prevHi) ++overflow[hiIdx];
                }
            }

            // Propagate carries for off-diagonal sum, then double in a single left-shift pass
            uint64_t carry = 0;
            for (int k = prodLen - 1; k >= 1; --k)
            {
                const uint64_t v = prod[k] + carry;
                const uint64_t c1 = (v < prod[k]) ? 1ULL : 0ULL;
                prod[k] = v;
                carry = overflow[k] + c1;
                overflow[k] = 0;
            }
            prod[0] += carry;
            overflow[0] = 0;

            uint64_t bitCarry = 0;
            for (int k = prodLen - 1; k >= 0; --k)
            {
                const uint64_t v = prod[k];
                prod[k] = (v << 1) | bitCarry;
                bitCarry = v >> 63;
            }

            // 2. Add diagonal squares (limbs[i]^2)
            for (int i = 0; i < wLimit && (2 * i) <= wLimit + 1; ++i)
            {
                const uint64_t ai = limbs[i];
                if (ai == 0) continue;
                uint64_t lo = 0, hi = 0;
                Mul64x64(ai, ai, lo, hi);

                const int loIdx = 2 * i + 1;
                const uint64_t prevLo = prod[loIdx];
                prod[loIdx] = prevLo + lo;
                if (prod[loIdx] < prevLo) ++overflow[loIdx];

                const int hiIdx = 2 * i;
                const uint64_t prevHi = prod[hiIdx];
                prod[hiIdx] = prevHi + hi;
                if (prod[hiIdx] < prevHi) ++overflow[hiIdx];
            }

            carry = 0;
            for (int k = prodLen - 1; k >= 1; --k)
            {
                const uint64_t v = prod[k] + carry;
                const uint64_t c1 = (v < prod[k]) ? 1ULL : 0ULL;
                prod[k] = v;
                carry = overflow[k] + c1;
            }
            prod[0] += carry;

            BigFloat res;
            res.sign = false;
            res.isZero = false;

            if (prod[0] != 0)
            {
                res.exp = exp + exp;
                for (int i = 0; i < wLimit; ++i)
                {
                    res.limbs[i] = prod[i];
                }
            }
            else
            {
                res.exp = exp + exp - 1;
                for (int i = 0; i < wLimit; ++i)
                {
                    res.limbs[i] = prod[i + 1];
                }
            }

            res.Normalize(wLimit);
            return res;
        }

        BigFloat MulUInt32(uint32_t m) const noexcept
        {
            if (isZero || m == 0) return BigFloat();

            uint64_t carry = 0;
            uint64_t temp[W]{};
            for (int i = W - 1; i >= 0; --i)
            {
                uint64_t lo = 0, hi = 0;
                Mul64x64(limbs[i], static_cast<uint64_t>(m), lo, hi);
                const uint64_t sum = lo + carry;
                const uint64_t c = (sum < lo) ? 1ULL : 0ULL;
                temp[i] = sum;
                carry = hi + c;
            }

            BigFloat res;
            res.sign = sign;
            res.isZero = false;
            if (carry > 0)
            {
                res.exp = exp + 1;
                res.limbs[0] = carry;
                for (int i = 1; i < W; ++i) res.limbs[i] = temp[i - 1];
            }
            else
            {
                res.exp = exp;
                for (int i = 0; i < W; ++i) res.limbs[i] = temp[i];
            }
            res.Normalize();
            return res;
        }

        // Reciprocal 1 / (*this) using Newton-Raphson iteration: x_{k+1} = x_k * (2 - d * x_k)
        BigFloat Reciprocal() const noexcept
        {
            if (isZero) return BigFloat();

            // Normalize exponent to 1 so dNorm is in [1, 2^64) and fits cleanly in double
            BigFloat dNorm = *this;
            dNorm.sign = false;
            const int64_t origExp = dNorm.exp;
            dNorm.exp = 1;

            const double approx = 1.0 / dNorm.ToDouble();
            BigFloat x(approx);
            const BigFloat two(2.0);

            // 5 Newton iterations double precision from 53 bits -> 106 -> 212 -> 424 -> 848 -> 1696 bits (> 1024)
            // Scale active precision progressively so early iterations avoid full 1024-bit work.
            constexpr int precSteps[5]{ 6, 10, 18, 32, 32 };
            for (int p : precSteps)
            {
                x = x.Mul(two.Sub(dNorm.Mul(x, p), p), p);
            }

            x.exp -= (origExp - 1);
            x.sign = sign;
            return x;
        }

        BigFloat operator/(const BigFloat& rhs) const noexcept
        {
            if (isZero || rhs.isZero) return BigFloat();
            return (*this) * rhs.Reciprocal();
        }

        std::string ToString(size_t maxDecimals = 24) const
        {
            if (isZero) return "+0.00000000";

            BigFloat val = Abs();
            std::string out = sign ? "-" : "+";

            // Extract integer part (for fractal coordinates, integer part is small)
            if (val.exp <= 0)
            {
                out += "0";
            }
            else if (val.exp == 1)
            {
                out += std::to_string(val.limbs[0]);
                val.limbs[0] = 0;
                val.Normalize();
            }
            else
            {
                uint64_t intVal = static_cast<uint64_t>(std::floor(val.ToDouble()));
                out += std::to_string(intVal);
                val = val - BigFloat(static_cast<double>(intVal));
                if (val.sign) val.SetZero();
            }

            out += ".";
            for (size_t d = 0; d < maxDecimals; ++d)
            {
                val = val.MulUInt32(10);
                if (!val.isZero && val.exp >= 1)
                {
                    uint32_t digit = static_cast<uint32_t>(std::min<uint64_t>(val.limbs[0], 9ULL));
                    out += static_cast<char>('0' + digit);
                    val.limbs[0] = 0;
                    val.Normalize();
                }
                else
                {
                    out += '0';
                }
            }
            return out;
        }

        std::wstring ToWString(size_t maxDecimals = 24) const
        {
            std::string s = ToString(maxDecimals);
            return std::wstring(s.begin(), s.end());
        }

        void FromString(std::string_view str)
        {
            BigFloat parsed;
            if (TryParse(str, parsed))
            {
                *this = parsed;
            }
            else
            {
                SetZero();
            }
        }

        static bool TryParse(std::string_view str, BigFloat& out)
        {
            out.SetZero();
            size_t pos = 0;
            while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\r' || str[pos] == '\n'))
            {
                pos++;
            }
            if (pos >= str.size()) return false;

            bool neg = false;
            if (str[pos] == '-') { neg = true; pos++; }
            else if (str[pos] == '+') { pos++; }

            bool hasDigits = false;
            BigFloat intPart(0.0);
            while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9')
            {
                hasDigits = true;
                uint32_t digit = static_cast<uint32_t>(str[pos] - '0');
                intPart = intPart.MulUInt32(10) + BigFloat(static_cast<double>(digit));
                pos++;
            }

            BigFloat fracPart(0.0);
            const BigFloat tenth = BigFloat(1.0) / BigFloat(10.0);
            if (pos < str.size() && str[pos] == '.')
            {
                pos++;
                BigFloat scale = tenth;
                while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9')
                {
                    hasDigits = true;
                    uint32_t digit = static_cast<uint32_t>(str[pos] - '0');
                    if (digit != 0)
                    {
                        fracPart += scale.MulUInt32(digit);
                    }
                    scale = scale * tenth;
                    pos++;
                }
            }

            if (!hasDigits) return false;

            int exp10 = 0;
            if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E'))
            {
                pos++;
                bool expNeg = false;
                if (pos < str.size() && str[pos] == '-') { expNeg = true; pos++; }
                else if (pos < str.size() && str[pos] == '+') { pos++; }

                bool hasExpDigits = false;
                while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9')
                {
                    hasExpDigits = true;
                    if (exp10 < 10000)
                    {
                        exp10 = exp10 * 10 + static_cast<int>(str[pos] - '0');
                    }
                    pos++;
                }
                if (!hasExpDigits) return false;
                if (expNeg) exp10 = -exp10;
            }

            while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\r' || str[pos] == '\n'))
            {
                pos++;
            }
            if (pos != str.size()) return false;

            out = intPart + fracPart;
            if (!out.isZero)
            {
                if (exp10 > 0)
                {
                    BigFloat pow10(1.0);
                    BigFloat base(10.0);
                    int e = std::min(exp10, 308);
                    while (e > 0)
                    {
                        if (e & 1) pow10 = pow10 * base;
                        base = base.Sqr();
                        e >>= 1;
                    }
                    out = out * pow10;
                }
                else if (exp10 < 0)
                {
                    BigFloat pow10Inv(1.0);
                    BigFloat base = tenth;
                    int e = std::min(-exp10, 308);
                    while (e > 0)
                    {
                        if (e & 1) pow10Inv = pow10Inv * base;
                        base = base.Sqr();
                        e >>= 1;
                    }
                    out = out * pow10Inv;
                }
                if (!out.isZero) out.sign = neg;
            }
            return true;
        }

        static bool TryParse(std::wstring_view wstr, BigFloat& out)
        {
            std::string ascii;
            ascii.reserve(wstr.size());
            for (wchar_t wc : wstr)
            {
                if (wc > 127) return false;
                ascii.push_back(static_cast<char>(wc));
            }
            return TryParse(std::string_view(ascii), out);
        }

    private:
        static constexpr int WordsForLimbs(int activeLimbs) noexcept
        {
            return std::clamp((activeLimbs / 2) + 1, 2, W);
        }

        static inline void Mul64x64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) noexcept
        {
#if defined(_MSC_VER) && defined(_M_X64)
            lo = _umul128(a, b, &hi);
#elif defined(_MSC_VER) && defined(_M_ARM64)
            lo = a * b;
            hi = __umulh(a, b);
#elif defined(__SIZEOF_INT128__)
            const unsigned __int128 p = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
            lo = static_cast<uint64_t>(p);
            hi = static_cast<uint64_t>(p >> 64);
#else
            const uint64_t aLo = a & 0xFFFFFFFFULL;
            const uint64_t aHi = a >> 32;
            const uint64_t bLo = b & 0xFFFFFFFFULL;
            const uint64_t bHi = b >> 32;
            const uint64_t p00 = aLo * bLo;
            const uint64_t p01 = aLo * bHi;
            const uint64_t p10 = aHi * bLo;
            const uint64_t p11 = aHi * bHi;
            const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
            lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
            hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
        }
    };
}
