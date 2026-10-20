#ifndef ZONE_H
#define ZONE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
{
	double x, y, w, h;
} Zone;

typedef struct
{
	double min_x, min_y, max_x, max_y;
	bool valid;
} ZoneBounds;

extern Zone*	g_mapZone[21];
extern size_t	g_mapZoneSize[21];
extern ZoneBounds g_mapZoneBounds[21];

#endif