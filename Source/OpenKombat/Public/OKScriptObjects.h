#pragma once

#include "CoreMinimal.h"
#include "OKCollision.h"

class FOKMkoFile;
class FOKFighter;
class FOKPlayer;
class FOKGameInfo;
class FOKMkObject;
class FOKScriptThread;
class FOKScriptRuntime;

/** Classes scripts can test for with is_proc (1.12). */
enum class EOKScriptClass : uint32
{
	None = 0,
	MiscPdata = 1,
	AnimEventPdata = 2,		// 92.1: what a frame event hands its process
	CollisionContext = 3,	// 79.1: one attack's contact record
	CharacterObj = 4,		// 81.1: MKCharacterObj. No object claims it yet (see ClassCharacterObj)
	CollisionObj = 5,		// 121.1: MKCollisionObj, a projectile's collision (OKObjects)
	ScreenShakePdata = 6,	// 91.1: a camera shake's parameters, attached to CameraEngine.p_screen_shaker
	Particle = 7,			// MKParticle (fx_create, 82.3): a playing particle system (OKObjects)
	FGCharacterObj = 8,		// 137.1: MKFGCharacterObj, a fighter
	SkeletalObj = 9,		// 59.1: MKSkeletalObj, which MKCharacterObj derives from
	Script = 10,			// 20.1: MKScript, a loaded script; our script instances (their values) claim it
	FatalityCinemaPdata = 11,	// 138.1: a finisher cinema's request (138.3), handed to FGFightEngineScript.p_fatality_cinema
	Cinema = 12,			// 39.1: MKCinema, a cinema scene (cinema_prepare)
	ArmorPdata = 13,		// 94.1: a fighter's armour (character_set_attack_armor), attached to its process 0x6B
};

/** Anything a script can refer to (@my_obj, @his_obj, ...). */
class OPENKOMBAT_API FOKScriptObject
{
public:
	virtual ~FOKScriptObject() = default;
	
	/** Type check without C++ RTTI: Unreal builds don't use dynamic_cast on plain classes.*/
	virtual FOKFighter* AsFighter() { return nullptr; }
	virtual FOKPlayer* AsPlayer() { return nullptr; }
	virtual FOKGameInfo* AsGameInfo() { return nullptr; }
	virtual FOKMkObject* AsMkObject() { return nullptr; }

	virtual FOKScriptThread* AsThread() { return nullptr; }
	virtual bool IsA(EOKScriptClass Class) const { return false; }
};

/**
* MKKE's attack struct (0x21C bytes, FIGHT-RE-NOTES §7.1). A fighter has two: the pending attack
* the setup natives fill, and the live attack launch_attack copies it into. Scripts read the live
* one by offset, so both sit in the fighter's memory with MKKE's layout.
*/
namespace OKAttack
{
	constexpr int32 Bytes = 0x21C;

	// "This block is set" flags
	constexpr uint32 HasBlockA = 0x04;
	constexpr uint32 HasBlockB = 0x08;
	constexpr uint32 HasHitbox = 0x0C;
	constexpr uint32 HasReactions = 0x10;
	constexpr uint32 HasFoley = 0x24;

	constexpr uint32 BlockA = 0x44;				// 6 words; [0] animation (asset import), [2] rate, [3] root-motion scale
	constexpr uint32 HitClass = 0x5C;
	constexpr uint32 BlockB = 0x90;				// 4 words, not named yet
	constexpr uint32 Hitbox = 0xA0;				// first frame, last frame, limb, radius scale, reach, flags

	constexpr uint32 ReactionScript = 0xB8;		// the script instance the reaction functions belong to
	constexpr uint32 StandingReaction = 0xBC;	// then three more directions, which 6.1 clears
	constexpr uint32 Damage = 0xCC;				// a fraction of the health bar
	constexpr uint32 ChipDamage = 0xD0;			// damage when blocked
	constexpr uint32 AirborneReaction = 0xD4;
	constexpr uint32 AirborneScript = 0xDC;		// the airborne reaction's script: a handle at +0xD8 {?, object +0xDC, serial +0xE0}
	constexpr uint32 HitStunOverride = 0xE4;	// the victim's breakout frame; 0 = none
	constexpr uint32 AttackFlags = 0xE8;

	constexpr uint32 FoleyCount = 0x184;
	constexpr uint32 FoleySounds = 0x188;		// 10 ints
	constexpr uint32 FoleyFrames = 0x1B0;		// 10 floats
	constexpr int32 MaxFoley = 10;

	// Reset values (FUN_0044A440): everything else is 0
	constexpr uint32 DefaultHitClass = 14;
	constexpr uint32 DefaultLimb = 22;
	constexpr uint32 NoFoleySound = 21;

	// Object handles in the struct. MKKE's copy (FUN_00899370) leaves these alone.
	constexpr uint32 HandleWords[] = { 0x60, 0x70, 0x80, 0xD8, 0xEC, 0x108, 0x118, 0x130, 0x140, 0x1DC, 0x1F0, 0x204 };
}

/**
 * What a fighter's body (its actor on screen) does for the scripts. The script side stays plain C++;
 * the actor implements this and attaches itself to its FOKFighter.
 */
class IOKFighterBody
{
public:
	virtual ~IOKFighterBody() = default;

	/** Plays a sequence from StartFrame at Rate, crossfading from the current pose over BlendFrames (60ths of a second). */
	virtual bool PlayAnimation(const FString& SequenceName, float StartFrame, float Rate, float BlendFrames, bool bLoop) = 0;
	virtual float GetAnimFrame() const = 0;
	virtual float GetAnimRate() const = 0;
	virtual void SetAnimRate(float Rate) = 0;
	/** Whether the playing sequence loops; off, it holds its last frame. */
	virtual void SetAnimLooping(bool bLooping) = 0;
	virtual bool IsAnimLooping() const = 0;
	/** Shows or hides the whole fighter (object virtual +0x34). */
	virtual void SetBodyHidden(bool bHide) = 0;
	virtual int32 GetAnimFrameCount() const = 0;
	/** A sequence's frame count by name, without playing it; 0 if the character has no such sequence. */
	virtual int32 GetSequenceFrameCount(const FString& SequenceName) = 0;
	/** Identifies the playing sequence; frame events armed during another sequence are dropped. */
	virtual uint32 GetAnimId() const = 0;
	/** The playing sequence's name (FrontPunch01). */
	virtual FString GetAnimName() const = 0;
	/** A bone's position in the world, by name; false if the mesh has no such bone. */
	virtual bool GetBoneWorldLocation(const FString& BoneName, FVector& OutLocation) const = 0;
	virtual FVector GetWorldLocation() const = 0;
	/** Height above the floor the fighter was placed on: MKKE's root height, 0 when standing. */
	virtual float GetHeight() const = 0;
	/** The direction the fighter faces (its actor's +X). */
	virtual FVector GetFacing() const = 0;
	/** Jumps the playing sequence to a frame. */
	virtual void SetAnimFrame(float InFrame) = 0;
	virtual void SetWorldLocation(const FVector& Location) = 0;
	/** The floor's height in the world: where the fighter was placed. */
	virtual double GetFloorZ() const = 0;
	/** The root translation the animation has now, extracted from the pose (in the world's axes). */
	virtual FVector GetRootOffset() const = 0;
	/** A bone's height above the floor at a frame of the playing sequence, with its root translation (standing on the floor). */
	virtual float GetSequenceBoneHeight(const FString& BoneName, float AtFrame) const = 0;
	/** setup_for_flip (59.26, object virtual +0x68): the mesh's mirror and the playing animation's both toggle. */
	virtual void SetupForFlip() = 0;
	/** Mesh +0x758 bit 0. */
	virtual bool IsMeshMirrored() const = 0;
	/** The playing animation node's mirror (flag 0x40), as a blend's flags bit 1 starts it. */
	virtual void SetAnimNodeMirrored(bool bMirrored) = 0;
	virtual bool IsAnimNodeMirrored() const = 0;
	/**
	 * A throw's victim (F10): shows the leader's frame of the same two-player sequence from its other tracks (group 1),
	 * placed so the two fighters' roots are where the animator put them (MKAnimTrackMatcher, FUN_00490900).
	 */
	virtual void FollowThrow(const IOKFighterBody& Leader) = 0;
};

/** A fighter: MKKE's MKFGCharacterObj. Scripts read its fields by offset, so it has memory in the runtime's heap. */
class OPENKOMBAT_API FOKFighter : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0xC580;				// sizeof(MKFGCharacterObj)
	static constexpr uint32 LiveAttack = 0x78F0;
	static constexpr uint32 PendingAttack = 0x7B0C;
	static constexpr uint32 Flags = 0x2E8;				// bit 1 (value 2): attack active
	static constexpr uint32 AttackLayer = 0x110;		// launch_attack sets -1 when it picks a limb layer
	static constexpr uint32 FrameEvents = 0xAAC0;		// 50 slots of 0x88 bytes; +4 = state: 2 free, 1 added, 0 armed
	static constexpr int32 FrameEventBytes = 0x88;
	static constexpr int32 MaxFrameEvents = 50;
	static constexpr uint32 FrameEventCount = 0xC550;
	static constexpr uint32 State = 0x1D4;				// set_state (81.185); the old one goes to PreviousState
	static constexpr uint32 PreviousState = 0x2F9C;
	static constexpr uint32 MoveFlag = 0x2CC;			// set_move_flag (81.61), and the tick it was set + 10 at +0x2D0
	static constexpr uint32 Commands = 0x30B8;			// 60 special-move command slots of 0x124 bytes; +0 = enabled
	static constexpr int32 CommandBytes = 0x124;
	static constexpr int32 MaxCommands = 60;
	static constexpr uint32 PlayerInfo = 0x7638;		// the fighter's player record (@my_info)
	static constexpr uint32 Tallies = 0x2FD0;			// [inf] a block of stats floats; the blood helpers add the damage at +0x48
	static constexpr uint32 Moveset = 0x518;				// FOKMoveset; +0x7F5C with flag 0x20000000 (the second style)
	static constexpr uint32 SecondaryMoveset = 0x7F5C;
	static constexpr uint32 SecondaryMovesetFlag = 0x20000000;
	static constexpr uint32 BreakoutFrames = 0x7628;		// set_breakout_frames (81.235), and the tick it was set at +0x762C
	static constexpr uint32 CharacterInfo = 0x2A0;		// the character's definition (copied from +0x7620): FOKCharacterInfo
	static constexpr uint32 CharacterScriptHandle = 0x2A8;	// handle to its MKScript (object here, serial +4; the handle starts at +0x2A4); the moveset is that script's
	static constexpr uint32 CommandList = 0x7524;		// up to 50 command records (bind_command, 81.215)
	static constexpr uint32 CommandCount = 0x75EC;
	static constexpr int32 MaxCommandList = 50;
	static constexpr uint32 FuncSlots = 0x77D8;		// 8 slots of 0x10: {?, script, serial, function}; the input scanners
	static constexpr int32 FuncSlotBytes = 0x10;
	static constexpr int32 MaxFuncSlots = 8;
	static constexpr uint32 GroundMove = 0x304;			// the ground-move type; 0, 3 and 4 mean none (FUN_00447D50)
	static constexpr uint32 OpponentHandle = 0x763C;		// handle to the opponent: object +0x7640, serial +0x7644 (@his_obj)
	static constexpr uint32 EntryAnims = 0xC578;			// player_entry's animations: [0] standing, [1] crouching
	static constexpr uint32 Holding = 0x284;			// 1 while it holds a throw victim (start_throw; ReleaseCharacters clears it)
	static constexpr uint32 SavedCharFlags = 0x7630;	// CharFlags as they were before a throw, restored when it lets go (+0x7634 = 1 while saved)
	static constexpr uint32 CharFlags = 0x1D0;			// constrain_enable / constrain_disable (81.241 / 81.242) set and clear bits
	static constexpr uint32 Invulnerable = 0x75F8;		// set_invulnerable (81.231): the tick it lasts until
	static constexpr uint32 CharLayer = 0x76F4;			// set_char_layer (137.9): 0..3
	static constexpr uint32 ButtonSlots = 0x30B4;			// 60 slots of 0x124, by code - 0x80000000 (FOKButtonSlot)
	static constexpr int32 ButtonSlotBytes = 0x124;
	static constexpr int32 MaxButtonSlots = 60;
	static constexpr uint32 ButtonDelays = 0x3038;		// up to 10 of {code, first frame held, delay} at +0x303C (81.203)
	static constexpr uint32 AttackScript = 0x2F80;		// XferToAttackScript: handle (+0x2F84), function +0x2F8C,
	static constexpr uint32 AttackFunction = 0x2F8C;	// direction code +0x2F90, button code +0x2F94; game_p_attack reads them
	static constexpr uint32 AttackDirection = 0x2F90;
	static constexpr uint32 AttackButton = 0x2F94;
	static constexpr uint32 MoveList = 0xC560;			// the move list (button) the last attack came from
	static constexpr uint32 Active = 0x10;				// Flags: the fighter is a player's active character (FUN_008983C0)
	static constexpr uint32 NotFree = 0x2FCC;			// 1 while the fighter is busy (in the air, reeling); FightPlayers::Update keeps it
	static constexpr uint32 Life = 0x78C0;				// float, a fraction of the health bar (team_life reads it)

	virtual FOKFighter* AsFighter() override { return this; }
	virtual bool IsA(EOKScriptClass Class) const override
	{
		return Class == EOKScriptClass::FGCharacterObj || Class == EOKScriptClass::CharacterObj || Class == EOKScriptClass::SkeletalObj;
	}

	/** One of the fighter's attack structs (LiveAttack or PendingAttack) as text; empty if nothing is set. */
	FString DescribeAttack(FOKScriptRuntime& Runtime, uint32 Which) const;
	
	FString Name;
	uint32 Handle = 0;		// the address of its memory, set when it's added to a runtime
	FOKPlayer* Player = nullptr;					// its player record (@my_info)
	IOKFighterBody* Body = nullptr;					// the actor showing this fighter, if any
	const FOKMkoFile* CharacterScript = nullptr;	// e.g. CHAR_Scorpion: its asset table names the animations
	float RootMotionScale = 1.0f;					// set_root_motion_scale (59.37); applied once root motion exists

	// Collision (FIGHT-RE-NOTES §7.3): MKKE keeps these in the fighter's PLAYER_COL_INFO object.
	int32 Index = 0;								// 0 = player 1 (@my_obj), 1 = player 2 (@his_obj)
	FOKFighter* Opponent = nullptr;
	TArray<FOKCollisionRow> CollisionRows;			// from the character script's collision table
	TArray<FOKCollisionSphere> AttackSpheres;		// built by check_attack_collision this frame
	TArray<FOKCollisionSphere> PreviousAttackSpheres;	// last frame's, to fill gaps when a limb moves fast
	float CollisionForward = 0.0f;					// collision_scan_flag (81.162): attack spheres move this far along the facing
	uint32 CollisionContext = 0;					// the newest CollisionContext made for this fighter (new_collision_context)
	FOKContact LastContact;							// where this fighter's attack last touched the opponent

	float Health = 1.0f;							// a fraction of the health bar
	bool bFacingRight = true;						// the opponent is to the right on screen (fighter virtual +0x19C)
	FVector LineDirection = FVector::YAxisVector;	// the fight line, left to right on screen (FUN_008B4230)
	FVector LineStart = FVector::ZeroVector;		// its left end (the stage's _Left constrain point): positions along it count from here
	double LineLength = 0.0;						// the arena's length (DAT_00DF0C20); 0 = no arena ends
	double SeparationWindow[2] = {};				// player pushing's window along the line (DAT_00DF0C28 / +0x2C), kept on player 1
	bool bSeparationWindow = false;
	uint32 CurrentAnim = 0;							// the asset value of the animation blend_to_ani last started (59.50)
	uint32 MainThread = 0;							// the fighter's own script thread (MKKE: fighter +0x2FA4); reactions switch it

	// A throw (F10): the victim follows the attacker's animation, as MKKE's MKAnimTrackMatcher does
	FOKFighter* ThrowLeader = nullptr;				// on the follower: the fighter whose animation it's locked to
	FString ThrowSequence;							// while the leader plays this, the link holds
	uint32 ThrowFlags = 0;							// start_throw's flags: bit 0, don't negate the follower's X/Y
	uint32 ThrowCleanupScript = 0;					// the leader's victim cleanup when the throw started (live attack +0x130 / +0x13C);
	uint32 ThrowCleanupFunction = 0;				// its attack may be reset before the throw lets go
};

/**
 * A player record (@my_info, @his_info): MKKE keeps four, 0xC8C bytes each, at 0xE89D7C (FUN_0080E540). It
 * holds the side's fighters, meter and stats; a fighter points at its record from +0x7638.
 */
class OPENKOMBAT_API FOKPlayer : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0xC8C;
	static constexpr uint32 ActiveCharacter = 0x18;		// a handle {?, object, serial}: get_active_char (118.3)
	static constexpr uint32 PartnerCharacter = 0x24;	// the tag partner, the same way
	static constexpr uint32 DamageMultiplier = 0xB60;	// float: adjust_damage_multiplier (78.88)
	static constexpr uint32 Side = 0xB3C;				// player_side (118.5)
	static constexpr uint32 ComboHits = 0xC54;			// the running combo: hits, damage (+0xC58), and more counters up to +0xC6C
	static constexpr uint32 ComboPercent = 0xC58;		// float: the combo's damage in percent of the life bar
	static constexpr uint32 ComboDamage = 0xC68;		// float: the same as a fraction
	static constexpr uint32 Meter = 0xC14;				// float, 0 to 1; a bar is 1/3 (AddMeter, FIGHT-RE-NOTES §8c)
	static constexpr uint32 InputOff = 0x944;			// 1 while the player can't act (player virtual +0xC sets it each frame)
	static constexpr uint32 InputBuffer = 0xB40;		// the controller's input buffer (OKInput)
	static constexpr uint32 ComboLength = 0xB48;		// the combo so far: length, then up to 5 steps (button << 16 | direction)
	static constexpr uint32 ComboSteps = 0xB4C;
	static constexpr uint32 ControlCount = 0xB98;		// a stack of up to 4 control states at +0xB64; the top must be 1 to act
	static constexpr uint32 ControlStack = 0xB64;
	static constexpr uint32 RoundsWon = 0xB9C;			// gamelogic.round_over adds one to the winner's
	static constexpr uint32 Opponent = 0xBE4;			// the other side's player record, @his_info (virtual +0x38 / +0x3C)
	static constexpr uint32 PendingButton = 0xBA4;		// a button the dispatcher takes as pressed, once
	static constexpr uint32 Scanning = 0x940;			// check_special_moves: 1 or 2 while a scanner pass runs, else 0

	virtual FOKPlayer* AsPlayer() override { return this; }

	uint32 Handle = 0;
	FOKFighter* Fighter = nullptr;		// the active fighter
};

/**
 * A character's definition (fighter +0x2A0, from +0x7620): blood colours (+0xC0..), the asset list +0x24 that
 * FUN_0044EC40 loads, a global table +0x28, and values *_game_init sets (+0x3C and +0xBC: 1.0 for Scorpion).
 * [inf] Its real size and where MKKE fills it (FUN_008A5270 / FUN_008A83F0) aren't read yet; scripts use up to +0x110.
 */
class OPENKOMBAT_API FOKCharacterInfo : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x200;
};

/**
 * A fighter's moveset (ANIM-RE-NOTES §2), at fighter +0x518: the animations the shared engine scripts ask for by
 * slot. The loader (FUN_008ECAB0) fills it from the character script's moveset_<char> when the fighter loads its
 * script, and scripts read rows by offset through 137.95. Offsets here are from the moveset's start.
 */
namespace FOKMoveset
{
	constexpr int32 Bytes = 0x29F4;
	constexpr uint32 Rows = 0x10;				// row n at Rows + n * RowBytes: anim, rate, root scale, 4 x {foley kind, frame}
	constexpr int32 RowBytes = 0x2C;
	constexpr int32 MaxRows = 50;
	constexpr uint32 AltStances = 0x8A8;		// moveset_alt_stance (7.534): up to 6 of {anim, rate, root scale, ?}
	constexpr int32 AltStanceBytes = 0x10;
	constexpr int32 MaxAltStances = 6;
	constexpr uint32 AltStanceCount = 0x908;
	constexpr uint32 MoveListCounts = 0x90C;	// the 5 move lists built from attack function names (ANIM §3)
	constexpr uint32 Script = 0x04;					// handle to the character script (object +4, serial +8)
	constexpr uint32 MoveLists = 0x920;				// 5 lists (FP, BP, FK, BK, other) of up to 60 moves of 0x1C: flags, function, 5 steps
	constexpr int32 MoveListBytes = 0x690;
	constexpr int32 MoveBytes = 0x1C;
	constexpr int32 MaxMoves = 60;
	constexpr uint32 SecondaryFlag = 0x29F0;		// 7.535; moveset_anim_flag (81.138)
	constexpr uint32 NoFoley = 21;					// a foley pair's kind when the row has none
}

/**
 * A command record (0x124 bytes, FIGHT-RE-NOTES §6.1): a special move, fatality, babality, tag move or X-ray.
 * Records live in the character script's globals; bind_command (81.215) adds one to the fighter's list.
 */
/** A button slot (fighter +0x30B4 + slot x 0x124): what pressing that button does (81.219 / 81.221 / 81.222). */
namespace FOKButtonSlot
{
	constexpr uint32 Bound = 0x00;
	constexpr uint32 Enabled = 0x04;			// enable_commands / disable_commands
	constexpr uint32 Allowed = 0x08;			// [inf] cleared by 78.13; nothing found sets it, so it starts at 1
	constexpr uint32 Attack = 0x14;			// 1: an attack from the move lists; 0: a script function
	constexpr uint32 Direct = 0x18;			// for script functions: 0 = the bound function
	constexpr uint32 Redirect = 0x1C;			// 1: act as the button at +0x20
	constexpr uint32 RedirectCode = 0x20;
	constexpr uint32 Script = 0x24;			// handle: object +0x28, serial +0x2C
	constexpr uint32 Callback = 0x30;			// a C predicate, as a native callback id (module 7's callback table)
	constexpr uint32 Function = 0x34;			// 1-based, in Script
	constexpr uint32 PressMode = 0x4C;			// 1: pressed this frame (player +0x14); 0: held (player +0x1C)
}

namespace OKCommand
{
	constexpr uint32 Script = 0x24;			// handle: object +0x28, serial +0x2C
	constexpr uint32 Function = 0x34;
	constexpr uint32 Window = 0x50;			// input window, frames
	constexpr uint32 Enabled = 0x6C;
	constexpr uint32 Type = 0x70;				// picks the matcher: 0 special, 3 fatality, 4 EX, 6/7 tag, 8 X-ray, 10 babality
	constexpr uint32 Button = 0x74;
	constexpr uint32 Sequence = 0x78;			// direction sequence, -1 terminated
	constexpr uint32 Flag = 0x7C;
	constexpr uint32 MinDistance = 0x84;
	constexpr uint32 MaxDistance = 0x88;
	constexpr uint32 Extra = 0x8C;
	constexpr uint32 MeterFlag = 0x94;
	constexpr uint32 MeterCost = 0x98;
	constexpr uint32 Name = 0x9C;				// 0x40 characters (tag moves)
	constexpr uint32 PartnerName = 0xDC;
}

/**
* MKKE's MiscPdata (0x164 bytes): parameters one process hands another. Scripts read and write
* its bytes by offset, so it lives in the runtime's heap.
*/
class OPENKOMBAT_API FOKPdata : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x164;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::MiscPdata; }
};

/** A finisher cinema's request (138.3, MKKE's 0x4C-byte FatalityCinemaPdata): play_fatality_cinema fills it in for p_fatality_cinema. */
class OPENKOMBAT_API FOKFatalityCinemaPdata : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x4C;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::FatalityCinemaPdata; }
};

/**
 * An MKCinema (39.1; cinema_prepare, 39.51): a cinema scene the scripts set up, bind fighters to and play (W4). The
 * script world plays it with FOKCinemaPlayer (the runtime's active cinema). Its bytes are MKKE's 0x94: fg_utils keeps
 * the callbacks' script at +0x54 and their data at +0x58, and the bound fighters' handles at +0x5C and +0x68.
 */
class OPENKOMBAT_API FOKCinemaObject : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x94;
	enum class EState : uint8 { Prepared, Requested, Playing, Done };

	FString Package;						// the cinema package (Scorpion_Fatality1)
	TMap<FString, FOKFighter*> Actors;		// cinema_bind_actor: group name (Attacker, Victim) -> fighter
	EState State = EState::Prepared;
	bool bSkip = false;
	float Position = 0.0f;					// seconds, while it plays
	float Length = 0.0f;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::Cinema; }
};

/**
 * MKKE's ScreenShakePdata (0x50 bytes, FUN_0044AA80): set by 91.5 (FUN_0044AAF0): +0x0C the shake vector, +0x18 a
 * second vector, +0x24 a count, +0x28 a flag, +0x30 / +0x34 (91.7), +0x38 / +0x3C (91.8). p_screen_shaker reads it.
 */
class OPENKOMBAT_API FOKScreenShakePdata : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x50;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::ScreenShakePdata; }
};

/**
 * MKKE's ArmorPdata (0x54 bytes, FUN_00450220), set by 94.5 (FUN_0044A9F0): +0x08 a value [unk] (1.0 from every caller),
 * +0x0C the points (float; one a hit, "shattered" at 0), +0x10 the damage scale, +0x14 the ticks left (float;
 * negative ones get 2^32), +0x48 the fighter. FGFightEngineScript.p_armor_on counts the ticks down.
 */
class OPENKOMBAT_API FOKArmorPdata : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x54;
	static constexpr uint32 Points = 0x0C;
	static constexpr uint32 DamageScale = 0x10;
	static constexpr uint32 Ticks = 0x14;
	static constexpr uint32 Owner = 0x48;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::ArmorPdata; }
};

/**
 * The fight camera as scripts see it: MKKE's MKFGCameraObj. Game info +0x180 (the camera manager in MKKE) points at it
 * too, and 171.15 / 151.21 return it, so one object stands in for both. FOKFightCamera reads it each frame.
 */
class OPENKOMBAT_API FOKCameraObject : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x600;
	static constexpr uint32 Busy = 0x08;				// p_winner_winpose waits while this is set (151.2); always 0 here
	static constexpr uint32 ShakePosition = 0x124;	// 3 floats: 149.23's first vector, added to the eye
	static constexpr uint32 ShakeRotation = 0x130;	// 3 words: its second, added to the view (pitch, yaw, roll)
	static constexpr uint32 ShakePaused = 0x154;		// 149.32 / 149.33
	static constexpr uint32 Variables = 0x178;		// 68 floats, from FGCameraEngine's CameraVariables (FUN_0086B510)
	static constexpr int32 NumVariables = 0x44;
	static constexpr uint32 BgndData = 0x378;		// 3 words from the stage (camera_set_bgnd_data, 171.11)
};

/**
 * MKKE's GFGGameInfo (0xE89B70): the fight's global state. Scripts reach it as @GFGGameInfo or @GGameInfo, and the engine's
 * own pointer to the game info (PTR_DAT_00DEF4B4, which natives such as 43.6 and 43.7 read whatever object they are
 * passed) holds the same address.
 */
class OPENKOMBAT_API FOKGameInfo : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x10000;			// real size unknown; scripts use offsets up to 0x82C4
	static constexpr uint32 State = 0x04;			// 43.4: the game state, 8 in a fight (MENU-RE-NOTES §3.1)
	static constexpr uint32 GameMode = 0x08;		// 43.5 / 43.6: 1 = versus (MENU-RE-NOTES §3)
	static constexpr uint32 Flags = 0x10;			// 1: the round is live, 0x20: round over, 0x80/0x100: finishing, 0x8000 / 0x4000: the
												// clock stops, 0x20000000: the win text is up (FIGHT §8b)
	static constexpr uint32 FrameCounter = 0x70;	// the fight's frame number
	static constexpr uint32 Camera = 0x180;			// the camera manager; ours is the FOKCameraObject
	static constexpr uint32 Settings = 0x184;		// the game-settings record, inline: +4 round time, +8 rounds to win
	static constexpr uint32 Players0 = 0x20C;		// MKKE's 4 player records (0xC8C each) start here; ours are separate objects
	static constexpr uint32 Winner = 0x80F4;		// the round winner's player record (gamelogic.round_over)
	static constexpr uint32 Clock = 0x8110;			// seconds left in the round
	static constexpr uint32 Round = 0x8114;			// the round number, from 1

	virtual FOKGameInfo* AsGameInfo() override { return this; }

	uint32 Handle = 0;
	FOKPlayer* Players[2] = {};						// get_player (43.24, GameInfo virtual +0x10)
};

/**
 * A CollisionContext (class 79.1, 0x20 bytes): one attack's contact record. new_collision_context (81.166)
 * makes one per launched attack. +0x08 set by launch_attack, +0x18 player slot, +0x1C the outcome:
 * 5 = nothing yet, 0 hit, 1 blocked (FIGHT-RE-NOTES §7.2).
 */
class OPENKOMBAT_API FOKCollisionContext : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x20;
	static constexpr uint32 Outcome = 0x1C;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::CollisionContext; }
};

/**
 * An MKAnimEventPdata (class 92.1, 0x20 bytes): what a frame event hands the process it starts. +0x08/+0x0C the
 * slot's two parameters, +0x10 the address of its text, +0x14 a handle {+0x18 object, +0x1C serial} (hitbox scans:
 * the attack's CollisionContext).
 */
class OPENKOMBAT_API FOKAnimEventPdata : public FOKScriptObject
{
public:
	static constexpr int32 Bytes = 0x20;

	virtual bool IsA(EOKScriptClass Class) const override { return Class == EOKScriptClass::AnimEventPdata; }
};