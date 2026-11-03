#include "OKScriptObjects.h"
#include "OKMkoFile.h"
#include "OKScriptRuntime.h"

FString FOKFighter::DescribeAttack(FOKScriptRuntime& Runtime, uint32 Which) const
{
	const uint8* Attack = Runtime.HeapBytes((Handle & 0x00FFFFFF) + Which, OKAttack::Bytes);
	if (!Attack)
	{
		return TEXT("(no memory)");
	}
	auto Word = [Attack](uint32 Offset) { uint32 Value; FMemory::Memcpy(&Value, Attack + Offset, 4); return Value; };
	auto Float = [Attack](uint32 Offset) { float Value; FMemory::Memcpy(&Value, Attack + Offset, 4); return Value; };

	TArray<FString> Lines;
	if (Word(OKAttack::HasBlockA))
	{
		const uint32 A = OKAttack::BlockA;
		Lines.Add(FString::Printf(TEXT("block A: animation import %u, %d, rate %g, %g, %g, %d"),
			Word(A), int32(Word(A + 4)), Float(A + 8), Float(A + 12), Float(A + 16), int32(Word(A + 20))));
	}
	if (Word(OKAttack::HasBlockB))
	{
		const uint32 B = OKAttack::BlockB;
		Lines.Add(FString::Printf(TEXT("block B: %08X %08X %08X %08X"), Word(B), Word(B + 4), Word(B + 8), Word(B + 12)));
	}
	if (Word(OKAttack::HasHitbox))
	{
		const uint32 H = OKAttack::Hitbox;
		Lines.Add(FString::Printf(TEXT("hitbox:frames %g-%g, limb %d, radius x%g, reach %g, flags %u, hit class %d"),
			Float(H), Float(H + 4), int32(Word(H + 8)), Float(H + 12), Float(H + 16), Word(H + 20), int32(Word(OKAttack::HitClass))));
	}
	if (Word(OKAttack::HasReactions))
	{
		const uint32 ScriptValue = Word(OKAttack::ReactionScript);
		const FOKScriptInstance* Owner = ((ScriptValue >> 24) == FOKScriptRuntime::InstanceTag)
			? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		if (Owner)
		{
			Lines.Add(FString::Printf(TEXT("reactions, standing %s, airborne %s, damage %g, chip %g, flags %08X, hit-stun override %g"),
				*Owner->File->DescribeFunctionRef(Word(OKAttack::StandingReaction)),
				*Owner->File->DescribeFunctionRef(Word(OKAttack::AirborneReaction)),
				Float(OKAttack::Damage), Float(OKAttack::ChipDamage), Word(OKAttack::AttackFlags), Float(OKAttack::HitStunOverride)));
		}
	}
	if (Word(OKAttack::HasFoley))
	{
		const int32 Count = FMath::Min(int32(Word(OKAttack::FoleyCount)), OKAttack::MaxFoley);
		for (int32 i = 0; i < Count; ++i)
		{
			Lines.Add(FString::Printf(TEXT("foley: sound %d at frame %g"),
				int32(Word(OKAttack::FoleySounds + 4 * i)), Float(OKAttack::FoleyFrames + 4 * i)));
		}
	}
	return FString::Join(Lines, TEXT("\n"));
}