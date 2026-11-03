#include "OKCollision.h"
#include "OKMkoFile.h"

namespace
{
	struct FBoneId
	{
		int32 Id;
		const TCHAR* Name;
	};

	// MKKE's standard bone ids, as its startup code registers them (the table at 0xE39588, filled by the
	// initialiser around 0xC2D4E0). Collision tables and limb lists name bones by these ids.
	const FBoneId BoneIds[] = {
		{ 1, TEXT("Hips") }, { 2, TEXT("Spine") }, { 3, TEXT("Spine1") }, { 4, TEXT("Spine2") },
		{ 5, TEXT("Neck") }, { 6, TEXT("Head") }, { 7, TEXT("LeftShoulder") }, { 8, TEXT("LeftArm") },
		{ 9, TEXT("LeftArmRoll") }, { 10, TEXT("LeftForeArm") }, { 11, TEXT("LeftForeArmRoll") }, { 12, TEXT("LeftHand") },
		{ 13, TEXT("LeftHandIndex1") }, { 14, TEXT("LeftHandIndex2") }, { 15, TEXT("LeftHandIndex3") }, { 16, TEXT("LeftHandIndex4") },
		{ 17, TEXT("LeftHandMiddle1") }, { 18, TEXT("LeftHandMiddle2") }, { 19, TEXT("LeftHandMiddle3") }, { 20, TEXT("LeftHandMiddle4") },
		{ 21, TEXT("LeftHandRing1") }, { 22, TEXT("LeftHandRing2") }, { 23, TEXT("LeftHandRing3") }, { 24, TEXT("LeftHandRing4") },
		{ 25, TEXT("LeftHandPinky1") }, { 26, TEXT("LeftHandPinky2") }, { 27, TEXT("LeftHandPinky3") }, { 28, TEXT("LeftHandPinky4") },
		{ 29, TEXT("LeftHandThumb1") }, { 30, TEXT("LeftHandThumb2") }, { 31, TEXT("LeftHandThumb3") }, { 32, TEXT("LeftHandThumb4") },
		{ 33, TEXT("RightShoulder") }, { 34, TEXT("RightArm") }, { 35, TEXT("RightArmRoll") }, { 36, TEXT("RightForeArm") },
		{ 37, TEXT("RightForeArmRoll") }, { 38, TEXT("RightHand") }, { 39, TEXT("RightHandIndex1") }, { 40, TEXT("RightHandIndex2") },
		{ 41, TEXT("RightHandIndex3") }, { 42, TEXT("RightHandIndex4") }, { 43, TEXT("RightHandMiddle1") }, { 44, TEXT("RightHandMiddle2") },
		{ 45, TEXT("RightHandMiddle3") }, { 46, TEXT("RightHandMiddle4") }, { 47, TEXT("RightHandRing1") }, { 48, TEXT("RightHandRing2") },
		{ 49, TEXT("RightHandRing3") }, { 50, TEXT("RightHandRing4") }, { 51, TEXT("RightHandPinky1") }, { 52, TEXT("RightHandPinky2") },
		{ 53, TEXT("RightHandPinky3") }, { 54, TEXT("RightHandPinky4") }, { 55, TEXT("RightHandThumb1") }, { 56, TEXT("RightHandThumb2") },
		{ 57, TEXT("RightHandThumb3") }, { 58, TEXT("RightHandThumb4") }, { 59, TEXT("LowerLeftShoulder") }, { 60, TEXT("LowerLeftArm") },
		{ 61, TEXT("LowerLeftArmRoll") }, { 62, TEXT("LowerLeftForeArm") }, { 63, TEXT("LowerLeftForeArmRoll") }, { 64, TEXT("LowerLeftHand") },
		{ 65, TEXT("LowerLeftHandIndex1") }, { 66, TEXT("LowerLeftHandIndex2") }, { 67, TEXT("LowerLeftHandIndex3") }, { 68, TEXT("LowerLeftHandIndex4") },
		{ 69, TEXT("LowerLeftHandMiddle1") }, { 70, TEXT("LowerLeftHandMiddle2") }, { 72, TEXT("LowerLeftHandMiddle3") }, { 73, TEXT("LowerLeftHandRing1") },
		{ 74, TEXT("LowerLeftHandRing2") }, { 75, TEXT("LowerLeftHandRing3") }, { 76, TEXT("LowerLeftHandRing4") }, { 77, TEXT("LowerLeftHandPinky1") },
		{ 78, TEXT("LowerLeftHandPinky2") }, { 79, TEXT("LowerLeftHandPinky3") }, { 80, TEXT("LowerLeftHandPinky4") }, { 81, TEXT("LowerLeftHandThumb1") },
		{ 82, TEXT("LowerLeftHandThumb2") }, { 83, TEXT("LowerLeftHandThumb3") }, { 84, TEXT("LowerLeftHandThumb4") }, { 85, TEXT("LowerRightShoulder") },
		{ 86, TEXT("LowerRightArm") }, { 87, TEXT("LowerRightArmRoll") }, { 88, TEXT("LowerRightForeArm") }, { 89, TEXT("LowerRightForeArmRoll") },
		{ 90, TEXT("LowerRightHand") }, { 91, TEXT("LowerRightHandIndex1") }, { 92, TEXT("LowerRightHandIndex2") }, { 93, TEXT("LowerRightHandIndex3") },
		{ 94, TEXT("LowerRightHandIndex4") }, { 95, TEXT("LowerRightHandMiddle1") }, { 96, TEXT("LowerRightHandMiddle2") }, { 97, TEXT("LowerRightHandMiddle3") },
		{ 98, TEXT("LowerRightHandMiddle4") }, { 99, TEXT("LowerRightHandRing1") }, { 100, TEXT("LowerRightHandRing2") }, { 101, TEXT("LowerRightHandRing3") },
		{ 102, TEXT("LowerRightHandRing4") }, { 103, TEXT("LowerRightHandPinky1") }, { 104, TEXT("LowerRightHandPinky2") }, { 105, TEXT("LowerRightHandPinky3") },
		{ 106, TEXT("LowerRightHandPinky4") }, { 107, TEXT("LowerRightHandThumb1") }, { 108, TEXT("LowerRightHandThumb2") }, { 110, TEXT("LowerRightHandThumb3") },
		{ 111, TEXT("LeftUpLeg") }, { 112, TEXT("LeftUpLegRoll") }, { 113, TEXT("LeftLeg") }, { 114, TEXT("LeftLegRoll") },
		{ 115, TEXT("LeftFoot") }, { 116, TEXT("LeftToeBase") }, { 117, TEXT("RightUpLeg") }, { 118, TEXT("RightUpLegRoll") },
		{ 119, TEXT("RightLeg") }, { 120, TEXT("RightLegRoll") }, { 121, TEXT("RightFoot") }, { 122, TEXT("RightToeBase") },
		{ 123, TEXT("position_locator") }, { 124, TEXT("Root") },
	};

	// The limbs an attack can scan with (block C's limb; the lists at 0xDF0B40, zero-terminated there).
	// -1 starts another chain. 1-3 arms, 4-6 legs, 7-14 longer chains and combinations, 15 body,
	// 16 head, 17-21 four-armed characters' lower arms.
	const int32 Limb1[] = { 12, 11, 10 };
	const int32 Limb2[] = { 38, 37, 36 };
	const int32 Limb3[] = { 12, 11, 10, -1, 38, 37, 36 };
	const int32 Limb4[] = { 115, 113 };
	const int32 Limb5[] = { 121, 119 };
	const int32 Limb6[] = { 121, 119, -1, 115, 113 };
	const int32 Limb7[] = { 12, 11, 10, 9, 8 };
	const int32 Limb8[] = { 38, 37, 36, 35, 34 };
	const int32 Limb9[] = { 115, 115, 113, 1 };
	const int32 Limb10[] = { 121, 121, 119, 1 };
	const int32 Limb11[] = { 38, 37, 36, 35, 34, -1, 12, 11, 10, 9, 8 };
	const int32 Limb12[] = { 38, 37, 36, 35, 34, -1, 121, 121, 119, 1 };
	const int32 Limb13[] = { 12, 11, 10, 9, 8, -1, 115, 115, 113, 1 };
	const int32 Limb14[] = { 115, 115, 113, 1, -1, 121, 121, 119, 1 };
	const int32 Limb15[] = { 4, 2, 1 };
	const int32 Limb16[] = { 5, 6 };
	const int32 Limb17[] = { 64, 63, 62 };
	const int32 Limb18[] = { 90, 89, 88 };
	const int32 Limb19[] = { 12, 11, 10, 9, 8, -1, 64, 63, 62, 61, 60 };
	const int32 Limb20[] = { 38, 37, 36, 35, 34, -1, 90, 89, 88, 87, 86 };
	const int32 Limb21[] = { 12, 11, 10, 9, 8, -1, 64, 63, 62, 61, 60, -1, 38, 37, 36, 35, 34, -1, 90, 89, 88, 87, 86 };
	const TConstArrayView<int32> Limbs[] = { TConstArrayView<int32>(), MakeArrayView(Limb1), MakeArrayView(Limb2), MakeArrayView(Limb3), MakeArrayView(Limb4), MakeArrayView(Limb5), MakeArrayView(Limb6), MakeArrayView(Limb7), MakeArrayView(Limb8), MakeArrayView(Limb9), MakeArrayView(Limb10), MakeArrayView(Limb11), MakeArrayView(Limb12), MakeArrayView(Limb13), MakeArrayView(Limb14), MakeArrayView(Limb15), MakeArrayView(Limb16), MakeArrayView(Limb17), MakeArrayView(Limb18), MakeArrayView(Limb19), MakeArrayView(Limb20), MakeArrayView(Limb21) };
}

const TCHAR* OKCollision::BoneName(int32 BoneId)
{
	for (const FBoneId& Entry : BoneIds)
	{
		if (Entry.Id == BoneId)
		{
			return Entry.Name;
		}
	}
	return nullptr;
}

TConstArrayView<int32> OKCollision::LimbBones(int32 Limb)
{
	return (Limb > 0 && Limb < int32(UE_ARRAY_COUNT(Limbs))) ? Limbs[Limb] : TConstArrayView<int32>();
}

bool OKCollision::ReadTable(const FOKMkoFile& Script, TArray<FOKCollisionRow>& OutRows)
{
	OutRows.Reset();
	const TConstArrayView<uint8> Pool = Script.GetPool();
	auto Word = [&Pool](uint32 Offset) { uint32 Value = 0; FMemory::Memcpy(&Value, Pool.GetData() + Offset, 4); return Value; };
	auto Float = [&Pool](uint32 Offset) { float Value = 0.0f; FMemory::Memcpy(&Value, Pool.GetData() + Offset, 4); return Value; };

	for (const FOKMkoGlobal& Global : Script.GetGlobals())
	{
		if (Global.ElementSize != 20 || Global.Size < 100 || Global.Size % 20 != 0 || int64(Global.PoolOffset) + Global.Size > Pool.Num())
		{
			continue;
		}
		// Every row's side must be 1, 2 or 3, and a pointer global must refer to the array.
		bool bRows = true;
		for (uint32 Row = 0; Row < Global.Size / 20 && bRows; ++Row)
		{
			const uint32 Side = Word(Global.PoolOffset + Row * 20 + 4);
			bRows = Side >= 1 && Side <= 3;
		}
		const bool bPointedTo = Script.GetFixups().ContainsByPredicate([&](const FOKMkoFixup& Fix) { return Fix.Kind == 2 && Fix.Value == Global.PoolOffset; });
		if (!bRows || !bPointedTo)
		{
			continue;
		}
		for (uint32 Row = 0; Row < Global.Size / 20; ++Row)
		{
			const uint32 At = Global.PoolOffset + Row * 20;
			FOKCollisionRow& Out = OutRows.AddDefaulted_GetRef();
			Out.BoneId = int32(Word(At));
			Out.Side = int32(Word(At + 4));
			Out.SecondaryRadius = Float(At + 8);
			Out.AttackRadius = Float(At + 12);
			Out.HurtRadius = Float(At + 16);
		}
		return true;
	}
	return false;
}

bool OKCollision::Touch(const FOKCollisionSphere& A, const FOKCollisionSphere& B, const FVector& Axis)
{
	const FVector Delta = A.Center - B.Center;
	const double Along = FVector::DotProduct(Delta, Axis);
	const double Height = Delta.Z;
	const double Reach = double(A.Radius) + B.Radius;
	return Along * Along + Height * Height <= Reach * Reach;
}
