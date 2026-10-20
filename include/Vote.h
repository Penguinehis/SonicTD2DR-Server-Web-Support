#ifndef VOTE_H
#define VOTE_H
#include <Log.h>
#include <stdint.h>

typedef enum
{
	VOTE_KICK,
	VOTE_PRACTICE
} VoteType;

typedef enum
{
	VOTE_SUCCESS = 1,
	VOTE_ALREADYVOTED = 0,
	VOTE_FULL = -1
} VoteState;

/* One slot per player that could possibly be connected. The old array held 7
   entries while every connected player is allowed to vote, so a busy lobby
   wrote past the end of the struct. */
#ifndef VOTE_MAX_VOTERS
#define VOTE_MAX_VOTERS 128
#endif

typedef struct
{
	VoteType	type;
	bool		ongoing;
	uint16_t	votes[VOTE_MAX_VOTERS];
	uint8_t		votecnt;
	uint16_t	novotes[VOTE_MAX_VOTERS];
	uint8_t		novotecnt;
	uint8_t		votetotal;
	double		countdown;
} Vote;

struct Server;
bool		vote_init(struct Server* server, Vote* vote, VoteType type, uint16_t id);
VoteState	vote_add(Vote* vote, uint16_t id);
VoteState	vote_add_no(Vote* vote, uint16_t id);
bool		vote_tick(struct Server* server, Vote* vote);
bool		vote_check(Vote* vote);
bool		vote_rejected(Vote* vote);

#endif
