#pragma once

#include "CoreMinimal.h"

class FOKMkoFile;

/** One row of a character's collision table, a global array in its script (FIGHT-RE-NOTES §7.3). Radii are in cm. */
struct FOKCollisionRow
{
	int32 BoneId = 0;				// one of MKKE's standard bone ids (OKCollision::BoneName)
	int32 Side = 0;					// 1 left, 2 right, 3 centre; a mirrored attack uses the neighbouring row of the other side
	float SecondaryRadius = 0.0f;	// a second sphere list (PLAYER_COL_INFO +0x188), probably for pushing [inf]
	float AttackRadius = 0.0f;		// times the attack's radius scale, for attack spheres
	float HurtRadius = 0.0f;		// the hurtbox; 0 = this bone can't be hit
};

/** A collision sphere in world space. */
struct FOKCollisionSphere
{
	int32 BoneId = 0;
	FVector Center = FVector::ZeroVector;
	float Radius = 0.0f;
};

/** Where the last attack sphere touched a hurtbox (MKKE keeps this at 0xDF0BA4..0xDF0BD0). */
struct FOKContact
{
	int32 AttackBone = 0;
	int32 HurtBone = 0;
	FVector Where = FVector::ZeroVector;		// the attack sphere's centre
	int64 SimFrame = 0;
};

namespace OKCollision
{
	/** The most attack spheres one scan builds (PLAYER_COL_INFO holds 36). */
	constexpr int32 MaxAttackSpheres = 36;

	/** The bone name of one of MKKE's standard bone ids (the table its startup code fills at 0xE39588), or null. */
	OPENKOMBAT_API const TCHAR* BoneName(int32 BoneId);

	/** The bone ids an attack limb uses (block C's limb, the lists at 0xDF0B40). -1 separates chains; empty for an unknown limb. */
	OPENKOMBAT_API TConstArrayView<int32> LimbBones(int32 Limb);

	/**
	 * The character's collision table: the global array of 20-byte rows {bone, side, radii} in its script that
	 * a pointer global refers to (fixup kind 2). [inf] How MKKE hands it to the fighter (player info +0x38)
	 * isn't traced; every character script checked has exactly one such array.
	 */
	OPENKOMBAT_API bool ReadTable(const FOKMkoFile& Script, TArray<FOKCollisionRow>& OutRows);

	/**
	 * MKKE's hit test is 2D (FUN_008B4320, FUN_008B0CD0): position along Axis, the line between the fighters, and
	 * height. Two spheres touch when that distance is within the sum of their radii. Depth across the screen
	 * plays no part.
	 */
	OPENKOMBAT_API bool Touch(const FOKCollisionSphere& A, const FOKCollisionSphere& B, const FVector& Axis);
}
