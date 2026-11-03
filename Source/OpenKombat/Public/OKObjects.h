#pragma once

#include "CoreMinimal.h"
#include "OKScriptObjects.h"

class AOKParticleActor;
class FOKScriptRuntime;
class FOKScriptThread;
class FOKMkoFile;

/** MKObj's fields that scripts and natives reach by offset (W1). */
namespace OKObject
{
	constexpr uint32 Gravity = 0x68;			// 1: the acceleration adds the gravity at +0x94 (MKObj::vf20 / vf24)
	constexpr uint32 Velocity = 0x6C;
	constexpr uint32 Acceleration = 0x78;
	constexpr uint32 GravityVector = 0x94;
	constexpr uint32 TerminalFlag = 0xA0;		// with gravity, the vertical speed stops at +0xA4
	constexpr uint32 Terminal = 0xA4;
	constexpr uint32 DieFlag = 0xA8;			// kill_obj_in_ticks (FUN_0042F640): deleted at frame +0xAC
	constexpr uint32 DieFrame = 0xAC;
}

/** MKCollisionObj's fields (0x1DC bytes, FUN_0089F180). */
namespace OKCollisionObj
{
	constexpr int32 Bytes = 0x1DC;
	constexpr uint32 Radius = 0xF8;				// col_sphere (121.21); a capsule's other sizes at +0xFC / +0x100
	constexpr uint32 Touched = 0x104;			// 1 once it touched; it tests no more until the cleanup clears it
	constexpr uint32 FromAir = 0x108;			// the owner was airborne (+0x2EC bit 0x80000), given to the hit as attack word 0
	constexpr uint32 DeleteAfterCallback = 0x120;	// 121.31; 1 by default: the object goes once a callback ran
	constexpr uint32 PlayerInfo = 0x170;		// the owner's player record; callbacks read it to find the owner
	constexpr uint32 Owner = 0x174;
	constexpr uint32 PropsFunction = 0x190;		// the function that sets the attack up (create_collision_obj's third argument)
	constexpr uint32 Shape = 0x194;				// 0 sphere, 1 capsule, 2 box, 3 swept
}

/** An MK object: projectiles, props and effects (MKObj). Its fields live in the runtime's heap. */
class OPENKOMBAT_API FOKMkObject : public FOKScriptObject
{
public:
	virtual FOKMkObject* AsMkObject() override { return this; }

	uint32 Handle = 0;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

/** MKCollisionObj (create_collision_obj, 137.188): a sphere that hits the owner's opponent like an attack. */
class OPENKOMBAT_API FOKCollisionObject : public FOKMkObject
{
public:
	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::CollisionObj; }

	struct FCallback
	{
		uint32 Script = 0;		// a script instance value
		uint32 Function = 0;	// as the script passed it (1-based, or an import)
	};

	FOKFighter* Owner = nullptr;
	FCallback Props;			// sets the attack up when it touches (r_spear_props)
	FCallback OnHit;			// col_hit_callback (121.32, +0x130)
	FCallback OnBlock;			// col_block_callback (121.33, +0x140)
	FCallback OnWall;			// col_wall_callback (121.36, FUN_00895FD0): kept; nothing hits a wall until W5
	uint32 Context = 0;			// its CollisionContext (+0x114)
};

/**
 * MKParticle (0x104 bytes, FUN_0043F310): a particle system playing in the world (W2). fx_attach puts it on a fighter's
 * bone, where it follows the bone until fx_start lets it go; obj_set_pos_vec moves it, against the bone while it's on one.
 */
class OPENKOMBAT_API FOKParticleObject : public FOKMkObject
{
public:
	static constexpr int32 Bytes = 0x104;

	virtual ~FOKParticleObject() override;
	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::Particle; }

	TWeakObjectPtr<AOKParticleActor> Actor;
	FOKFighter* AttachedTo = nullptr;
	FString Bone;
	FVector Offset = FVector::ZeroVector;	// from the bone, in the fighter's frame (x along its facing, z up)
	float DrawScale = 1.0f;					// fx_set_scale (52.41): the actor's DrawScale3D
	float ComponentScale = 1.0f;			// fx_scale (82.19): the component's +0x2C4, times the owner's scale
	bool bDeleteWhenDone = false;			// +0x100: fx_create's second argument; fx_activate (82.20) sets it
};

namespace OKObjects
{
	/** fx_create (FUN_0043F310): a particle object playing the named system; 0 if there's no such system. */
	OPENKOMBAT_API uint32 CreateParticle(FOKScriptRuntime& Runtime, const FString& SystemName, bool bDeleteWhenDone);

	/** Moves a particle object's actor to where the object is: on its bone, or where it was put. */
	OPENKOMBAT_API void PlaceParticle(FOKParticleObject& Object);

	/** create_collision_obj (FUN_008A4C70): a collision object at its owner, with the props function to run on contact. */
	OPENKOMBAT_API uint32 CreateCollisionObject(FOKScriptRuntime& Runtime, FOKFighter& Owner, uint32 Script, uint32 Function);

	/** The MK object behind a handle, or null. */
	OPENKOMBAT_API FOKMkObject* Find(FOKScriptRuntime& Runtime, uint32 Handle);

	/** One frame of every MK object, after the fighters moved: lifetimes, motion, then collision objects' contact tests. */
	OPENKOMBAT_API void Tick(FOKScriptRuntime& Runtime);

	/**
	 * col_reschedule_cleanup (121.13, FUN_008ADF90): the contact a collision object made last frame resolved as a hit
	 * with the attack its props function sets up, then the hit or block callback.
	 */
	OPENKOMBAT_API void Cleanup(FOKScriptRuntime& Runtime, FOKCollisionObject& Object);

	/**
	 * FUN_008AD6C0 (scripted_hit, 137.89; and a collision object's contact): the props function runs on the owner and
	 * sets its pending attack up; that attack, with Word0 as its first word, hits the victim through Context (made when
	 * it is 0), and the owner's live attack comes back after. Returns the context.
	 */
	OPENKOMBAT_API uint32 ScriptedHit(FOKScriptRuntime& Runtime, FOKFighter& Owner, FOKFighter& Victim, const FOKMkoFile& Script,
		int32 Function, uint32 Context, uint32 Word0);
}
