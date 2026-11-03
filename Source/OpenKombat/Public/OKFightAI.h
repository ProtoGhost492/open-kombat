#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

class FOKFighter;
class FOKMkoFile;
class FOKPlayer;
class FOKScriptRuntime;

/**
 * A computer player (system F9). MKKE's AI is an AIDrone driving the shared MKAI script and a personality script
 * (AI_Normal ...), with module 197's natives underneath. Ours is a small C++ player that presses the same inputs a
 * human would, through the same input buffer, so it works for any game whose moves come from inputs. Its choices use
 * MKAI's own difficulty tables (10 entries each, read from MKAI.mko as MKKE's BindTunables does) and MKKE's rules
 * where they are known: when to block (FUN_0088BAD0), how often to attack and the impatience that raises it
 * (FUN_0088DFF0). Special moves are entered from the fighter's own command records.
 */
class OPENKOMBAT_API FOKFightAI
{
public:
	/** Reads MKAI's tables. Difficulty 0..9 picks the column. */
	void Setup(const FOKMkoFile* MkaiScript, int32 InDifficulty, uint32 Seed);

	/** One sim frame: the buttons and directions the AI holds (OKInput masks). */
	uint32 Think(FOKScriptRuntime& Runtime, FOKFighter& Me, FOKFighter& Him, uint32 Frame);

private:
	enum class ETable : uint8 { Attacking, Blocking, MinTimeBlocking, MaxBlocksInARow, AttackAfterBlock, MaxTicksWithoutAttack,
		Throw, LowAttack, Stall, UseSpecial, Count };

	int32 Table(ETable Which) const { return Tables[int32(Which)]; }
	bool Roll(int32 Percent) { return Random.RandRange(0, 99) < Percent; }

	/** Queues masks to hold, one per frame. */
	void Plan(std::initializer_list<TPair<uint32, int32>> Steps);
	/** A special move from the fighter's command records, as presses; false if it has none it can enter. */
	bool PlanSpecial(FOKScriptRuntime& Runtime, const FOKFighter& Me, uint32 Towards, uint32 Away);
	void PlanAttack(FOKScriptRuntime& Runtime, const FOKFighter& Me, double Separation, uint32 Towards, uint32 Away, uint32 Frame);

	int32 Tables[int32(ETable::Count)] = {};
	int32 Difficulty = 4;
	FRandomStream Random;
	TArray<uint32> Planned;			// masks still to hold, first at the end
	bool bPlanCommitted = false;	// a special's input: a block mustn't break it
	uint32 LastAttack = 0;			// the frame it last attacked (drone +0xD0)
	uint32 BlockingUntil = 0;
	bool bHeWasAttacking = false;	// last frame: so it decides about each attack once, as it starts
	int32 BlocksInARow = 0;
	bool bJustBlocked = false;
};
