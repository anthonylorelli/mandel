#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>
#include <sstream>

namespace Mandelbrot
{
    // 1024-bit significand floating-point number (32 x 32-bit limbs in base 2^32)
    // Provides ~308 decimal digits of precision with zero heap allocations.
    // Value = (-1)^sign * sum_{i=0}^{N-1} (limbs[i] * B^{-(i+1)}) * B^exp, where B = 2^32.
    class BigFloat
    {
    public:
        static constexpr int N = 32; // 32 * 32 = 1024 bits of mantissa (~308 decimal digits)

        bool sign{ false };
        bool isZero{ true };
        int64_t exp{ 0 };
        uint32_t limbs[N]{};

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

        void SetZero()
        {
            sign = false;
            isZero = true;
            exp = 0;
            std::memset(limbs, 0, sizeof(limbs));
        }

        void Normalize()
        {
            int firstNonZero = -1;
            for (int i = 0; i < N; ++i)
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
                for (int i = 0; i < N - firstNonZero; ++i)
                {
                    limbs[i] = limbs[i + firstNonZero];
                }
                for (int i = N - firstNonZero; i < N; ++i)
                {
                    limbs[i] = 0;
                }
            }
        }

        void FromDouble(double val)
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
            double m = std::frexp(val, &bitExp); // val = m * 2^bitExp, m in [0.5, 1.0)

            // Floor division so that 32*(exp - 1) < bitExp <= 32*exp
            int64_t bExp = bitExp - 1;
            int64_t q = (bExp >= 0) ? (bExp / 32) : ((bExp - 31) / 32);
            exp = q + 1;

            int shift = static_cast<int>(bitExp - 32 * exp); // in [-31, 0]
            double scaled = std::ldexp(m, shift);

            constexpr double BASE = 4294967296.0; // 2^32
            for (int i = 0; i < 4 && scaled > 0.0; ++i)
            {
                scaled *= BASE;
                uint32_t limb = static_cast<uint32_t>(scaled);
                limbs[i] = limb;
                scaled -= static_cast<double>(limb);
            }

            isZero = false;
            Normalize();
        }

        double ToDouble() const
        {
            if (isZero) return 0.0;

            constexpr double INV_BASE1 = 1.0 / 4294967296.0;
            constexpr double INV_BASE2 = INV_BASE1 * INV_BASE1;

            double mant = static_cast<double>(limbs[0])
                        + static_cast<double>(limbs[1]) * INV_BASE1
                        + static_cast<double>(limbs[2]) * INV_BASE2;

            int64_t bitShift = 32 * (exp - 1);
            if (bitShift > 1024) bitShift = 1024;
            if (bitShift < -1075) return 0.0;

            double res = std::ldexp(mant, static_cast<int>(bitShift));
            return sign ? -res : res;
        }

        BigFloat Abs() const
        {
            BigFloat res = *this;
            if (!res.isZero) res.sign = false;
            return res;
        }

        BigFloat operator-() const
        {
            BigFloat res = *this;
            if (!res.isZero) res.sign = !res.sign;
            return res;
        }

        static int CompareMagnitude(const BigFloat& a, const BigFloat& b)
        {
            if (a.isZero && b.isZero) return 0;
            if (a.isZero) return -1;
            if (b.isZero) return 1;
            if (a.exp != b.exp) return (a.exp > b.exp) ? 1 : -1;
            for (int i = 0; i < N; ++i)
            {
                if (a.limbs[i] != b.limbs[i])
                {
                    return (a.limbs[i] > b.limbs[i]) ? 1 : -1;
                }
            }
            return 0;
        }

        static BigFloat AddMagnitude(const BigFloat& a, const BigFloat& b, bool resultSign)
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
                return AddMagnitude(b, a, resultSign);
            }

            const int64_t diff = a.exp - b.exp;
            if (diff >= N)
            {
                BigFloat r = a;
                r.sign = resultSign;
                return r;
            }

            BigFloat res;
            res.sign = resultSign;
            res.isZero = false;

            uint64_t carry = 0;
            uint32_t temp[N]{};
            for (int i = N - 1; i >= 0; --i)
            {
                uint64_t sum = static_cast<uint64_t>(a.limbs[i]) + carry;
                if (i >= diff)
                {
                    sum += static_cast<uint64_t>(b.limbs[i - diff]);
                }
                temp[i] = static_cast<uint32_t>(sum & 0xFFFFFFFFULL);
                carry = sum >> 32;
            }

            if (carry > 0)
            {
                res.exp = a.exp + 1;
                res.limbs[0] = static_cast<uint32_t>(carry);
                for (int i = 1; i < N; ++i)
                {
                    res.limbs[i] = temp[i - 1];
                }
            }
            else
            {
                res.exp = a.exp;
                for (int i = 0; i < N; ++i)
                {
                    res.limbs[i] = temp[i];
                }
            }

            res.Normalize();
            return res;
        }

        // Assumes |a| >= |b|
        static BigFloat SubMagnitude(const BigFloat& a, const BigFloat& b, bool resultSign)
        {
            if (b.isZero)
            {
                BigFloat r = a;
                if (!r.isZero) r.sign = resultSign;
                return r;
            }

            const int64_t diff = a.exp - b.exp;
            if (diff > N)
            {
                BigFloat r = a;
                r.sign = resultSign;
                return r;
            }

            int64_t borrow = 0;
            uint32_t temp[N + 1]{};
            for (int i = N; i >= 0; --i)
            {
                int64_t av = (i < N) ? static_cast<int64_t>(a.limbs[i]) : 0;
                int64_t bv = (i >= diff && (i - diff) < N) ? static_cast<int64_t>(b.limbs[i - diff]) : 0;
                int64_t sub = av - bv - borrow;
                if (sub < 0)
                {
                    sub += (1LL << 32);
                    borrow = 1;
                }
                else
                {
                    borrow = 0;
                }
                temp[i] = static_cast<uint32_t>(sub);
            }

            int firstNonZero = -1;
            for (int i = 0; i <= N; ++i)
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
            for (int i = 0; i < N; ++i)
            {
                int srcIdx = firstNonZero + i;
                res.limbs[i] = (srcIdx <= N) ? temp[srcIdx] : 0;
            }
            return res;
        }

        BigFloat operator+(const BigFloat& rhs) const
        {
            if (isZero) return rhs;
            if (rhs.isZero) return *this;

            if (sign == rhs.sign)
            {
                return AddMagnitude(*this, rhs, sign);
            }

            int cmp = CompareMagnitude(*this, rhs);
            if (cmp == 0)
            {
                return BigFloat(); // Zero
            }
            else if (cmp > 0)
            {
                return SubMagnitude(*this, rhs, sign);
            }
            else
            {
                return SubMagnitude(rhs, *this, rhs.sign);
            }
        }

        BigFloat operator-(const BigFloat& rhs) const
        {
            return *this + (-rhs);
        }

        BigFloat& operator+=(const BigFloat& rhs)
        {
            *this = *this + rhs;
            return *this;
        }

        BigFloat& operator-=(const BigFloat& rhs)
        {
            *this = *this - rhs;
            return *this;
        }

        BigFloat Mul(const BigFloat& rhs, int activeLimbs = N) const
        {
            if (isZero || rhs.isZero) return BigFloat();

            const int limit = std::clamp(activeLimbs, 1, N);
            uint64_t prod[N + 3]{};
            for (int i = 0; i < limit; ++i)
            {
                uint64_t ai = limbs[i];
                if (ai == 0) continue;
                int maxJ = std::min(limit - 1, N + 1 - i);
                for (int j = 0; j <= maxJ; ++j)
                {
                    uint64_t bj = rhs.limbs[j];
                    if (bj == 0) continue;
                    uint64_t p = ai * bj;
                    prod[i + j + 1] += (p & 0xFFFFFFFFULL);
                    prod[i + j] += (p >> 32);
                }
            }

            for (int k = N + 2; k >= 1; --k)
            {
                prod[k - 1] += prod[k] >> 32;
                prod[k] &= 0xFFFFFFFFULL;
            }

            BigFloat res;
            res.sign = (sign != rhs.sign);
            res.isZero = false;

            if (prod[0] != 0)
            {
                res.exp = exp + rhs.exp;
                for (int i = 0; i < N; ++i)
                {
                    res.limbs[i] = static_cast<uint32_t>(prod[i]);
                }
            }
            else
            {
                res.exp = exp + rhs.exp - 1;
                for (int i = 0; i < N; ++i)
                {
                    res.limbs[i] = static_cast<uint32_t>(prod[i + 1]);
                }
            }

            res.Normalize();
            return res;
        }

        BigFloat operator*(const BigFloat& rhs) const
        {
            return Mul(rhs, N);
        }

        BigFloat Sqr(int activeLimbs = N) const
        {
            return Mul(*this, activeLimbs);
        }

        BigFloat MulUInt32(uint32_t m) const
        {
            if (isZero || m == 0) return BigFloat();

            uint64_t carry = 0;
            uint32_t temp[N]{};
            for (int i = N - 1; i >= 0; --i)
            {
                uint64_t p = static_cast<uint64_t>(limbs[i]) * static_cast<uint64_t>(m) + carry;
                temp[i] = static_cast<uint32_t>(p & 0xFFFFFFFFULL);
                carry = p >> 32;
            }

            BigFloat res;
            res.sign = sign;
            res.isZero = false;
            if (carry > 0)
            {
                res.exp = exp + 1;
                res.limbs[0] = static_cast<uint32_t>(carry);
                for (int i = 1; i < N; ++i) res.limbs[i] = temp[i - 1];
            }
            else
            {
                res.exp = exp;
                for (int i = 0; i < N; ++i) res.limbs[i] = temp[i];
            }
            res.Normalize();
            return res;
        }

        // Reciprocal 1 / (*this) using Newton-Raphson iteration: x_{k+1} = x_k * (2 - d * x_k)
        BigFloat Reciprocal() const
        {
            if (isZero) return BigFloat();

            // Normalize exponent to 1 so dNorm is in [1, 2^32) and fits cleanly in double
            BigFloat dNorm = *this;
            dNorm.sign = false;
            int64_t origExp = dNorm.exp;
            dNorm.exp = 1;

            double approx = 1.0 / dNorm.ToDouble();
            BigFloat x(approx);
            const BigFloat two(2.0);

            // 5 Newton iterations double precision from 53 bits -> 106 -> 212 -> 424 -> 848 -> 1696 bits (> 1024)
            for (int i = 0; i < 5; ++i)
            {
                x = x * (two - dNorm * x);
            }

            x.exp -= (origExp - 1);
            x.sign = sign;
            return x;
        }

        BigFloat operator/(const BigFloat& rhs) const
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
                    uint32_t digit = val.limbs[0];
                    if (digit > 9) digit = 9;
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
    };
}
