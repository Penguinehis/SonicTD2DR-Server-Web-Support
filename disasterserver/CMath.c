#include <CMath.h>
#include <string.h>

float lerp(float a, float b, float f)
{
	return a * (1.0f - f) + (b * f);
}

float sign(float x)
{
	return ((x) > 0) ? 1.f : (((x) < 0) ? -1.f : 0.f);
}

float vector2_dist(Vector2* a, Vector2* b)
{
	return sqrtf(powf(b->x - a->x, 2) + powf(b->y - a->y, 2));
}

float vector2_dist_squared(Vector2* a, Vector2* b)
{
	float x = b->x - a->x;
	float y = b->y - a->y;
	return x * x + y * y;
}

Vector2 vector2_dir(Vector2* a, Vector2* b)
{
	return (Vector2) { sign(a->x - b->x), sign(a->y - b->y) };
}

uint16_t game_float_to_half(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));

    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = (int32_t)((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;

    if (exponent <= 0)
    {
        if (exponent < -10)
            return (uint16_t)sign;

        mantissa |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exponent);
        uint32_t value = mantissa >> shift;

        if ((mantissa >> (shift - 1)) & 1)
            value++;

        return (uint16_t)(sign | value);
    }

    if (exponent >= 31)
    {
        if (mantissa == 0)
            return (uint16_t)(sign | 0x7c00);

        mantissa >>= 13;
        return (uint16_t)(sign | 0x7c00 | mantissa | (mantissa == 0));
    }

    uint32_t value = (uint32_t)(exponent << 10) | (mantissa >> 13);
    if (mantissa & 0x1000)
        value++;

    return (uint16_t)(sign | value);
}

float game_half_to_float(uint16_t h)
{
    uint32_t sign = ((uint32_t)h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    uint32_t bits;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            exponent = 1;
            while ((mantissa & 0x400) == 0)
            {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x3ff;
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7f800000 | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }

    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}
