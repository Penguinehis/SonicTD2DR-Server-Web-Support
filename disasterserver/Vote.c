#include <Vote.h>
#include <States.h>
#include <Config.h>

bool vote_init(Server* server, Vote* vote, VoteType type, uint16_t id)
{
	vote->votecnt = 0;
	vote->novotecnt = 0;
	vote->votetotal = 0;
	vote->type = type;

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* peer = (PeerData*)server->peers.ptr[i];
		if (!peer)
			continue;

		peer->can_vote = (peer->id != id);

		if (peer->can_vote && vote->votetotal < 0xFF)
			vote->votetotal++;
	}

	if (vote->votetotal <= 1)
		return false;

	vote->ongoing = true;
	vote->countdown = g_config.votekick_vote_seconds * TICKSPERSEC;
	memset(vote->votes, 0, sizeof(vote->votes));
	memset(vote->novotes, 0, sizeof(vote->novotes));
	return true;
}

VoteState vote_add(Vote* vote, uint16_t id)
{
	for (int i = 0; i < vote->votecnt; i++)
	{
		if (vote->votes[i] == id)
			return VOTE_ALREADYVOTED;
	}

	for (int i = 0; i < vote->novotecnt; i++)
	{
		if (vote->novotes[i] == id)
			return VOTE_ALREADYVOTED;
	}

	/* Never write past the array even if more players are connected than it
	   can hold; the vote just stops accepting new ballots. */
	if (vote->votecnt >= VOTE_MAX_VOTERS)
		return VOTE_FULL;

	vote->votes[vote->votecnt++] = id;

	if (vote->votecnt + vote->novotecnt >= vote->votetotal)
		return VOTE_FULL;

	return VOTE_SUCCESS;
}

VoteState vote_add_no(Vote* vote, uint16_t id)
{
	for (int i = 0; i < vote->votecnt; i++)
	{
		if (vote->votes[i] == id)
			return VOTE_ALREADYVOTED;
	}

	for (int i = 0; i < vote->novotecnt; i++)
	{
		if (vote->novotes[i] == id)
			return VOTE_ALREADYVOTED;
	}

	if (vote->novotecnt >= VOTE_MAX_VOTERS)
		return VOTE_FULL;

	vote->novotes[vote->novotecnt++] = id;

	if (vote->votecnt + vote->novotecnt >= vote->votetotal)
		return VOTE_FULL;

	return VOTE_SUCCESS;
}

bool vote_tick(Server* server, Vote* vote)
{
	vote->countdown -= server->delta;
	if (vote->countdown <= 0)
		return false;

	return true;
}

bool vote_check(Vote* vote)
{
	return vote->votecnt > (vote->votetotal / 2.0);
}

/* True once enough players said no that a majority can no longer be reached. */
bool vote_rejected(Vote* vote)
{
	return vote->novotecnt >= (vote->votetotal / 2.0);
}
