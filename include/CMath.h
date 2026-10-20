#ifndef CMATH_H
#define CMATH_H
#include <math.h>
#include <stdint.h>

typedef struct
{
	float x;
	float y;
} Vector2;

float	lerp(float a, float b, float f);
float	sign(float x);

float	vector2_dist(Vector2* a, Vector2* b);
float	vector2_dist_squared(Vector2* a, Vector2* b);
Vector2	vector2_dir(Vector2* a, Vector2* b);
uint16_t game_float_to_half(float f);
float game_half_to_float(uint16_t h);

#endif
