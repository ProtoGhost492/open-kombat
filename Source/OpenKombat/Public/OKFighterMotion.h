#pragma once

#include "CoreMinimal.h"

class FOKScriptRuntime;
class FOKFighter;

/**
 * Fighter motion (system F3, ANIM-RE-NOTES §4.3): what moves a fighter besides its animation's root motion.
 * MKKE keeps it in the fighter at +0x304..+0x3B0. The module 81 natives set it up; the fighter's step runs it once
 * a frame, after the animation: launches and jumps follow a closed-form parabola that puts the hips on a height,
 * and force-aways push the fighter along the fight line.
 */
namespace OKMotion
{
	constexpr double MaxSeparation = 600.0;			// DAT_00DF0BD8: the furthest apart the fighters can be (7.53 reads it)

	// MKCharacterObj fields (the offsets scripts and natives use)
	constexpr uint32 MoveKind = 0x304;				// 0 none, 1 launched, 2/3 in the air with no landing, 4 held, 5 in the air,
												// 6 waiting for the landing frame, 7 landing, 8 landed
	constexpr uint32 ForceAway = 0x308;			// 5 pushing, 8 done
	constexpr uint32 LandTicks = 0x318;			// the air clock reading at touchdown
	constexpr uint32 PushVelocity = 0x31C;		// x, y in units a second
	constexpr uint32 VelocityZ = 0x324;
	constexpr uint32 Gravity = 0x328;
	constexpr uint32 AirClock = 0x32C;			// ticks since the launch
	constexpr uint32 PushClock = 0x330;
	constexpr uint32 Speed = 0x334;				// how fast the air clock runs; stop sets 0
	constexpr uint32 StartZ = 0x338;			// the hips' height when launched
	constexpr uint32 StartVelocityZ = 0x33C;
	constexpr uint32 Tracked = 0x340;			// the hips' x, y, and the path's z (+0x348)
	constexpr uint32 PathZ = 0x348;
	constexpr uint32 LandAnim = 0x34C;			// the animation the landing is timed against
	constexpr uint32 LandFrame = 0x350;			// its frame at touchdown
	constexpr uint32 LandRate = 0x354;			// the rate that gets it there
	constexpr uint32 EarlyTicks = 0x358;		// how many ticks before touchdown the landing starts (-1: never early)
	constexpr uint32 StartFrame = 0x35C;
	constexpr uint32 LandBlend = 0x360;
	constexpr uint32 LandFunction = 0x364;		// where the fighter's thread goes on landing: a function (1-based or an import)
	constexpr uint32 LandScript = 0x368;		// of this script
	constexpr uint32 TargetHeight = 0x36C;		// launch_to_height: the height to reach
	constexpr uint32 TargetTime = 0x370;		// and when, in seconds
	constexpr uint32 SolveStart = 0x37C;		// flags for the object update: take the start height
	constexpr uint32 SolveLaunch = 0x380;		// solve gravity and speed for a target height
	constexpr uint32 NoLandTransfer = 0x388;	// 1: landing leaves the thread alone (launch_and_land)
	constexpr uint32 KeepRate = 0x38C;			// 1: don't retime the animation to the landing
	constexpr uint32 LandOffset = 0x390;		// land this much above the floor
	constexpr uint32 MoveTick = 0x394;			// the tick a move stopped
	constexpr uint32 AfterLandInput = 0x398;
	constexpr uint32 PushDecay = 0x3A4;
	constexpr uint32 PushDecayStart = 0x3A8;	// push clock readings: decay from here
	constexpr uint32 PushStop = 0x3AC;			// and stop here
	constexpr uint32 InAir = 0x3B0;
	constexpr uint32 ObjectVelocity = 0x6C;		// MKObj's own velocity and acceleration (+0x78); fighters don't integrate them
	constexpr uint32 ObjectAcceleration = 0x78;

	constexpr uint32 AirFlag = 8;				// fighter flags (+0x2E8): init_air_move sets it, init_ground_move clears it
	constexpr uint32 StillFlag = 0x80000;

	/** Virtual +0x10C (FUN_00447DD0): back on the ground. bResetRate puts the animation rate back to 1. */
	OPENKOMBAT_API void InitGroundMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter, bool bResetRate);
	/** FUN_00446CF0: in the air (flag 8), keeping the animation's rate. */
	OPENKOMBAT_API void InitAirMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter);
	/** FUN_00446D30: on the ground and still (flag 0x80000). */
	OPENKOMBAT_API void InitStillMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter, bool bResetRate);
	/** stop (81.41, FUN_00447D00): no force-away, and the air clock stops. */
	OPENKOMBAT_API void Stop(FOKScriptRuntime& Runtime, FOKFighter& Fighter);
	/** FUN_00447D50: a move stopped part-way (blend_to_stance ends it with StopMove). */
	OPENKOMBAT_API bool IsStoppedMidMove(FOKScriptRuntime& Runtime, const FOKFighter& Fighter);
	/** Virtual +0x14C (FUN_0044C200): no move at all. */
	OPENKOMBAT_API void StopMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter);

	/**
	 * Virtual +0x154 (FUN_00450D00): a launch with a start speed and gravity, landing at LandFrame of the playing
	 * animation (or Anim from StartFrame), and sending the thread to Script.Function on landing.
	 */
	OPENKOMBAT_API void Launch(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float VelocityZ, float Gravity, float LandFrame, float EarlyTicks,
		uint32 Script, uint32 Function, uint32 Anim, float StartFrame, float Blend, uint32 KeepRate);
	/** Virtual +0x158 (FUN_00450E60): the same, with no thread transfer on landing. */
	OPENKOMBAT_API void LaunchAndLand(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float VelocityZ, float Gravity, float LandFrame, float LandOffset);
	/** Virtual +0x15C (FUN_0044B730): reach Height (above the floor) after Frames, then come down. The rest as Launch. */
	OPENKOMBAT_API void LaunchToHeight(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Height, float Frames, float LandFrame, float EarlyTicks,
		uint32 Script, uint32 Function, uint32 Anim, float StartFrame, float Blend, uint32 KeepRate);
	/** Virtual +0x160 (FUN_0044B7B0): the same as LaunchAndLand. */
	OPENKOMBAT_API void LaunchToHeightGuaranteed(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Height, float Frames, float LandFrame);
	/** Virtual +0x170 (FUN_0044BAD0): in the air with a vertical speed and no gravity or landing. */
	OPENKOMBAT_API void SetAirborne(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float VelocityZ);
	/** FUN_0044BB90 (81.107): how fast the air clock and the animation run; 0 freezes both. */
	OPENKOMBAT_API void SetAirSpeed(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Scale);

	/**
	 * Virtual +0x140 (FUN_0044BD70): pushed along the fight line at Speed (units a second; positive is away from the
	 * opponent). After DecayStart ticks the speed decays by Decay a tick, and the push ends Extra ticks later.
	 */
	OPENKOMBAT_API void ForceAwayFromOpponent(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Speed, float DecayStart, float Decay, float Extra);
	/** FUN_0044BA10 and virtual +0x1F4: the same, but backwards from where the fighter faces. */
	OPENKOMBAT_API void ForceAwayFacing(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Speed, float DecayStart, float Decay, float Extra);

	/** The object update (virtual +0x14, FUN_00448240): solves a launch started this frame. Before the animation advances. */
	OPENKOMBAT_API void PreStep(FOKScriptRuntime& Runtime, FOKFighter& Fighter);
	/** The fighter's step (virtual +0x150, FUN_004508A0): the air path and the force-away. After the animation and root motion. */
	OPENKOMBAT_API void Step(FOKScriptRuntime& Runtime, FOKFighter& Fighter);
	/** Player pushing (FUN_008B4BF0), after both fighters' steps: overlapping fighters move apart, no more than 600 apart, inside the arena. */
	OPENKOMBAT_API void PushApart(FOKScriptRuntime& Runtime, FOKFighter& A, FOKFighter& B);
}
