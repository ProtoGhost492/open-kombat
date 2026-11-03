#include "OKFightAI.h"
#include "OKInput.h"
#include "OKMkoFile.h"
#include "OKNatives.h"
#include "OKScriptObjects.h"
#include "OKScriptRuntime.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarAITrace(TEXT("ok.AI.Trace"), 0, TEXT("1 logs the computer player's decisions."));
static TAutoConsoleVariable<int32> CVarAIThrows(TEXT("ok.AI.Throws"), 1,
	TEXT("0 stops the computer throwing."));

namespace
{
	// MKAI's tables (FIGHT-RE-NOTES §8.1), in ETable's order
	const TCHAR* const TableNames[] = {
		TEXT("attacking_table"),					// per mille per frame
		TEXT("likelihood_in_blocking_table"),		// percent
		TEXT("min_time_in_blocking"),				// frames
		TEXT("max_num_blocks_in_a_row_table"),
		TEXT("likelihood_attack_after_block_table"),
		TEXT("max_ticks_without_attack_table"),
		TEXT("likelihood_to_attempt_throw_table"),
		TEXT("likelihood_to_low_attack_table"),
		TEXT("likelihood_to_stall_table"),
		TEXT("likelihood_to_use_pro_move"),
	};

	// [design] Distances along the fight line, between the fighters' roots
	constexpr double AttackRange = 200.0;		// a normal can reach
	constexpr double BlockRange = 320.0;		// an attack this close is worth blocking
	constexpr double WalkInRange = 170.0;		// walk in until this close
	constexpr double ThrowRange = 120.0;

	// OKInput masks
	constexpr uint32 Up = 1, Down = 2, Block = 0x8000, Throw = 0x2000;
	constexpr uint32 Buttons[] = { 0x40, 0x10, 0x20, 0x80 };	// FP BP FK BK

	/** A command record's motion step as a mask: towards, away, up, down (OKInput::MatchMotionAt's codes); 0 for others. */
	uint32 StepMask(uint32 Code, uint32 Towards, uint32 Away)
	{
		switch (Code)
		{
		case 0x80000001: case 0x8000000D: return Towards;
		case 0x80000002: case 0x8000000E: return Away;
		case 0x80000003: return Up;
		case 0x80000004: return Down;
		default: return 0;
		}
	}

	uint32 Word(const FOKScriptRuntime& Runtime, uint32 Address)
	{
		uint32 Value = 0;
		Runtime.LoadAnyWord(Address, Value);
		return Value;
	}
}

void FOKFightAI::Setup(const FOKMkoFile* MkaiScript, int32 InDifficulty, uint32 Seed)
{
	Difficulty = FMath::Clamp(InDifficulty, 0, 9);
	Random.Initialize(int32(Seed ^ 0xA1u));
	static_assert(UE_ARRAY_COUNT(TableNames) == int32(ETable::Count), "a name per table");
	const TConstArrayView<uint8> Pool = MkaiScript ? MkaiScript->GetPool() : TConstArrayView<uint8>();
	for (int32 i = 0; i < int32(ETable::Count); ++i)
	{
		const uint32 Hash = FOKMkoFile::Fnv1(TableNames[i]);
		const FOKMkoGlobal* Global = MkaiScript ? MkaiScript->GetGlobals().FindByPredicate([Hash](const FOKMkoGlobal& G) { return G.NameHash == Hash; }) : nullptr;
		// Each table has 10 ints, one per difficulty (FUN_00458740 checks the count)
		if (Global && Global->Size == 40 && int64(Global->PoolOffset) + 40 <= Pool.Num())
		{
			FMemory::Memcpy(&Tables[i], Pool.GetData() + Global->PoolOffset + Difficulty * 4, 4);
		}
		else
		{
			UE_LOG(LogOKVM, Warning, TEXT("AI: no %s in MKAI"), TableNames[i]);
		}
	}
}

void FOKFightAI::Plan(std::initializer_list<TPair<uint32, int32>> Steps)
{
	Planned.Reset();
	for (const TPair<uint32, int32>& Step : Steps)
	{
		for (int32 i = 0; i < Step.Value; ++i)
		{
			Planned.Insert(Step.Key, 0);
		}
	}
	bPlanCommitted = false;
}

bool FOKFightAI::PlanSpecial(FOKScriptRuntime& Runtime, const FOKFighter& Me, uint32 Towards, uint32 Away)
{
	// The records a plain special's matcher would take (OKNatives' MatchSpecial): enabled, type 0, one button, a motion
	TArray<TPair<TArray<uint32>, uint32>> Choices;
	const uint32 Count = FMath::Min(Word(Runtime, Me.Handle + FOKFighter::CommandCount), uint32(FOKFighter::MaxCommandList));
	for (uint32 i = 0; i < Count; ++i)
	{
		const uint32 Record = Word(Runtime, Me.Handle + FOKFighter::CommandList + i * 4);
		const uint32 Button = OKInput::CodeToMask(Word(Runtime, Record + OKCommand::Button));
		const uint32 Code = Word(Runtime, Record + OKCommand::Button);
		const uint32 Sequence = Word(Runtime, Record + OKCommand::Sequence);
		if (!Word(Runtime, Record + OKCommand::Enabled) || Word(Runtime, Record + OKCommand::Type) != 0 || Sequence == 0
			|| Word(Runtime, Record + 4) != 1 || Word(Runtime, Record + 8) != 1 || Word(Runtime, Record + 0x60) != 0
			|| Code < 0x8000000F || Code > 0x80000012 || Button == 0)
		{
			continue;
		}
		TArray<uint32> Steps;
		bool bUsable = true;
		for (uint32 Step = 0; Step < 10; ++Step)
		{
			const uint32 StepCode = Word(Runtime, Sequence + Step * 4);
			if (StepCode == 0xFFFFFFFFu)
			{
				break;
			}
			const uint32 Mask = StepMask(StepCode, Towards, Away);
			bUsable &= Mask != 0;
			Steps.Add(Mask);
		}
		if (bUsable && Steps.Num() > 0)
		{
			Choices.Emplace(MoveTemp(Steps), Button);
		}
	}
	if (Choices.Num() == 0)
	{
		return false;
	}
	const TPair<TArray<uint32>, uint32>& Choice = Choices[Random.RandRange(0, Choices.Num() - 1)];
	// Each step held 2 frames with a frame between, then the button with the last step held
	Planned.Reset();
	auto Hold = [this](uint32 Mask, int32 Frames) { for (int32 i = 0; i < Frames; ++i) { Planned.Insert(Mask, 0); } };
	for (const uint32 Mask : Choice.Key)
	{
		Hold(Mask, 2);
		Hold(0, 1);
	}
	Hold(Choice.Key.Last() | Choice.Value, 2);
	Hold(0, 10);
	bPlanCommitted = true;
	if (CVarAITrace.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogOKVM, Log, TEXT("AI: special, %d steps, button %X"), Choice.Key.Num(), Choice.Value);
	}
	return true;
}

void FOKFightAI::PlanAttack(FOKScriptRuntime& Runtime, const FOKFighter& Me, double Separation, uint32 Towards, uint32 Away, uint32 Frame)
{
	LastAttack = Frame;
	const bool bTrace = CVarAITrace.GetValueOnGameThread() != 0;
	if (CVarAIThrows.GetValueOnGameThread() != 0 && Separation < ThrowRange && Roll(Table(ETable::Throw)))
	{
		Plan({ { Throw, 2 }, { 0, 12 } });
		if (bTrace) { UE_LOG(LogOKVM, Log, TEXT("AI: throw")); }
		return;
	}
	if (Roll(Table(ETable::UseSpecial)) && PlanSpecial(Runtime, Me, Towards, Away))
	{
		return;
	}
	const uint32 Button = Buttons[Random.RandRange(0, UE_ARRAY_COUNT(Buttons) - 1)];
	const uint32 Low = Roll(Table(ETable::LowAttack)) ? Down : 0;
	Plan({ { Low | Button, 2 }, { Low, 8 } });
	if (bTrace) { UE_LOG(LogOKVM, Log, TEXT("AI: attack %X%s"), Button, Low ? TEXT(" low") : TEXT("")); }
}

uint32 FOKFightAI::Think(FOKScriptRuntime& Runtime, FOKFighter& Me, FOKFighter& Him, uint32 Frame)
{
	if (!Me.Body || !Him.Body || Me.Health <= 0.0f || Him.Health <= 0.0f)
	{
		Planned.Reset();
		return 0;
	}
	const uint32 Towards = Me.bFacingRight ? 8 : 4;
	const uint32 Away = Me.bFacingRight ? 4 : 8;
	const double Separation = FMath::Abs(FVector::DotProduct(Him.Body->GetWorldLocation() - Me.Body->GetWorldLocation(), Me.LineDirection));
	const bool bTrace = CVarAITrace.GetValueOnGameThread() != 0;

	// Blocking (FUN_0088BAD0): only against an attack that's live, and not a throw or an unblockable (hit classes 4, 7, 8)
	const bool bHeAttacks = (Word(Runtime, Him.Handle + FOKFighter::Flags) & 2) != 0;
	const uint32 HitClass = Word(Runtime, Him.Handle + FOKFighter::LiveAttack + OKAttack::HitClass);
	if (bHeAttacks && !bPlanCommitted && Separation < BlockRange && HitClass != 4 && HitClass != 7 && HitClass != 8)
	{
		// One decision per attack, as it starts: MKAI rolls likelihood_in_blocking, up to max_num_blocks_in_a_row in a row
		if (!bHeWasAttacking)
		{
			if (BlocksInARow < Table(ETable::MaxBlocksInARow) && Roll(Table(ETable::Blocking)))
			{
				++BlocksInARow;
				BlockingUntil = Frame + uint32(FMath::Max(Table(ETable::MinTimeBlocking), 12));
				Planned.Reset();
				if (bTrace) { UE_LOG(LogOKVM, Log, TEXT("AI: block")); }
			}
			else
			{
				BlocksInARow = 0;
			}
		}
	}
	bHeWasAttacking = bHeAttacks;
	if (Frame < BlockingUntil || (bHeAttacks && BlockingUntil != 0 && Frame < BlockingUntil + 30))
	{
		bJustBlocked = true;
		return Block;
	}
	if (bJustBlocked)
	{
		// After a block, a counter (likelihood_attack_after_block)
		bJustBlocked = false;
		BlockingUntil = 0;
		if (Separation < AttackRange && Roll(Table(ETable::AttackAfterBlock)))
		{
			PlanAttack(Runtime, Me, Separation, Towards, Away, Frame);
		}
	}

	if (Planned.Num() > 0)
	{
		return Planned.Pop(EAllowShrinking::No);
	}
	bPlanCommitted = false;
	if (!OKCanAct(Runtime, Me))
	{
		return 0;
	}

	// Attacking (FUN_0088DFF0): attacking_table per mille a frame, doubled past max_ticks_without_attack, x8 past three times it
	int32 Chance = Table(ETable::Attacking);
	const uint32 Waited = Frame - LastAttack;
	const uint32 Patience = uint32(FMath::Max(1, Table(ETable::MaxTicksWithoutAttack)));
	Chance *= Waited > 3 * Patience ? 8 : (Waited > Patience ? 2 : 1);
	if (Separation < AttackRange && Random.RandRange(0, 999) < Chance)
	{
		PlanAttack(Runtime, Me, Separation, Towards, Away, Frame);
	}
	else if (Separation >= AttackRange && Random.RandRange(0, 999) < Chance / 4 && Roll(Table(ETable::UseSpecial)) && PlanSpecial(Runtime, Me, Towards, Away))
	{
		LastAttack = Frame;		// from range: a projectile, if it has one
	}
	else if (Roll(Table(ETable::Stall)) && Random.RandRange(0, 99) < 3)
	{
		Plan({ { 0, 30 } });
		if (bTrace) { UE_LOG(LogOKVM, Log, TEXT("AI: stall")); }
	}
	else if (Separation > WalkInRange)
	{
		Plan({ { Towards, 10 } });
	}
	else if (Random.RandRange(0, 99) < 2)
	{
		Plan({ { Away, 12 } });
	}
	return Planned.Num() > 0 ? Planned.Pop(EAllowShrinking::No) : 0;
}
