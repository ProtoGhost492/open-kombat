#include "OKNatives.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "OKAudio.h"
#include "OKMkoFile.h"
#include "OKScriptObjects.h"
#include "OKScriptRuntime.h"
#include "OKScriptThread.h"
#include "OKInput.h"
#include "OKFighterMotion.h"
#include "OKObjects.h"
#include "OKCollision.h"
#include "OKParticles.h"
#include "OKScriptLibrary.h"
#include "OKScriptSubsystem.h"
#include "Engine/Engine.h"
#include "OKLocalization.h"

namespace
{
	float BitsToFloat(uint32 Bits)
	{
		float Value;
		FMemory::Memcpy(&Value, &Bits, 4);
		return Value;
	}

	uint32 FloatToBits(float Value)
	{
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, 4);
		return Bits;
	}

	float ArgFloat(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Raw = 0;
		Thread.ArgRaw(Index, Raw);
		return BitsToFloat(Raw);
	}

	uint32 ArgWord(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Raw = 0;
		Thread.ArgRaw(Index, Raw);
		return Raw;
	}

	// Natives that do nothing in MKKE's release build.
	bool NoOp(FOKScriptThread&)
	{
		return true;
	}

	// 1.1 jump(func)
	bool Jump(FOKScriptThread& Thread)
	{
		uint32 FunctionRef = 0;
		return Thread.ArgRaw(0, FunctionRef) && Thread.JumpTo(FunctionRef);
	}

	// 1.5 loop_sleep(ticks)
	bool LoopSleep(FOKScriptThread& Thread)
	{
		uint32 Ticks = 0;
		return Thread.ArgRaw(0, Ticks) && Thread.LoopSleep(Ticks);
	}

	// 1.22 find_proc (really "my attached object"): the first object attached to the running process
	bool FindProc(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetFirstAttachedObject());
		return true;
	}

	// 1.12 is_proc(obj, class) (really "is a"): obj if it's of that class, otherwise 0
	bool IsProc(FOKScriptThread& Thread)
		{
			uint32 Obj = 0, Class = 0;
			if (!Thread.ArgRaw(0, Obj) || !Thread.ArgRaw(1, Class))
			{
				return false;
			}
			if ((Obj >> 24) == FOKScriptRuntime::InstanceTag)
			{
				// a script instance's value stands for its MKScript object
				const bool bScript = Class == FOKScriptRuntime::ClassValue(EOKScriptClass::Script)
					&& Thread.GetRuntime().FindInstanceById(int32(Obj & 0x00FFFFFF)) != nullptr;
				Thread.SetResult(bScript ? Obj : 0);
				return true;
			}
			const FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Obj);
			const bool bMatch = Object && (Class >> 24) == FOKScriptRuntime::ClassTag
				&& Object->IsA(EOKScriptClass(Class & 0x00FFFFFF));
			Thread.SetResult(bMatch ? Obj : 0);
			return true;
		}

	// 20.1: the MKScript class (FUN_00DE8238), for is_proc
	bool ClassScript(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::Script));
		return true;
	}

	// *.0 (about 120 modules): destroy an object (FUN_008D1080)
	bool DestroyObject(FOKScriptThread& Thread)
	{
		uint32 Obj = 0;
		if (!Thread.ArgValue(0, Obj))
		{
			return false;
		}
		Thread.GetRuntime().DestroyObject(Obj);
		return true;
	}

	// 95.1: the MiscPdata class, for is_proc
	bool ClassMiscPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::MiscPdata));
		return true;
	}

	// 95.3: a new MiscPdata
	bool NewMiscPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetRuntime().CreatePdata());
		return true;
	}

	// 95.5 / 195.5 attach(obj, process, flag): obj+8 = flag; attach obj to the process (FUN_0044A7F0)
	bool AttachToProcess(FOKScriptThread& Thread)
	{
		uint32 Obj = 0, ProcessHandle = 0, Flag = 0;
		if (!Thread.ArgValue(0, Obj) || !Thread.ArgRaw(1, ProcessHandle) || !Thread.ArgRaw(2, Flag))
		{
			return false;
		}
		if (Obj == 0)
		{
			return true;
		}
		if ((Obj >> 24) == FOKScriptRuntime::HeapTag && !Thread.StoreBytes(Obj + 8, &Flag, 4))
		{
			return false;
		}
		FOKScriptObject* Target = Thread.GetRuntime().ResolveHandle(ProcessHandle);
		if (FOKScriptThread* Process = Target ? Target->AsThread() : nullptr)
		{
			Process->AttachObject(Obj);
		}
		return true;
	}

	// 6.81 pow(x, y) (called color_scale in the table)
	bool Pow(FOKScriptThread& Thread)
	{
		uint32 X = 0, Y = 0;
		if (!Thread.ArgRaw(0, X) || !Thread.ArgRaw(1, Y))
		{
			return false;
		}
		Thread.SetResult(FloatToBits(float(FMath::Pow(double(BitsToFloat(X)), double(BitsToFloat(Y))))));
		return true;
	}

	// 43.34 set_color_scale(r, g, b): the screen colour scale (FUN_00407B30)
	bool SetColorScale(FOKScriptThread& Thread)
	{
		uint32 R = 0, G = 0, B = 0;
		if (!Thread.ArgRaw(0, R) || !Thread.ArgRaw(1, G) || !Thread.ArgRaw(2, B))
		{
			return false;
		}
		Thread.GetRuntime().SetColorScale(FVector3f(BitsToFloat(R), BitsToFloat(G), BitsToFloat(B)));
		return true;
	}

	// Write a word into a fighter's memory.
	bool PutFighterWord(FOKScriptThread& Thread, const FOKFighter& Fighter, uint32 Offset, uint32 Value)
	{
		return Thread.StoreBytes(Fighter.Handle + Offset, &Value, 4);
	}

	// Write a word into a fighter's pending attack.
	bool PutAttackWord(FOKScriptThread& Thread, const FOKFighter& Fighter, uint32 Offset, uint32 Value)
	{
		return PutFighterWord(Thread, Fighter, FOKFighter::PendingAttack + Offset, Value);
	}

	// Clear an attack struct to MKKE's reset values (FUN_0044A440).
	bool ResetAttack(FOKScriptThread& Thread, uint32 Attack)
	{
		uint8 Bytes[OKAttack::Bytes] = {};
		auto Put = [&Bytes](uint32 Offset, uint32 Value) { FMemory::Memcpy(Bytes + Offset, &Value, 4); };
		Put(OKAttack::HitClass, OKAttack::DefaultHitClass);
		Put(OKAttack::Hitbox + 8, OKAttack::DefaultLimb);
		for (int32 i = 0; i < OKAttack::MaxFoley; ++i)
		{
			Put(OKAttack::FoleySounds + 4 * i, OKAttack::NoFoleySound);
		}
		return Thread.StoreBytes(Attack, Bytes, OKAttack::Bytes);
	}

	// Free all 50 frame-event slots (FUN_00896640). MKKE also kills process 13 on the fighter and, when the
	// fighter has a player record (+0x7638), clears the attack-active bit. Neither exists here yet.
	bool ResetFrameEvents(FOKScriptThread& Thread, const FOKFighter& Fighter)
	{
		if (!PutFighterWord(Thread, Fighter, FOKFighter::FrameEventCount, 0))
		{
			return false;
		}
		for (int32 i = 0; i < FOKFighter::MaxFrameEvents; ++i)
		{
			if (!PutFighterWord(Thread, Fighter, FOKFighter::FrameEvents + i * FOKFighter::FrameEventBytes + 4, 2))
			{
				return false;
			}
		}
		return true;
	}

	// 6.1 set_attack_reactions(standRx, airRx, damage, chip, flags, &stunOverride, hasStunOverride)
	// No object argument: it applies to the running script's my_obj (FUN_008B60A0).
	bool SetAttackReactions(FOKScriptThread& Thread)
	{
		uint32 Raw[5];
		uint32 HasStun = 0;
		for (int32 i = 0; i < 5; ++i)
		{
			if (!Thread.ArgRaw(i, Raw[i]))
			{
				return false;
			}
		}
		if (!Thread.ArgRaw(6, HasStun))
		{
			return false;
		}

		// Argument 5 is a frame location where sttmpd stored a double.
		float Stun = 0.0f;
		if (HasStun != 0)
		{
			double Stored = 0.0;
			if (!Thread.LoadBytes(Thread.ArgFrameAddress(5), &Stored, 8))
			{
				return false;
			}
			Stun = float(Stored);
		}

		FOKFighter* Fighter = Thread.MyFighter();
		if (!Fighter)
		{
			return true;
		}
		// FUN_00896EC0: one standing reaction (the other three directions cleared), the airborne one,
		// damage, chip, flags, the stun override, and the script the reactions belong to.
		const uint32 Fields[][2] = {
			{ OKAttack::ReactionScript, Thread.CurrentInstanceValue() },
			{ OKAttack::StandingReaction, Raw[0] },
			{ OKAttack::StandingReaction + 4, 0 },
			{ OKAttack::StandingReaction + 8, 0 },
			{ OKAttack::StandingReaction + 12, 0 },
			{ OKAttack::AirborneReaction, Raw[1] },
			{ OKAttack::AirborneScript, Thread.CurrentInstanceValue() },
			{ OKAttack::AirborneScript + 4, Thread.CurrentInstanceValue() },
			{ OKAttack::Damage, Raw[2] },
			{ OKAttack::ChipDamage, Raw[3] },
			{ OKAttack::AttackFlags, Raw[4] },
			{ OKAttack::HitStunOverride, FloatToBits(Stun) },
			{ OKAttack::HasReactions, 1 },
		};
		for (const auto& Field : Fields)
		{
			if (!PutAttackWord(Thread, *Fighter, Field[0], Field[1]))
			{
				return false;
			}
		}
		return true;
	}

	// 6.95 random(n): a number in [0, n) from MKKE's generator (FUN_00492050).
	bool Random(FOKScriptThread& Thread)
	{
		uint32 N = 0;
		if (!Thread.ArgRaw(0, N))
		{
			return false;
		}
		const uint32 R = Thread.GetRuntime().NextRandom();
		Thread.SetResult(uint32((uint64(R) * N) >> 31));
		return true;
	}

	// 137.249 set_attack_block_a(obj, anim, ?, rate, rootMotionScale, ?, ?): pending attack +0x44..+0x58 (FUN_00896B40)
	bool SetAttackBlockA(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;		// MKKE does nothing when the object isn't a fighter
		}
		for (int32 i = 0; i < 6; ++i)
		{
			uint32 Value = 0;
			if (!Thread.ArgRaw(1 + i, Value) || !PutAttackWord(Thread, *Fighter, OKAttack::BlockA + 4 * i, Value))
			{
				return false;
			}
		}
		return PutAttackWord(Thread, *Fighter, OKAttack::HasBlockA, 1);
	}

	// 137.250 set_attack_block_a2(obj, anim, ?, rate, rootMotionScale, ?, ?, keep) (FUN_00896BA0): 137.249, and with keep 0
	// fighter +0x3A0 = 0 (FUN_0044BC50)
	bool SetAttackBlockA2(FOKScriptThread& Thread)
	{
		uint32 Keep = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!SetAttackBlockA(Thread) || !Thread.ArgRaw(7, Keep))
		{
			return false;
		}
		if (Fighter && Keep == 0)
		{
			Thread.GetRuntime().SetHeapWord(Fighter->Handle + 0x3A0, 0);
		}
		return true;
	}

	uint32 MovesetOf(FOKScriptRuntime& Runtime, const FOKFighter* Fighter);

	// 137.264 throw_entry(obj, slot, ?, rate, rootMotionScale, ?) (FUN_00896810): set_attack_block_a with the animation from
	// moveset row `slot` (44 is Throw_Miss, the whiff a throw starts as)
	bool ThrowEntry(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Moveset = MovesetOf(Thread.GetRuntime(), Fighter);
		if (!Fighter || Moveset == 0)
		{
			return true;
		}
		uint32 Slot = 0, Anim = 0;
		if (!Thread.ArgValue(1, Slot) || Slot >= uint32(FOKMoveset::MaxRows) || !Thread.LoadBytes(Moveset + FOKMoveset::Rows + Slot * FOKMoveset::RowBytes, &Anim, 4)
			|| !PutAttackWord(Thread, *Fighter, OKAttack::BlockA, Anim))
		{
			return false;
		}
		for (int32 i = 1; i < 6; ++i)
		{
			uint32 Value = 0;
			if (!Thread.ArgRaw(1 + i, Value) || !PutAttackWord(Thread, *Fighter, OKAttack::BlockA + 4 * i, Value))
			{
				return false;
			}
		}
		return PutAttackWord(Thread, *Fighter, OKAttack::HasBlockA, 1);
	}

	// 137.246 air_throw(obj): fighter +0x7C80 = 1 (FUN_008DA360)
	bool AirThrow(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || PutFighterWord(Thread, *Fighter, 0x7C80, 1);
	}

	// 137.257 set_attack_block_b(obj, a, b, c, d): pending attack +0x90..+0x9C (FUN_00896A40)
	bool SetAttackBlockB(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		for (int32 i = 0; i < 4; ++i)
		{
			uint32 Value = 0;
			if (!Thread.ArgRaw(1 + i, Value) || !PutAttackWord(Thread, *Fighter, OKAttack::BlockB + 4 * i, Value))
			{
				return false;
			}
		}
		return PutAttackWord(Thread, *Fighter, OKAttack::HasBlockB, 1);
	}

	// 137.259 set_attack_block_c(obj, first, last, limb, radiusScale, reach, flags, hitClass): the hitbox (FUN_00896960)
	bool SetAttackBlockC(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		for (int32 i = 0; i < 7; ++i)
		{
			uint32 Value = 0;
			const uint32 Offset = (i < 6) ? OKAttack::Hitbox + 4 * i : OKAttack::HitClass;
			if (!Thread.ArgRaw(1 + i, Value) || !PutAttackWord(Thread, *Fighter, Offset, Value))
			{
				return false;
			}
		}
		return PutAttackWord(Thread, *Fighter, OKAttack::HasHitbox, 1);
	}

	// 7.16 set_attack_foley(&pairs, count): up to 10 {sound, frame} pairs (FUN_00896C70). The pairs sit in
	// the caller's temporaries, 12 byte each: an int sound id (sttmp), then the frame as a double (sttmpd).
	// count is the number of values, so two per pair.
	bool SetAttackFoley(FOKScriptThread& Thread)
	{
		uint32 Count = 0;
		if (!Thread.ArgRaw(1, Count))
		{
			return false;
		}
		const uint32 Pairs = Thread.ArgFrameAddress(0);
		FOKFighter* Fighter = Thread.MyFighter();
		if (!Fighter)
		{
			return true;
		}
		if (!PutAttackWord(Thread, *Fighter, OKAttack::HasFoley, 1) || !PutAttackWord(Thread, *Fighter, OKAttack::FoleyCount, 0))
		{
			return false;
		}
		int32 Stored = 0;
		for (uint32 Value = 0; Value + 1 < Count; Value += 2)
		{
			const uint32 Pair = Pairs + (Value / 2) * 12;
			int32 Sound = 0;
			double Frame = 0.0;
			if (!Thread.LoadBytes(Pair, &Sound, 4) || !Thread.LoadBytes(Pair + 4, &Frame, 8))
			{
				return false;
			}
			if (Sound > 19 || Stored >= OKAttack::MaxFoley)
			{
				break;		// MKKE stops at the first bad sound id, or when the table is full
			}
			if (!PutAttackWord(Thread, *Fighter, OKAttack::FoleySounds + 4 * Stored, uint32(Sound))
				|| !PutAttackWord(Thread, *Fighter, OKAttack::FoleyFrames + 4 * Stored, FloatToBits(float(Frame)))
				|| !PutAttackWord(Thread, *Fighter, OKAttack::FoleyCount, uint32(Stored + 1)))
			{
				return false;
			}
			++Stored;
		}
		return true;
	}

	// 81.166 new_collision_context(fighter): a CollisionContext (class 79.1) for the attack being launched
	// (FUN_0044E060). +0x18 = the fighter's player slot, +0x1C = 5 (no outcome yet). It becomes the fighter's
	// current context, which last_contact_hit (6.251) reads. Was named popup_flag.
	bool NewCollisionContext(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Context = Runtime.CreateOwned(MakeUnique<FOKCollisionContext>(), FOKCollisionContext::Bytes);
		if (Context == 0)
		{
			return Thread.NativeFail(TEXT("new_collision_context: the heap is full"));
		}
		Runtime.SetHeapWord(Context + 4, Context);					// serial, for handles (resolve_handle, 23.4)
		Runtime.SetHeapWord(Context + 0x18, uint32(Fighter->Index));
		Runtime.SetHeapWord(Context + FOKCollisionContext::Outcome, 5);
		Fighter->CollisionContext = Context;
		Thread.SetResult(Context);
		return true;
	}

	// 137.118 launch_attack(fighter, recordStats): make the pending attack live (FUN_008AAF60).
	bool LaunchAttack(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		const uint32 Live = Fighter->Handle + FOKFighter::LiveAttack;
		const uint32 Pending = Fighter->Handle + FOKFighter::PendingAttack;
		if (!ResetAttack(Thread, Live) || !ResetFrameEvents(Thread, *Fighter))
		{
			return false;
		}

		// Copy pending into live, leaving the handle words as the reset left them.
		uint8 Attack[OKAttack::Bytes];
		if (!Thread.LoadBytes(Pending, Attack, OKAttack::Bytes))
		{
			return false;
		}
		for (const uint32 Offset : OKAttack::HandleWords)
		{
			FMemory::Memzero(Attack + Offset, 4);
		}
		if (!Thread.StoreBytes(Live, Attack, OKAttack::Bytes))
		{
			return false;
		}

		// FUN_008A1120 picks the bone mask the attack plays on, by limb; applying it needs a mesh.
		// Even without one, the layer is marked (+0x110 = -1) unless the limb is 0 or +0x20 is set.
		uint32 Word20 = 0, Limb = 0;
		FMemory::Memcpy(&Word20, Attack + 0x20, 4);
		FMemory::Memcpy(&Limb, Attack + OKAttack::Hitbox + 8, 4);
		if (Word20 == 0 && Limb != 0 && !PutFighterWord(Thread, *Fighter, FOKFighter::AttackLayer, 0xFFFFFFFF))
		{
			return false;
		}

		// recordStats fills a per-player stats table for the AI and telemetry; not needed yet.
		return ResetAttack(Thread, Pending);
	}

	// 137.226 "launch_attack_flag", really reset_frame_events(fighter) (FUN_00896640)
	bool ResetFrameEventsNative(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || ResetFrameEvents(Thread, *Fighter);
	}

	// 1.0 sleep(ticks)
	bool Sleep(FOKScriptThread& Thread)
	{
		uint32 Ticks = 0;
		return Thread.ArgRaw(0, Ticks) && Thread.SleepFor(Ticks);
	}

	// 1.3 die()
	bool Die(FOKScriptThread& Thread)
	{
		Thread.Die();
		return true;
	}

	// 6.70 start_proc(procId, funcIndex, obj, script, stackMode, group): start a script process (FUN_00492590)
	bool StartProc(FOKScriptThread& Thread)
	{
		uint32 Raw[6];
		for (int32 i = 0; i < 6; ++i)
		{
			if (!Thread.ArgRaw(i, Raw[i]))
			{
				return false;
			}
		}
		const uint32 ProcId = Raw[0], FuncIndex = Raw[1], ScriptRef = Raw[3], StackMode = Raw[4], Group = Raw[5];

		// script 0 = the running script; otherwise a pointer to a script instance (fixup kind 6).
		const FOKMkoFile* Script = Thread.GetCurrentScript();
		if (ScriptRef != 0)
		{
			const FOKScriptInstance* Instance = ((ScriptRef >> 24) == FOKScriptRuntime::InstanceTag)
				? Thread.GetRuntime().FindInstanceById(int32(ScriptRef & 0x00FFFFFF)) : nullptr;
			if (!Instance)
			{
				return Thread.NativeFail(FString::Printf(TEXT("start_proc: %08X isn't a script"), ScriptRef));
			}
			Script = Instance->File;
		}
		// A function in another script (&fg_utils.StopWallet) is an import reference, which names its own script.
		int32 Function = int32(FuncIndex) - 1;
		if ((FuncIndex & 0x80000000u) != 0)
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			const FOKScriptInstance* From = Runtime.GetInstance(*Thread.GetCurrentScript());
			FOKScriptInstance* Target = nullptr;
			if (!From || !Runtime.ResolveFunction(*From, FuncIndex, Target, Function))
			{
				return Thread.NativeFail(FString::Printf(TEXT("start_proc: import %08X doesn't resolve"), FuncIndex));
			}
			Script = Target->File;
		}

		FString Error;
		FOKScriptThread* Child = Thread.GetRuntime().StartProcess(*Script, Function,
			int32(ProcId), int32(Group), StackMode != 0, Error);
		if (!Child)
		{
			return Thread.NativeFail(TEXT("start_proc: ") + Error);
		}
		// A process started on a fighter is adopted by it: @my_obj is that fighter (FUN_00492590, object virtual +0x10).
		// The object can come as a setarg operand (start_obj_proc(188, 212, <victim>, ...)), so it's read as a value.
		uint32 OwnerHandle = 0;
		Thread.ArgValue(2, OwnerHandle);
		FOKScriptObject* Owner = Thread.GetRuntime().ResolveHandle(OwnerHandle);
		if (FOKFighter* Fighter = Owner ? Owner->AsFighter() : nullptr)
		{
			Child->BindFighter(*Fighter);
		}
		Thread.SetResult(Child->GetHandle());
		return true;
	}

	// 6.71 kill_proc(obj, procId): obj 0 = every process with this id
	bool KillProc(FOKScriptThread& Thread)
	{
		uint32 Obj = 0, ProcId = 0;
		if (!Thread.ArgRaw(0, Obj) || !Thread.ArgRaw(1, ProcId))
		{
			return false;
		}
		if (Obj == 0)
		{
			Thread.GetRuntime().KillProcesses(int32(ProcId));
		}
		return true;		// processes attached to objects come later
	}

	// 6.72 proc_exists(obj, procId): a live process with this id, or 0
	bool ProcExists(FOKScriptThread& Thread)
	{
		uint32 Obj = 0, ProcId = 0;
		if (!Thread.ArgRaw(0, Obj) || !Thread.ArgRaw(1, ProcId))
		{
			return false;
		}
		const FOKScriptThread* Found = (Obj == 0) ? Thread.GetRuntime().FindProcess(int32(ProcId)) : nullptr;
		Thread.SetResult(Found ? Found->GetHandle() : 0);
		return true;
	}

	// --- Frame events: callbacks at animation frames (fighter +0xAAC0, 50 slots of 0x88 bytes) ---------

	uint32 FighterWord(FOKScriptThread& Thread, const FOKFighter& Fighter, uint32 Offset)
	{
		uint32 Value = 0;
		Thread.LoadBytes(Fighter.Handle + Offset, &Value, 4);
		return Value;
	}

	uint32 EventSlot(int32 Slot)
	{
		return FOKFighter::FrameEvents + uint32(Slot) * FOKFighter::FrameEventBytes;
	}

	// 137.227 add_frame_event(fighter, kind, fromFrame, toFrame, function, script, object) -> slot, or 50 if none is free
	// (FUN_008964E0). Kind 0 is the hitbox scan; 1-3 are sounds. Function is a 1-based function number in
	// Script (0 = the running script). The slot waits (state 1) until arm_frame_events.
	bool AddFrameEventSlot(FOKScriptThread& Thread, int32& OutSlot)
	{
		OutSlot = FOKFighter::MaxFrameEvents;
		uint32 Raw[6];
		for (int32 i = 0; i < 6; ++i)
		{
			if (!Thread.ArgRaw(1 + i, Raw[i]))
			{
				return false;
			}
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		const uint32 Free = 2;
		const int32 Count = int32(FighterWord(Thread, *Fighter, FOKFighter::FrameEventCount));
		int32 Slot = FOKFighter::MaxFrameEvents;
		if (Count < FOKFighter::MaxFrameEvents && FighterWord(Thread, *Fighter, EventSlot(Count) + 4) == Free)
		{
			Slot = Count;		// the next unused slot
			if (!PutFighterWord(Thread, *Fighter, FOKFighter::FrameEventCount, uint32(Count + 1)))
			{
				return false;
			}
		}
		else
		{
			for (int32 i = 0; i < FMath::Min(Count, FOKFighter::MaxFrameEvents); ++i)
			{
				if (FighterWord(Thread, *Fighter, EventSlot(i) + 4) == Free)
				{
					Slot = i;
					break;
				}
			}
		}
		if (Slot == FOKFighter::MaxFrameEvents)
		{
			return true;
		}
		// Kind 0 with no object makes one in MKKE (FUN_0044DF70); that comes with the hitbox scan.
		const uint32 Script = Raw[4] != 0 ? Raw[4] : Thread.CurrentInstanceValue();
		// The object is kept as a handle {+0x7C, +0x80 object, +0x84 serial}. A scan (kind 0) with none uses the
		// fighter's current CollisionContext (FUN_0044DF70).
		const uint32 Object = (Raw[5] == 0 && Raw[0] == 0) ? Fighter->CollisionContext : Raw[5];
		uint32 Serial = 0;
		if ((Object >> 24) == FOKScriptRuntime::HeapTag)
		{
			Thread.LoadBytes(Object + 4, &Serial, 4);
		}
		const uint32 Base = EventSlot(Slot);
		const uint32 Fields[][2] = {
			{ Base + 0x00, Raw[0] },		// kind
			{ Base + 0x08, Raw[1] },		// from frame (float)
			{ Base + 0x0C, Raw[2] },		// to frame (float)
			{ Base + 0x10, Raw[3] },		// function
			{ Base + 0x14, Script },
			{ Base + 0x78, 0 },
			{ Base + 0x7C, 0 },
			{ Base + 0x80, Object },
			{ Base + 0x84, Serial },
			{ Base + 0x04, 1 },			// state: added, waiting to be armed
		};
		for (const auto& Field : Fields)
		{
			if (!PutFighterWord(Thread, *Fighter, Field[0], Field[1]))
			{
				return false;
			}
		}
		OutSlot = Slot;
		return true;
	}

	bool AddFrameEvent(FOKScriptThread& Thread)
	{
		int32 Slot = 0;
		if (!AddFrameEventSlot(Thread, Slot))
		{
			return false;
		}
		Thread.SetResult(uint32(Slot));
		return true;
	}

	// 137.228 add_armed_frame_event(fighter, kind, from, to, function, script, object) -> slot: add_frame_event,
	// then armed at once for the animation playing now (FUN_008A0390). Was named voice_at_frame.
	bool AddArmedFrameEvent(FOKScriptThread& Thread)
	{
		int32 Slot = 0;
		if (!AddFrameEventSlot(Thread, Slot))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Slot < FOKFighter::MaxFrameEvents)
		{
			const uint32 Now = Fighter->Body ? Fighter->Body->GetAnimId() : 0;		// events belong to the animation playing now
			if (!PutFighterWord(Thread, *Fighter, EventSlot(Slot) + 0x78, Now) || !PutFighterWord(Thread, *Fighter, EventSlot(Slot) + 4, 0))
			{
				return false;
			}
		}
		Thread.SetResult(uint32(Slot));
		return true;
	}

	// 137.229 arm_frame_events(fighter): every added event (state 1) remembers the animation playing now
	// and becomes armed (state 0). Was named launch_attack_anim.
	bool ArmFrameEvents(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		const uint32 Now = Fighter->Body ? Fighter->Body->GetAnimId() : 0;		// events belong to the animation playing now
		const int32 Count = FMath::Min(int32(FighterWord(Thread, *Fighter, FOKFighter::FrameEventCount)), FOKFighter::MaxFrameEvents);
		for (int32 i = 0; i < Count; ++i)
		{
			if (FighterWord(Thread, *Fighter, EventSlot(i) + 4) == 1)
			{
				if (!PutFighterWord(Thread, *Fighter, EventSlot(i) + 0x78, Now) || !PutFighterWord(Thread, *Fighter, EventSlot(i) + 4, 0))
				{
					return false;
				}
			}
		}
		return true;
	}

	// 137.230 set_frame_event_param(fighter, slot, index, value): one of a slot's two parameter words
	// (slot +0x20 / +0x24; FUN_00896460), e.g. the sound a whoosh event plays. Was named random_sound_at_frame.
	bool SetFrameEventParam(FOKScriptThread& Thread)
	{
		uint32 Slot = 0, Index = 0, Value = 0;
		if (!Thread.ArgRaw(1, Slot) || !Thread.ArgRaw(2, Index) || !Thread.ArgRaw(3, Value))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || Slot >= uint32(FOKFighter::MaxFrameEvents) || Index >= 2)
		{
			return true;
		}
		return PutFighterWord(Thread, *Fighter, EventSlot(int32(Slot)) + 0x20 + Index * 4, Value);
	}

	// 137.231 set_frame_event_text(fighter, slot, 0, text): copies a string (under 80 characters, e.g. a
	// voice category) into the slot at +0x28 (FUN_00896490). Was named random_voice_at_frame.
	bool SetFrameEventText(FOKScriptThread& Thread)
	{
		uint32 Slot = 0, Index = 0, Text = 0;
		if (!Thread.ArgRaw(1, Slot) || !Thread.ArgRaw(2, Index) || !Thread.ArgRaw(3, Text))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || Slot >= uint32(FOKFighter::MaxFrameEvents) || Index != 0)
		{
			return true;
		}
		TArray<uint8> Bytes;
		for (uint8 Char = 1; Char != 0 && Bytes.Num() <= 80; )
		{
			if (!Thread.LoadBytes(Text + Bytes.Num(), &Char, 1))
			{
				return false;
			}
			Bytes.Add(Char);
		}
		if (Bytes.Num() > 80)
		{
			return true;		// MKKE ignores strings of 80 characters or more
		}
		return Thread.StoreBytes(Fighter->Handle + EventSlot(int32(Slot)) + 0x28, Bytes.GetData(), Bytes.Num());
	}

	// --- Hit detection (FIGHT-RE-NOTES §7.3) --------------------------------------------------------

	/** A fighter from a raw argument word (setarg-patched arguments hold the handle itself). */
	FOKFighter* RawFighter(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Handle = 0;
		FOKScriptObject* Object = Thread.ArgRaw(Index, Handle) ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		return Object ? Object->AsFighter() : nullptr;
	}

	// 92.1: the MKAnimEventPdata class, for is_a (1.12)
	bool ClassAnimEventPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::AnimEventPdata));
		return true;
	}

	// 79.1: the CollisionContext class, for is_a (1.12)
	bool ClassCollisionContext(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::CollisionContext));
		return true;
	}

	// 23.4 resolve_handle(&handle) -> the object a handle {?, object, serial} refers to while it's alive, else 0
	// (FUN_008C2920). Our heap objects keep their own handle at +4 as the serial.
	bool ResolveHandle(FOKScriptThread& Thread)
	{
		uint32 Address = 0;
		if (!Thread.ArgValue(0, Address))
		{
			return false;
		}
		Thread.SetResult(0);
		uint32 Object = 0, Serial = 0, Actual = 0;
		if (Address == 0 || !Thread.LoadBytes(Address + 4, &Object, 4) || !Thread.LoadBytes(Address + 8, &Serial, 4))
		{
			return Address == 0;
		}
		if ((Object >> 24) == FOKScriptRuntime::InstanceTag)
		{
			Thread.SetResult(Thread.GetRuntime().FindInstanceById(int32(Object & 0x00FFFFFF)) && Serial == Object ? Object : 0);
			return true;
		}
		if ((Object >> 24) != FOKScriptRuntime::HeapTag)
		{
			// Objects without heap memory (processes): their handle is their serial, valid while the object lives.
			// DoFightEffect waits on p_FightEffect this way.
			Thread.SetResult(Serial == Object && Thread.GetRuntime().ResolveHandle(Object) ? Object : 0);
			return true;
		}
		if (!Thread.GetRuntime().ResolveHandle(Object) || !Thread.LoadBytes(Object + 4, &Actual, 4))
		{
			return true;
		}
		Thread.SetResult(Actual == Serial ? Object : 0);
		return true;
	}

	// 52.87 obj_get_pos(obj, outVector) / 52.88: a fighter's or MK object's position into three floats (FUN_00433410)
	bool ObjGetPos(FOKScriptThread& Thread)
	{
		uint32 Out = 0;
		if (!Thread.ArgRaw(1, Out))
		{
			return false;
		}
		uint32 Handle = 0;
		FOKScriptObject* Object = Thread.ArgValue(0, Handle) ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		FOKFighter* Fighter = Object ? Object->AsFighter() : nullptr;
		FOKMkObject* MkObject = Object ? Object->AsMkObject() : nullptr;
		const FVector Where = (Fighter && Fighter->Body) ? Fighter->Body->GetWorldLocation() : MkObject ? MkObject->Location : FVector::ZeroVector;
		const float Vector[3] = { float(Where.X), float(Where.Y), float(Where.Z) };
		return Out == 0 || Thread.StoreBytes(Out, Vector, sizeof(Vector));
	}

	// 81.162 collision_scan_flag(fighter, forward): attack spheres move this far along the facing (PLAYER_COL_INFO
	// +0x190, FUN_008AF400). The scan passes the hitbox's reach.
	bool CollisionScanFlag(FOKScriptThread& Thread)
	{
		uint32 Forward = 0;
		if (!Thread.ArgRaw(1, Forward))
		{
			return false;
		}
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			Fighter->CollisionForward = BitsToFloat(Forward);
		}
		return true;
	}

	// 137.152 can_hit(attacker, victim, hitClass, ?) -> 0 when the victim can be hit (FUN_0089E220 filters on the
	// victim's state code and flags; that comes with hit reactions). Was named ground_pound.
	bool CanHit(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 7.654 is_online -> 0: no online play
	bool IsOnline(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	const FOKCollisionRow* FindCollisionRow(const FOKFighter& Fighter, int32 BoneId)
	{
		return Fighter.CollisionRows.FindByPredicate([BoneId](const FOKCollisionRow& Row) { return Row.BoneId == BoneId; });
	}

	// 137.92 check_attack_collision(fighter, limb, radiusScale, reach, noGapFill): builds the attack spheres
	// (FUN_008B0360 -> FUN_008AFF90). One sphere per bone of the limb's chains, moved along the facing by
	// collision_scan_flag's distance; after each chain one more sphere Reach past its last bone, along the limb
	// (the fist or foot tip). Then, unless noGapFill, a sphere halfway to where each one was last frame when it
	// moved more than twice its radius, so a fast limb can't pass through a body between frames.
	bool CheckAttackCollision(FOKScriptThread& Thread)
	{
		uint32 Limb = 0, RadiusScale = 0, Reach = 0, NoGapFill = 0;
		if (!Thread.ArgRaw(1, Limb) || !Thread.ArgRaw(2, RadiusScale) || !Thread.ArgRaw(3, Reach) || !Thread.ArgRaw(4, NoGapFill))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || !Fighter->Body)
		{
			return true;
		}
		const float Scale = BitsToFloat(RadiusScale);
		const float Beyond = BitsToFloat(Reach);
		TArray<FOKCollisionSphere>& Spheres = Fighter->AttackSpheres;
		Spheres.Reset();
		const FVector Forward = Fighter->Body->GetFacing() * Fighter->CollisionForward;
		const TConstArrayView<int32> Bones = OKCollision::LimbBones(int32(Limb));
		TArray<int32, TInlineAllocator<8>> Chain;		// this chain's spheres
		const FOKCollisionRow* LastRow = nullptr;
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			if (Bones[i] < 0)
			{
				Chain.Reset();
				continue;
			}
			const FOKCollisionRow* Row = FindCollisionRow(*Fighter, Bones[i]);
			const TCHAR* Name = OKCollision::BoneName(Bones[i]);
			FVector Where = FVector::ZeroVector;
			if (Row && Name && Spheres.Num() < OKCollision::MaxAttackSpheres && Fighter->Body->GetBoneWorldLocation(Name, Where))
			{
				Chain.Add(Spheres.Add({ Row->BoneId, Where + Forward, Row->AttackRadius * Scale }));
				LastRow = Row;
			}
			const bool bChainEnds = i + 1 == Bones.Num() || Bones[i + 1] < 0;
			if (bChainEnds && Beyond != 0.0f && Chain.Num() >= 2 && LastRow && Spheres.Num() < OKCollision::MaxAttackSpheres)
			{
				const FVector Before = Spheres[Chain[Chain.Num() - 2]].Center;
				const FVector Last = Spheres[Chain.Last()].Center;
				Spheres.Add({ LastRow->BoneId, Last + (Last - Before).GetSafeNormal() * Beyond, LastRow->AttackRadius * Scale });
			}
		}

		// On the first frame of a scan there is nothing to fill from (MKKE copies the new spheres over the old).
		if (NoGapFill == 0 && Fighter->PreviousAttackSpheres.Num() > 0)
		{
			const int32 Count = FMath::Min(Spheres.Num(), Fighter->PreviousAttackSpheres.Num());
			for (int32 i = 0; i < Count && Spheres.Num() < OKCollision::MaxAttackSpheres; ++i)
			{
				const FOKCollisionSphere Now = Spheres[i];
				const FVector Before = Fighter->PreviousAttackSpheres[i].Center;
				if (FVector::Dist(Now.Center, Before) > Now.Radius * 2.0f)
				{
					Spheres.Add({ Now.BoneId, (Now.Center + Before) * 0.5, Now.Radius });
				}
			}
		}
		return true;
	}

	// 137.93 check_attack_collision_simple(fighter) -> 1 if one of its attack spheres touches the opponent's hurtboxes
	// (FUN_008B0CD0). The contact is recorded and the spheres are used up.
	bool CheckAttackCollisionSimple(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKFighter* Victim = Fighter ? Fighter->Opponent : nullptr;
		if (!Fighter || !Fighter->Body || !Victim || !Victim->Body)
		{
			return true;
		}
		const FVector Axis = (Victim->Body->GetWorldLocation() - Fighter->Body->GetWorldLocation()).GetSafeNormal2D();
		for (const FOKCollisionSphere& Attack : Fighter->AttackSpheres)
		{
			for (const FOKCollisionRow& Row : Victim->CollisionRows)
			{
				const TCHAR* Name = OKCollision::BoneName(Row.BoneId);
				FVector Where = FVector::ZeroVector;
				if (Row.HurtRadius <= 0.0f || !Name || !Victim->Body->GetBoneWorldLocation(Name, Where))
				{
					continue;
				}
				if (OKCollision::Touch(Attack, { Row.BoneId, Where, Row.HurtRadius }, Axis))
				{
					Fighter->LastContact = { Attack.BoneId, Row.BoneId, Attack.Center, Thread.GetRuntime().GetSimFrame() };
					Fighter->AttackSpheres.Reset();
					Thread.SetResult(1);
					return true;
				}
			}
		}
		return true;
	}

	/**
	 * XferToReaction (FUN_0089ECC0): the victim's own script thread drops what it was doing and continues at the
	 * reaction function from its next tick. A fighter that has no thread yet gets one.
	 */
	bool XferToReaction(FOKScriptRuntime& Runtime, FOKFighter& Victim, FOKScriptInstance& Script, int32 Function)
	{
		FOKScriptObject* Object = Runtime.ResolveHandle(Victim.MainThread);
		FOKScriptThread* Main = Object ? Object->AsThread() : nullptr;
		if (Main && Main->IsAlive())
		{
			return Main->TransferTo(Script, Function);
		}
		FString Error;
		Main = Runtime.StartProcess(*Script.File, Function, 0, 14, true, Error);
		if (!Main)
		{
			UE_LOG(LogOKVM, Warning, TEXT("XferToReaction: %s"), *Error);
			return false;
		}
		Main->BindFighter(Victim);
		Victim.MainThread = Main->GetHandle();
		return true;
	}

	// 137.90 collision_scan(attacker, victim, context): resolves a contact (OKResolveContact)
	bool CollisionScan(FOKScriptThread& Thread)
	{
		uint32 Context = 0;
		if (!Thread.ArgRaw(2, Context))
		{
			return false;
		}
		FOKFighter* Attacker = Thread.ArgFighter(0);
		FOKFighter* Victim = RawFighter(Thread, 1);
		if (Attacker && Victim)
		{
			OKResolveContact(Thread.GetRuntime(), *Attacker, *Victim, Context);
		}
		return true;
	}

	// 6.251 last_contact_hit(source, mode), 6.253 last_contact_blocked, 6.252 last_contact_missed: the outcome of the last contact
	// (FUN_00451150). Source 0 = the running script's fighter's current CollisionContext, otherwise a context.
	// [inf] mode adds a check that only matters online.
	bool ContactOutcomeIs(FOKScriptThread& Thread, uint32 Wanted)
	{
		uint32 Context = 0;
		if (!Thread.ArgRaw(0, Context))
		{
			return false;
		}
		if (Context == 0)
		{
			FOKFighter* Me = Thread.MyFighter();
			Context = Me ? Me->CollisionContext : 0;
		}
		uint32 Outcome = 5;
		const FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Context);
		if (Object && Object->IsA(EOKScriptClass::CollisionContext) && !Thread.LoadBytes(Context + FOKCollisionContext::Outcome, &Outcome, 4))
		{
			return false;
		}
		Thread.SetResult(Object && Outcome == Wanted ? 1 : 0);
		return true;
	}

	bool LastContactHit(FOKScriptThread& Thread) { return ContactOutcomeIs(Thread, 0); }
	bool LastContactBlocked(FOKScriptThread& Thread) { return ContactOutcomeIs(Thread, 1); }
	bool LastContactMissed(FOKScriptThread& Thread) { return ContactOutcomeIs(Thread, 2); }

	// 137.232 cancel_frame_events(fighter, kind): frees every armed frame event of that kind (FUN_00896600); the
	// scan uses it to stop scanning after a one-hit attack connects. Was named frame_events_flag.
	bool CancelFrameEvents(FOKScriptThread& Thread)
	{
		uint32 Kind = 0;
		if (!Thread.ArgRaw(1, Kind))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		const int32 Count = FMath::Min(int32(FighterWord(Thread, *Fighter, FOKFighter::FrameEventCount)), FOKFighter::MaxFrameEvents);
		for (int32 i = 0; i < Count; ++i)
		{
			if (FighterWord(Thread, *Fighter, EventSlot(i) + 4) == 0 && FighterWord(Thread, *Fighter, EventSlot(i)) == Kind
				&& !PutFighterWord(Thread, *Fighter, EventSlot(i) + 4, 2))
			{
				return false;
			}
		}
		return true;
	}

	// 6.67 vm_self -> the running process (FUN_004601B0); sound helpers pass it on
	bool VmSelf(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetHandle());
		return true;
	}

	// 43.41 (also 136.58, 248.14, 252.15): an object's virtual +0x30. On GGameInfo it returns the callback table
	// sounds are played through (SoundEngine.play_*_sound), which comes with audio: 0 for now. [unk]
	bool ObjectCallbackTable(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	/**
	 * GameInfo's callback tables (virtuals +0x2C..+0x3C, FUN_00810D50 and its neighbours): each is a global of one
	 * engine script, found by name once and cached in GameInfo. A table is {script, function numbers...}, and the
	 * shared scripts call through it with callref (FightEngineScript.exit_to_stance -> FGFightEngineScript).
	 * [inf] MKKE takes each script from a GameInfo handle (+0x88, +0x118, +0x148, +0x130, and 0xE89C04 for
	 * g_moving_tbl); OpenKombat names the script that defines the global.
	 */
	uint32 CallbackTable(FOKScriptThread& Thread, const TCHAR* ScriptName, const TCHAR* GlobalName, uint32 CacheOffset)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		uint32 Info = 0, Cached = 0;
		if (Thread.ArgValue(0, Info) && Runtime.LoadHeapWord(Info + CacheOffset, Cached) && Cached != 0)
		{
			return Cached;
		}
		const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
		const FOKMkoFile* File = (Scripts && Scripts->GetLibrary()) ? Scripts->GetLibrary()->FindScript(FString(ScriptName) + TEXT(".mko")) : nullptr;
		FOKScriptInstance* Instance = File ? Runtime.GetInstance(*File) : nullptr;
		if (!Instance)
		{
			return 0;
		}
		uint32 Hash = 0x811C9DC5;		// FNV-1, as FindGlobal (FUN_00458620) hashes names
		for (const TCHAR* C = GlobalName; *C; ++C)
		{
			Hash = (Hash * 0x01000193) ^ uint8(*C);
		}
		for (const FOKMkoGlobal& Global : File->GetGlobals())
		{
			if (Global.NameHash == Hash)
			{
				const uint32 Table = FOKScriptRuntime::MemoryAddress(Instance->Id, Instance->PoolBase + Global.PoolOffset);
				Runtime.SetHeapWord(Info + CacheOffset, Table);
				return Table;
			}
		}
		return 0;
	}

	bool FightEngineTable(FOKScriptThread& Thread)
	{
		Thread.SetResult(CallbackTable(Thread, TEXT("FGFightEngineScript"), TEXT("g_fight_engine_tbl"), 0x82C8));
		return true;
	}
	bool SoundTable(FOKScriptThread& Thread)
	{
		Thread.SetResult(CallbackTable(Thread, TEXT("FGSoundEngine"), TEXT("g_sound_tbl"), 0x82CC));
		return true;
	}
	bool CameraTable(FOKScriptThread& Thread)
	{
		Thread.SetResult(CallbackTable(Thread, TEXT("FGCameraEngine"), TEXT("g_camera_tbl"), 0x82D0));
		return true;
	}
	bool EffectsTable(FOKScriptThread& Thread)
	{
		Thread.SetResult(CallbackTable(Thread, TEXT("FGEffectsEngine"), TEXT("g_effects_tbl"), 0x82D4));
		return true;
	}
	bool MovingTable(FOKScriptThread& Thread)
	{
		Thread.SetResult(CallbackTable(Thread, TEXT("Moving"), TEXT("g_moving_tbl"), 0x82D8));
		return true;
	}

	// 81.167 current_collision_context(fighter) -> the fighter's current CollisionContext (FUN_0044DF70). Was named
	// attack_result_state.
	bool CurrentCollisionContext(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? Fighter->CollisionContext : 0);
		return true;
	}

	// 81.182 register_miss(fighter, context): an attack that ended without touching anything (outcome still 5)
	// becomes a miss, outcome 2 (FUN_00447190); last_contact_missed (6.252) reads it.
	bool RegisterMiss(FOKScriptThread& Thread)
	{
		uint32 Context = 0, Outcome = 0;
		if (!Thread.ArgRaw(1, Context))
		{
			return false;
		}
		const FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Context);
		if (!Object || !Object->IsA(EOKScriptClass::CollisionContext) || !Thread.LoadBytes(Context + FOKCollisionContext::Outcome, &Outcome, 4))
		{
			return true;
		}
		const uint32 Missed = 2;
		return Outcome != 5 || Thread.StoreBytes(Context + FOKCollisionContext::Outcome, &Missed, 4);
	}

	// Audio (module 193 = FMOD, and the sound events): nothing plays yet. They return 0.
	bool Silent(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 193.2 play_sound(vm, sound, kind, obj, ...) and 193.1 (the same; voices pass kind 1) (the VM's virtual +0x20): an FMOD
	// event. sound is an asset reference, group/event (snd_fightfx1/sfx_hit_face); a voice or foley names only the event,
	// which is found in the banks of obj, the fighter making it (W6). [inf] The returned sound handle isn't kept: 0.
	bool PlaySound(FOKScriptThread& Thread)
	{
		uint32 Value = 0;
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const FOKMkoAsset* Asset = Thread.ArgValue(1, Value) ? Runtime.FindAsset(Value) : nullptr;
		FOKFighter* Owner = RawFighter(Thread, 3);
		if (!Owner)
		{
			Owner = Thread.ArgFighter(3);
		}
		if (!Asset && (Value >> 24) >= FOKScriptRuntime::FirstMemoryTag)
		{
			// SoundEngine's voice and foley tables hold event names as strings (vo_atk_hit, snd_foley_short): the owner's
			FString Event;
			for (uint32 Char = 0; Event.Len() < 64 && Runtime.LoadAnyWord(Value + Event.Len(), Char) && (Char & 0xFF) != 0;)
			{
				Event.AppendChar(TCHAR(Char & 0xFF));
			}
			FOKAudio::Get().Play(FString(), Event, Owner ? Owner->Name : FString());
		}
		if (Asset)
		{
			FOKAudio::Get().Play(Asset->Set, Asset->Object, Owner ? Owner->Name : FString());
		}
		Thread.SetResult(0);
		return true;
	}

	// --- Reactions (SharedReactions) ----------------------------------------------------------

	// 59.41 anim_frame_count(obj) -> float: the playing sequence's frame count (FUN_00442330)
	bool AnimFrameCount(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(FloatToBits((Fighter && Fighter->Body) ? float(Fighter->Body->GetAnimFrameCount()) : 0.0f));
		return true;
	}

	// 81.61 set_move_flag(obj, value): fighter +0x2CC, and +0x2D0 = the game tick + 10 as a float (FUN_00446FE0)
	bool SetMoveFlag(FOKScriptThread& Thread)
	{
		uint32 Value = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Value) || !Fighter)
		{
			return Fighter == nullptr;
		}
		const float Until = float(Thread.GetRuntime().GetSimFrame() + 10);
		return Thread.StoreBytes(Fighter->Handle + FOKFighter::MoveFlag, &Value, 4)
			&& Thread.StoreBytes(Fighter->Handle + FOKFighter::MoveFlag + 4, &Until, 4);
	}

	// 81.185 set_state(obj, state): the old state goes to +0x2F9C (FUN_00446B60)
	bool SetState(FOKScriptThread& Thread)
	{
		uint32 Value = 0, Old = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Value) || !Fighter)
		{
			return Fighter == nullptr;
		}
		return Thread.LoadBytes(Fighter->Handle + FOKFighter::State, &Old, 4)
			&& Thread.StoreBytes(Fighter->Handle + FOKFighter::PreviousState, &Old, 4)
			&& Thread.StoreBytes(Fighter->Handle + FOKFighter::State, &Value, 4);
	}

	// 81.214 disable_commands(obj, command): clears a command slot's enabled word. Commands are numbered
	// 0x80000000 + slot; anything else means slot 0 (FUN_0044F5A0).
	bool DisableCommands(FOKScriptThread& Thread)
	{
		uint32 Command = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Command) || !Fighter)
		{
			return Fighter == nullptr;
		}
		const uint32 Slot = Command + 0x80000000u < uint32(FOKFighter::MaxCommands) ? Command + 0x80000000u : 0;
		const uint32 Zero = 0;
		return Thread.StoreBytes(Fighter->Handle + FOKFighter::Commands + Slot * FOKFighter::CommandBytes, &Zero, 4);
	}

	// 118.45 team_life(info) -> float: the active fighter's life (+0x78C0), plus the tag partner's when it's in
	// the fight (FUN_00894290). No player record: 1. One fighter a side here.
	bool TeamLife(FOKScriptThread& Thread)
	{
		uint32 Info = 0;
		FOKScriptObject* Object = Thread.ArgValue(0, Info) ? Thread.GetRuntime().ResolveHandle(Info) : nullptr;
		FOKPlayer* Player = Object ? Object->AsPlayer() : nullptr;
		Thread.SetResult(FloatToBits((Player && Player->Fighter) ? Player->Fighter->Health : 1.0f));
		return true;
	}

	// 78.88 adjust_damage_multiplier(info, scale): info +0xB60 *= scale, but not below 0.01 (FUN_004464A0)
	bool AdjustDamageMultiplier(FOKScriptThread& Thread)
	{
		uint32 Info = 0, Scale = 0;
		if (!Thread.ArgValue(0, Info) || !Thread.ArgRaw(1, Scale))
		{
			return false;
		}
		FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Info);
		if (!Object || !Object->AsPlayer())
		{
			return true;
		}
		float Multiplier = 0.0f;
		if (!Thread.LoadBytes(Info + FOKPlayer::DamageMultiplier, &Multiplier, 4))
		{
			return false;
		}
		Multiplier = FMath::Max(Multiplier * BitsToFloat(Scale), 0.01f);
		return Thread.StoreBytes(Info + FOKPlayer::DamageMultiplier, &Multiplier, 4);
	}

	// 81.1: the MKCharacterObj class, for is_a (1.12). Fighters claim it. The blood-effect helpers that test for it then
	// add the damage to floats behind the opponent's +0x7638 (its player record, +0xBD0) and +0x2FD0 (FOKTallies, +0x48),
	// and tint the blood (FGEffectsEngine.game_set_blood_tint).
	bool ClassCharacterObj(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::CharacterObj));
		return true;
	}

	// 137.1: the MKFGCharacterObj class, for is_a. Fighters claim it.
	bool ClassFGCharacterObj(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::FGCharacterObj));
		return true;
	}

	// 59.1 (named get_teleport_obj): the MKSkeletalObj class, for is_a (FUN_008C7830). Fighters claim it; skeletal props
	// (spawn_skel_actor) aren't made yet.
	bool ClassSkeletalObj(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::SkeletalObj));
		return true;
	}

	// 23.3 destroy_handle(&handle): destroys the object the handle refers to (virtual +4), then releases the handle
	// (FUN_0042D7D0). A process is killed; other objects stay until the runtime frees them.
	bool DestroyHandle(FOKScriptThread& Thread)
	{
		uint32 Address = 0, Object = 0, Serial = 0, Actual = 0;
		if (!Thread.ArgValue(0, Address))
		{
			return false;
		}
		if (Address != 0 && Thread.LoadBytes(Address + 4, &Object, 4) && Thread.LoadBytes(Address + 8, &Serial, 4))
		{
			FOKScriptObject* Target = Thread.GetRuntime().ResolveHandle(Object);
			const bool bAlive = Target && ((Object >> 24) != FOKScriptRuntime::HeapTag || (Thread.LoadBytes(Object + 4, &Actual, 4) && Actual == Serial));
			if (bAlive && Target->AsThread() && Target->AsThread() != &Thread)
			{
				Target->AsThread()->KillNow(TEXT("destroyed through a handle (23.3)"));
			}
		}
		const uint32 Empty[2] = { 0, 0xFFFFFFFFu };
		return Address == 0 || Thread.StoreBytes(Address + 4, Empty, sizeof(Empty));
	}

	// 23.5 set_handle(&handle, obj): {object, serial}; our heap objects' serial is their word at +4, others' their value (FUN_0042D780)
	bool SetHandle(FOKScriptThread& Thread)
	{
		uint32 Address = 0, Object = 0, Serial = 0;
		if (!Thread.ArgValue(0, Address) || !Thread.ArgRaw(1, Object))
		{
			return false;
		}
		Serial = Object;
		if ((Object >> 24) == FOKScriptRuntime::HeapTag && !Thread.LoadBytes(Object + 4, &Serial, 4))
		{
			return false;
		}
		const uint32 Handle[2] = { Object, Object != 0 ? Serial : 0xFFFFFFFFu };
		return Address == 0 || Thread.StoreBytes(Address + 4, Handle, sizeof(Handle));
	}

	// 23.6 release_handle(&handle): object 0, serial -1 (FUN_0042D650)
	bool ReleaseHandle(FOKScriptThread& Thread)
	{
		uint32 Address = 0;
		if (!Thread.ArgValue(0, Address))
		{
			return false;
		}
		const uint32 Empty[2] = { 0, 0xFFFFFFFFu };
		return Address == 0 || Thread.StoreBytes(Address + 4, Empty, sizeof(Empty));
	}

	// Natives that return 0 until their system exists: effects, the camera, rumble, AI events, face animation.
	bool ReturnZero(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	bool ReturnOne(FOKScriptThread& Thread)
	{
		Thread.SetResult(1);
		return true;
	}

	// --- The character bootstrap: moveset, command records, *_game_init ----------------------------

	/** The moveset the loader is filling (OKInitCharacter); 0 at run time, when the moveset natives are stubs. */
	uint32 GBuildingMoveset = 0;

	/** The moveset natives (FUN_008ECAB0's table): native index, row, whether the call carries four foley pairs. */
	const int32 MovesetNatives[][3] = { { 532, 0, 0 },{ 536, 1, 0 },{ 537, 2, 0 },{ 538, 3, 0 },{ 539, 4, 0 },{ 540, 5, 0 },{ 541, 6, 0 },{ 542, 7, 0 },{ 543, 8, 0 },{ 544, 9, 0 },{ 545, 10, 0 },{ 546, 11, 0 },{ 547, 12, 0 },{ 548, 13, 0 },{ 549, 14, 0 },{ 550, 15, 0 },{ 551, 16, 0 },{ 552, 17, 0 },{ 553, 18, 0 },{ 554, 19, 0 },{ 555, 20, 0 },{ 556, 21, 0 },{ 557, 22, 0 },{ 558, 23, 0 },{ 559, 24, 0 },{ 560, 25, 0 },{ 561, 26, 0 },{ 562, 27, 0 },{ 563, 28, 0 },{ 564, 29, 0 },{ 565, 30, 0 },{ 566, 31, 0 },{ 567, 32, 0 },{ 568, 33, 0 },{ 569, 34, 0 },{ 570, 35, 0 },{ 571, 36, 0 },{ 572, 37, 0 },{ 573, 38, 0 },{ 574, 39, 0 },{ 575, 40, 0 },{ 576, 41, 0 },{ 577, 42, 0 },{ 578, 43, 0 },{ 579, 44, 0 },{ 580, 45, 0 },{ 581, 46, 0 },{ 489, 1, 1 },{ 490, 3, 1 },{ 491, 4, 1 },{ 492, 5, 1 },{ 493, 6, 1 },{ 494, 7, 1 },{ 495, 8, 1 },{ 496, 9, 1 },{ 497, 10, 1 },{ 498, 11, 1 },{ 499, 12, 1 },{ 500, 13, 1 },{ 501, 14, 1 },{ 502, 15, 1 },{ 503, 16, 1 },{ 504, 17, 1 },{ 505, 18, 1 },{ 506, 19, 1 },{ 507, 20, 1 },{ 508, 22, 1 },{ 509, 23, 1 },{ 510, 24, 1 },{ 511, 25, 1 },{ 512, 26, 1 },{ 513, 27, 1 },{ 514, 28, 1 },{ 515, 30, 1 },{ 516, 31, 1 },{ 517, 32, 1 },{ 518, 33, 1 },{ 519, 34, 1 },{ 520, 35, 1 },{ 521, 36, 1 },{ 522, 37, 1 },{ 523, 38, 1 },{ 524, 39, 1 },{ 525, 40, 1 },{ 526, 41, 1 },{ 527, 42, 1 },{ 528, 43, 1 },{ 529, 44, 1 },{ 530, 45, 1 },{ 531, 46, 1 },{ 533, 49, 0 } };

	// 7.489-7.533, 7.536-7.581 moveset_<slot>(anim, rate, rootScale [, 4 x {foley kind, frame}]): the loader
	// copies the call's raw operands into the row; foley kinds default to 21 (none). At run time each is a stub.
	bool MovesetEntry(FOKScriptThread& Thread)
	{
		if (GBuildingMoveset == 0)
		{
			return true;
		}
		const int32 Index = int32(Thread.GetNativeId() & 0xFFFF);
		const int32* Entry = nullptr;
		for (const auto& Candidate : MovesetNatives)
		{
			if (Candidate[0] == Index)
			{
				Entry = Candidate;
				break;
			}
		}
		if (!Entry)
		{
			return true;
		}
		uint32 Row[11] = {};
		for (int32 i = 0; i < 3; ++i)
		{
			if (!Thread.ArgRaw(i, Row[i]))
			{
				return false;
			}
		}
		const uint32 RowAddress = GBuildingMoveset + FOKMoveset::Rows + Entry[1] * FOKMoveset::RowBytes;
		if (Entry[2] == 0)
		{
			// The short form sets the four kinds to "none" and leaves the frames as they were.
			bool bOk = Thread.StoreBytes(RowAddress, Row, 12);
			for (int32 i = 0; i < 4; ++i)
			{
				bOk &= Thread.StoreBytes(RowAddress + 12 + i * 8, &FOKMoveset::NoFoley, 4);
			}
			return bOk;
		}
		for (int32 i = 3; i < 11; ++i)
		{
			if (!Thread.ArgRaw(i, Row[i]))
			{
				return false;
			}
		}
		return Thread.StoreBytes(RowAddress, Row, sizeof(Row));
	}

	// 7.534 moveset_alt_stance(anim, rate, rootScale, ?): up to 6 alternate stances
	bool MovesetAltStance(FOKScriptThread& Thread)
	{
		if (GBuildingMoveset == 0)
		{
			return true;
		}
		uint32 Count = 0;
		uint32 Stance[4] = {};
		if (!Thread.LoadBytes(GBuildingMoveset + FOKMoveset::AltStanceCount, &Count, 4))
		{
			return false;
		}
		if (Count >= uint32(FOKMoveset::MaxAltStances))
		{
			return true;
		}
		for (int32 i = 0; i < 4; ++i)
		{
			if (!Thread.ArgRaw(i, Stance[i]))
			{
				return false;
			}
		}
		const uint32 NewCount = Count + 1;
		return Thread.StoreBytes(GBuildingMoveset + FOKMoveset::AltStances + Count * FOKMoveset::AltStanceBytes, Stance, sizeof(Stance))
			&& Thread.StoreBytes(GBuildingMoveset + FOKMoveset::AltStanceCount, &NewCount, 4);
	}

	// 7.535: marks the moveset as using a secondary skeleton (+0x29F0; Jade's)
	bool MovesetSecondary(FOKScriptThread& Thread)
	{
		const uint32 One = 1;
		return GBuildingMoveset == 0 || Thread.StoreBytes(GBuildingMoveset + FOKMoveset::SecondaryFlag, &One, 4);
	}

	/** A fighter's moveset (fighter virtual +0x1A8 = FUN_00893160): +0x518, or +0x7F5C in its second style. */
	uint32 MovesetOf(FOKScriptRuntime& Runtime, const FOKFighter* Fighter)
	{
		uint32 Flags = 0;
		if (!Fighter || !Runtime.LoadHeapWord(Fighter->Handle + FOKFighter::Flags, Flags))
		{
			return 0;
		}
		return Fighter->Handle + ((Flags & FOKFighter::SecondaryMovesetFlag) ? FOKFighter::SecondaryMoveset : FOKFighter::Moveset);
	}

	// 137.95 / 81.136 moveset(obj) -> the fighter's moveset, whose rows scripts read by offset
	bool Moveset(FOKScriptThread& Thread)
	{
		Thread.SetResult(MovesetOf(Thread.GetRuntime(), Thread.ArgFighter(0)));
		return true;
	}

	// 81.138 moveset_anim_flag(obj) -> moveset +0x29F0 != 0: its animations play on a second skeleton (FUN_00446DD0)
	bool MovesetAnimFlag(FOKScriptThread& Thread)
	{
		const uint32 Moveset = MovesetOf(Thread.GetRuntime(), Thread.ArgFighter(0));
		uint32 Flag = 0;
		if (Moveset != 0 && !Thread.LoadBytes(Moveset + FOKMoveset::SecondaryFlag, &Flag, 4))
		{
			return false;
		}
		Thread.SetResult(Flag != 0 ? 1 : 0);
		return true;
	}

	/** Writes {offset, value} pairs into a command record. */
	bool StoreFields(FOKScriptThread& Thread, uint32 Record, std::initializer_list<TPair<uint32, uint32>> Fields)
	{
		for (const TPair<uint32, uint32>& Field : Fields)
		{
			if (!Thread.StoreBytes(Record + Field.Key, &Field.Value, 4))
			{
				return false;
			}
		}
		return true;
	}

	/** A command record's address (argument 0, an object operand) and its raw arguments 1..Count. */
	bool CommandArgs(FOKScriptThread& Thread, int32 Count, uint32& OutRecord, uint32 (&OutArgs)[8])
	{
		if (!Thread.ArgValue(0, OutRecord))
		{
			return false;
		}
		for (int32 i = 1; i <= Count; ++i)
		{
			if (!Thread.ArgRaw(i, OutArgs[i]))
			{
				return false;
			}
		}
		return OutRecord != 0;
	}

	/** Copies a script string (under 64 characters) into a command record. */
	bool StoreName(FOKScriptThread& Thread, uint32 To, uint32 From)
	{
		uint8 Name[0x40] = {};
		for (int32 i = 0; From != 0 && i < 0x3F; ++i)
		{
			if (!Thread.LoadBytes(From + i, &Name[i], 1))
			{
				return false;
			}
			if (Name[i] == 0)
			{
				break;
			}
		}
		return Thread.StoreBytes(To, Name, sizeof(Name));
	}

	// 81.215 bind_command(obj, &command, script, function): resets the record (FUN_00449DF0), binds its function
	// and adds it to the fighter's command list, up to 50 (FUN_0044F220)
	bool BindCommand(FOKScriptThread& Thread)
	{
		uint32 Record = 0, Script = 0, Function = 0;
		if (!Thread.ArgRaw(1, Record) || !Thread.ArgRaw(2, Script) || !Thread.ArgRaw(3, Function))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 Count = 0;
		if (!Fighter || Record == 0 || !Thread.LoadBytes(Fighter->Handle + FOKFighter::CommandCount, &Count, 4))
		{
			return Fighter == nullptr || Record == 0;
		}
		if (Count >= uint32(FOKFighter::MaxCommandList))
		{
			return true;
		}
		const bool bReset = StoreFields(Thread, Record, {
			{ 0x00, 0 }, { 0x04, 0 }, { 0x1C, 0 }, { 0x20, 0xFFFFFFFFu }, { 0x30, 0 }, { 0x38, 0 }, { 0x3C, 0 }, { 0x40, 0 },
			{ 0x44, 0 }, { 0x48, 0 }, { 0x4C, 1 }, { 0x54, 0 }, { 0x58, 0 }, { 0x5C, 1 }, { 0x60, 0 }, { 0x64, 0 },
			{ OKCommand::Enabled, 0 } });
		const uint32 NewCount = Count + 1;
		return bReset
			&& StoreFields(Thread, Record, { { OKCommand::Script + 4, Script }, { OKCommand::Script + 8, Script }, { OKCommand::Function, Function } })
			&& Thread.StoreBytes(Fighter->Handle + FOKFighter::CommandList + Count * 4, &Record, 4)
			&& Thread.StoreBytes(Fighter->Handle + FOKFighter::CommandCount, &NewCount, 4);
	}

	// 66.2 command_init(&cmd): constructs a command record (FUN_0044F910). +8 = 1 marks it usable (the matchers want
	// it), then the reset bind_command also does (FUN_00449DF0). Every script's __global__ runs it on its records.
	bool CommandInit(FOKScriptThread& Thread)
	{
		uint32 R = 0;
		if (!Thread.ArgValue(0, R) || R == 0)
		{
			return R == 0;
		}
		return StoreFields(Thread, R, {
			{ 0x00, 0 }, { 0x04, 0 }, { 0x08, 1 }, { 0x0C, 0 }, { 0x10, 0 }, { 0x11C, 0 },
			{ 0x1C, 0 }, { 0x20, 0xFFFFFFFFu }, { 0x30, 0 }, { 0x34, 0 }, { 0x38, 0 }, { 0x3C, 0 }, { 0x40, 0 }, { 0x44, 0 },
			{ 0x48, 0 }, { 0x4C, 1 }, { 0x54, 0 }, { 0x58, 0 }, { 0x5C, 1 }, { 0x60, 0 }, { 0x64, 0 }, { OKCommand::Enabled, 0 } });
	}

	// 28.4 (obj, a, b, c, d): sets four words, a vector or colour (FUN_008C2F50)
	bool SetFour(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 4, R, A) && StoreFields(Thread, R, { { 0, A[1] }, { 4, A[2] }, { 8, A[3] }, { 12, A[4] } });
	}

	// 147.5 (obj): zeroes six words (FUN_008F0500)
	bool ZeroSix(FOKScriptThread& Thread)
	{
		uint32 R = 0;
		return Thread.ArgValue(0, R) && (R == 0 || StoreFields(Thread, R, { { 0, 0 }, { 4, 0 }, { 8, 0 }, { 12, 0 }, { 16, 0 }, { 20, 0 } }));
	}

	// 66.10 register_special(&cmd, button, flag, sequence, window, meterFlag, meterCost): type 0 (FUN_0044A0A0)
	bool RegisterSpecial(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 6, R, A) && StoreFields(Thread, R, { { OKCommand::Type, 0 }, { OKCommand::Button, A[1] }, { 0x80, 0 },
			{ OKCommand::Flag, A[2] }, { OKCommand::Sequence, A[3] }, { OKCommand::Extra, 0 }, { 0x90, 0 }, { OKCommand::Enabled, 1 },
			{ OKCommand::Window, A[4] }, { OKCommand::MeterFlag, A[5] }, { OKCommand::MeterCost, A[6] } });
	}

	// 66.11 register_special_dist(&cmd, extra, button, flag, sequence, window, meterFlag, meterCost): type 1 (FUN_0044A100)
	bool RegisterSpecialDist(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 7, R, A) && StoreFields(Thread, R, { { OKCommand::Enabled, 1 }, { OKCommand::Type, 1 },
			{ OKCommand::Button, A[2] }, { OKCommand::Extra, A[1] }, { OKCommand::Flag, A[3] }, { OKCommand::Sequence, A[4] }, { 0x90, 0 },
			{ 0x80, 0 }, { OKCommand::Window, A[5] }, { OKCommand::MeterFlag, A[6] }, { OKCommand::MeterCost, A[7] } });
	}

	// 66.12 register_ex_special(&cmd, button, flag, sequence, window, meterCost, +0x80): type 4 (FUN_0044A170)
	bool RegisterExSpecial(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 6, R, A) && StoreFields(Thread, R, { { OKCommand::Button, A[1] }, { OKCommand::Flag, A[2] },
			{ 0x80, A[6] }, { OKCommand::Extra, 0 }, { 0x90, 0 }, { OKCommand::Enabled, 1 }, { OKCommand::Type, 4 },
			{ OKCommand::Sequence, A[3] }, { OKCommand::Window, A[4] }, { OKCommand::MeterFlag, 0 }, { OKCommand::MeterCost, A[5] } });
	}

	// 66.13 register_ex_special_dist(&cmd, extra, button, flag, sequence, window, meterCost, +0x80): type 5 (FUN_0044A290)
	bool RegisterExSpecialDist(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 7, R, A) && StoreFields(Thread, R, { { OKCommand::Button, A[2] }, { OKCommand::Extra, A[1] },
			{ OKCommand::Flag, A[3] }, { 0x80, A[7] }, { 0x90, 0 }, { OKCommand::Enabled, 1 }, { OKCommand::Type, 5 },
			{ OKCommand::Sequence, A[4] }, { OKCommand::Window, A[5] }, { OKCommand::MeterFlag, 0 }, { OKCommand::MeterCost, A[6] } });
	}

	/** 66.15 / 66.16: (&cmd, minDistance, maxDistance, button, flag, sequence, window) (FUN_0044A360 / FUN_0044A3D0) */
	bool RegisterFinisher(FOKScriptThread& Thread, uint32 Type)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 6, R, A) && StoreFields(Thread, R, { { OKCommand::Flag, A[4] }, { OKCommand::MinDistance, A[1] },
			{ OKCommand::Button, A[3] }, { OKCommand::Sequence, A[5] }, { OKCommand::MaxDistance, A[2] }, { OKCommand::Enabled, 1 },
			{ OKCommand::Type, Type }, { 0x80, 0 }, { OKCommand::Window, A[6] }, { 0x90, 0 }, { OKCommand::MeterFlag, 0 }, { OKCommand::MeterCost, 0 } });
	}
	bool RegisterFatality(FOKScriptThread& Thread) { return RegisterFinisher(Thread, 3); }
	bool RegisterBabality(FOKScriptThread& Thread) { return RegisterFinisher(Thread, 10); }

	// 66.17 register_tag_assist(&cmd, button, flag, sequence, window, name, partnerName, meterCost): type 6 (FUN_00449F90)
	bool RegisterTagAssist(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 7, R, A) && StoreFields(Thread, R, { { OKCommand::Flag, A[2] }, { OKCommand::Button, A[1] },
			{ OKCommand::Extra, 0 }, { 0x90, 0 }, { OKCommand::Enabled, 1 }, { OKCommand::Type, 6 }, { 0x80, 0 },
			{ OKCommand::Sequence, A[3] }, { OKCommand::Window, A[4] }, { OKCommand::MeterCost, A[7] } })
			&& StoreName(Thread, R + OKCommand::Name, A[5]) && StoreName(Thread, R + OKCommand::PartnerName, A[6]);
	}

	// 66.18 register_tag_special(&cmd, button, flag, sequence, window, name, meterCost): type 7 (FUN_0044A020)
	bool RegisterTagSpecial(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 6, R, A) && StoreFields(Thread, R, { { OKCommand::Button, A[1] }, { OKCommand::Sequence, A[3] },
			{ OKCommand::Flag, A[2] }, { OKCommand::Extra, 0 }, { 0x90, 0 }, { OKCommand::Enabled, 1 }, { OKCommand::Type, 7 },
			{ 0x80, 0 }, { OKCommand::Window, A[4] }, { OKCommand::MeterCost, A[6] } })
			&& StoreName(Thread, R + OKCommand::Name, A[5]);
	}

	// 66.19 register_xray(&cmd, meterCost): type 8, button -1 (FUN_0044A1D0)
	bool RegisterXray(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 1, R, A) && StoreFields(Thread, R, { { OKCommand::Extra, 0 }, { 0x90, 0 }, { OKCommand::Enabled, 1 },
			{ OKCommand::Type, 8 }, { OKCommand::Button, 0xFFFFFFFFu }, { OKCommand::Flag, 0 }, { 0x80, 0 }, { OKCommand::Sequence, 0 },
			{ OKCommand::Window, 0 }, { OKCommand::MeterFlag, 0 }, { OKCommand::MeterCost, A[1] } });
	}

	// 66.20 register_xray_air(&cmd, minHeight, meterCost): type 9, the same fields, the height above the floor it needs
	// at +0x8C (FUN_0044A230)
	bool RegisterXrayAir(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 2, R, A) && StoreFields(Thread, R, { { OKCommand::Extra, A[1] }, { 0x90, 0 }, { OKCommand::Enabled, 1 },
			{ OKCommand::Type, 9 }, { OKCommand::Button, 0xFFFFFFFFu }, { OKCommand::Flag, 0 }, { 0x80, 0 }, { OKCommand::Sequence, 0 },
			{ OKCommand::Window, 0 }, { OKCommand::MeterFlag, 0 }, { OKCommand::MeterCost, A[2] } });
	}

	// 66.23 (named command_flags) (&cmd, a, b) (FUN_00449E60): the record's +0x11C and +0x120. [unk] what reads them
	bool CommandWords11C(FOKScriptThread& Thread)
	{
		uint32 R = 0, A[8] = {};
		return CommandArgs(Thread, 2, R, A) && StoreFields(Thread, R, { { 0x11C, A[1] }, { 0x120, A[2] } });
	}

	// --- Leaving an attack: back to stance (FGFightEngineScript.exit_attack_breakout_bothplyrs) -------

	// 81.235 set_breakout_frames(obj, frames): +0x7628, and the tick it was set at +0x762C (FUN_00446DF0)
	bool SetBreakoutFrames(FOKScriptThread& Thread)
	{
		uint32 Frames = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Frames) || !Fighter)
		{
			return Fighter == nullptr;
		}
		const uint32 Now = uint32(Thread.GetRuntime().GetSimFrame());
		return Thread.StoreBytes(Fighter->Handle + FOKFighter::BreakoutFrames, &Frames, 4)
			&& Thread.StoreBytes(Fighter->Handle + FOKFighter::BreakoutFrames + 4, &Now, 4);
	}

	// 137.169 breakout_test(obj) -> fighter virtual +0x1FC (FUN_00895220): 0 when it stands on the ground (flag 8
	// clear, state not 0x4605, and +0x74 or +0x9C zero with its feet within 160 of the floor), else 1.
	// [inf] The floor is at height 0 until stages exist.
	bool BreakoutTest(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 Flags = 0, State = 0;
		float A = 0.0f, B = 0.0f;
		if (!Fighter || !Thread.LoadBytes(Fighter->Handle + FOKFighter::Flags, &Flags, 4) || !Thread.LoadBytes(Fighter->Handle + FOKFighter::State, &State, 4)
			|| !Thread.LoadBytes(Fighter->Handle + 0x74, &A, 4) || !Thread.LoadBytes(Fighter->Handle + 0x9C, &B, 4))
		{
			Thread.SetResult(0);
			return Fighter == nullptr;
		}
		const float Height = Fighter->Body ? Fighter->Body->GetHeight() : 0.0f;
		const bool bGrounded = (Flags & 8) == 0 && State != 0x4605 && (A == 0.0f || B == 0.0f) && Height <= 160.0f;
		Thread.SetResult(bGrounded ? 0 : 1);
		return true;
	}

	// 137.97 / 81.189 exit_to_stance_now(obj) -> fighter virtual +0x1BC (FUN_00892EB0): destroys the objects in the
	// lists at +0x2F2C and +0x2F40 (FUN_0042D8C0) and clears +0x7890..+0x789C. [inf] OpenKombat keeps no such lists yet.
	bool ExitToStanceNow(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Zero[4] = {};
		return !Fighter || Thread.StoreBytes(Fighter->Handle + 0x7890, Zero, sizeof(Zero));
	}

	// 137.187 game_event_exit(obj) (FUN_00896010): destroys the object handle +0xAAB4 refers to, and prunes the
	// object lists at +0x7F48 and +0x228 (FUN_0042D950). [inf] The lists aren't kept yet.
	bool GameEventExit(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		uint32 Handle[2] = {};
		if (!Thread.LoadBytes(Fighter->Handle + 0xAAB4 + 4, Handle, sizeof(Handle)))
		{
			return false;
		}
		if (FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Handle[0]))
		{
			if (FOKScriptThread* Process = Object->AsThread())
			{
				Process->KillNow(TEXT("game_event_exit"));
			}
		}
		const uint32 Empty[2] = { 0, 0xFFFFFFFFu };
		return Thread.StoreBytes(Fighter->Handle + 0xAAB4 + 4, Empty, sizeof(Empty));
	}

	// 137.224 take_control(obj) (FUN_00896730): every frame event freed, the player's input state reset (player
	// +0xB48 = 1, +0xB4C..+0xB5C = -1), the attack-active bit cleared, +0x7924 = 0. [inf] It also resets an
	// attack struct (FUN_0044A440, ~100 fields) and kills the fighter's group-13 processes; not done yet.
	bool TakeControl(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		bool bOk = true;
		const uint32 Free = 2, Zero = 0;
		for (int32 Slot = 0; Slot < FOKFighter::MaxFrameEvents; ++Slot)
		{
			bOk &= Thread.StoreBytes(Fighter->Handle + FOKFighter::FrameEvents + Slot * FOKFighter::FrameEventBytes + 4, &Free, 4);
		}
		bOk &= Thread.StoreBytes(Fighter->Handle + FOKFighter::FrameEventCount, &Zero, 4);
		if (Fighter->Player)
		{
			const uint32 Reset[6] = { 1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
			bOk &= Thread.StoreBytes(Fighter->Player->Handle + 0xB48, Reset, sizeof(Reset));
			uint32 Flags = 0;
			bOk &= Thread.LoadBytes(Fighter->Handle + FOKFighter::Flags, &Flags, 4);
			Flags &= ~2u;
			bOk &= Thread.StoreBytes(Fighter->Handle + FOKFighter::Flags, &Flags, 4);
		}
		return bOk && Thread.StoreBytes(Fighter->Handle + 0x7924, &Zero, 4);
	}

	/** Switches the fighter's main thread (+0x2FA4) to a function of one of the engine scripts (FUN_0045C610). */
	bool TransferMainThread(FOKScriptThread& Thread, FOKFighter& Fighter, const TCHAR* FunctionName, const TCHAR* ScriptName = TEXT("FGFightEngineScript"))
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptObject* Object = Runtime.ResolveHandle(Fighter.MainThread);
		FOKScriptThread* Main = Object ? Object->AsThread() : nullptr;
		FOKScriptInstance* Fight = Runtime.FindInstanceByName(ScriptName);
		const int32 Function = Fight ? Fight->File->FindFunctionByName(FunctionName) : INDEX_NONE;
		if (!Main || Function == INDEX_NONE)
		{
			return true;		// MKKE does nothing without a main thread or the function
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s's thread goes to %s.%s"), Runtime.GetSimFrame(), *Fighter.Name, ScriptName, FunctionName);
		return Main->TransferTo(*Fight, Function);
	}

	// 137.82 / 81.227 invalid_state_recover(obj) -> fighter virtual +0x1D0 (FUN_0089B3E0): breakout frames 0, then
	// the main thread goes to p_player_entry, or p_player_entry_back_into_duck when the fighter is crouching
	// (playing its moveset's Duckstance, Stance2duck or Stance2DuckTurnaround, rows 2, 4, 5).
	// [inf] MKKE takes p_player_entry straight away in some modes (DAT_00E91C64, DAT_00E89B80); not modelled.
	bool InvalidStateRecover(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		const uint32 Zero = 0;
		Thread.StoreBytes(Fighter->Handle + FOKFighter::BreakoutFrames, &Zero, 4);
		bool bDucking = false;
		const uint32 Moveset = MovesetOf(Thread.GetRuntime(), Fighter);
		if (Fighter->Body && Moveset)
		{
			for (const int32 Row : { 2, 4, 5 })
			{
				uint32 Anim = 0;
				const FOKMkoAsset* Asset = Thread.LoadBytes(Moveset + FOKMoveset::Rows + Row * FOKMoveset::RowBytes, &Anim, 4) ? Thread.GetRuntime().FindAsset(Anim) : nullptr;
				bDucking |= Asset && Asset->Object == Fighter->Body->GetAnimName();
			}
		}
		return TransferMainThread(Thread, *Fighter, bDucking ? TEXT("p_player_entry_back_into_duck") : TEXT("p_player_entry"));
	}

	// 137.83 / 81.229 -> fighter virtual +0x1D8 (FUN_0089B300): the main thread goes to p_duck_return_from_entry
	bool DuckReturnFromEntry(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || TransferMainThread(Thread, *Fighter, TEXT("p_duck_return_from_entry"), TEXT("BlockingDucking"));
	}

	// 137.84 / 81.228 -> fighter virtual +0x1D4 (FUN_0089B370): the main thread goes to p_duck_to_stand
	bool DuckToStand(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || TransferMainThread(Thread, *Fighter, TEXT("p_duck_to_stand"), TEXT("BlockingDucking"));
	}

	// --- Stance (FGFightEngineScript.blend_to_stance) -------------------------------------------------

	// 81.213 enable_commands(obj, command): sets a command slot's enabled word; the reverse of 81.214 (FUN_0044F570)
	bool EnableCommands(FOKScriptThread& Thread)
	{
		uint32 Command = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Command) || !Fighter)
		{
			return Fighter == nullptr;
		}
		const uint32 Slot = Command + 0x80000000u < uint32(FOKFighter::MaxCommands) ? Command + 0x80000000u : 0;
		const uint32 One = 1;
		return Thread.StoreBytes(Fighter->Handle + FOKFighter::Commands + Slot * FOKFighter::CommandBytes, &One, 4);
	}

	// 81.111 stance_anim(obj): clears +0x3A0 (FUN_0044BC50)
	bool StanceAnim(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Zero = 0;
		return !Fighter || Thread.StoreBytes(Fighter->Handle + 0x3A0, &Zero, 4);
	}

	// 59.50 is_stance_anim(obj) -> the playing animation (FUN_00442BA0: the anim node's sequence), which
	// blend_to_stance compares with moveset row 0. Ours is the asset value blend_to_ani started.
	bool CurrentAnimation(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? Fighter->CurrentAnim : 0);
		return true;
	}

	// 81.196 blend_to_correct_stance(obj) -> 1 when bone 121 is further along the facing than bone 115
	// (FUN_0044ED50), so the stance blend starts from the foot that's in front
	bool BlendToCorrectStance(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(0);
		const TCHAR* A = OKCollision::BoneName(121);
		const TCHAR* B = OKCollision::BoneName(115);
		FVector PA, PB;
		if (Fighter && Fighter->Body && A && B && Fighter->Body->GetBoneWorldLocation(A, PA) && Fighter->Body->GetBoneWorldLocation(B, PB))
		{
			const FVector Facing = Fighter->Body->GetFacing();
			Thread.SetResult(FVector::DotProduct(PB, Facing) < FVector::DotProduct(PA, Facing) ? 1 : 0);
		}
		return true;
	}

	// 137.120 blend_to_stance_native(obj) -> 0 (FUN_008932E0): a move stopped part-way (+0x304 not 0, 3 or 4, +0x394
	// set, the air clock stopped: FUN_00447D50) is ended through virtual +0x14C.
	bool BlendToStanceNative(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(0);
		if (Fighter && OKMotion::IsStoppedMidMove(Thread.GetRuntime(), *Fighter))
		{
			OKMotion::StopMove(Thread.GetRuntime(), *Fighter);
		}
		return true;
	}

	// 81.65 init_ground_move(obj, resetRate) -> fighter virtual +0x10C (FUN_00447DD0): flags 8 and 0x80000 off, the
	// animation's rate back to 1 if asked (FightEngineScript.init_ground_move asks, _no_anispeed_reset doesn't), the
	// group-11 processes killed, no force-away, and unless the move is 8 the air clock stopped.
	bool InitGroundMove(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::InitGroundMove(Thread.GetRuntime(), *Fighter, ArgWord(Thread, 1) != 0);
		}
		return true;
	}

	// 81.236 opponent(obj, ?) -> fighter virtual +0x1E0 (FUN_0044EC90): the character the handle at +0x763C refers
	// to, the opponent (MKKE binds @his_obj to the same handle). Was named char_vcall_1e0.
	bool Opponent(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 Handle[2] = {};
		Thread.SetResult(0);
		if (!Fighter || !Thread.LoadBytes(Fighter->Handle + FOKFighter::OpponentHandle + 4, Handle, sizeof(Handle)))
		{
			return Fighter == nullptr;
		}
		FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Handle[0]);
		Thread.SetResult(Object && Object->AsFighter() && Handle[1] == Handle[0] ? Handle[0] : 0);
		return true;
	}

	// 137.287 player_entry(obj, kind, channelName) (FUN_008AAD00): plays an entry animation on each of the mesh's
	// named channels (or just channelName) through virtual +0x88. Kind 4 picks +0xC578[0] standing or [1]
	// crouching (FUN_00897E60); kind 3 means none. With no entry animation set MKKE does nothing, and nothing
	// sets one in OpenKombat yet, so finding one is reported.
	bool PlayerEntry(FOKScriptThread& Thread)
	{
		uint32 Kind = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Kind) || !Fighter || Kind == 3)
		{
			return Fighter == nullptr || Kind == 3;
		}
		uint32 PreviousState = 0, Anim = 0;
		if (Kind == 4)
		{
			Thread.LoadBytes(Fighter->Handle + FOKFighter::PreviousState, &PreviousState, 4);
			Kind = (PreviousState & 0x8100) != 0 ? 1 : 0;		// [inf] FUN_00895330's ducking test is 0 here
		}
		if (Kind > 1 || !Thread.LoadBytes(Fighter->Handle + FOKFighter::EntryAnims + Kind * 4, &Anim, 4))
		{
			return true;
		}
		return Anim == 0 || Thread.NativeFail(TEXT("player_entry: playing entry animations on the mesh channels isn't implemented"));
	}

	// --- The idle loop (FGFightEngineScript.p_player_entry / p_player_loop) -------------------------------

	// 6.231 game_event(event, obj) (FUN_00407CF0): runs process_game_event(event, obj) in the object's own script (a
	// fighter's character script, through the callback at +0x7648) and in the stage's script, then starts a process
	// for each handler registered for the event (FUN_00407540). [inf] OpenKombat has no stage and nothing registers
	// handlers yet, so only the object's script runs. Like the callback, it runs at once.
	bool RunGameEvent(FOKScriptThread& Thread, uint32 Event, uint32 Obj);

	bool GameEvent(FOKScriptThread& Thread)
	{
		uint32 Event = 0, Obj = 0;
		if (!Thread.ArgRaw(0, Event) || !Thread.ArgRaw(1, Obj))
		{
			return false;
		}
		return RunGameEvent(Thread, Event, Obj);
	}

	// 2.8 (event) (FUN_00408140): game_event for the running thread's @my_obj
	bool GameEventMine(FOKScriptThread& Thread)
	{
		uint32 Event = 0;
		if (!Thread.ArgRaw(0, Event))
		{
			return false;
		}
		FOKFighter* Mine = Thread.MyFighter();
		return !Mine || RunGameEvent(Thread, Event, Mine->Handle);
	}

	bool RunGameEvent(FOKScriptThread& Thread, uint32 Event, uint32 Obj)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptObject* Object = Runtime.ResolveHandle(Obj);
		FOKFighter* Fighter = Object ? Object->AsFighter() : nullptr;
		if (!Fighter || !Fighter->CharacterScript)
		{
			return true;
		}
		const int32 Function = Fighter->CharacterScript->FindFunctionByName(TEXT("process_game_event"));
		if (Function == INDEX_NONE)
		{
			return true;
		}
		FOKScriptThread Handler(Runtime);
		Handler.BindFighter(*Fighter);
		const uint32 Args[2] = { Event, Obj };
		uint32 Result = 0;
		FString Error;
		return Handler.Run(*Fighter->CharacterScript, Function, Args, Result, Error) || Thread.NativeFail(TEXT("process_game_event: ") + Error);
	}

	// 81.179 opponent_flag(obj) -> bit 1 of its character info's +0xA0 (FUN_00447620). [unk] what the bit means;
	// p_FinishHimEffect asks it of the loser
	bool OpponentFlag(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 Info = 0, Bits = 0;
		if (Fighter && Thread.LoadBytes(Fighter->Handle + FOKFighter::CharacterInfo, &Info, 4) && Info != 0)
		{
			Thread.LoadBytes(Info + 0xA0, &Bits, 4);
		}
		Thread.SetResult(Bits & 1);
		return true;
	}

	// 81.184 state(obj) -> the state code, +0x1D4 (FUN_00446B90). Was named is_finished_animating.
	bool GetState(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 State = 0;
		if (Fighter)
		{
			Thread.LoadBytes(Fighter->Handle + FOKFighter::State, &State, 4);
		}
		Thread.SetResult(State);
		return true;
	}

	// 81.231 set_invulnerable(obj, ticks): +0x75F8 = the tick it lasts until (FUN_00446A50)
	bool SetInvulnerable(FOKScriptThread& Thread)
	{
		uint32 Ticks = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Ticks) || !Fighter)
		{
			return Fighter == nullptr;
		}
		const uint32 Until = uint32(Thread.GetRuntime().GetSimFrame()) + Ticks;
		return Thread.StoreBytes(Fighter->Handle + FOKFighter::Invulnerable, &Until, 4);
	}

	/** Sets or clears bits of a fighter word. */
	bool ChangeBits(FOKScriptThread& Thread, FOKFighter& Fighter, uint32 Offset, uint32 Set, uint32 Clear)
	{
		uint32 Word = 0;
		if (!Thread.LoadBytes(Fighter.Handle + Offset, &Word, 4))
		{
			return false;
		}
		Word = (Word & ~Clear) | Set;
		return Thread.StoreBytes(Fighter.Handle + Offset, &Word, 4);
	}

	// 81.70 set_char_flags(obj, on): flag 0x200 (FUN_004475F0)
	bool SetCharFlags(FOKScriptThread& Thread)
	{
		uint32 On = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, On) || !Fighter)
		{
			return Fighter == nullptr;
		}
		return ChangeBits(Thread, *Fighter, FOKFighter::Flags, On ? 0x200u : 0u, On ? 0u : 0x200u);
	}

	// 81.241 constrain_enable(obj, mask) / 81.242 constrain_disable(obj, mask): +0x1D0 |= mask / &= ~mask (FUN_004486A0 / B0)
	bool ConstrainEnable(FOKScriptThread& Thread)
	{
		uint32 Mask = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Thread.ArgRaw(1, Mask) ? false : (!Fighter || ChangeBits(Thread, *Fighter, FOKFighter::CharFlags, Mask, 0));
	}
	bool ConstrainDisable(FOKScriptThread& Thread)
	{
		uint32 Mask = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Thread.ArgRaw(1, Mask) ? false : (!Fighter || ChangeBits(Thread, *Fighter, FOKFighter::CharFlags, 0, Mask));
	}

	// 137.112 set_throw_flag(obj, on): on clears flag 0x8000000, off sets it (FUN_00892D20)
	bool SetThrowFlag(FOKScriptThread& Thread)
	{
		uint32 On = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, On) || !Fighter)
		{
			return Fighter == nullptr;
		}
		return ChangeBits(Thread, *Fighter, FOKFighter::Flags, On ? 0u : 0x8000000u, On ? 0x8000000u : 0u);
	}

	// 137.274 back_to_normal_a(obj): clears flag 0x40 (FUN_008DAA90)
	bool ClearFlag40(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || ChangeBits(Thread, *Fighter, FOKFighter::Flags, 0, 0x40);
	}

	// 137.159 am_i_ducking(obj): the previous state has bit 0x8100, or FUN_00895330's crouch test (FUN_00895390).
	// [inf] That test asks the input buffer for Down, and nothing is pressed yet.
	bool AmIDucking(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 Previous = 0;
		if (Fighter)
		{
			Thread.LoadBytes(Fighter->Handle + FOKFighter::PreviousState, &Previous, 4);
		}
		Thread.SetResult((Previous & 0x8100) != 0 ? 1 : 0);
		return true;
	}

	// 6.200 plyr_back_to_normal(record): +0x70 = 0, +0x74 = 14, +0xF0 = 0, +0xB4 = 0x80000000, +0xB0 = 0 (FUN_008899F0)
	bool PlyrBackToNormal(FOKScriptThread& Thread)
	{
		uint32 Record = 0;
		if (!Thread.ArgRaw(0, Record))
		{
			return false;
		}
		return Record == 0 || StoreFields(Thread, Record, { { 0x70, 0 }, { 0x74, 14 }, { 0xF0, 0 }, { 0xB4, 0x80000000u }, { 0xB0, 0 } });
	}

	// 68.3 reset_attack(&attack): resets an attack struct (FUN_0044A440): 123 writes, from its disassembly
	bool ResetAttack(FOKScriptThread& Thread)
	{
		uint32 Attack = 0;
		if (!Thread.ArgValue(0, Attack))
		{
			return false;
		}
		static const uint32 Writes[][2] = { { 0x64, 0x0 }, { 0x68, 0xFFFFFFFF }, { 0x10C, 0x0 }, { 0x110, 0xFFFFFFFF }, { 0x11C, 0x0 }, { 0x120, 0xFFFFFFFF }, { 0x74, 0x0 }, { 0x78, 0xFFFFFFFF }, { 0x84, 0x0 }, { 0x88, 0xFFFFFFFF }, { 0x1E0, 0x0 }, { 0x1E4, 0xFFFFFFFF }, { 0x1F4, 0x0 }, { 0x1F8, 0xFFFFFFFF }, { 0xE4, 0x0 }, { 0x0, 0x0 }, { 0x4, 0x0 }, { 0x28, 0x0 }, { 0x8, 0x0 }, { 0xC, 0x0 }, { 0x10, 0x0 }, { 0x14, 0x0 }, { 0x18, 0x0 }, { 0x1C, 0x0 }, { 0x20, 0x0 }, { 0x168, 0x0 }, { 0x170, 0x0 }, { 0x174, 0x0 }, { 0x17C, 0x0 }, { 0x180, 0x0 }, { 0x24, 0x0 }, { 0x214, 0x0 }, { 0x184, 0x0 }, { 0x128, 0x0 }, { 0x134, 0x0 }, { 0x138, 0xFFFFFFFF }, { 0x13C, 0x0 }, { 0x12C, 0x0 }, { 0x144, 0x0 }, { 0x148, 0xFFFFFFFF }, { 0x14C, 0x0 }, { 0x2C, 0x0 }, { 0x1D8, 0x0 }, { 0x34, 0x0 }, { 0x16C, 0x0 }, { 0x3C, 0x0 }, { 0x40, 0x0 }, { 0x30, 0x0 }, { 0x38, 0x0 }, { 0x44, 0x0 }, { 0x48, 0x0 }, { 0x4C, 0x0 }, { 0x50, 0x0 }, { 0x54, 0x0 }, { 0x58, 0x0 }, { 0x5C, 0xE }, { 0x6C, 0x0 }, { 0x7C, 0x0 }, { 0x8C, 0x0 }, { 0x90, 0x0 }, { 0x94, 0x0 }, { 0x98, 0x0 }, { 0x9C, 0x0 }, { 0xA0, 0x0 }, { 0xA4, 0x0 }, { 0xA8, 0x16 }, { 0xAC, 0x0 }, { 0xB0, 0x0 }, { 0xB4, 0x0 }, { 0xB8, 0x0 }, { 0xBC, 0x0 }, { 0xC0, 0x0 }, { 0xC4, 0x0 }, { 0xC8, 0x0 }, { 0xCC, 0x0 }, { 0xD0, 0x0 }, { 0xD4, 0x0 }, { 0xDC, 0x0 }, { 0xE0, 0xFFFFFFFF }, { 0xE8, 0x0 }, { 0x114, 0x0 }, { 0x124, 0x0 }, { 0x150, 0x0 }, { 0x154, 0x0 }, { 0x158, 0x0 }, { 0x15C, 0x0 }, { 0x160, 0x0 }, { 0x164, 0x0 }, { 0x178, 0x0 }, { 0x188, 0x15 }, { 0x18C, 0x15 }, { 0x190, 0x15 }, { 0x194, 0x15 }, { 0x198, 0x15 }, { 0x19C, 0x15 }, { 0x1A0, 0x15 }, { 0x1A4, 0x15 }, { 0x1A8, 0x15 }, { 0x1AC, 0x15 }, { 0x1B0, 0x0 }, { 0x1B4, 0x0 }, { 0x1B8, 0x0 }, { 0x1BC, 0x0 }, { 0x1C0, 0x0 }, { 0x1C4, 0x0 }, { 0x1C8, 0x0 }, { 0x1CC, 0x0 }, { 0x1D0, 0x0 }, { 0x1D4, 0x0 }, { 0x1E8, 0x0 }, { 0x1EC, 0x0 }, { 0x1FC, 0x0 }, { 0x210, 0x0 }, { 0x208, 0x0 }, { 0x20C, 0xFFFFFFFF }, { 0x200, 0x0 }, { 0x218, 0x0 }, { 0xF0, 0x0 }, { 0xF4, 0xFFFFFFFF }, { 0x104, 0x0 }, { 0x100, 0x0 }, { 0xFC, 0x0 }, { 0xF8, 0x0 } };
		for (const auto& W : Writes)
		{
			if (Attack != 0 && !Thread.StoreBytes(Attack + W[0], &W[1], 4))
			{
				return false;
			}
		}
		return true;
	}

	// 6.74 kill_obj_proc(obj, procId): kills obj's processes with that id, or every one when obj is 0 (FUN_00492690)
	bool KillObjProc(FOKScriptThread& Thread)
	{
		uint32 Obj = 0, ProcId = 0;
		if (!Thread.ArgRaw(0, Obj) || !Thread.ArgRaw(1, ProcId))
		{
			return false;
		}
		Thread.GetRuntime().KillProcesses(int32(ProcId), Obj);
		return true;
	}

	void EndCombo(FOKScriptRuntime& Runtime, FOKPlayer& Player);

	// 118.48 combo_message(info) (FUN_00898F60): ends the side's combo. It shows the combo message and records
	// stats, then clears the counters at player +0xC54..+0xC6C, resets the damage multiplier (+0xB60) to 1 and clears
	// the active fighter's +0x3B8..+0x3C8. [inf] The HUD message and the stats aren't done; only the reset is.
	bool ComboMessage(FOKScriptThread& Thread)
	{
		uint32 Info = 0;
		if (!Thread.ArgValue(0, Info))
		{
			return false;
		}
		FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Info);
		if (FOKPlayer* Player = Object ? Object->AsPlayer() : nullptr)
		{
			EndCombo(Thread.GetRuntime(), *Player);
		}
		return true;
	}

	// 118.4 get_partner_char(info, ?) -> the tag partner the handle at player +0x24 refers to (FUN_0044CB60). One
	// fighter a side in OpenKombat, so 0.
	bool GetPartnerChar(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 137.9 set_char_layer(obj, layer) (FUN_00894990): +0x76F4; 1 becomes 2 once a layer is set, and the fighter is
	// shown unless the layer is 0 or 3 (virtual +0x44). [inf] Showing and hiding comes with the body.
	bool SetCharLayer(FOKScriptThread& Thread)
	{
		uint32 Layer = 0, Current = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Layer) || !Fighter)
		{
			return Fighter == nullptr;
		}
		Thread.LoadBytes(Fighter->Handle + FOKFighter::CharLayer, &Current, 4);
		if (Layer == 1)
		{
			Layer = 2;
		}
		else if (Current == 0)
		{
			return true;
		}
		return Thread.StoreBytes(Fighter->Handle + FOKFighter::CharLayer, &Layer, 4);
	}

	// --- Input (FIGHT-RE-NOTES §6.4) ------------------------------------------------------------------------

	uint32 ReadWord(FOKScriptRuntime& Runtime, uint32 Address)
	{
		uint32 Value = 0;
		Runtime.LoadAnyWord(Address, Value);
		return Value;
	}

	void WriteWord(FOKScriptRuntime& Runtime, uint32 Address, uint32 Value)
	{
		Runtime.StoreAnyWord(Address, Value);
	}

	/**
	 * The damage itself (fighter virtual +0x1F8, FUN_00899A20): the life, then the combo's damage on the victim's player as a
	 * fraction (+0xC68) and in percent (+0xC58). [inf] MKKE's player virtual +0x4C also scales it in modes OpenKombat
	 * doesn't have yet (the tower's buffs, online).
	 */
	void ApplyDamage(FOKScriptRuntime& Runtime, FOKFighter& Victim, float Damage)
	{
		Victim.Health = FMath::Max(0.0f, Victim.Health - Damage);
		WriteWord(Runtime, Victim.Handle + FOKFighter::Life, FloatToBits(Victim.Health));
		if (FOKPlayer* Player = Victim.Player)
		{
			WriteWord(Runtime, Player->Handle + FOKPlayer::ComboDamage, FloatToBits(BitsToFloat(ReadWord(Runtime, Player->Handle + FOKPlayer::ComboDamage)) + Damage));
			WriteWord(Runtime, Player->Handle + FOKPlayer::ComboPercent, FloatToBits(BitsToFloat(ReadWord(Runtime, Player->Handle + FOKPlayer::ComboPercent)) + Damage * 100.0f));
		}
	}

	// 137.86 damage_char(obj, damage, ?) (fighter virtual +0x1F8): scripted damage, as throws deal it (damage_him)
	bool DamageChar(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			uint32 Damage = 0;
			Thread.ArgValue(1, Damage);
			ApplyDamage(Thread.GetRuntime(), *Fighter, BitsToFloat(Damage));
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s takes %g damage, health %g"), Thread.GetRuntime().GetSimFrame(), *Fighter->Name, BitsToFloat(Damage), Fighter->Health);
		}
		Thread.SetResult(0);
		return true;
	}

	/**
	 * combo_message_impl (FUN_00898F60): ends the combo the player's fighter is taking, unless it's still reeling
	 * (states 0x4604, 0x4607, 0x460C). [inf] The HUD message (FUN_00892FE0, two or more hits) and the stats aren't
	 * shown; we log combos of two or more hits.
	 */
	void EndCombo(FOKScriptRuntime& Runtime, FOKPlayer& Player)
	{
		if (!Player.Fighter)
		{
			return;
		}
		const uint32 State = ReadWord(Runtime, Player.Fighter->Handle + FOKFighter::State);
		if (State == 0x4604 || State == 0x4607 || State == 0x460C)
		{
			return;
		}
		const uint32 Hits = ReadWord(Runtime, Player.Handle + FOKPlayer::ComboHits);
		if (Hits > 1)
		{
			const float Percent = BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::ComboPercent));
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s took a %u-hit combo, %.0f%%"), Runtime.GetSimFrame(), *Player.Fighter->Name, Hits, Percent);
			// The attacker's half of the screen shows it (FUN_00892FE0). [inf] For about a second and a half.
			FOKHudBanner Banner;
			Banner.Kind = FOKHudBanner::EKind::Combo;
			Banner.Value = int32(Hits);
			Banner.Percent = Percent;
			Banner.Side = 1 - Player.Fighter->Index;
			Banner.Start = Runtime.GetSimFrame();
			Banner.Until = Banner.Start + 90;
			Runtime.GetHud().ShowBanner(Banner);
		}
		for (uint32 Offset = FOKPlayer::ComboHits; Offset <= 0xC6C; Offset += 4)
		{
			WriteWord(Runtime, Player.Handle + Offset, 0);
		}
		WriteWord(Runtime, Player.Handle + FOKPlayer::DamageMultiplier, FloatToBits(1.0f));
		for (uint32 Offset = 0x3B8; Offset < 0x3CC; Offset += 4)
		{
			WriteWord(Runtime, Player.Fighter->Handle + Offset, 0);
		}
	}

	FOKPlayer* ArgPlayer(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Handle = 0;
		FOKScriptObject* Object = Thread.ArgValue(Index, Handle) ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		return Object ? Object->AsPlayer() : nullptr;
	}

	uint32 SlotAddress(const FOKFighter& Fighter, uint32 Code)
	{
		const uint32 Slot = Code + 0x80000000u;
		return Fighter.Handle + FOKFighter::ButtonSlots + (Slot < uint32(FOKFighter::MaxButtonSlots) ? Slot : 0) * FOKFighter::ButtonSlotBytes;
	}

	/** The top of the player's control stack (FUN_004467B0): 1 when the player is in control. */
	uint32 ControlTop(FOKScriptRuntime& Runtime, const FOKPlayer& Player)
	{
		const uint32 Count = ReadWord(Runtime, Player.Handle + FOKPlayer::ControlCount);
		return Count ? ReadWord(Runtime, Player.Handle + FOKPlayer::ControlStack - 4 + Count * 4) : 0;
	}

	bool Airborne(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
	{
		const uint32 Flags = ReadWord(Runtime, Fighter.Handle + FOKFighter::Flags), State = ReadWord(Runtime, Fighter.Handle + FOKFighter::State);
		const uint32 A = ReadWord(Runtime, Fighter.Handle + 0x74), B = ReadWord(Runtime, Fighter.Handle + 0x9C);
		const float Height = Fighter.Body ? Fighter.Body->GetHeight() : 0.0f;
		return !((Flags & 8) == 0 && State != 0x4605 && (A == 0 || B == 0) && Height <= 160.0f);
	}

	/** FUN_00895A00: alive, in control, not in a locked state, and past its +0x2F6C wait. */
	bool CanAct(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
	{
		const uint32 State = ReadWord(Runtime, Fighter.Handle + FOKFighter::State);
		return Fighter.Player && ControlTop(Runtime, *Fighter.Player) == 1 && (State & 0x200) == 0 && State != 0x2004 && State != 0x2006
			&& State != 0x2008 && (State & 0x800) == 0 && ReadWord(Runtime, Fighter.Handle + 0x2F6C) <= uint32(Runtime.GetSimFrame()) && Fighter.Health != 0.0f;
	}

	/** FUN_00895A90: CanAct's checks for moving (walk, jump, crouch), with its own wait at +0x2F68. */
	bool CanMove(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
	{
		const uint32 State = ReadWord(Runtime, Fighter.Handle + FOKFighter::State);
		return Fighter.Player && ControlTop(Runtime, *Fighter.Player) == 1 && (State & 0x4000) == 0 && (State & 0x800) == 0
			&& ReadWord(Runtime, Fighter.Handle + 0x2F68) <= uint32(Runtime.GetSimFrame()) && Fighter.Health != 0.0f;
	}

	/** FUN_0089EEB0(fighter, 1): allowed to block. [inf] Its tower and +0x7710 checks are off outside the tower. */
	bool CanBlock(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
	{
		const uint32 State = ReadWord(Runtime, Fighter.Handle + FOKFighter::State), Now = uint32(Runtime.GetSimFrame());
		return CanAct(Runtime, Fighter) && !Airborne(Runtime, Fighter) && (State & 0x200) == 0 && (State & 0x800) == 0 && (State & 0x100) == 0
			&& State != 0x620A && State != 0x620B && ReadWord(Runtime, Fighter.Handle + 0x2F58) == 0 && ReadWord(Runtime, Fighter.Handle + 0x2F5C) == 0
			&& ReadWord(Runtime, Fighter.Handle + 0x2F60) <= Now && ReadWord(Runtime, Fighter.Handle + 0x2F64) <= Now;
	}

	/**
	 * A button slot's predicate (+0x30E4), a callback id into module 7's callback table (0xDF0D04): 2 throw
	 * (FUN_008A1B00), 3 tag (FUN_008A1AA0), 5 attack (FUN_0089ED90), 7 flip stance (FUN_0089EDE0), 8 walk
	 * (FUN_00895A90), 9 block (FUN_008A4560), 10 jump (FUN_0089EE30), 11 crouch (FUN_0089EE70). The tower checks
	 * they also make are off outside the Challenge Tower.
	 */
	bool ButtonPredicate(FOKScriptRuntime& Runtime, const FOKFighter& Fighter, uint32 Callback)
	{
		switch (Callback)
		{
		case 0x5C060002:
		{
			const uint32 Info = ReadWord(Runtime, Fighter.Handle + FOKFighter::CharacterInfo);
			return (ReadWord(Runtime, Info + 0xA0) & 8) == 0 && CanAct(Runtime, Fighter) && !Airborne(Runtime, Fighter);
		}
		case 0x5C060003: return false;		// tag: needs a partner
		case 0x5C060005: return CanAct(Runtime, Fighter);
		case 0x5C060007: return CanAct(Runtime, Fighter) && !Airborne(Runtime, Fighter);
		case 0x5C060008: case 0x5C06000A: case 0x5C06000B: return CanMove(Runtime, Fighter);
		case 0x5C060009: return CanBlock(Runtime, Fighter);
		default:
			UE_LOG(LogOKVM, Warning, TEXT("%s: button predicate %08X isn't known"), *Fighter.Name, Callback);
			return false;
		}
	}

	/** Runs a function of a script at once on a thread of its own, bound to the fighter, with one argument. */
	void RunNow(FOKScriptRuntime& Runtime, FOKFighter& Fighter, const TCHAR* ScriptName, const TCHAR* FunctionName, uint32 Arg)
	{
		FOKScriptInstance* Script = Runtime.FindInstanceByName(ScriptName);
		const int32 Function = Script ? Script->File->FindFunctionByName(FunctionName) : INDEX_NONE;
		if (Function == INDEX_NONE)
		{
			return;
		}
		FOKScriptThread Thread(Runtime);
		Thread.BindFighter(Fighter);
		uint32 Result = 0;
		FString Error;
		const uint32 Args[1] = { Arg };
		if (!Thread.Run(*Script->File, Function, Args, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s.%s: %s"), ScriptName, FunctionName, *Error);
		}
	}

	/** FUN_00448A40: every button slot and command record disabled until the scripts enable them again. */
	void DisableAllCommands(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
	{
		for (int32 Slot = 0; Slot < FOKFighter::MaxButtonSlots; ++Slot)
		{
			WriteWord(Runtime, Fighter.Handle + FOKFighter::ButtonSlots + Slot * FOKFighter::ButtonSlotBytes + FOKButtonSlot::Enabled, 0);
		}
		const uint32 Count = ReadWord(Runtime, Fighter.Handle + FOKFighter::CommandCount);
		for (uint32 i = 0; i < Count && i < uint32(FOKFighter::MaxCommandList); ++i)
		{
			WriteWord(Runtime, ReadWord(Runtime, Fighter.Handle + FOKFighter::CommandList + i * 4) + 4, 0);
		}
	}

	/** Fighter virtual +0x1C4 (FUN_00448A90): every button slot and command enabled. */
	void EnableAllCommandsOf(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
	{
		for (int32 Slot = 0; Slot < FOKFighter::MaxButtonSlots; ++Slot)
		{
			const uint32 S = Fighter.Handle + FOKFighter::ButtonSlots + Slot * FOKFighter::ButtonSlotBytes;
			WriteWord(Runtime, S + FOKButtonSlot::Enabled, 1);
			WriteWord(Runtime, S + 0x60, 0);
		}
		const uint32 Count = ReadWord(Runtime, Fighter.Handle + FOKFighter::CommandCount);
		for (uint32 i = 0; i < Count && i < uint32(FOKFighter::MaxCommandList); ++i)
		{
			const uint32 Record = ReadWord(Runtime, Fighter.Handle + FOKFighter::CommandList + i * 4);
			WriteWord(Runtime, Record + 4, 1);
			WriteWord(Runtime, Record + 0x60, 0);
		}
	}

	/** FUN_00446850: the player's combo string is emptied (length 1, no steps). */
	void ResetCombo(FOKScriptRuntime& Runtime, const FOKPlayer& Player)
	{
		WriteWord(Runtime, Player.Handle + FOKPlayer::ComboLength, 1);
		for (int32 i = 0; i < 5; ++i)
		{
			WriteWord(Runtime, Player.Handle + FOKPlayer::ComboSteps + i * 4, 0xFFFFFFFFu);
		}
	}

	/** FUN_008966A0 / F0 / 40 and the rest of take_control: frame events freed, the combo reset, +0x7924 cleared. */
	void ResetForMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
	{
		for (int32 Slot = 0; Slot < FOKFighter::MaxFrameEvents; ++Slot)
		{
			WriteWord(Runtime, Fighter.Handle + FOKFighter::FrameEvents + Slot * FOKFighter::FrameEventBytes + 4, 2);
		}
		WriteWord(Runtime, Fighter.Handle + FOKFighter::FrameEventCount, 0);
		if (Fighter.Player)
		{
			ResetCombo(Runtime, *Fighter.Player);
			WriteWord(Runtime, Fighter.Handle + FOKFighter::Flags, ReadWord(Runtime, Fighter.Handle + FOKFighter::Flags) & ~2u);
		}
		WriteWord(Runtime, Fighter.Handle + 0x7924, 0);
	}

	/** Switches the fighter's main thread to a function of a script instance (FUN_0045C610). */
	bool TransferMain(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKScriptInstance* Script, int32 Function, const TCHAR* Why)
	{
		FOKScriptObject* Object = Runtime.ResolveHandle(Fighter.MainThread);
		FOKScriptThread* Main = Object ? Object->AsThread() : nullptr;
		if (!Main || !Script || !Script->File->GetFunctions().IsValidIndex(Function))
		{
			return false;
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s %s -> %s.%s"), Runtime.GetSimFrame(), *Fighter.Name, Why,
			*Script->File->GetName().Replace(TEXT(".mko"), TEXT("")), *Script->File->GetFunctions()[Function].Name);
		return Main->TransferTo(*Script, Function);
	}

	FOKScriptInstance* InstanceFromValue(FOKScriptRuntime& Runtime, uint32 Value)
	{
		return (Value >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(Value & 0x00FFFFFF)) : nullptr;
	}

	/** game_event (FUN_00407CF0) from C++: the object's own process_game_event. */
	void GameEventOf(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Event)
	{
		const int32 Function = Fighter.CharacterScript ? Fighter.CharacterScript->FindFunctionByName(TEXT("process_game_event")) : INDEX_NONE;
		if (Function == INDEX_NONE)
		{
			return;
		}
		FOKScriptThread Handler(Runtime);
		Handler.BindFighter(Fighter);
		const uint32 Args[2] = { Event, Fighter.Handle };
		uint32 Result = 0;
		FString Error;
		Handler.Run(*Fighter.CharacterScript, Function, Args, Result, Error);
	}

	/**
	 * XferToAttackScript (FUN_0089FE60): game events 8 and 9, the move recorded at +0x2F80..+0x2F94, and the
	 * main thread switched to FightEngineScript.p_attack, which goes through the fight table to game_p_attack.
	 */
	bool XferToAttackScript(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Script, uint32 Function, uint32 Direction, uint32 Button, bool bSpecial)
	{
		GameEventOf(Runtime, Fighter, 8);
		GameEventOf(Runtime, Fighter, 9);
		WriteWord(Runtime, Fighter.Handle + 0x7C84, bSpecial ? 1 : 0);
		WriteWord(Runtime, Fighter.Handle + FOKFighter::AttackScript + 4, Script);
		WriteWord(Runtime, Fighter.Handle + FOKFighter::AttackScript + 8, Script);
		WriteWord(Runtime, Fighter.Handle + FOKFighter::AttackDirection, Direction);
		WriteWord(Runtime, Fighter.Handle + FOKFighter::AttackFunction, Function);
		WriteWord(Runtime, Fighter.Handle + FOKFighter::AttackButton, Button);
		WriteWord(Runtime, Fighter.Handle + 0x2FF4, Function);
		FOKScriptInstance* Fight = Runtime.FindInstanceByName(TEXT("FightEngineScript"));
		return TransferMain(Runtime, Fighter, Fight, Fight ? Fight->File->FindFunctionByName(TEXT("p_attack")) : INDEX_NONE,
			bSpecial ? TEXT("special move") : TEXT("attack"));
	}

	/**
	 * The move-list lookup (FUN_00449C90): the first move of a list whose steps start with the player's combo string
	 * and continue with Word (button << 16 | direction). A move with no direction for that button is the fallback.
	 */
	uint32 FindMove(FOKScriptRuntime& Runtime, uint32 Moveset, uint32 List, const FOKPlayer& Player, uint32 Wanted, uint32& OutDirection)
	{
		List = FMath::Min<uint32>(List, 4);
		const uint32 Count = ReadWord(Runtime, Moveset + FOKMoveset::MoveListCounts + List * 4);
		const uint32 Base = Moveset + FOKMoveset::MoveLists + List * FOKMoveset::MoveListBytes;
		int32 Fallback = INDEX_NONE;
		for (uint32 i = 0; i < Count && i < uint32(FOKMoveset::MaxMoves); ++i)
		{
			const uint32 Move = Base + i * FOKMoveset::MoveBytes;
			auto Step = [&](uint32 u) { return ReadWord(Runtime, Move + 8 + u * 4); };
			for (uint32 u = 0; u < 5; ++u)
			{
				const uint32 Have = ReadWord(Runtime, Player.Handle + FOKPlayer::ComboSteps + u * 4);
				if (Have == 0xFFFFFFFFu)
				{
					const bool bLast = u + 1 > 4 || Step(u + 1) == 0xFFFFFFFFu;
					if (Step(u) == Wanted && bLast)
					{
						OutDirection = (Step(u) & 0xFFFF) | 0x80000000u;
						return Move;
					}
					if (bLast && (Step(u) & 0xFFFF) == 0 && (Step(u) >> 16) == (Wanted >> 16))
					{
						Fallback = int32(i);
					}
					break;
				}
				if (Step(u) != Have)
				{
					break;
				}
			}
		}
		if (Fallback != INDEX_NONE)
		{
			OutDirection = 0xFFFFFFFFu;
			return Base + uint32(Fallback) * FOKMoveset::MoveBytes;
		}
		return 0;
	}

	/** FUN_00448BB0: FightEngineScript.plyr_back_to_normal_from_button_press(fighter), at once. */
	void BackToNormalFromButton(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
	{
		RunNow(Runtime, Fighter, TEXT("FightEngineScript"), TEXT("plyr_back_to_normal_from_button_press"), Fighter.Handle);
	}

	/**
	 * Starting an attack from a button (FUN_008A7920 -> FUN_008A1D10 -> FUN_008A0600): the direction held (or block,
	 * throw, flip or tag) picks a move in the button's list; jump states use their own codes. [inf] The turnaround
	 * case (FUN_008953B0, fighter virtual +0x198) is left out: fighters here always face each other.
	 */
	bool StartAttack(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player, uint32 Code)
	{
		ResetForMove(Runtime, Fighter);
		uint32 Direction = OKPlayerInput::Direction(Runtime, Player);
		if (Direction == 0x80000000u)
		{
			for (const uint32 Held : { 0x8000001Cu, 0x8000001Du, 0x8000001Bu, 0x8000001Eu })
			{
				if (OKPlayerInput::Held(Runtime, Player, Held))
				{
					Direction = Held;
					break;
				}
			}
		}
		const uint32 State = ReadWord(Runtime, Fighter.Handle + FOKFighter::State);
		bool bExact = false;
		if (State == 0x200E || ((State == 0x2010 || State == 0x2011) && OKPlayerInput::Held(Runtime, Player, 0x80000003))) { Direction = 0x8000002A; bExact = true; }
		else if (State == 0x2005) { Direction = 0x80000024; bExact = true; }
		else if (State == 0x2007) { Direction = 0x80000025; bExact = true; }
		else if (State == 0x2009) { Direction = 0x80000026; bExact = true; }

		const uint32 Moveset = MovesetOf(Runtime, &Fighter);
		WriteWord(Runtime, Fighter.Handle + FOKFighter::MoveList, Code - 0x8000000Fu);
		uint32 Found = Direction;
		const uint32 MoveWord = (Code << 16) | (Direction & 0xFFFF);
		const uint32 Move = FindMove(Runtime, Moveset, Code - 0x8000000Fu, Player, MoveWord, Found);
		if (!Move || (ReadWord(Runtime, Move) & 1) || (bExact && Found != Direction))
		{
			ResetForMove(Runtime, Fighter);
			return false;
		}
		// FUN_00446470 appends to the combo string. [inf] OpenKombat appends the step matched.
		for (uint32 i = 0; i < 5; ++i)
		{
			if (ReadWord(Runtime, Player.Handle + FOKPlayer::ComboSteps + i * 4) == 0xFFFFFFFFu)
			{
				WriteWord(Runtime, Player.Handle + FOKPlayer::ComboSteps + i * 4, (Code << 16) | (Found & 0xFFFF));
				break;
			}
		}
		const uint32 Function = ReadWord(Runtime, Move + 4), Flags = ReadWord(Runtime, Move);
		WriteWord(Runtime, Fighter.Handle + 0x2D0, 0);
		BackToNormalFromButton(Runtime, Fighter);
		WriteWord(Runtime, Fighter.Handle + 0x2F0, 0);
		DisableAllCommands(Runtime, Fighter);
		WriteWord(Runtime, Fighter.Handle + 0x2F54, 0);
		if (Flags & 2)
		{
			WriteWord(Runtime, Fighter.Handle + 0x214, 1);
		}
		return XferToAttackScript(Runtime, Fighter, ReadWord(Runtime, Moveset + FOKMoveset::Script), Function, Found, Code, false);
	}

	/** XferToGenericCommand (FUN_0089FFC0): the main thread goes to the slot's bound function (walk, jump, block...). */
	bool XferToGenericCommand(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Code)
	{
		const uint32 S = SlotAddress(Fighter, Code);
		FOKScriptInstance* Script = InstanceFromValue(Runtime, ReadWord(Runtime, S + FOKButtonSlot::Script + 4));
		return TransferMain(Runtime, Fighter, Script, int32(ReadWord(Runtime, S + FOKButtonSlot::Function)) - 1, TEXT("command"));
	}

	/** Button delays (FUN_004493A0 / FUN_00449400 / FUN_00449450): some buttons act only after being held a while. */
	bool DelayPassed(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Code)
	{
		const uint32 Count = ReadWord(Runtime, Fighter.Handle + FOKFighter::ButtonDelays);
		const uint32 Now = uint32(Runtime.GetSimFrame());
		for (uint32 i = 0; i < Count && i < 10; ++i)
		{
			const uint32 Entry = Fighter.Handle + FOKFighter::ButtonDelays + 4 + i * 12;
			if (ReadWord(Runtime, Entry) == Code)
			{
				if (ReadWord(Runtime, Entry + 4) == 0)
				{
					WriteWord(Runtime, Entry + 4, Now);
				}
				return ReadWord(Runtime, Entry + 8) <= Now - ReadWord(Runtime, Entry + 4);
			}
		}
		return true;
	}

	void ResetDelay(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Code)
	{
		const uint32 Count = ReadWord(Runtime, Fighter.Handle + FOKFighter::ButtonDelays);
		for (uint32 i = 0; i < Count && i < 10; ++i)
		{
			const uint32 Entry = Fighter.Handle + FOKFighter::ButtonDelays + 4 + i * 12;
			if (ReadWord(Runtime, Entry) == Code)
			{
				WriteWord(Runtime, Entry + 4, 0);
				return;
			}
		}
	}

	/** The plain-button order (0xDF0948): chords and combinations before single buttons and directions. */
	const uint32 ButtonOrder[] = {
		0x80000030, 0x80000031, 0x80000014, 0x80000015, 0x80000016, 0x80000017, 0x80000018, 0x80000019, 0x80000034, 0x80000035,
		0x80000036, 0x80000037, 0x80000038, 0x80000032, 0x80000033, 0x80000039, 0x80000005, 0x80000006, 0x80000007, 0x80000008,
		0x8000001C, 0x8000002C, 0x8000002B, 0x8000000F, 0x80000010, 0x80000011, 0x80000012, 0x8000001F, 0x80000020, 0x8000002E,
		0x8000002F, 0x80000021, 0x80000022, 0x80000001, 0x80000002, 0x8000002D, 0x80000003, 0x80000004, 0x8000000D, 0x8000000E,
		0x8000001E, 0x8000001B, 0x8000001D };

	/** The plain buttons (FUN_008AAA70): the first pressed button with a bound, enabled slot whose predicate passes acts. */
	bool PlainButtons(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player)
	{
		const uint32 Pending = ReadWord(Runtime, Player.Handle + FOKPlayer::PendingButton);
		WriteWord(Runtime, Player.Handle + FOKPlayer::PendingButton, 0x80000000u);
		for (const uint32 Code : ButtonOrder)
		{
			const uint32 S = SlotAddress(Fighter, Code);
			if (Pending != Code)
			{
				const bool bDown = ReadWord(Runtime, S + FOKButtonSlot::PressMode) == 0 ? OKPlayerInput::HeldCommand(Runtime, Player, Code) : OKPlayerInput::Pressed(Runtime, Player, Code);
				if (!bDown)
				{
					ResetDelay(Runtime, Fighter, Code);
					continue;
				}
				if (!DelayPassed(Runtime, Fighter, Code))
				{
					continue;
				}
			}
			if (!ReadWord(Runtime, S + FOKButtonSlot::Bound) || !ReadWord(Runtime, S + FOKButtonSlot::Enabled) || !ReadWord(Runtime, S + FOKButtonSlot::Allowed))
			{
				continue;
			}
			if (const uint32 Callback = ReadWord(Runtime, S + FOKButtonSlot::Callback))
			{
				WriteWord(Runtime, Fighter.Handle + 0x75F0, S);
				if (!ButtonPredicate(Runtime, Fighter, Callback))
				{
					continue;
				}
			}
			uint32 Use = Code;
			if (ReadWord(Runtime, S + FOKButtonSlot::Redirect))
			{
				Use = ReadWord(Runtime, S + FOKButtonSlot::RedirectCode);
			}
			const uint32 U = SlotAddress(Fighter, Use);
			const uint32 State = ReadWord(Runtime, Fighter.Handle + FOKFighter::State);
			if (ReadWord(Runtime, U + FOKButtonSlot::Attack) == 1)
			{
				if (State == 0x200F)
				{
					WriteWord(Runtime, Fighter.Handle + 0x39C, Use);		// [inf] buffered for later (virtual +0x148)
					return false;
				}
				if (StartAttack(Runtime, Fighter, Player, Use))
				{
					return true;
				}
			}
			else if (State != 0x200F)
			{
				if (ReadWord(Runtime, U + FOKButtonSlot::Direct) == 0)
				{
					BackToNormalFromButton(Runtime, Fighter);
					DisableAllCommands(Runtime, Fighter);
					ResetForMove(Runtime, Fighter);
					WriteWord(Runtime, Fighter.Handle + 0x2F54, 0);
					return XferToGenericCommand(Runtime, Fighter, Use);
				}
				UE_LOG(LogOKVM, Warning, TEXT("%s: button %08X runs a move-list command (FUN_008A06F0), which isn't implemented"), *Fighter.Name, Use);
			}
		}
		return false;
	}

	/** FUN_0089C920 (begin_special_move): the record counts its use; the fighter drops what it was doing. */
	void BeginSpecialMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Record)
	{
		WriteWord(Runtime, Record + 0x48, ReadWord(Runtime, Record + 0x48) + 1);
		WriteWord(Runtime, Record + 0x44, ReadWord(Runtime, Record + 0x44) + 1);
		BackToNormalFromButton(Runtime, Fighter);
		WriteWord(Runtime, Fighter.Handle + 0x2F54, 0);
		ResetForMove(Runtime, Fighter);
	}

	/**
	 * XferToSpecialMove (FUN_008A75F0, and FUN_008A76D0 for EX moves): the record's function (+0x34) of its script
	 * (+0x24) becomes the fighter's attack. bSpecial is the transfer's last flag (1 for plain specials).
	 */
	bool XferToSpecialMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Record, bool bSpecial)
	{
		const uint32 Script = ReadWord(Runtime, Record + OKCommand::Script + 4), Function = ReadWord(Runtime, Record + OKCommand::Function);
		if (Function == 0)
		{
			return true;
		}
		WriteWord(Runtime, Fighter.Handle + 0x214, ReadWord(Runtime, Record + 0xC));
		const uint32 Zero[4] = {};
		for (int32 i = 0; i < 4; ++i)
		{
			WriteWord(Runtime, Fighter.Handle + 0x7890 + i * 4, Zero[i]);		// exit_to_stance_now (virtual +0x1BC)
		}
		DisableAllCommands(Runtime, Fighter);
		return XferToAttackScript(Runtime, Fighter, Script, Function, 0xFFFFFFFFu, 0xFFFFFFFFu, bSpecial);
	}

	static TAutoConsoleVariable<int32> CVarTraceSpecials(TEXT("ok.Input.TraceSpecials"), 0,
		TEXT("1: log each special move whose button is pressed, and which of its tests fail. 2: every special move, every frame."));

	/**
	 * Matcher type 0, a special move (FUN_008A9510): its gate (FUN_008A1EF0: enabled, able to act, on the ground,
	 * past its waits), its button pressed this frame, and its motion. [unk] Records with flag +0x7C also accept the
	 * player's +0x2C test (FUN_00453A80), not decoded yet.
	 */
	bool MatchSpecial(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player, uint32 Record)
	{
		const uint32 Now = uint32(Runtime.GetSimFrame());
		if (CVarTraceSpecials.GetValueOnAnyThread() >= 2 || (CVarTraceSpecials.GetValueOnAnyThread() != 0 && OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button))))
		{
			const uint32 Seq = ReadWord(Runtime, Record + OKCommand::Sequence);
			UE_LOG(LogOKVM, Log, TEXT("frame %u: %s special %08X (function %u): enabled %u/%u, +0x60 %u, can act %d, airborne %d, waits %u/%u, motion %d (window %u, sequence %08X: %08X %08X %08X)"),
				Now, *Fighter.Name, Record, ReadWord(Runtime, Record + OKCommand::Function), ReadWord(Runtime, Record + 4), ReadWord(Runtime, Record + 8),
				ReadWord(Runtime, Record + 0x60), CanAct(Runtime, Fighter), Airborne(Runtime, Fighter), ReadWord(Runtime, Record + 0x38), ReadWord(Runtime, Record + 0x40),
				Seq == 0 || OKPlayerInput::Motion(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Window), Seq), ReadWord(Runtime, Record + OKCommand::Window), Seq,
				Seq ? ReadWord(Runtime, Seq) : 0, Seq ? ReadWord(Runtime, Seq + 4) : 0, Seq ? ReadWord(Runtime, Seq + 8) : 0);
		}
		if (ReadWord(Runtime, Record + 4) != 1 || ReadWord(Runtime, Record + 8) != 1 || ReadWord(Runtime, Record + 0x60) != 0
			|| !CanAct(Runtime, Fighter) || Airborne(Runtime, Fighter) || ReadWord(Runtime, Record + 0x38) >= Now || ReadWord(Runtime, Record + 0x40) >= Now)
		{
			return false;
		}
		const uint32 Sequence = ReadWord(Runtime, Record + OKCommand::Sequence);
		if (!OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button))
			|| (Sequence != 0 && !OKPlayerInput::Motion(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Window), Sequence)))
		{
			return false;
		}
		BeginSpecialMove(Runtime, Fighter, Record);
		// XferToSpecialMove gains the record's meter (+0.02 for a special) when its flag (+0x94) is set.
		if (ReadWord(Runtime, Record + OKCommand::MeterFlag) != 0)
		{
			OKAddMeter(Runtime, Player, BitsToFloat(ReadWord(Runtime, Record + OKCommand::MeterCost)));
		}
		return XferToSpecialMove(Runtime, Fighter, Record, true);
	}

	/**
	 * Matcher type 4, an EX special (FUN_008A9BB0): the same gate, and the meter covers the cost (+0x98) unless the
	 * meter is full; a cost of 100 never matches below full. Then XferToEXSpecialMove spends it.
	 * [inf] Records with +0x80 set test their button through the player's virtual +0x1C instead of +0x14; both are
	 * taken as a press here.
	 */
	bool MatchExSpecial(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player, uint32 Record)
	{
		const uint32 Now = uint32(Runtime.GetSimFrame());
		if (CVarTraceSpecials.GetValueOnAnyThread() >= 2 || (CVarTraceSpecials.GetValueOnAnyThread() != 0 && OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button))))
		{
			UE_LOG(LogOKVM, Log, TEXT("frame %u: %s EX special %08X (function %u): button %08X pressed %d, cost %g, meter %g, +0x80 %u"),
				Now, *Fighter.Name, Record, ReadWord(Runtime, Record + OKCommand::Function), ReadWord(Runtime, Record + OKCommand::Button),
				OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button)), BitsToFloat(ReadWord(Runtime, Record + OKCommand::MeterCost)),
				BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::Meter)), ReadWord(Runtime, Record + 0x80));
		}
		if (ReadWord(Runtime, Record + 4) != 1 || ReadWord(Runtime, Record + 8) != 1 || ReadWord(Runtime, Record + 0x60) != 0
			|| !CanAct(Runtime, Fighter) || Airborne(Runtime, Fighter) || ReadWord(Runtime, Record + 0x38) >= Now || ReadWord(Runtime, Record + 0x40) >= Now)
		{
			return false;
		}
		const float Cost = BitsToFloat(ReadWord(Runtime, Record + OKCommand::MeterCost));
		const float Meter = BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::Meter));
		if ((Meter < Cost || Cost == 100.0f) && Meter < 1.0f)
		{
			return false;
		}
		const uint32 Sequence = ReadWord(Runtime, Record + OKCommand::Sequence);
		if (!OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button))
			|| (Sequence != 0 && !OKPlayerInput::Motion(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Window), Sequence)))
		{
			return false;
		}
		BeginSpecialMove(Runtime, Fighter, Record);
		if (Cost != 100.0f)
		{
			OKAddMeter(Runtime, Player, -Cost);
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %u: %s EX special, meter %.3f"), Now, *Fighter.Name, BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::Meter)));
		return XferToSpecialMove(Runtime, Fighter, Record, true);
	}

	/**
	 * Matcher type 3, a fatality (FatalityMatcher, FUN_008A9F90): only while the finishing window is open (settings
	 * +0x10, the fatalities option, on; game info flags 0x80 set and 0x100, a finisher playing, clear), with the
	 * fighters strictly between the record's distances and the opponent dizzy (state 0x4205; MKKE asks that it can be
	 * hit and isn't flagged +0x2EC & 0x80). Then its button and motion. The middle of the range goes to game info
	 * +0x54, where the cinema is placed, and the record's function takes over as a special move.
	 */
	bool MatchFatality(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player, uint32 Record)
	{
		const uint32 Game = Runtime.FindBinding(TEXT("GFGGameInfo"));
		const uint32 GameFlags = ReadWord(Runtime, Game + FOKGameInfo::Flags);
		FOKFighter* Him = Fighter.Opponent;
		if (CVarTraceSpecials.GetValueOnAnyThread() >= 2 || (CVarTraceSpecials.GetValueOnAnyThread() != 0 && OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button))))
		{
			UE_LOG(LogOKVM, Log, TEXT("frame %u: %s fatality %08X: option %u, game flags %08X, opponent state %04X +0x2EC %08X, %.0f apart (%g-%g), motion %d"),
				uint32(Runtime.GetSimFrame()), *Fighter.Name, Record, ReadWord(Runtime, Game + 0x194), GameFlags,
				Him ? ReadWord(Runtime, Him->Handle + FOKFighter::State) : 0u, Him ? ReadWord(Runtime, Him->Handle + 0x2EC) : 0u,
				(Him && Him->Body && Fighter.Body) ? FVector::Dist2D(Fighter.Body->GetWorldLocation(), Him->Body->GetWorldLocation()) : -1.0,
				BitsToFloat(ReadWord(Runtime, Record + OKCommand::MinDistance)), BitsToFloat(ReadWord(Runtime, Record + OKCommand::MaxDistance)),
				ReadWord(Runtime, Record + OKCommand::Sequence) == 0 || OKPlayerInput::Motion(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Window), ReadWord(Runtime, Record + OKCommand::Sequence)));
		}
		if (ReadWord(Runtime, Game + 0x194) == 0 || (GameFlags & 0x80) == 0 || (GameFlags & 0x100) != 0 || !Him || !Fighter.Body || !Him->Body)
		{
			return false;
		}
		const float Distance = float(FVector::Dist2D(Fighter.Body->GetWorldLocation(), Him->Body->GetWorldLocation()));
		const float Near = BitsToFloat(ReadWord(Runtime, Record + OKCommand::MinDistance));
		const float Far = BitsToFloat(ReadWord(Runtime, Record + OKCommand::MaxDistance));
		if (Distance <= Near || Distance >= Far || (ReadWord(Runtime, Him->Handle + 0x2EC) & 0x80) != 0
			|| ReadWord(Runtime, Him->Handle + FOKFighter::State) != 0x4205)
		{
			return false;
		}
		WriteWord(Runtime, Game + 0x54, FloatToBits((Near + Far) * 0.5f));
		const uint32 Sequence = ReadWord(Runtime, Record + OKCommand::Sequence);
		if (!OKPlayerInput::Pressed(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Button))
			|| (Sequence != 0 && !OKPlayerInput::Motion(Runtime, Player, ReadWord(Runtime, Record + OKCommand::Window), Sequence)))
		{
			return false;
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %u: %s fatality (function %u) at %.0f apart"), uint32(Runtime.GetSimFrame()), *Fighter.Name,
			ReadWord(Runtime, Record + OKCommand::Function), Distance);
		BeginSpecialMove(Runtime, Fighter, Record);
		return XferToSpecialMove(Runtime, Fighter, Record, false);
	}

	/**
	 * Matcher type 8, an X-ray (FUN_008A99B0): the special gate, the meter covers the cost (three bars) unless it's full,
	 * the X-ray chord (0x80000038, flip stance and block) this frame, and no cinema on either fighter (FUN_00451030).
	 * Then two ticks of invulnerability and XferToEXSpecialMove, which spends the cost. MKKE also asks that there is
	 * an opponent (virtual +0x1E0(0)). Type 9, an air X-ray (FUN_008A9AB0, registered by 66.20), has the air gate
	 * (FUN_008A1F80): in the air, and higher above the floor than the record's +0x8C.
	 */
	bool MatchXray(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player, uint32 Record, bool bAir)
	{
		const uint32 Now = uint32(Runtime.GetSimFrame());
		const float Cost = BitsToFloat(ReadWord(Runtime, Record + OKCommand::MeterCost));
		const float Meter = BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::Meter));
		if (CVarTraceSpecials.GetValueOnAnyThread() >= 2 || (CVarTraceSpecials.GetValueOnAnyThread() != 0 && OKPlayerInput::Pressed(Runtime, Player, 0x80000038)))
		{
			UE_LOG(LogOKVM, Log, TEXT("frame %u: %s X-ray %08X (function %u): can act %d, airborne %d, cost %g, meter %g, cinema %d"),
				Now, *Fighter.Name, Record, ReadWord(Runtime, Record + OKCommand::Function), CanAct(Runtime, Fighter), Airborne(Runtime, Fighter),
				Cost, Meter, Runtime.GetActiveCinema() != 0);
		}
		const bool bPlace = bAir
			? (Airborne(Runtime, Fighter) && Fighter.Body && Fighter.Body->GetHeight() > BitsToFloat(ReadWord(Runtime, Record + OKCommand::Extra)))
			: !Airborne(Runtime, Fighter);
		if (!CanAct(Runtime, Fighter) || !bPlace || ReadWord(Runtime, Record + 0x38) >= Now || ReadWord(Runtime, Record + 0x40) >= Now)
		{
			return false;
		}
		if ((Meter < Cost || Cost == 100.0f) && Meter < 1.0f)
		{
			return false;
		}
		if (!OKPlayerInput::Pressed(Runtime, Player, 0x80000038) || Runtime.GetActiveCinema() != 0 || !Fighter.Opponent)
		{
			return false;
		}
		WriteWord(Runtime, Fighter.Handle + FOKFighter::Invulnerable, Now + 2);
		BeginSpecialMove(Runtime, Fighter, Record);
		if (Cost != 100.0f)
		{
			OKAddMeter(Runtime, Player, -Cost);
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %u: %s X-ray, meter %.3f"), Now, *Fighter.Name, BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::Meter)));
		return XferToSpecialMove(Runtime, Fighter, Record, true);
	}

	/** CheckSpecialMoves (FUN_008AB6F0): the fighter's command records, in order; the first that matches wins. */
	bool CheckCommands(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FOKPlayer& Player)
	{
		if (Fighter.Opponent && (ReadWord(Runtime, Fighter.Opponent->Handle + 0x2EC) & 0x400000))
		{
			return false;
		}
		const uint32 Count = ReadWord(Runtime, Fighter.Handle + FOKFighter::CommandCount);
		for (uint32 i = 0; i < Count && i < uint32(FOKFighter::MaxCommandList); ++i)
		{
			const uint32 Record = ReadWord(Runtime, Fighter.Handle + FOKFighter::CommandList + i * 4);
			if (!ReadWord(Runtime, Record + OKCommand::Enabled) || ReadWord(Runtime, Record + 0x10))
			{
				continue;
			}
			const uint32 Type = ReadWord(Runtime, Record + OKCommand::Type);
			if (Type == 0)
			{
				if (MatchSpecial(Runtime, Fighter, Player, Record))
				{
					return true;
				}
			}
			else if (Type == 4)
			{
				if (MatchExSpecial(Runtime, Fighter, Player, Record))
				{
					return true;
				}
			}
			else if (Type == 3)
			{
				if (MatchFatality(Runtime, Fighter, Player, Record))
				{
					return true;
				}
			}
			else if (Type == 8 || Type == 9)
			{
				if (MatchXray(Runtime, Fighter, Player, Record, Type == 9))
				{
					return true;
				}
			}
			// [unk] Types 1, 2, 5-7 and 10 (special variants, babality, tag) have their own matchers, not ported yet.
		}
		return false;
	}

	// 78.67 input_pressed(info, code, &frame, consume): a press waiting in the buffer (FUN_00451FB0)
	bool InputPressed(FOKScriptThread& Thread)
	{
		uint32 Code = 0, Out = 0, Consume = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Code) || !Thread.ArgRaw(2, Out) || !Thread.ArgRaw(3, Consume))
		{
			return false;
		}
		uint32 Frame = 0;
		const bool bFound = Player && OKPlayerInput::TryPress(Thread.GetRuntime(), *Player, Code, &Frame, Consume != 0);
		if (bFound && Out != 0)
		{
			Thread.StoreBytes(Out, &Frame, 4);
		}
		Thread.SetResult(bFound ? 1 : 0);
		return true;
	}

	// 78.71 (info, code) (FUN_00446120): a waiting press of the code is consumed, so it doesn't start anything else
	// (Sub-Zero's X-ray takes the press that started it). The disassembly prints the code as an import.
	bool ConsumePress(FOKScriptThread& Thread)
	{
		uint32 Code = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (Player && Thread.ArgRaw(1, Code))
		{
			OKPlayerInput::TryPress(Thread.GetRuntime(), *Player, Code, nullptr, true);
		}
		return true;
	}

	// 78.69 input_pressed_dir(info, code, direction, &frame, consume): the same, the press recorded with that
	// direction held (FUN_00446180). Its chord codes 0x17 and 0x18 both test BP + BK, as in the exe.
	bool InputPressedDir(FOKScriptThread& Thread)
	{
		uint32 Code = 0, Direction = 0, Out = 0, Consume = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Code) || !Thread.ArgRaw(2, Direction) || !Thread.ArgRaw(3, Out) || !Thread.ArgRaw(4, Consume))
		{
			return false;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		uint32 Buffer = 0, Frame = 0;
		if (Player)
		{
			Runtime.LoadHeapWord(Player->Handle + FOKPlayer::InputBuffer, Buffer);
		}
		auto Chord = [&](uint32 A, uint32 B, uint32 Dir) { return OKInput::NextChord(Runtime, Buffer, A, B, Dir, true, &Frame); };
		auto Next = [&](uint32 C, uint32 Dir) { return OKInput::NextPress(Runtime, Buffer, OKInput::CodeToMask(C), Dir, true, &Frame); };
		bool bFound = false, bChord = true;
		switch (Code)
		{
		case 0x80000032: bFound = Chord(0x80, 0x10, 0xFFFFFFFFu) || Next(0x8000001E, 0xFFFFFFFFu); bChord = false; break;
		case 0x80000033: bFound = Chord(0x20, 0x80, 0xFFFFFFFFu) || Next(0x8000001B, 0xFFFFFFFFu); bChord = false; break;
		case 0x80000039: bFound = Chord(0x40, 0x20, 0xFFFFFFFFu) || Next(0x8000001D, 0xFFFFFFFFu); bChord = false; break;
		case 0x80000014: bFound = Chord(0x40, 0x10, Direction); break;
		case 0x80000015: bFound = Chord(0x40, 0x20, Direction); break;
		case 0x80000016: bFound = Chord(0x40, 0x80, Direction); break;
		case 0x80000017: case 0x80000018: bFound = Chord(0x10, 0x80, Direction); break;
		case 0x80000019: bFound = Chord(0x20, 0x80, Direction); break;
		case 0x80000034: bFound = Chord(0x40, 0x8000, Direction); break;
		case 0x80000035: bFound = Chord(0x10, 0x8000, Direction); break;
		case 0x80000036: bFound = Chord(0x20, 0x8000, Direction); break;
		case 0x80000037: bFound = Chord(0x80, 0x8000, Direction); break;
		case 0x80000038: bFound = Chord(0x4000, 0x8000, Direction); break;
		default: bFound = Player && Next(Code, Direction); bChord = false; break;
		}
		if (bFound && Consume)
		{
			OKInput::Consume(Runtime, Buffer);
			if (bChord)
			{
				OKInput::Consume(Runtime, Buffer);
			}
		}
		if (bFound && Out != 0)
		{
			Thread.StoreBytes(Out, &Frame, 4);
		}
		Thread.SetResult(bFound ? 1 : 0);
		return true;
	}

	// 78.24 last_input(info, &button, &direction): the newest press, if it's unread (FUN_00426EC0)
	bool LastInput(FOKScriptThread& Thread)
	{
		uint32 OutButton = 0, OutDirection = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, OutButton) || !Thread.ArgRaw(2, OutDirection))
		{
			return false;
		}
		uint32 Buffer = 0, Button = 0, Direction = 0;
		if (Player)
		{
			Thread.GetRuntime().LoadHeapWord(Player->Handle + FOKPlayer::InputBuffer, Buffer);
		}
		const bool bFound = OKInput::LastPress(Thread.GetRuntime(), Buffer, Button, Direction);
		if (bFound)
		{
			Thread.StoreBytes(OutButton, &Button, 4);
			Thread.StoreBytes(OutDirection, &Direction, 4);
		}
		Thread.SetResult(bFound ? 1 : 0);
		return true;
	}

	/** FUN_008952A0: able to block (on the ground, its waits past, alive) with Block held. */
	bool BlockHeld(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
	{
		const uint32 Now = uint32(Runtime.GetSimFrame());
		return Fighter.Player && !Airborne(Runtime, Fighter) && ReadWord(Runtime, Fighter.Handle + 0x2F58) == 0 && ReadWord(Runtime, Fighter.Handle + 0x2F5C) == 0
			&& ReadWord(Runtime, Fighter.Handle + 0x2F60) <= Now && ReadWord(Runtime, Fighter.Handle + 0x2F64) <= Now && Fighter.Health > 0.0f
			&& OKPlayerInput::Held(Runtime, *Fighter.Player, 0x8000001C);
	}

	// 137.157 is_holding_block(obj): FUN_00895310 -> FUN_008952A0, then the result is Down held. [inf] So it reads as
	// "blocking low".
	bool IsHoldingBlock(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(Fighter && BlockHeld(Runtime, *Fighter) && OKPlayerInput::Held(Runtime, *Fighter->Player, 0x80000004) ? 1 : 0);
		return true;
	}

	// 137.156 (named is_crouch_blocking) (obj) -> FUN_008952A0 itself: blocking, high or low
	bool IsBlocking(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && BlockHeld(Thread.GetRuntime(), *Fighter) ? 1 : 0);
		return true;
	}

	// 78.13 (info, slot): clears button slot +0x30BC of the player's active fighter (FUN_0044D140)
	bool DisallowButton(FOKScriptThread& Thread)
	{
		uint32 Slot = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Slot) || !Player || !Player->Fighter || Slot >= uint32(FOKFighter::MaxButtonSlots))
		{
			return true;
		}
		WriteWord(Thread.GetRuntime(), Player->Fighter->Handle + FOKFighter::ButtonSlots + Slot * FOKFighter::ButtonSlotBytes + FOKButtonSlot::Allowed, 0);
		return true;
	}

	/** Binds a button slot (FUN_0044F3B0 / FUN_0044F490). */
	void BindSlot(FOKScriptThread& Thread, FOKFighter& Fighter, uint32 Code, uint32 PressMode, uint32 Script, uint32 Callback, uint32 Function, bool bAttack)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 S = SlotAddress(Fighter, Code);
		WriteWord(Runtime, S + FOKButtonSlot::Bound, 1);
		WriteWord(Runtime, S + FOKButtonSlot::Callback, Callback);
		WriteWord(Runtime, S + FOKButtonSlot::Script + 4, Script);
		WriteWord(Runtime, S + FOKButtonSlot::Script + 8, Script);
		WriteWord(Runtime, S + FOKButtonSlot::PressMode, PressMode);
		WriteWord(Runtime, S + FOKButtonSlot::Attack, bAttack ? 1 : 0);
		WriteWord(Runtime, S + FOKButtonSlot::Direct, bAttack ? 1 : 0);
		WriteWord(Runtime, S + FOKButtonSlot::Function, Function);
		WriteWord(Runtime, S + FOKButtonSlot::Redirect, 0);
	}

	// 81.219 bind_button(obj, code, script, predicate): an attack button, pressed this frame (FUN_0044F3B0)
	bool BindButton(FOKScriptThread& Thread)
	{
		uint32 Code = 0, Script = 0, Callback = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Code) || !Thread.ArgRaw(2, Script) || !Thread.ArgRaw(3, Callback))
		{
			return false;
		}
		if (Fighter)
		{
			BindSlot(Thread, *Fighter, Code, 1, Script, Callback, 0, true);
		}
		return true;
	}

	// 81.221 bind_button_func(obj, code, pressMode, script, predicate, function) (FUN_0044F490)
	bool BindButtonFunc(FOKScriptThread& Thread)
	{
		uint32 Code = 0, Mode = 0, Script = 0, Callback = 0, Function = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Code) || !Thread.ArgRaw(2, Mode) || !Thread.ArgRaw(3, Script) || !Thread.ArgRaw(4, Callback) || !Thread.ArgRaw(5, Function))
		{
			return false;
		}
		if (Fighter)
		{
			BindSlot(Thread, *Fighter, Code, Mode, Script, Callback, Function, false);
		}
		return true;
	}

	// 81.222 bind_button_named(obj, code, pressMode, script, predicate, "function") (FUN_0044F500)
	bool BindButtonNamed(FOKScriptThread& Thread)
	{
		uint32 Code = 0, Mode = 0, Script = 0, Callback = 0, Name = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Code) || !Thread.ArgRaw(2, Mode) || !Thread.ArgRaw(3, Script) || !Thread.ArgRaw(4, Callback) || !Thread.ArgRaw(5, Name))
		{
			return false;
		}
		FOKScriptInstance* Instance = InstanceFromValue(Thread.GetRuntime(), Script);
		if (!Fighter || !Instance)
		{
			return true;		// MKKE binds nothing without the script
		}
		FString FunctionName;
		for (uint32 i = 0; i < 64; ++i)
		{
			uint8 Char = 0;
			if (!Thread.LoadBytes(Name + i, &Char, 1) || Char == 0)
			{
				break;
			}
			FunctionName.AppendChar(TCHAR(Char));
		}
		const int32 Function = Instance->File->FindFunctionByName(FunctionName);
		if (Function != INDEX_NONE)
		{
			BindSlot(Thread, *Fighter, Code, Mode, Script, Callback, uint32(Function + 1), false);
		}
		return true;
	}

	// 81.203 moveset_command_button(obj, code, delay): the button acts once held for delay frames (FUN_00449310)
	bool SetButtonDelay(FOKScriptThread& Thread)
	{
		uint32 Code = 0, Delay = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Code) || !Thread.ArgRaw(2, Delay) || !Fighter)
		{
			return Fighter == nullptr;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Count = ReadWord(Runtime, Fighter->Handle + FOKFighter::ButtonDelays);
		for (uint32 i = 0; i < Count && i < 10; ++i)
		{
			const uint32 Entry = Fighter->Handle + FOKFighter::ButtonDelays + 4 + i * 12;
			if (ReadWord(Runtime, Entry) == Code)
			{
				WriteWord(Runtime, Entry + 8, Delay);
				return true;
			}
		}
		const uint32 Entry = Fighter->Handle + FOKFighter::ButtonDelays + 4 + FMath::Min<uint32>(Count, 9) * 12;
		WriteWord(Runtime, Entry, Code);
		WriteWord(Runtime, Entry + 4, 0);
		WriteWord(Runtime, Entry + 8, Delay);
		WriteWord(Runtime, Fighter->Handle + FOKFighter::ButtonDelays, FMath::Min<uint32>(Count + 1, 10));
		return true;
	}

	// 81.217 clear_moveset_commands(obj): every button slot unbound
	bool ClearButtons(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		for (int32 Slot = 0; Fighter && Slot < FOKFighter::MaxButtonSlots; ++Slot)
		{
			WriteWord(Thread.GetRuntime(), Fighter->Handle + FOKFighter::ButtonSlots + Slot * FOKFighter::ButtonSlotBytes, 0);
		}
		return true;
	}

	/** 6.102-6.105: the movement scripts GameInfo holds (+0x94 Moving, +0xA0 Jumping, +0xAC BlockingDucking, +0xB8 GenericThrow). */
	bool ScriptInstanceValue(FOKScriptThread& Thread, const TCHAR* Name)
	{
		FOKScriptInstance* Instance = Thread.GetRuntime().FindInstanceByName(Name);
		if (!Instance)
		{
			const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
			const FOKMkoFile* File = (Scripts && Scripts->GetLibrary()) ? Scripts->GetLibrary()->FindScript(FString(Name) + TEXT(".mko")) : nullptr;
			Instance = File ? Thread.GetRuntime().GetInstance(*File) : nullptr;
		}
		Thread.SetResult(Instance ? (FOKScriptRuntime::InstanceTag << 24) | uint32(Instance->Id) : 0);
		return true;
	}
	bool MovingScript(FOKScriptThread& Thread) { return ScriptInstanceValue(Thread, TEXT("Moving")); }
	bool JumpingScript(FOKScriptThread& Thread) { return ScriptInstanceValue(Thread, TEXT("Jumping")); }
	bool BlockingDuckingScript(FOKScriptThread& Thread) { return ScriptInstanceValue(Thread, TEXT("BlockingDucking")); }
	bool GenericThrowScript(FOKScriptThread& Thread) { return ScriptInstanceValue(Thread, TEXT("GenericThrow")); }

	// 137.113 / 81.205 enable_all_commands(obj) -> fighter virtual +0x1C4 (FUN_00448A90). Was named turnaround.
	bool EnableAllCommands(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			EnableAllCommandsOf(Thread.GetRuntime(), *Fighter);
		}
		return true;
	}

	// 137.236 combo_branch(obj, n, &button, &direction) -> the n-th move one step longer than the player's combo string
	// (FUN_00896770 -> FUN_00449AC0), its next step's button and direction; turnaround steps retry as plain ones
	bool ComboBranch(FOKScriptThread& Thread)
	{
		uint32 N = 0, OutButton = 0, OutDirection = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, N) || !Thread.ArgRaw(2, OutButton) || !Thread.ArgRaw(3, OutDirection))
		{
			return false;
		}
		Thread.SetResult(0);
		if (!Fighter || !Fighter->Player)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Moveset = MovesetOf(Runtime, Fighter);
		const uint32 List = ReadWord(Runtime, Fighter->Handle + FOKFighter::MoveList);
		const uint32 Info = Fighter->Player->Handle;
		if (List > 4)
		{
			return true;
		}
		const uint32 Base = Moveset + FOKMoveset::MoveLists + List * FOKMoveset::MoveListBytes;
		for (int32 Retry = 0; Retry < 4; ++Retry)
		{
			const uint32 Count = ReadWord(Runtime, Moveset + FOKMoveset::MoveListCounts + List * 4);
			const uint32 Length = ReadWord(Runtime, Info + FOKPlayer::ComboLength);
			uint32 Skip = N;
			for (uint32 i = 0; i < Count && i < uint32(FOKMoveset::MaxMoves); ++i)
			{
				const uint32 Move = Base + i * FOKMoveset::MoveBytes;
				auto Step = [&](uint32 u) { return ReadWord(Runtime, Move + 8 + u * 4); };
				bool bPrefix = true;
				for (uint32 u = 0; u < Length && bPrefix; ++u)
				{
					const uint32 Have = ReadWord(Runtime, Info + FOKPlayer::ComboSteps + u * 4);
					bPrefix = Have != 0xFFFFFFFFu && Step(u) == Have;
				}
				if (bPrefix && (ReadWord(Runtime, Move) & 1) == 0 && Length < 5 && Step(Length) != 0xFFFFFFFFu && (Length == 4 || Step(Length + 1) == 0xFFFFFFFFu))
				{
					if (Skip == 0)
					{
						const uint32 Button = (Step(Length) >> 16) | 0x80000000u, Direction = (Step(Length) & 0xFFFF) | 0x80000000u;
						Thread.StoreBytes(OutButton, &Button, 4);
						Thread.StoreBytes(OutDirection, &Direction, 4);
						Thread.SetResult(Move);
						return true;
					}
					--Skip;
				}
			}
			// A turnaround step (0x23, 0x3A-0x3C) retries as its plain direction.
			const uint32 First = ReadWord(Runtime, Info + FOKPlayer::ComboSteps);
			const uint32 Kind = (First & 0xFFFF) | 0x80000000u;
			uint32 Plain = 0;
			if (Kind == 0x80000023) { Plain = First & 0xFFFF0000u; }
			else if (Kind == 0x8000003A) { Plain = (First & 0xFFFF0000u) | 2; }
			else if (Kind == 0x8000003C) { Plain = (First & 0xFFFF0000u) | 1; }
			else if (Kind == 0x8000003B) { Plain = (First & 0xFFFF0000u) | 3; }
			else { return true; }
			WriteWord(Runtime, Info + FOKPlayer::ComboSteps, Plain);
		}
		return true;
	}

	// 1.10 create_proc(script, stackMode, procId, group, function) -> the process (FUN_0045D8C0): start_proc without
	// an owner, and with no fallback to the running script.
	bool CreateProc(FOKScriptThread& Thread)
	{
		uint32 Raw[5];
		for (int32 i = 0; i < 5; ++i)
		{
			if (!Thread.ArgRaw(i, Raw[i]))
			{
				return false;
			}
		}
		const uint32 ScriptRef = Raw[0], StackMode = Raw[1], ProcId = Raw[2], Group = Raw[3], FuncIndex = Raw[4];
		const FOKScriptInstance* Instance = ((ScriptRef >> 24) == FOKScriptRuntime::InstanceTag)
			? Thread.GetRuntime().FindInstanceById(int32(ScriptRef & 0x00FFFFFF)) : nullptr;
		if (!Instance)
		{
			Thread.SetResult(0);
			return true;
		}
		FString Error;
		FOKScriptThread* Child = Thread.GetRuntime().StartProcess(*Instance->File, int32(FuncIndex) - 1,
			int32(ProcId), int32(Group), StackMode != 0, Error);
		if (!Child)
		{
			return Thread.NativeFail(TEXT("create_proc: ") + Error);
		}
		Thread.SetResult(Child->GetHandle());
		return true;
	}

	// 137.7 (named char_in_danger) obj -> the player record of the fighter's side (FUN_0080E540 indexes the records
	// by fighter +0x7638 -> +0xB3C). Scripts read its damage multiplier (+0xB60) and such from it.
	bool FighterPlayer(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Player ? Fighter->Player->Handle : 0);
		return true;
	}

	// 118.47 life_query(info, life, seconds): animates the side's life bar from the last value to life over seconds
	// (FUN_00892970), then remembers it. [inf] Nothing draws life bars yet, so only the check of the object stands.
	bool LifeQuery(FOKScriptThread& Thread)
	{
		uint32 Life = 0, Seconds = 0;
		return Thread.ArgRaw(1, Life) && Thread.ArgRaw(2, Seconds);
	}

	// --- Movement (system F3). The state lives in the fighter at +0x304..+0x3B0; OKMotion steps it each frame. ---

	// 81.35 is_airborne(obj) -> the fighter's height above the floor, a float (FUN_00447FB0: actor z - GameInfo's floor)
	bool IsAirborne(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Fighter->Body)
		{
			Thread.SetResult(FloatToBits(Fighter->Body->GetHeight()));
		}
		return true;
	}

	// 81.41 stop(obj) (FUN_00447D00): kills the fighter's group-11 processes, ends a force-away and stops the air clock
	bool StopNative(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::Stop(Thread.GetRuntime(), *Fighter);
		}
		return true;
	}

	// 81.67 init_air_move(obj, ?) (FUN_00446CF0): flag 8 on (in the air), flag 0x80000 off; the animation keeps its rate
	bool InitAirMove(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::InitAirMove(Thread.GetRuntime(), *Fighter);
		}
		return true;
	}

	// 81.68 init_still_move(obj, resetRate) (FUN_00446D30): init_ground_move, then flag 0x80000 on
	bool InitStillMove(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::InitStillMove(Thread.GetRuntime(), *Fighter, ArgWord(Thread, 1) != 0);
		}
		return true;
	}

	// 81.69 init_3d_move(obj) (FUN_0044D8A0): flag 8 off, and character flag 4 (+0x1D0) off
	bool Init3dMove(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Runtime.SetHeapWord(Fighter->Handle + FOKFighter::Flags, ReadWord(Runtime, Fighter->Handle + FOKFighter::Flags) & ~OKMotion::AirFlag);
		Runtime.SetHeapWord(Fighter->Handle + FOKFighter::CharFlags, ReadWord(Runtime, Fighter->Handle + FOKFighter::CharFlags) & ~4u);
		return true;
	}

	// 81.87 after_land_input(obj) -> virtual +0x144 (FUN_00457500): +0x398
	bool AfterLandInput(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? ReadWord(Thread.GetRuntime(), Fighter->Handle + OKMotion::AfterLandInput) : 0);
		return true;
	}

	// 81.91 launch_and_land_ex(obj, speedZ, gravity, landFrame, earlyTicks, script, function, anim, startFrame, blend, ?,
	// keepRate) -> virtual +0x154 (FUN_00450D00). A speed of -123456 keeps the current one.
	bool LaunchAndLandEx(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::Launch(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4),
				ArgWord(Thread, 5), ArgWord(Thread, 6), ArgWord(Thread, 7), ArgFloat(Thread, 8), ArgFloat(Thread, 9), ArgWord(Thread, 11));
		}
		return true;
	}

	// 81.92 launch_and_land(obj, speedZ, gravity, landFrame, landOffset) -> virtual +0x158 (FUN_00450E60): the thread
	// isn't sent anywhere on landing; scripts wait for it (WaitForLandLL).
	bool LaunchAndLand(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::LaunchAndLand(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		}
		return true;
	}

	// 81.93 launch_to_height(obj, height, frames, landFrame, earlyTicks, script, function, anim, startFrame, blend, ?,
	// keepRate) -> virtual +0x15C (FUN_0044B730). Jumps use it through FGFightEngineScript.launch_and_land_to_height.
	bool LaunchToHeight(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::LaunchToHeight(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4),
				ArgWord(Thread, 5), ArgWord(Thread, 6), ArgWord(Thread, 7), ArgFloat(Thread, 8), ArgFloat(Thread, 9), ArgWord(Thread, 11));
		}
		return true;
	}

	// 81.94 launch_to_height_guaranteed(obj, height, frames, landFrame) -> virtual +0x160 (FUN_0044B7B0)
	bool LaunchToHeightGuaranteed(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::LaunchToHeightGuaranteed(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3));
		}
		return true;
	}

	// 81.95 force_away_from_opponent(obj, speed, decayStart, decay, extra) -> virtual +0x164 -> +0x140 (FUN_0044BD70).
	// A negative speed moves towards the opponent (jump_forward passes -350).
	bool ForceAwayFromOpponent(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::ForceAwayFromOpponent(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		}
		return true;
	}

	// 81.96 force_away_facing(obj, speed, decayStart, decay, extra) -> virtual +0x168 (FUN_0044BA10): backwards along
	// the fight line from where the fighter faces
	bool ForceAwayFacing(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::ForceAwayFacing(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		}
		return true;
	}

	// 81.97 stop_force_away(obj) -> virtual +0x16C (FUN_0044B810): +0x308 = 0
	bool StopForceAway(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			Thread.GetRuntime().SetHeapWord(Fighter->Handle + OKMotion::ForceAway, 0);
		}
		return true;
	}

	// 81.98 (named set_air_flag) obj, speedZ -> virtual +0x170 (FUN_0044BAD0): in the air with that vertical speed,
	// no gravity and no landing (move 2)
	bool SetAirborne(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::SetAirborne(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1));
		}
		return true;
	}

	// 81.103 is_landed(obj) -> virtual +0x184 (FUN_0044B710): no move, or landed (+0x304 is 0 or 8)
	bool IsLanded(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Move = Fighter ? ReadWord(Thread.GetRuntime(), Fighter->Handle + OKMotion::MoveKind) : 0;
		Thread.SetResult(Move == 0 || Move == 8 ? 1 : 0);
		return true;
	}

	// 81.107 (named set_gravity) obj, scale (FUN_0044BB90): really the air clock's speed. The animation runs at the
	// landing rate times it; 0 freezes both.
	bool SetAirSpeed(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			OKMotion::SetAirSpeed(Thread.GetRuntime(), *Fighter, ArgFloat(Thread, 1));
		}
		return true;
	}

	// 137.146 set_moving_speedup(obj, scale) (FUN_008D8BC0): +0xAA94, what walk and step rates are multiplied by
	bool SetMovingSpeedup(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			Thread.GetRuntime().SetHeapWord(Fighter->Handle + 0xAA94, ArgWord(Thread, 1));
		}
		return true;
	}

	// 81.246 snap_to_ground(obj) -> virtual +0x1E4 (FUN_0044E9A0): the fighter moved down (or up) so its root bone is
	// on the floor. Our root bone is at the actor.
	bool SnapToGround(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Fighter->Body)
		{
			FVector Location = Fighter->Body->GetWorldLocation();
			Location.Z = Fighter->Body->GetFloorZ();
			Fighter->Body->SetWorldLocation(Location);
		}
		return true;
	}

	/**
	 * 137.75 teleport_in_front(obj, distance, height, ?, ?, push) (FUN_00898D70 -> FUN_008924C0): the fighter is put
	 * distance from its opponent (virtual +0x1E0(0)) along the flat line from itself to the opponent, so -300 lands on
	 * its own side and 300 behind; at the opponent's height, or height above the floor when that isn't 0. [unk] With push set MKKE also moves the opponent a little along the same line, by a
	 * constant that depends on distance; that and the fourth and fifth arguments aren't read.
	 */
	bool TeleportInFront(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKFighter* Him = Fighter ? Fighter->Opponent : nullptr;
		if (!Fighter || !Fighter->Body || !Him || !Him->Body)
		{
			return true;
		}
		const FVector His = Him->Body->GetWorldLocation();
		FVector Towards = His - Fighter->Body->GetWorldLocation();
		Towards.Z = 0.0;
		Towards = Towards.GetSafeNormal();
		FVector Location = His + Towards * ArgFloat(Thread, 1);
		const float Height = ArgFloat(Thread, 2);
		if (Height != 0.0f)
		{
			Location.Z = Fighter->Body->GetFloorZ() + Height;
		}
		Fighter->Body->SetWorldLocation(Location);
		return true;
	}

	// 81.247 (named ground_height) obj -> 1 when the hips are less than 1 unit above the floor (FUN_004486D0)
	bool HipsOnFloor(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FVector Hips;
		Thread.SetResult(Fighter && Fighter->Body && Fighter->Body->GetBoneWorldLocation(TEXT("Hips"), Hips)
			&& Hips.Z < Fighter->Body->GetFloorZ() + 1.0 ? 1 : 0);
		return true;
	}

	// 81.117 (named chest_to_screen_flag) obj -> fighter virtual +0x19C (FUN_00447020): 1 when the opponent is further
	// along the fight line (to the right on screen), or there is none; also 1 in game mode 9
	bool OpponentOnRight(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(1);
		if (Fighter && Fighter->Body && Fighter->Opponent && Fighter->Opponent->Body)
		{
			const FVector& Line = Fighter->LineDirection;
			Thread.SetResult(FVector::DotProduct(Fighter->Body->GetWorldLocation(), Line) < FVector::DotProduct(Fighter->Opponent->Body->GetWorldLocation(), Line) ? 1 : 0);
		}
		return true;
	}

	// 59.51 (named anim_turnaround) obj -> the fighter's animation node on its main channel (FUN_0043EA10), or 0. Scripts
	// pass it to node natives (58.43). [inf] Our fighter has one node, so the fighter's own handle stands for it.
	bool AnimNode(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Body && Fighter->Body->GetAnimFrameCount() > 0 ? Fighter->Handle : 0);
		return true;
	}

	// 58.43 (node, frame) (FUN_00441CA0 -> FUN_00435D10): the node's sequence jumps to a frame, clamped to its length.
	// The node is 59.51's: the fighter's handle here.
	bool SetAnimFrameNative(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Fighter->Body)
		{
			Fighter->Body->SetAnimFrame(FMath::Max(0.0f, ArgFloat(Thread, 1)));
		}
		return true;
	}

	// 81.249 bone_id_with_flip(obj, boneId), also 59.78 -> virtual +0xB4 (FUN_0042FCE0): the bone id on the other side when
	// the mesh is mirrored (+0x758 bit 0). [inf] Our mirror reflects the drawn mesh, so a bone's world position is already
	// where the mirrored bone shows: the id comes back unchanged.
	bool BoneIdWithFlip(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? ArgWord(Thread, 1) : 0);
		return true;
	}

	// 81.243 (named height_level) obj, boneId, &vector, also 59.82 -> virtual +0xC4 (FUN_00448810): a bone's world position, by
	// MKKE bone id, into a script vector. 81.244 (obj, boneId, &vector, flip) (FUN_00448840) first mirrors the id when flip is
	// set (virtual +0xB4: unchanged here). A missing bone gives (0, 0, 0).
	bool BoneWorldPosition(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Out = ArgWord(Thread, 2);
		FVector Where = FVector::ZeroVector;
		const TCHAR* Bone = OKCollision::BoneName(int32(ArgWord(Thread, 1)));
		if (!Fighter || !Fighter->Body || !Bone || !Fighter->Body->GetBoneWorldLocation(Bone, Where))
		{
			Where = FVector::ZeroVector;
		}
		const float Values[3] = { float(Where.X), float(Where.Y), float(Where.Z) };
		return Thread.StoreBytes(Out, Values, sizeof(Values));
	}

	// 42.1 (stage) -> the stage object's virtual +4: its floor height (GameInfo +0x1C; FUN_00447FB0 measures heights from it).
	// [inf] Our stage stand-in has no code, so the floor the fighters stand on answers.
	bool StageFloorHeight(FOKScriptThread& Thread)
	{
		FOKFighter* Mine = Thread.MyFighter();
		Thread.SetResult(FloatToBits(Mine && Mine->Body ? float(Mine->Body->GetFloorZ()) : 0.0f));
		return true;
	}

	// 137.252 set_attack_callback(obj, script, function) (FUN_00896AE0): a handle to the script at +0x7B7C and the function at
	// +0x7B88, in the pending attack. [inf] What runs it (a hit callback, e.g. fstyle_callback_slowtime_ticks_4) isn't read yet.
	bool SetAttackCallback(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			Runtime.SetHeapWord(Fighter->Handle + 0x7B80, ArgWord(Thread, 1));
			Runtime.SetHeapWord(Fighter->Handle + 0x7B84, ArgWord(Thread, 1));		// our serial is the value
			Runtime.SetHeapWord(Fighter->Handle + 0x7B88, ArgWord(Thread, 2));
		}
		return true;
	}

	// --- MK objects (W1, OKObjects) ---------------------------------------------------------------------------

	/** An MK object from an argument: argument 0 through its mode bits (as FUN_00458930 resolves it), others raw. */
	FOKMkObject* ArgMkObject(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Handle = 0;
		const bool bRead = Index == 0 ? Thread.ArgValue(0, Handle) : Thread.ArgRaw(Index, Handle);
		return bRead ? OKObjects::Find(Thread.GetRuntime(), Handle) : nullptr;
	}

	FOKCollisionObject* ArgCollisionObject(FOKScriptThread& Thread, int32 Index)
	{
		FOKMkObject* Object = ArgMkObject(Thread, Index);
		return Object && Object->IsA(EOKScriptClass::CollisionObj) ? static_cast<FOKCollisionObject*>(Object) : nullptr;
	}

	/** A fighter or an MK object from a raw argument, for natives that take either. */
	FOKScriptObject* ArgAnyObject(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Handle = 0;
		const bool bRead = Index == 0 ? Thread.ArgValue(0, Handle) : Thread.ArgRaw(Index, Handle);
		return bRead && Handle != 0 ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
	}

	/** Where a fighter or MK object is (MKObj +8 -> actor +0xDC), or false. */
	bool ObjectLocation(FOKScriptObject* Object, FVector& OutLocation)
	{
		FOKFighter* Fighter = Object ? Object->AsFighter() : nullptr;
		if (Fighter && Fighter->Body)
		{
			OutLocation = Fighter->Body->GetWorldLocation();
			return true;
		}
		FOKMkObject* MkObject = Object ? Object->AsMkObject() : nullptr;
		if (MkObject)
		{
			OutLocation = MkObject->Location;
		}
		return MkObject != nullptr;
	}

	/** A string argument (a pool address), up to 80 characters. */
	FString ArgCString(FOKScriptThread& Thread, int32 Index)
	{
		const uint32 Address = ArgWord(Thread, Index);
		FString Text;
		for (uint8 Char = 0; Address != 0 && Text.Len() < 80 && Thread.LoadBytes(Address + Text.Len(), &Char, 1) && Char != 0; )
		{
			Text.AppendChar(TCHAR(Char));
		}
		return Text;
	}

	/**
	 * An offset in an object's frame (FUN_00430890, FUN_0043D860): x along its facing, y to its right (mirrored when
	 * it faces left, virtual +0x19C), z up. Fighters use their facing; MK objects their rotation.
	 */
	FVector FrameOffset(FOKScriptObject* Object, const FVector& Offset)
	{
		FOKFighter* Fighter = Object ? Object->AsFighter() : nullptr;
		if (Fighter && Fighter->Body)
		{
			const FVector Facing = Fighter->Body->GetFacing().GetSafeNormal2D();
			const FVector Right(-Facing.Y, Facing.X, 0.0);
			return Facing * Offset.X + Right * (Fighter->bFacingRight ? Offset.Y : -Offset.Y) + FVector::UpVector * Offset.Z;
		}
		FOKMkObject* MkObject = Object ? Object->AsMkObject() : nullptr;
		return MkObject ? MkObject->Rotation.RotateVector(Offset) : Offset;
	}

	bool AddToVectorArg(FOKScriptThread& Thread, uint32 Address, const FVector& Add)
	{
		float Vector[3];
		if (Address == 0 || !Thread.LoadBytes(Address, Vector, sizeof(Vector)))
		{
			return Address == 0;
		}
		Vector[0] += float(Add.X);
		Vector[1] += float(Add.Y);
		Vector[2] += float(Add.Z);
		return Thread.StoreBytes(Address, Vector, sizeof(Vector));
	}

	// 137.188 create_collision_obj(obj, script, propsFunction) -> a collision object for the fighter (FUN_008A7570)
	bool CreateCollisionObj(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? OKObjects::CreateCollisionObject(Thread.GetRuntime(), *Fighter, ArgWord(Thread, 1), ArgWord(Thread, 2)) : 0);
		return true;
	}

	// 121.1: the MKCollisionObj class, for is_a (1.12). Was named col_reschedule.
	bool ClassCollisionObj(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::CollisionObj));
		return true;
	}

	// 121.13 col_reschedule_cleanup(colObj) (FUN_008ADF90): resolves the contact it made, then its callback
	bool ColRescheduleCleanup(FOKScriptThread& Thread)
	{
		if (FOKCollisionObject* Object = ArgCollisionObject(Thread, 0))
		{
			OKObjects::Cleanup(Thread.GetRuntime(), *Object);
		}
		return true;
	}

	// 121.21 col_sphere(colObj, radius) (FUN_00895CA0): a sphere; the capsule sizes are cleared
	bool ColSphere(FOKScriptThread& Thread)
	{
		FOKCollisionObject* Object = ArgCollisionObject(Thread, 0);
		if (Object)
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			Runtime.SetHeapWord(Object->Handle + OKCollisionObj::Radius, ArgWord(Thread, 1));
			Runtime.SetHeapWord(Object->Handle + 0xFC, 0);
			Runtime.SetHeapWord(Object->Handle + 0x100, 0);
			Runtime.SetHeapWord(Object->Handle + OKCollisionObj::Shape, 0);
		}
		return true;
	}

	/** 121.30 (+0x1D4, a speed scale against walls) and 121.31 (+0x120, delete after a callback): a word of the object. */
	template <uint32 Offset>
	bool ColWord(FOKScriptThread& Thread)
	{
		if (FOKCollisionObject* Object = ArgCollisionObject(Thread, 0))
		{
			Thread.GetRuntime().SetHeapWord(Object->Handle + Offset, ArgWord(Thread, 1));
		}
		return true;
	}

	// 137.155 reaction_rerouter(obj, script, function) (FUN_00895120): fighter +0xAAA8 = the function its next reaction
	// goes to instead, in that script. [inf] Our reaction dispatch doesn't read it yet, so the usual reaction plays.
	bool ReactionRerouter(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			uint32 Function = 0;
			Thread.ArgValue(2, Function);
			Thread.GetRuntime().SetHeapWord(Fighter->Handle + 0xAAA8, Function);
		}
		return true;
	}

	// 121.32 col_hit_callback / 121.33 col_block_callback (colObj, script, function) (FUN_00895F10): kept unless no script
	bool ColHitCallback(FOKScriptThread& Thread)
	{
		FOKCollisionObject* Object = ArgCollisionObject(Thread, 0);
		const uint32 Script = ArgWord(Thread, 1);
		if (Object && Script != 0)
		{
			Object->OnHit = { Script, ArgWord(Thread, 2) };
		}
		return true;
	}

	bool ColBlockCallback(FOKScriptThread& Thread)
	{
		FOKCollisionObject* Object = ArgCollisionObject(Thread, 0);
		const uint32 Script = ArgWord(Thread, 1);
		if (Object && Script != 0)
		{
			Object->OnBlock = { Script, ArgWord(Thread, 2) };
		}
		return true;
	}

	bool ColWallCallback(FOKScriptThread& Thread)
	{
		FOKCollisionObject* Object = ArgCollisionObject(Thread, 0);
		const uint32 Script = ArgWord(Thread, 1);
		if (Object && Script != 0)
		{
			Object->OnWall = { Script, ArgWord(Thread, 2) };
		}
		return true;
	}

	// 6.148 (named obj_attach_bone) obj, owner, bone (FUN_00440620): puts the object where the owner's bone is now, or
	// at the owner without a bone. It doesn't follow it after.
	bool PlaceAtBone(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		FOKScriptObject* Owner = ArgAnyObject(Thread, 1);
		if (!Object || !Owner)
		{
			return true;
		}
		const FString Bone = ArgCString(Thread, 2);
		FOKFighter* Fighter = Owner->AsFighter();
		FVector Where = FVector::ZeroVector;
		if (!Bone.IsEmpty() && Fighter && Fighter->Body && Fighter->Body->GetBoneWorldLocation(Bone, Where))
		{
			Object->Location = Where;
		}
		else if (ObjectLocation(Owner, Where))
		{
			if (!Bone.IsEmpty())
			{
				UE_LOG(LogOKVM, Warning, TEXT("obj_attach_bone: no bone %s; the object goes to its owner"), *Bone);
			}
			Object->Location = Where;
		}
		return true;
	}

	// 6.146 (named obj_attach_to) obj, other (FUN_00437390): puts the object where the other one is now
	bool PlaceAtObject(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		FVector Where = FVector::ZeroVector;
		if (Object && ObjectLocation(ArgAnyObject(Thread, 1), Where))
		{
			Object->Location = Where;
		}
		return true;
	}

	// 52.78 kill_obj_in_ticks(obj, ticks) (FUN_0042F640): deleted at that frame; 0 ticks cancels it
	bool KillObjInTicks(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		if (Object)
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			const uint32 Ticks = ArgWord(Thread, 1);
			Runtime.SetHeapWord(Object->Handle + OKObject::DieFlag, Ticks != 0 ? 1 : 0);
			Runtime.SetHeapWord(Object->Handle + OKObject::DieFrame, uint32(Runtime.GetSimFrame()) + Ticks);
		}
		return true;
	}

	// 52.79 obj_set_velocity(obj, x, y, z) (FUN_0042EFD0)
	bool ObjSetVelocity(FOKScriptThread& Thread)
	{
		if (FOKMkObject* Object = ArgMkObject(Thread, 0))
		{
			for (int32 i = 0; i < 3; ++i)
			{
				Thread.GetRuntime().SetHeapWord(Object->Handle + OKObject::Velocity + i * 4, ArgWord(Thread, 1 + i));
			}
		}
		return true;
	}

	// 52.80 obj_get_velocity(obj, outVector) (FUN_0042F050)
	bool ObjGetVelocity(FOKScriptThread& Thread)
	{
		const uint32 Out = ArgWord(Thread, 1);
		uint32 Words[3] = {};
		if (FOKMkObject* Object = ArgMkObject(Thread, 0))
		{
			for (int32 i = 0; i < 3; ++i)
			{
				Thread.GetRuntime().LoadHeapWord(Object->Handle + OKObject::Velocity + i * 4, Words[i]);
			}
		}
		return Out == 0 || Thread.StoreBytes(Out, Words, sizeof(Words));
	}

	// 6.138 (named spawn_char_obj) obj, &vector, x, y, z (FUN_0043D860): adds the point (x, y, z) of the object's frame,
	// in the world, to the vector
	bool AddFramePoint(FOKScriptThread& Thread)
	{
		FOKScriptObject* Object = ArgAnyObject(Thread, 0);
		FVector Where = FVector::ZeroVector;
		if (!ObjectLocation(Object, Where))
		{
			return true;
		}
		const FVector Offset(ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		return AddToVectorArg(Thread, ArgWord(Thread, 1), Where + FrameOffset(Object, Offset));
	}

	// 6.139 (named spawn_particle) obj, &vector, x, y, z (FUN_00430890): adds (x, y, z) turned into the object's frame.
	// The spear adds its 1200 forward to its collision object's velocity (+0x6C) this way.
	bool AddFrameVector(FOKScriptThread& Thread)
	{
		FOKScriptObject* Object = ArgAnyObject(Thread, 0);
		if (!Object)
		{
			return true;
		}
		const FVector Offset(ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		return AddToVectorArg(Thread, ArgWord(Thread, 1), FrameOffset(Object, Offset));
	}

	// --- Particle effects (W2, OKParticles) ----------------------------------------------------------------------

	FOKParticleObject* ArgParticle(FOKScriptThread& Thread, int32 Index)
	{
		FOKMkObject* Object = ArgMkObject(Thread, Index);
		return Object && Object->IsA(EOKScriptClass::Particle) ? static_cast<FOKParticleObject*>(Object) : nullptr;
	}

	/** The ParticleSystem an asset value names (fx_blood/Blood_Bang05_InGame_FX), or empty. */
	FString ArgSystemName(FOKScriptThread& Thread, int32 Index, bool bThroughMode)
	{
		uint32 Value = 0;
		const bool bRead = bThroughMode ? Thread.ArgValue(Index, Value) : Thread.ArgRaw(Index, Value);
		const FOKMkoAsset* Asset = bRead ? Thread.GetRuntime().FindAsset(Value) : nullptr;
		return Asset ? Asset->Object : FString();
	}

	/** A pointer argument, read through the operand's mode: a setarg copy (<L4>) or a plain local holding it (vec_edge(L68)). */
	uint32 ArgAddress(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Address = 0;
		Thread.ArgValue(Index, Address);
		return Address;
	}

	/** Three floats a pointer argument points at; false for a null pointer. */
	bool ArgVector(FOKScriptThread& Thread, int32 Index, FVector& Out)
	{
		const uint32 Address = ArgAddress(Thread, Index);
		float Parts[3] = {};
		if (Address == 0 || !Thread.LoadBytes(Address, Parts, sizeof(Parts)))
		{
			return false;
		}
		Out = FVector(Parts[0], Parts[1], Parts[2]);
		return true;
	}

	// 82.3 fx_create(&ParticleSystem, deleteWhenDone) -> an MKParticle playing it (FUN_0043F310), or 0
	bool FxCreate(FOKScriptThread& Thread)
	{
		const FString SystemName = ArgSystemName(Thread, 0, true);
		Thread.SetResult(SystemName.IsEmpty() ? 0 : OKObjects::CreateParticle(Thread.GetRuntime(), SystemName, ArgWord(Thread, 1) != 0));
		return true;
	}

	// 6.123 play_fx_at_pos(&ParticleSystem, &position, &rotation, &scale) -> an MKParticle (FUN_0043F620): made with
	// deleteWhenDone set, then each of the three set if given. The rotation is (pitch, yaw) in radians.
	bool PlayFxAtPos(FOKScriptThread& Thread)
	{
		const FString SystemName = ArgSystemName(Thread, 0, false);
		const uint32 Handle = SystemName.IsEmpty() ? 0 : OKObjects::CreateParticle(Thread.GetRuntime(), SystemName, true);
		FOKParticleObject* Object = Handle ? static_cast<FOKParticleObject*>(OKObjects::Find(Thread.GetRuntime(), Handle)) : nullptr;
		if (Object)
		{
			FVector Value;
			if (ArgVector(Thread, 1, Value))
			{
				Object->Location = Value;
			}
			if (ArgVector(Thread, 2, Value))
			{
				Object->Rotation = FRotator(FMath::RadiansToDegrees(Value.X), FMath::RadiansToDegrees(Value.Y), 0.0);
			}
			if (ArgVector(Thread, 3, Value))
			{
				Object->DrawScale = float(Value.X);
			}
			OKObjects::PlaceParticle(*Object);
		}
		Thread.SetResult(Handle);
		return true;
	}

	// 137.191 play_char_fx(obj, &ParticleSystem, deleteWhenDone, flags) -> an MKParticle for the fighter (FUN_008AE580),
	// scaled like it. [inf] flags 2 and 4 hand it to an attachment record (FUN_0044FDA0) that keeps it on the fighter.
	bool PlayCharFx(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const FString SystemName = ArgSystemName(Thread, 1, false);
		const uint32 Handle = Fighter && !SystemName.IsEmpty() ? OKObjects::CreateParticle(Thread.GetRuntime(), SystemName, ArgWord(Thread, 2) != 0) : 0;
		FOKParticleObject* Object = Handle ? static_cast<FOKParticleObject*>(OKObjects::Find(Thread.GetRuntime(), Handle)) : nullptr;
		if (Object && Fighter->Body)
		{
			Object->Location = Fighter->Body->GetWorldLocation();
			Object->AttachedTo = (ArgWord(Thread, 3) & 6) != 0 ? Fighter : nullptr;
			OKObjects::PlaceParticle(*Object);
		}
		Thread.SetResult(Handle);
		return true;
	}

	// 52.54 fx_attach(obj, owner, bone, flags) (FUN_0043E2D0): onto the owner's bone (its id turned into a name by the
	// owner's virtual +0x54). An MK object owner only places it.
	bool FxAttach(FOKScriptThread& Thread)
	{
		FOKParticleObject* Object = ArgParticle(Thread, 0);
		FOKScriptObject* Owner = ArgAnyObject(Thread, 1);
		if (!Object || !Owner)
		{
			return true;
		}
		if (FOKFighter* Fighter = Owner->AsFighter())
		{
			const TCHAR* Bone = OKCollision::BoneName(int32(ArgWord(Thread, 2)));
			Object->AttachedTo = Fighter;
			Object->Bone = Bone ? Bone : TEXT("");
			Object->Offset = FVector::ZeroVector;
			UE_CLOG(FOKParticles::IsTracing(), LogOKVM, Log, TEXT("frame %lld: fx_attach to %s's %s (bone %u, flags %u)"), Thread.GetRuntime().GetSimFrame(), *Fighter->Name, *Object->Bone, ArgWord(Thread, 2), ArgWord(Thread, 3));
		}
		else
		{
			ObjectLocation(Owner, Object->Location);
		}
		OKObjects::PlaceParticle(*Object);
		return true;
	}

	// 52.61 fx_start(obj) (FUN_00433E40): SetBase(None): it leaves its bone and stays where it is
	bool FxStart(FOKScriptThread& Thread)
	{
		if (FOKParticleObject* Object = ArgParticle(Thread, 0))
		{
			OKObjects::PlaceParticle(*Object);
			if (Object->AttachedTo && Object->AttachedTo->Body)
			{
				const double Yaw = Object->AttachedTo->Body->GetFacing().Rotation().Yaw;
				Object->Rotation = (FRotator(0.0, Yaw, 0.0).Quaternion() * Object->Rotation.Quaternion()).Rotator();
			}
			Object->AttachedTo = nullptr;
		}
		return true;
	}

	// 52.95 obj_set_pos_vec(obj, x, y, z) (MKObj::vf58): the place, against the bone for an effect on one
	bool ObjSetPosVec(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		if (!Object)
		{
			return true;
		}
		const FVector Value(ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3));
		FOKParticleObject* Particle = Object->IsA(EOKScriptClass::Particle) ? static_cast<FOKParticleObject*>(Object) : nullptr;
		UE_CLOG(Particle && FOKParticles::IsTracing(), LogOKVM, Log, TEXT("frame %lld: obj_set_pos_vec %s (%s)"), Thread.GetRuntime().GetSimFrame(), *Value.ToCompactString(), Particle && Particle->AttachedTo ? TEXT("on a bone") : TEXT("in the world"));
		if (Particle && Particle->AttachedTo)
		{
			Particle->Offset = Value;
		}
		else
		{
			Object->Location = Value;
		}
		if (Particle)
		{
			OKObjects::PlaceParticle(*Particle);
		}
		return true;
	}

	// 52.41 fx_set_scale(obj, scale) (FUN_0042F160): the actor's DrawScale3D
	bool FxSetScale(FOKScriptThread& Thread)
	{
		if (FOKParticleObject* Object = ArgParticle(Thread, 0))
		{
			Object->DrawScale = ArgFloat(Thread, 1);
			OKObjects::PlaceParticle(*Object);
		}
		return true;
	}

	// 82.19 fx_scale(fx, scale, owner) (FUN_00430430): the component's scale, times the owner's (virtual +8, 1 here)
	bool FxScale(FOKScriptThread& Thread)
	{
		if (FOKParticleObject* Object = ArgParticle(Thread, 0))
		{
			Object->ComponentScale = ArgFloat(Thread, 1);
			OKObjects::PlaceParticle(*Object);
		}
		return true;
	}

	// 52.96 fx_set_to_facing(obj, &rotation) (FUN_0043A450): (pitch, yaw) in radians
	bool FxSetToFacing(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		FVector Value;
		if (Object && ArgVector(Thread, 1, Value))
		{
			Object->Rotation = FRotator(FMath::RadiansToDegrees(Value.X), FMath::RadiansToDegrees(Value.Y), Object->Rotation.Roll);
			if (Object->IsA(EOKScriptClass::Particle))
			{
				OKObjects::PlaceParticle(static_cast<FOKParticleObject&>(*Object));
			}
		}
		return true;
	}

	// 52.89 fx_set_facing(obj, &rotation) (FUN_0043A380): reads it back, (pitch, yaw, roll) in radians
	bool FxGetFacing(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		const uint32 Out = ArgWord(Thread, 1);
		if (!Object || Out == 0)
		{
			return true;
		}
		const float Parts[3] = { FMath::DegreesToRadians(float(Object->Rotation.Pitch)), FMath::DegreesToRadians(float(Object->Rotation.Yaw)), FMath::DegreesToRadians(float(Object->Rotation.Roll)) };
		return Thread.StoreBytes(Out, Parts, sizeof(Parts));
	}

	/** An FRotator as UE3 stores it: (pitch, yaw, roll) in units of 65536 a turn. */
	FRotator FromRotatorUnits(const int32 Units[3])
	{
		constexpr double ToDegrees = 360.0 / 65536.0;
		return FRotator(Units[0] * ToDegrees, Units[1] * ToDegrees, Units[2] * ToDegrees);
	}

	// 52.91 obj_get_rot(obj, &rotator) (FUN_00433490): the actor's rotation (+0xE8), in UE3 rotator units
	bool ObjGetRot(FOKScriptThread& Thread)
	{
		FOKScriptObject* Object = ArgAnyObject(Thread, 0);
		const uint32 Out = ArgWord(Thread, 1);
		FOKFighter* Fighter = Object ? Object->AsFighter() : nullptr;
		FOKMkObject* MkObject = Object ? Object->AsMkObject() : nullptr;
		if (Out == 0 || (!MkObject && !(Fighter && Fighter->Body)))
		{
			return true;
		}
		const FRotator Rotation = MkObject ? MkObject->Rotation : Fighter->Body->GetFacing().Rotation();
		constexpr double ToUnits = 65536.0 / 360.0;
		const int32 Units[3] = { FMath::RoundToInt32(Rotation.Pitch * ToUnits), FMath::RoundToInt32(Rotation.Yaw * ToUnits), FMath::RoundToInt32(Rotation.Roll * ToUnits) };
		return Thread.StoreBytes(Out, Units, sizeof(Units));
	}

	// 52.98 obj_copy_rotation(obj, &rotator) (FUN_004338B0): sets the rotation from UE3 rotator units. The iceball's hit
	// effect takes its rotation from the iceball's collision object this way.
	bool ObjCopyRotation(FOKScriptThread& Thread)
	{
		FOKMkObject* Object = ArgMkObject(Thread, 0);
		const uint32 From = ArgWord(Thread, 1);
		int32 Units[3] = {};
		if (!Object || From == 0 || !Thread.LoadBytes(From, Units, sizeof(Units)))
		{
			return true;
		}
		Object->Rotation = FromRotatorUnits(Units);
		if (Object->IsA(EOKScriptClass::Particle))
		{
			OKObjects::PlaceParticle(static_cast<FOKParticleObject&>(*Object));
		}
		return true;
	}

	// 82.21 fx_stop_emitter(fx) (FUN_004366D0): every emitter stops spawning
	bool FxStopEmitter(FOKScriptThread& Thread)
	{
		FOKParticleObject* Object = ArgParticle(Thread, 0);
		if (Object && Object->Actor.IsValid())
		{
			Object->Actor->StopEmitters();
		}
		return true;
	}

	// 82.20 fx_activate(fx) (FUN_00430540): sets +0x100, as fx_create's second argument does
	bool FxActivate(FOKScriptThread& Thread)
	{
		if (FOKParticleObject* Object = ArgParticle(Thread, 0))
		{
			Object->bDeleteWhenDone = true;
		}
		return true;
	}

	// 82.17 fx_blood_tint(fx, &name, x, y, z) (FUN_004302E0): the component's SetVectorParameter (virtual +0x1F0). The
	// name is one 30.2 made: FGEffectsEngine's FX_Blood_Color.
	bool FxBloodTint(FOKScriptThread& Thread)
	{
		FOKParticleObject* Object = ArgParticle(Thread, 0);
		uint32 Name = 0;
		const uint32 NameAddress = ArgWord(Thread, 1);
		if (!Object || !Object->Actor.IsValid() || NameAddress == 0 || !Thread.LoadBytes(NameAddress, &Name, 4))
		{
			return true;
		}
		FString Text;
		for (uint8 Char = 0; Name != 0 && Text.Len() < 64 && Thread.LoadBytes(Name + Text.Len(), &Char, 1) && Char != 0; )
		{
			Text.AppendChar(TCHAR(Char));
		}
		UE_CLOG(FOKParticles::IsTracing(), LogOKVM, Log, TEXT("fx_blood_tint %s = %g %g %g"), *Text, ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		if (!Text.IsEmpty())
		{
			Object->Actor->SetVectorParameter(FName(*Text), FVector(ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4)));
		}
		return true;
	}

	// 30.2 (name, text): makes an FName (8 bytes) from the text; __global__ makes FX_Blood_Color and FX_Decal_Color with
	// it. OpenKombat keeps the text: the first word is it, interned.
	bool MakeName(FOKScriptThread& Thread)
	{
		uint32 Out = 0;
		Thread.ArgValue(0, Out);
		const FString Text = ArgCString(Thread, 1);
		const uint32 Words[2] = { Text.IsEmpty() ? 0u : Thread.GetRuntime().InternString(Text), 0u };
		return Out == 0 || Thread.StoreBytes(Out, Words, sizeof(Words));
	}

	// 137.64 face_like(obj, &rotation): the fighter's rotation as UE3 rotator units (65536 a turn), (pitch, yaw, roll).
	// EffectsEngine.fx_set_to_my_facing turns the yaw into radians with pi / 32767.
	bool FaceLike(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Out = ArgWord(Thread, 1);
		if (!Fighter || !Fighter->Body || Out == 0 || Thread.GetRuntime().ResolveHandle(Out))
		{
			return true;
		}
		const FVector Facing = Fighter->Body->GetFacing();
		const int32 Words[3] = { 0, FMath::RoundToInt32(FMath::Atan2(Facing.Y, Facing.X) * 32768.0 / UE_DOUBLE_PI), 0 };
		return Thread.StoreBytes(Out, Words, sizeof(Words));
	}

	// 81.23 xfer_to_script(obj, script, function): the fighter's own thread continues there (XferToReaction)
	bool XferToScript(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter)
		{
			OKTransferMainThread(Thread.GetRuntime(), *Fighter, ArgWord(Thread, 1), ArgWord(Thread, 2));
		}
		return true;
	}

	// 6.99 (named pushback_corner_value) -> how far apart the two players' fighters are, across the floor
	// (FUN_00492790 -> FUN_00491F50); -1 without both
	bool FightersDistance(FOKScriptThread& Thread)
	{
		FOKFighter* Me = Thread.MyFighter();
		FOKFighter* Him = Me ? Me->Opponent : nullptr;
		float Distance = -1.0f;
		if (Me && Me->Body && Him && Him->Body)
		{
			Distance = float(FVector::Dist2D(Me->Body->GetWorldLocation(), Him->Body->GetWorldLocation()));
		}
		Thread.SetResult(FloatToBits(Distance));
		return true;
	}

	// 6.250 contact_valid(obj) (FUN_00451090): a collision object's CollisionContext, a CollisionContext itself, else 0
	bool ContactValid(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgWord(Thread, 0);
		FOKScriptObject* Object = Handle != 0 ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		uint32 Result = 0;
		if (Object && Object->IsA(EOKScriptClass::CollisionObj))
		{
			const uint32 Context = static_cast<FOKCollisionObject*>(Object)->Context;
			Result = Thread.GetRuntime().ResolveHandle(Context) ? Context : 0;
		}
		else if (Object && Object->IsA(EOKScriptClass::CollisionContext))
		{
			Result = Handle;
		}
		Thread.SetResult(Result);
		return true;
	}

	// 81.169 (named branch_on_result) obj, context (FUN_004476B0): deletes the context, once _hit_branch has read it
	bool DeleteContext(FOKScriptThread& Thread)
	{
		Thread.GetRuntime().DestroyObject(ArgWord(Thread, 1));
		return true;
	}

	// 1.2 jump_vm(script, function) (FUN_0045C560): the running thread drops its calls and continues there.
	// [inf] MKKE's state 4 may continue in the same frame; we continue from the next.
	bool JumpVm(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 ScriptValue = ArgWord(Thread, 0);
		const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		FOKScriptInstance* Target = nullptr;
		int32 Function = INDEX_NONE;
		if (!From || !Runtime.ResolveFunction(*From, ArgWord(Thread, 1), Target, Function))
		{
			return Thread.NativeFail(FString::Printf(TEXT("jump_vm: no function %u in %08X"), ArgWord(Thread, 1), ScriptValue));
		}
		return Thread.TransferTo(*Target, Function);
	}

	// 137.270 two_player_rx_force(obj, victimCleanup, attackerCleanup) (FUN_00897100): the reactions that clean up a
	// two-player move if it's cut short, kept in the live attack: flags +0x7A18 / +0x7A1C, scripts +0x7A20 / +0x7A30
	// (handles), functions +0x7A2C / +0x7A3C. [inf] Nothing runs them yet.
	bool TwoPlayerRxSlots(FOKScriptThread& Thread, uint32 Attack)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const FOKMkoFile* Script = Thread.GetCurrentScript();
		FOKScriptInstance* From = Script ? Runtime.GetInstance(*Script) : nullptr;
		if (!Fighter || !From)
		{
			return true;
		}
		const uint32 Slots[2][3] = { { Attack + 0x128, Attack + 0x130, Attack + 0x13C }, { Attack + 0x12C, Attack + 0x140, Attack + 0x14C } };
		for (int32 i = 0; i < 2; ++i)
		{
			FOKScriptInstance* Target = nullptr;
			int32 Function = INDEX_NONE;
			if (!Runtime.ResolveFunction(*From, ArgWord(Thread, 1 + i), Target, Function))
			{
				break;
			}
			const uint32 Value = (FOKScriptRuntime::InstanceTag << 24) | uint32(Target->Id);
			WriteWord(Runtime, Fighter->Handle + Slots[i][0], 1);
			WriteWord(Runtime, Fighter->Handle + Slots[i][1] + 4, Value);
			WriteWord(Runtime, Fighter->Handle + Slots[i][1] + 8, Value);
			WriteWord(Runtime, Fighter->Handle + Slots[i][2], uint32(Function + 1));
		}
		return true;
	}

	bool TwoPlayerRxForce(FOKScriptThread& Thread) { return TwoPlayerRxSlots(Thread, FOKFighter::LiveAttack); }

	// 137.271 two_player_rx_setup(obj, victimCleanup, attackerCleanup) (FUN_00897040): the same in the pending attack (+0x7C34 ...),
	// so they go live with the attack
	bool TwoPlayerRxSetup(FOKScriptThread& Thread) { return TwoPlayerRxSlots(Thread, FOKFighter::PendingAttack); }

	// 137.272 two_player_rx_clear(obj) (FUN_008971D0, "ClearTwoPlayerCleanupContext"): the live attack's two cleanups go
	bool TwoPlayerRxClear(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			for (const uint32 Offset : { 0x7A18u, 0x7A1Cu, 0x7A2Cu, 0x7A3Cu })
			{
				WriteWord(Thread.GetRuntime(), Fighter->Handle + Offset, 0);
			}
		}
		return true;
	}

	// 137.283 throw_flag(obj) / 137.282 throw_towards_flag(obj) (FUN_00897D80 / FUN_00897CD0): count a throw, back or towards
	// (+0x2FE8 all, +0x2FF0 / +0x2FEC), for the stats and an achievement OpenKombat doesn't keep
	template <uint32 Counter>
	bool CountThrow(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			WriteWord(Runtime, Fighter->Handle + 0x2FE8, ReadWord(Runtime, Fighter->Handle + 0x2FE8) + 1);
			WriteWord(Runtime, Fighter->Handle + Counter, ReadWord(Runtime, Fighter->Handle + Counter) + 1);
		}
		return true;
	}

	// 137.109 first_hit_detected(obj) (FUN_008D8520): fighter +0x2E8 |= 0x10000000. [unk] MKKE then calls FUN_00892D70
	// unless a debug flag is set.
	bool FirstHitDetected(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			WriteWord(Thread.GetRuntime(), Fighter->Handle + FOKFighter::Flags, ReadWord(Thread.GetRuntime(), Fighter->Handle + FOKFighter::Flags) | 0x10000000);
		}
		return true;
	}

	// 137.102 / 81.186 get_char_script(obj, n) (fighter virtual +0x1B8, FUN_0044EF20): the scripts a fighter can name. 3 is
	// its own character script; the others (game logic, the partner's ...) aren't needed yet.
	bool GetCharScript(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Which = ArgWord(Thread, 1);
		if (Fighter && Which == 1)
		{
			// FUN_00810200: the generic throw's script (game info's, as 6.105 gives it); the escapes find p_throw_reversed_attacker in it
			return ScriptInstanceValue(Thread, TEXT("GenericThrow"));
		}
		Thread.SetResult(Fighter && Which == 3 ? ReadWord(Thread.GetRuntime(), Fighter->Handle + FOKFighter::CharacterScriptHandle + 4) : 0);
		if (Fighter && Which != 3)
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: get_char_script %u isn't known"), *Fighter->Name, Which);
		}
		return true;
	}

	// 81.21 start_throw(attacker, victim, anim, startFrame, blendFrames, flags, matchBlendFrames, ?, ?, ?) (FUN_0044F6C0): both
	// fighters play one sequence, the victim locked to the attacker (ANIM-RE-NOTES §4.5). The victim's thread waits in
	// p_player_idle_no_input_in_two_player until the throw lets go (OKUpdateThrow).
	bool StartThrow(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		auto Value = [&Thread](int32 Index) { uint32 Raw = 0; Thread.ArgValue(Index, Raw); return Raw; };
		FOKFighter* Attacker = Thread.ArgFighter(0);
		FOKFighter* Victim = RawFighter(Thread, 1);
		const FOKMkoAsset* Asset = Runtime.FindAsset(Value(2));
		if (!Attacker || !Victim || !Asset || !Attacker->Body || !Victim->Body)
		{
			return true;
		}
		const float StartFrame = BitsToFloat(Value(3));
		const float BlendFrames = BitsToFloat(Value(4));
		const uint32 Flags = Value(5);

		WriteWord(Runtime, Attacker->Handle + FOKFighter::Holding, 1);
		if (Victim->ThrowLeader != Attacker)
		{
			OKTransferMainThreadTo(Runtime, *Victim, TEXT("FightEngineScript"), TEXT("p_player_idle_no_input_in_two_player"));
		}
		for (FOKFighter* Fighter : { Attacker, Victim })
		{
			WriteWord(Runtime, Fighter->Handle + FOKFighter::SavedCharFlags, ReadWord(Runtime, Fighter->Handle + FOKFighter::CharFlags));
			WriteWord(Runtime, Fighter->Handle + FOKFighter::SavedCharFlags + 4, 1);
		}
		if (Flags & 1)
		{
			Attacker->Body->SetupForFlip();
		}
		// The victim takes the attacker's mirroring: the sequence places both in the attacker's space.
		if (Victim->Body->IsMeshMirrored() != Attacker->Body->IsMeshMirrored())
		{
			Victim->Body->SetupForFlip();
		}

		Attacker->Body->PlayAnimation(Asset->Object, StartFrame, 1.0f, BlendFrames, false);
		Victim->Body->PlayAnimation(Asset->Object, Attacker->Body->GetAnimFrame(), Attacker->Body->GetAnimRate(), BlendFrames, false);
		Victim->ThrowLeader = Attacker;
		Victim->ThrowSequence = Attacker->Body->GetAnimName();
		Victim->ThrowFlags = Flags;
		if (ReadWord(Runtime, Attacker->Handle + FOKFighter::LiveAttack + 0x128) != 0)
		{
			Victim->ThrowCleanupScript = ReadWord(Runtime, Attacker->Handle + FOKFighter::LiveAttack + 0x130 + 4);
			Victim->ThrowCleanupFunction = ReadWord(Runtime, Attacker->Handle + FOKFighter::LiveAttack + 0x13C);
		}
		Victim->Body->FollowThrow(*Attacker->Body);

		WriteWord(Runtime, Victim->Handle + 0x3B4, 1);
		for (FOKFighter* Fighter : { Attacker, Victim })
		{
			WriteWord(Runtime, Fighter->Handle + FOKFighter::CharFlags, (ReadWord(Runtime, Fighter->Handle + FOKFighter::CharFlags) & ~0x94u) | 0x20);
		}
		WriteWord(Runtime, Attacker->Handle + FOKFighter::Flags, ReadWord(Runtime, Attacker->Handle + FOKFighter::Flags) & ~0x80000u);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s throws %s: %s from frame %g"), Runtime.GetSimFrame(), *Attacker->Name, *Victim->Name, *Victim->ThrowSequence, StartFrame);
		return true;
	}

	// 137.278 set_throw_id(obj, function) (FUN_008AD390): while the fighter holds a throw victim (+0x284), it lets go
	// (ReleaseCharacters, virtual +0xE4) and the victim takes that function of the running script as its reaction
	// (FUN_008AD280): r_scorpion_throw_out partway through Scorpion's throw, r_char_throw_reversed_victim when a throw is
	// broken. The victim's animation runs on by itself.
	bool SetThrowId(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKFighter* Attacker = Thread.ArgFighter(0);
		FOKFighter* Victim = Attacker ? Attacker->Opponent : nullptr;
		if (!Victim || ReadWord(Runtime, Attacker->Handle + FOKFighter::Holding) == 0)
		{
			return true;
		}
		OKReleaseThrow(Runtime, *Victim, false);
		WriteWord(Runtime, Attacker->Handle + FOKFighter::Holding, 0);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s sends %s to reaction %u"), Runtime.GetSimFrame(), *Attacker->Name, *Victim->Name, ArgWord(Thread, 1));
		OKTransferMainThread(Runtime, *Victim, Thread.CurrentInstanceValue(), ArgWord(Thread, 1));
		return true;
	}

	// 137.238 set_props(obj, n) (FUN_00896C10): pending attack +0x5C (fighter +0x7B68)
	bool SetProps(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			WriteWord(Thread.GetRuntime(), Fighter->Handle + 0x7B68, ArgWord(Thread, 1));
		}
		return true;
	}

	// 137.243 set_duck_reaction(obj, script, function) (FUN_00896DC0): the reaction a crouching victim takes, pending attack
	// +0x1E8 (fighter +0x7CF4). [unk] where MKKE keeps the script handle. [inf] Hits don't pick it yet: the standing one plays.
	bool SetDuckReaction(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			WriteWord(Thread.GetRuntime(), Fighter->Handle + 0x7CF4, ArgWord(Thread, 2));
		}
		return true;
	}

	// 137.242 set_props_params(obj, a, b) (FUN_00896D30): pending attack +0x168 = 1, +0x16C = a, +0x170 = b
	bool SetPropsParams(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			WriteWord(Runtime, Fighter->Handle + 0x7C74, 1);
			WriteWord(Runtime, Fighter->Handle + 0x7C78, ArgWord(Thread, 1));
			WriteWord(Runtime, Fighter->Handle + 0x7C7C, ArgWord(Thread, 2));
		}
		return true;
	}

	// 66.7 set_command_window(obj, ticks) (FUN_00449F00): fighter +0x38 = this frame + ticks
	bool SetCommandWindow(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			WriteWord(Runtime, Fighter->Handle + 0x38, uint32(Runtime.GetSimFrame()) + ArgWord(Thread, 1));
		}
		return true;
	}

	/**
	 * The move callback (FUN_0044E5C0): set_move_callback's function (fighter +0x2D8 a script handle {?, object +0x2DC,
	 * serial +0x2E0}, +0x2E4 the function) runs on the fighter at once, and the handle is let go, so it runs once. The
	 * fighter's animation starts (FUN_008A4080, vf108) call it after the new animation is set; 81.62 calls it directly.
	 * r_hit_airborn sets one, then blends to its landing animation, so it runs there.
	 */
	void RunMoveCallback(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
	{
		const uint32 ScriptValue = ReadWord(Runtime, Fighter.Handle + 0x2DC);
		const uint32 FunctionValue = ReadWord(Runtime, Fighter.Handle + 0x2E4);
		if (ScriptValue == 0)
		{
			return;
		}
		WriteWord(Runtime, Fighter.Handle + 0x2DC, 0);
		WriteWord(Runtime, Fighter.Handle + 0x2E0, 0);
		const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		FOKScriptInstance* Target = nullptr;
		int32 Function = INDEX_NONE;
		if (!From || !Runtime.ResolveFunction(*From, FunctionValue, Target, Function))
		{
			return;
		}
		FOKScriptThread Call(Runtime);
		Call.BindFighter(Fighter);
		uint32 Result = 0;
		FString Error;
		if (!Call.Run(*Target->File, Function, {}, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: %s"), *Target->File->GetFunctions()[Function].Name, *Error);
		}
	}

	// 81.63 set_move_callback(obj, script, function) (FUN_00446BB0): +0x2D8 the script's handle, +0x2E4 the function
	bool SetMoveCallback(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			uint32 Script = 0;
			Thread.ArgValue(1, Script);
			WriteWord(Runtime, Fighter->Handle + 0x2DC, Script);
			WriteWord(Runtime, Fighter->Handle + 0x2E0, Script);		// our serial is the value
			WriteWord(Runtime, Fighter->Handle + 0x2E4, ArgWord(Thread, 2));
		}
		return true;
	}

	// 81.62 (obj): runs the move callback now
	bool RunMoveCallbackNative(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			RunMoveCallback(Thread.GetRuntime(), *Fighter);
		}
		return true;
	}

	// 81.50 glitch_to_ani_frame_dir(obj, anim, frame, rate, flags, ?) (virtual +0xFC): blend_to_ani without the blend
	bool GlitchToAni(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const FOKMkoAsset* Asset = Fighter && Fighter->Body ? Thread.GetRuntime().FindAsset(ArgWord(Thread, 1)) : nullptr;
		if (!Asset)
		{
			return true;
		}
		const uint32 Flags = ArgWord(Thread, 4);
		Fighter->Body->PlayAnimation(Asset->Object, ArgFloat(Thread, 2), ArgFloat(Thread, 3), 0.0f, (Flags & 2) != 0);
		Fighter->Body->SetAnimNodeMirrored((Flags & 1) != 0);
		Fighter->CurrentAnim = ArgWord(Thread, 1);
		RunMoveCallback(Thread.GetRuntime(), *Fighter);
		return true;
	}

	/** FUN_00894840: the ArmorPdata attached to the fighter's armour process (id 0x6B), or 0. [inf] Online, player flag 0x100 can cancel it. */
	uint32 FindArmor(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
	{
		uint32 Armor = 0;
		Runtime.ForEachProcess([&Runtime, &Fighter, &Armor](const FOKScriptThread& Process)
		{
			if (Armor == 0 && Process.IsAlive() && Process.GetProcId() == 0x6B && Process.GetOwner() == Fighter.Handle)
			{
				const uint32 Attached = Process.GetFirstAttachedObject();
				const FOKScriptObject* Object = Runtime.ResolveHandle(Attached);
				Armor = (Object && Object->IsA(EOKScriptClass::ArmorPdata)) ? Attached : 0;
			}
		});
		return Armor;
	}

	/**
	 * Whether the victim blocks the attacker's live attack (FUN_00899890). Not when the attacker's move is unblockable
	 * (+0x214, a move's flag 2 or a special's record +0x0C) or the chip would leave less than 0.001 of life; otherwise by
	 * the attack's hit class (+0x5C): 0 and 1 (high, mid) and 0x0B / 0x0C are blocked standing or low, 2 and 3 (low) only
	 * low, 5 (overhead) only standing, and the rest (4, 0x0D, the reset value 14) not at all. Blocking is BlockHeld
	 * (FUN_008952A0); low is Down held too (is_holding_block, FUN_00895310).
	 * [inf] Left out: the contact's unblockable flag (its flags byte & 0x10), the AI drone's own say (FUN_0088D0B0;
	 * ours presses Block), and a fighter's chance of a block failing (FUN_00448920, +0x2FBC..+0x2FC8, the tower's).
	 */
	bool BlocksHit(FOKScriptRuntime& Runtime, const FOKFighter& Attacker, const FOKFighter& Victim)
	{
		const uint32 Attack = Attacker.Handle + FOKFighter::LiveAttack;
		if (ReadWord(Runtime, Attacker.Handle + 0x214) != 0 || !Victim.Player
			|| Victim.Health - BitsToFloat(ReadWord(Runtime, Attack + OKAttack::ChipDamage)) < 0.001f || !BlockHeld(Runtime, Victim))
		{
			return false;
		}
		const bool bLow = OKPlayerInput::Held(Runtime, *Victim.Player, 0x80000004);
		switch (ReadWord(Runtime, Attack + OKAttack::HitClass))
		{
		case 0: case 1: case 0x0B: case 0x0C: return true;
		case 2: case 3: return bLow;
		case 5: return !bLow;
		default: return false;
		}
	}

	/**
	 * A blocked hit (ResolveHit step 8, FUN_0089E980 and ApplyReaction with blocked set): the victim takes the attack's chip
	 * damage (+0xD0) and a BlockingDucking reaction by the attack's strength bits (+0xE8 & 0x0FFE0000), its side counts the
	 * block (player +0xBD8), and the contact ends blocked (outcome 1). The combo doesn't count it and the attacker gains no
	 * meter. [inf] MKKE's AttackFlag can read those bits from the contact instead; not here.
	 */
	bool BlockContact(FOKScriptRuntime& Runtime, FOKFighter& Attacker, FOKFighter& Victim, uint32 Context)
	{
		if (!BlocksHit(Runtime, Attacker, Victim))
		{
			return false;
		}
		const uint32 Attack = Attacker.Handle + FOKFighter::LiveAttack;
		static const TCHAR* const Reactions[] = { TEXT("r_block_hit_weak"), TEXT("r_block_hit_medium"), TEXT("r_block_hit_strong"),
			TEXT("r_block_hit_nomove_weak"), TEXT("r_block_hit_nomove_medium"), TEXT("r_block_hit_nomove_strong"), TEXT("r_block_hit_stumble"),
			TEXT("r_block_hit_behind"), TEXT("r_block_hit_jab"), TEXT("r_block_hit_nomove_jab"), TEXT("r_block_hit_nitro") };
		const uint32 Strength = ReadWord(Runtime, Attack + OKAttack::AttackFlags) & 0x0FFE0000;
		const TCHAR* Reaction = Reactions[0];
		for (int32 Bit = 0; Bit < UE_ARRAY_COUNT(Reactions); ++Bit)
		{
			if (Strength & (0x20000u << Bit))
			{
				Reaction = Reactions[Bit];
				break;
			}
		}
		const float Chip = BitsToFloat(ReadWord(Runtime, Attack + OKAttack::ChipDamage));
		WriteWord(Runtime, Victim.Player->Handle + 0xBD8, ReadWord(Runtime, Victim.Player->Handle + 0xBD8) + 1);
		if ((Context >> 24) == FOKScriptRuntime::HeapTag)
		{
			WriteWord(Runtime, Context + FOKCollisionContext::Outcome, 1);
		}
		ApplyDamage(Runtime, Victim, Chip);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s blocks %s's attack (hit class %u), chip %g, %s"), Runtime.GetSimFrame(), *Victim.Name, *Attacker.Name,
			ReadWord(Runtime, Attack + OKAttack::HitClass), Chip, Reaction);
		FOKScriptInstance* Blocking = Runtime.FindInstanceByName(TEXT("BlockingDucking"));
		const int32 Function = Blocking ? Blocking->File->FindFunctionByName(Reaction) : INDEX_NONE;
		if (Function != INDEX_NONE)
		{
			XferToReaction(Runtime, Victim, *Blocking, Function);
		}
		return true;
	}

	/**
	 * A combo breaker (ResolveHit step 8: the gate FUN_008955C0, then FUN_008A7180). The victim's character info names its
	 * breaker reactions (+0x60 standing, +0x64 in the air; both must be set), its side has two bars (DAT_00D653BC,
	 * 0.6667), the attack is breakable (+0xE8 bit 0x200), the round is live (game flag 1), the victim is alive and
	 * mid-combo (player +0xC54), and its player holds Towards and Block (player virtual +0x44, FUN_00446030). Then the
	 * attacker's two-player links are cleared, the contact ends with outcome 3, the victim's combo count goes to 0, and
	 * the victim takes the reaction from its own character script with no damage; r_combo_breaker spends the meter.
	 * [inf] Left out: the tower's no-breaker rules (FUN_008F1AB0), the AI drone's roll to break (FUN_0088DDE0; ours
	 * presses the buttons), and the input buffer's breaker events (0x38 / 0x39).
	 */
	bool BreakCombo(FOKScriptRuntime& Runtime, FOKFighter& Attacker, FOKFighter& Victim, uint32 Context)
	{
		FOKPlayer* Player = Victim.Player;
		const uint32 Info = ReadWord(Runtime, Victim.Handle + FOKFighter::CharacterInfo);
		if (!Player || Info == 0 || ReadWord(Runtime, Info + 0x60) == 0 || ReadWord(Runtime, Info + 0x64) == 0
			|| BitsToFloat(ReadWord(Runtime, Player->Handle + FOKPlayer::Meter)) < 0.6666709780693054f
			|| (ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::AttackFlags) & 0x200) == 0
			|| (ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::Flags) & 1) == 0
			|| Victim.Health <= 0.0f || ReadWord(Runtime, Player->Handle + FOKPlayer::ComboHits) == 0)
		{
			return false;
		}
		uint32 Towards = 0, Away = 0;
		OKPlayerInput::FacingMasks(Victim, Towards, Away);
		if (!OKInput::IsHeld(Runtime, ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer), Towards)
			|| !OKPlayerInput::HeldCommand(Runtime, *Player, 0x8000001C))
		{
			return false;
		}
		const bool bAir = Airborne(Runtime, Victim);
		const uint32 FunctionValue = ReadWord(Runtime, Info + (bAir ? 0x64 : 0x60));
		const uint32 Slot = Attacker.Handle + FOKFighter::LiveAttack;
		for (const uint32 Offset : { 0x128u, 0x13Cu, 0x12Cu, 0x14Cu })
		{
			WriteWord(Runtime, Slot + Offset, 0);
		}
		if ((Context >> 24) == FOKScriptRuntime::HeapTag)
		{
			WriteWord(Runtime, Context + FOKCollisionContext::Outcome, 3);
		}
		WriteWord(Runtime, Player->Handle + 0xBD4, ReadWord(Runtime, Player->Handle + 0xBD4) + 1);
		WriteWord(Runtime, Player->Handle + FOKPlayer::ComboHits, 0);
		const uint32 ScriptValue = ReadWord(Runtime, Victim.Handle + FOKFighter::CharacterScriptHandle + 4);
		const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		FOKScriptInstance* Target = nullptr;
		int32 Function = INDEX_NONE;
		const bool bFound = From && Runtime.ResolveFunction(*From, FunctionValue, Target, Function);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s breaks %s's combo%s (%s)"), Runtime.GetSimFrame(), *Victim.Name, *Attacker.Name, bAir ? TEXT(" in the air") : TEXT(""),
			bFound ? *Target->File->GetFunctions()[Function].Name : TEXT("no function"));
		if (bFound)
		{
			XferToReaction(Runtime, Victim, *Target, Function);
		}
		return true;
	}

	// 137.70 is_armor_active(obj)
	bool IsArmorActive(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? FindArmor(Thread.GetRuntime(), *Fighter) : 0);
		return true;
	}

	/**
	 * Armour (ResolveHit step 7, FUN_0089D190): an armoured victim takes the hit's damage, or the chip if it blocks, times
	 * the armour's scale, without a reaction, and the attack counts as blocked. Each hit takes a point; at 0 the effects are
	 * the "shattered" ones. An attacker with set_attack_ignores_armor (+0x2EC bit 0x800000) breaks it instead: the points go
	 * to 0 and the hit lands. A hit that would kill lands too.
	 * [obs] The points don't gate it: a shattered armour still takes hits until its ticks run out or the move turns it off
	 * (character_armor_off), as most do on their active frames. [inf] The tower's armour rules (+0x2EC bit 0x100000) and
	 * MKKE's virtual +0x4C scaling are left out.
	 */
	bool AbsorbWithArmor(FOKScriptRuntime& Runtime, FOKFighter& Attacker, FOKFighter& Victim, float Damage)
	{
		const uint32 Armor = FindArmor(Runtime, Victim);
		if (Armor == 0)
		{
			return false;
		}
		const bool bBreaks = (ReadWord(Runtime, Attacker.Handle + 0x2EC) & 0x800000) != 0;
		float Points = BitsToFloat(ReadWord(Runtime, Armor + FOKArmorPdata::Points));
		const float Taken = Damage * BitsToFloat(ReadWord(Runtime, Armor + FOKArmorPdata::DamageScale));
		if (!bBreaks && Victim.Health - Taken <= 0.0f)
		{
			return false;
		}
		Points = bBreaks ? 0.0f : Points - 1.0f;
		WriteWord(Runtime, Armor + FOKArmorPdata::Points, FloatToBits(Points));
		if (!bBreaks)
		{
			ApplyDamage(Runtime, Victim, Taken);
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s's armour %s %s's hit (%g damage, %g points left)"), Runtime.GetSimFrame(), *Victim.Name,
			bBreaks ? TEXT("breaks on") : TEXT("takes"), *Attacker.Name, bBreaks ? 0.0f : Taken, Points);
		FOKScriptInstance* Engine = Runtime.FindInstanceByName(TEXT("FGFightEngineScript"));
		const int32 Effects = Engine ? Engine->File->FindFunctionByName(Points <= 0.0f ? TEXT("character_my_armor_shattered_effects") : TEXT("character_my_armor_hit_effects")) : INDEX_NONE;
		if (Effects != INDEX_NONE)
		{
			FOKScriptThread Call(Runtime);
			Call.BindFighter(Victim);
			uint32 Result = 0;
			FString Error;
			if (!Call.Run(*Engine->File, Effects, {}, Result, Error))
			{
				UE_LOG(LogOKVM, Warning, TEXT("%s: %s"), *Engine->File->GetFunctions()[Effects].Name, *Error);
			}
		}
		return !bBreaks;
	}

	// 118.14 add_meter(info, delta) (AddMeter)
	bool AddMeterNative(FOKScriptThread& Thread)
	{
		if (FOKPlayer* Player = ArgPlayer(Thread, 0))
		{
			OKAddMeter(Thread.GetRuntime(), *Player, ArgFloat(Thread, 1));
		}
		return true;
	}

	// 81.71 (named cinema_participant) obj, script, function, procData (FUN_0044DCE0): runs the function on the fighter at
	// once. The hit scan runs the attack's callback (live attack +0x74 / +0x7C) this way.
	bool RunOnFighter(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 ScriptValue = ArgWord(Thread, 1);
		const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		FOKScriptInstance* Target = nullptr;
		int32 Function = INDEX_NONE;
		if (!Fighter || !From || !Runtime.ResolveFunction(*From, ArgWord(Thread, 2), Target, Function))
		{
			return true;
		}
		FOKScriptThread Call(Runtime);
		Call.BindFighter(*Fighter);
		uint32 Result = 0;
		FString Error;
		if (!Call.Run(*Target->File, Function, {}, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: %s"), *Target->File->GetFunctions()[Function].Name, *Error);
		}
		return true;
	}

	// 137.261 set_hit_callback(obj, script, function) (FUN_00896900): the pending attack's handle at +0x7C14, function +0x7C20
	bool SetHitCallback(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			Runtime.SetHeapWord(Fighter->Handle + 0x7C18, ArgWord(Thread, 1));
			Runtime.SetHeapWord(Fighter->Handle + 0x7C1C, ArgWord(Thread, 1));		// our serial is the value
			Runtime.SetHeapWord(Fighter->Handle + 0x7C20, ArgWord(Thread, 2));
		}
		return true;
	}

	// 22.9 (process, obj) (FUN_00458490): attaches the object to the process, as its data
	bool AttachObjectToProcess(FOKScriptThread& Thread)
	{
		uint32 ProcessHandle = 0;
		FOKScriptObject* Target = Thread.ArgValue(0, ProcessHandle) ? Thread.GetRuntime().ResolveHandle(ProcessHandle) : nullptr;
		FOKScriptThread* Process = Target ? Target->AsThread() : nullptr;
		if (Process && ArgWord(Thread, 1) != 0)
		{
			Process->AttachObject(ArgWord(Thread, 1));
		}
		return true;
	}

	// 78.70 (info, button) (FUN_00446070): the input buffer reads past one press, two for a chord (FUN_00492500 lists
	// them: codes 0x80000014..19, 0x34..38 and 0x05..08)
	bool ConsumeComboPress(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Player)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Buffer = ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer);
		const uint32 Code = ArgWord(Thread, 1) - 0x80000000u;
		OKInput::Consume(Runtime, Buffer);
		if ((Code >= 0x14 && Code <= 0x19) || (Code >= 0x34 && Code <= 0x38) || (Code >= 0x05 && Code <= 0x08))
		{
			OKInput::Consume(Runtime, Buffer);
		}
		return true;
	}

	// 78.25 (info) (FUN_00425B00): the input buffer's reader catches up with the newest press, and the releases are cleared
	bool CatchUpPresses(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Buffer = Player ? ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer) : 0;
		uint8* Bytes = Buffer ? Runtime.HeapBytes(Buffer & 0x00FFFFFF, OKInput::Bytes) : nullptr;
		if (!Bytes)
		{
			return true;
		}
		const uint8 Head = Bytes[OKInput::PressHead];
		Bytes[OKInput::Cursor] = Head;
		FMemory::Memcpy(Bytes + 0x5FC, Bytes + OKInput::Presses + Head * OKInput::EntryBytes, 8);
		for (int32 i = 0; i < OKInput::RingSize; ++i)
		{
			FMemory::Memzero(Bytes + OKInput::Releases + i * OKInput::EntryBytes + 4, 4);
		}
		return true;
	}

	// 78.74 (info, &combo) (FUN_004523B0): the combo's next step (combo +8, by the combo length so far) goes into the
	// first free one of player +0xB4C's five; the length goes up, to at most 4. [inf] An AI drone also hears of it.
	bool RecordComboStep(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		const uint32 Combo = ArgWord(Thread, 1);
		if (!Player || Combo == 0)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Length = ReadWord(Runtime, Player->Handle + FOKPlayer::ComboLength);
		for (uint32 i = 0; i < 5; ++i)
		{
			if (ReadWord(Runtime, Player->Handle + FOKPlayer::ComboSteps + i * 4) == 0xFFFFFFFFu)
			{
				WriteWord(Runtime, Player->Handle + FOKPlayer::ComboSteps + i * 4, ReadWord(Runtime, Combo + 8 + Length * 4));
				break;
			}
		}
		WriteWord(Runtime, Player->Handle + FOKPlayer::ComboLength, FMath::Min(Length + 1, 4u));
		return true;
	}

	// 118.58 xfer_fstyle_combo(info, kind, moveset, function) (FUN_008A4F00): records the move in the player's history
	// and marks the fighter's +0x2EC by kind: 0 and 2 set 0x400, 1 and 3 set 0x200, 4 sets 0x400. [inf] The history
	// (player +0x48, for stats and the AI) isn't kept.
	bool RecordMoveKind(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Player || !Player->Fighter)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Kind = ArgWord(Thread, 1);
		const uint32 Address = Player->Fighter->Handle + 0x2EC;
		uint32 Bits = ReadWord(Runtime, Address) & ~0x600u;
		Bits |= (Kind == 0 || Kind == 2 || Kind == 4) ? 0x400u : (Kind == 1 || Kind == 3) ? 0x200u : 0u;
		WriteWord(Runtime, Address, Bits);
		return true;
	}

	// 137.211 xfer_to_moveset_script(obj, function) (FUN_008A0260): the fighter's own thread continues at a function of
	// its moveset's script (the character script)
	bool XferToMovesetScript(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptInstance* Script = Fighter && Fighter->CharacterScript ? Runtime.GetInstance(*Fighter->CharacterScript) : nullptr;
		if (Script)
		{
			OKTransferMainThread(Runtime, *Fighter, (FOKScriptRuntime::InstanceTag << 24) | uint32(Script->Id), ArgWord(Thread, 1));
		}
		return true;
	}

	// 1.6 xfer_thread(script, function): the running thread starts over at a function (1-based) of a script
	// instance (FUN_0045C610). game_p_attack ends with it, into the move the attack transfer recorded (+0x2F80).
	bool XferThread(FOKScriptThread& Thread)
	{
		uint32 ScriptRef = 0, Function = 0;
		if (!Thread.ArgRaw(0, ScriptRef) || !Thread.ArgRaw(1, Function))
		{
			return false;
		}
		FOKScriptInstance* Instance = ((ScriptRef >> 24) == FOKScriptRuntime::InstanceTag)
			? Thread.GetRuntime().FindInstanceById(int32(ScriptRef & 0x00FFFFFF)) : nullptr;
		if (!Instance)
		{
			return Thread.NativeFail(FString::Printf(TEXT("xfer_thread: %08X isn't a script"), ScriptRef));
		}
		return Thread.TransferTo(*Instance, int32(Function) - 1)
			|| Thread.NativeFail(FString::Printf(TEXT("xfer_thread: %s has no function %u"), *Instance->File->GetName(), Function));
	}

	// 7.639 command_matched(obj, command) (FUN_00895680): really "the command is available": never online, else 1
	// when the record is enabled (+8) and past both of its waits (+0x38, +0x40). No press is tested; the move checks
	// (generic_tagswap1_check) test their own.
	bool CommandMatched(FOKScriptThread& Thread)
	{
		uint32 Record = 0;
		if (!Thread.ArgRaw(1, Record))
		{
			return false;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Now = uint32(Runtime.GetSimFrame());
		Thread.SetResult(ReadWord(Runtime, Record + 8) != 0 && ReadWord(Runtime, Record + 0x38) < Now && ReadWord(Runtime, Record + 0x40) < Now ? 1 : 0);
		return true;
	}

	// 6.7 vm_call1(script, function): calls a function (1-based) of a script instance (FUN_0045C090), with no
	// arguments (the wrapper passes a count of 0). special_move_combo calls the move check_special_moves found.
	bool VmCall1(FOKScriptThread& Thread)
	{
		uint32 ScriptRef = 0, Function = 0;
		if (!Thread.ArgRaw(0, ScriptRef) || !Thread.ArgRaw(1, Function))
		{
			return false;
		}
		FOKScriptInstance* Instance = ((ScriptRef >> 24) == FOKScriptRuntime::InstanceTag)
			? Thread.GetRuntime().FindInstanceById(int32(ScriptRef & 0x00FFFFFF)) : nullptr;
		if (!Instance)
		{
			return Thread.NativeFail(FString::Printf(TEXT("vm_call1: %08X isn't a script"), ScriptRef));
		}
		return Thread.CallFunction(*Instance, int32(Function) - 1, {});
	}

	// 6.202 (named special_move_combo) info: when the player is driven by an AI drone (+0xBAC, class AIDrone), runs
	// the drone's scripted inputs (FUN_00890030 -> FUN_0088EF90). [inf] No AI drones here: players are people.
	bool AIDroneStep(FOKScriptThread& Thread)
	{
		return true;
	}

	// 6.245 combo_input_state(obj): 1 while the fighter linked at +0x298 (a handle, serial +0x29C) has +0x788C set
	// or is in state 0x460F (FUN_008F37E0); special_move_combo then skips its check.
	// [inf] What links a fighter at +0x298 isn't read yet; nothing sets it here, so this is 0.
	bool ComboInputState(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptObject* Object = Fighter ? Runtime.ResolveHandle(ReadWord(Runtime, Fighter->Handle + 0x298)) : nullptr;
		const FOKFighter* Linked = Object ? Object->AsFighter() : nullptr;
		Thread.SetResult(Linked && (ReadWord(Runtime, Linked->Handle + 0x788C) != 0
			|| ReadWord(Runtime, Linked->Handle + FOKFighter::State) == 0x460F) ? 1 : 0);
		return true;
	}

	// 7.674 (named abort_2in1_check_n674) obj: online flags 0x1080000 of the fighter's side, in an online mode
	// (FUN_008F19F0). Never set, offline.
	bool OnlineAbort2in1(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 81.207 (named command_flag) obj -> fighter virtual +0x1C8 (FUN_00448B10): attacks only. Every button and
	// command is disabled, the attack buttons are enabled again (virtual +0x1CC, FUN_008929F0), and so are the
	// command records marked to stay on (+0x5C = 1). Walks and steps call it so a press can interrupt them.
	bool EnableAttacksOnly(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		DisableAllCommands(Runtime, *Fighter);
		static const uint32 Attacks[] = { 0x8000000F, 0x80000010, 0x80000011, 0x80000012, 0x80000032, 0x80000033, 0x80000039,
			0x80000030, 0x80000031, 0x8000001B, 0x8000001E, 0x80000014, 0x80000015, 0x80000016, 0x80000017, 0x80000018,
			0x80000019, 0x8000001D, 0x80000034, 0x80000035, 0x80000036, 0x80000037, 0x80000038, 0x80000005, 0x80000006,
			0x80000007, 0x80000008 };
		for (const uint32 Code : Attacks)
		{
			WriteWord(Runtime, SlotAddress(*Fighter, Code) + FOKButtonSlot::Enabled, 1);		// FUN_0044F540
		}
		const uint32 Count = ReadWord(Runtime, Fighter->Handle + FOKFighter::CommandCount);
		for (uint32 i = 0; i < Count && i < uint32(FOKFighter::MaxCommandList); ++i)
		{
			const uint32 Record = ReadWord(Runtime, Fighter->Handle + FOKFighter::CommandList + i * 4);
			if (ReadWord(Runtime, Record + 0x5C) == 1)
			{
				WriteWord(Runtime, Record + 4, 1);
			}
		}
		return true;
	}

	// 78.52 (info, code): held, with walking, crouching and jumping excluding each other (player virtual +0x1C)
	bool InputHeldCommand(FOKScriptThread& Thread)
	{
		uint32 Code = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Code) || !Player)
		{
			return Player == nullptr;
		}
		Thread.SetResult(OKPlayerInput::HeldCommand(Thread.GetRuntime(), *Player, Code) ? 1 : 0);
		return true;
	}

	// 78.50 (info, code): pressed this frame (player virtual +0x14)
	bool InputJustPressed(FOKScriptThread& Thread)
	{
		uint32 Code = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Code) || !Player)
		{
			return Player == nullptr;
		}
		Thread.SetResult(OKPlayerInput::Pressed(Thread.GetRuntime(), *Player, Code) ? 1 : 0);
		return true;
	}

	// 78.83 (info, window, sequence): the motion at sequence was entered within window frames (FUN_00451940)
	bool InputMotion(FOKScriptThread& Thread)
	{
		uint32 Window = 0, Sequence = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Window) || !Thread.ArgRaw(2, Sequence) || !Player)
		{
			return Player == nullptr;
		}
		Thread.SetResult(OKPlayerInput::Motion(Thread.GetRuntime(), *Player, Window, Sequence) ? 1 : 0);
		return true;
	}

	// 78.75 (info, code): the input counts as held last frame too, so holding it on isn't a new press
	// (FUN_00451850 -> FUN_00425620). Towards and away become right or left by the active fighter's facing.
	bool InputSwallowPress(FOKScriptThread& Thread)
	{
		uint32 Code = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Code) || !Player || !Player->Fighter)
		{
			return Player == nullptr || Player->Fighter == nullptr;
		}
		uint32 Mask = 0;
		if (Code == 0x80000001 || Code == 0x80000002)
		{
			uint32 Towards = 0, Away = 0;
			OKPlayerInput::FacingMasks(*Player->Fighter, Towards, Away);
			Mask = Code == 0x80000001 ? Towards : Away;
		}
		else
		{
			Mask = OKInput::CodeToMask(Code);
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Buffer = ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer);
		if (Buffer && (ReadWord(Runtime, Buffer + OKInput::State) & 0xC9) != 0)
		{
			WriteWord(Runtime, Buffer + OKInput::PreviousHeld, ReadWord(Runtime, Buffer + OKInput::PreviousHeld) | Mask);
		}
		return true;
	}

	// 7.637 (named game_check_bufferred_moves_upon_exit) obj: online flags 0x1020000 of the fighter's side, in an
	// online mode (FUN_008F1EE0). Never set, offline.
	bool OnlineBufferedExit(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 81.116 is_opponent_behind(obj, ?) -> fighter virtual +0x198: not virtual +0x194 (FUN_00894390), which is 1 when the
	// opponent is further along the fight line in the direction the fighter faces (its actor's rotation, FUN_00448020).
	// With no opponent, behind.
	bool IsOpponentBehind(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		bool bInFront = false;
		if (Fighter && Fighter->Body && Fighter->Opponent && Fighter->Opponent->Body)
		{
			const FVector& Line = Fighter->LineDirection;
			const double Mine = FVector::DotProduct(Fighter->Body->GetWorldLocation(), Line);
			const double Theirs = FVector::DotProduct(Fighter->Opponent->Body->GetWorldLocation(), Line);
			bInFront = FVector::DotProduct(Fighter->Body->GetFacing(), Line) >= 0.0 ? Mine < Theirs : Theirs < Mine;
		}
		Thread.SetResult(bInFront ? 0 : 1);
		return true;
	}

	// 59.49 anim_at_end(obj) (FUN_00441770 with 0): the playing animation stops looping (node flag 0x20 off), so it
	// holds at its end. ani_to_end calls it before waiting out the last frame.
	bool AnimAtEnd(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Fighter->Body)
		{
			Fighter->Body->SetAnimLooping(false);
		}
		return true;
	}

	// 59.48 (named anim_ticks_left2) obj: 1 while the playing animation loops (node flag 0x20, FUN_00442290 ->
	// FUN_00441300). ani_x_more_ticks then just sleeps instead of aiming for a frame.
	bool AnimIsLooping(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Body && Fighter->Body->IsAnimLooping() ? 1 : 0);
		return true;
	}

	// 6.26 vec_rotation(y, x) -> atan2(y, x), radians (FUN_008B7CF0). rotate_towards_him_with_shuffle passes the
	// offset to the opponent, so this is the yaw that faces them.
	bool VecRotation(FOKScriptThread& Thread)
	{
		uint32 Y = 0, X = 0;
		if (!Thread.ArgRaw(0, Y) || !Thread.ArgRaw(1, X))
		{
			return false;
		}
		Thread.SetResult(FloatToBits(FMath::Atan2(BitsToFloat(Y), BitsToFloat(X))));
		return true;
	}

	// 59.114 obj_yaw(obj, &rotation): the object's rotation in degrees as {roll, pitch, yaw} (virtual +0xD0, then
	// FUN_005EEE10 from UE3's 65536-per-turn units)
	bool ObjYaw(FOKScriptThread& Thread)
	{
		uint32 Out = 0;
		if (!Thread.ArgRaw(1, Out))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const FRotator Rotation = (Fighter && Fighter->Body) ? Fighter->Body->GetFacing().Rotation() : FRotator::ZeroRotator;
		const float Degrees[3] = { float(Rotation.Roll), float(Rotation.Pitch), float(Rotation.Yaw) };
		return Out == 0 || Thread.StoreBytes(Out, Degrees, sizeof(Degrees));
	}

	// 59.69 is_flipped(obj) (FUN_00442B60): mesh +0x758 bit 0, XOR the animation node's flag 0x80. [inf] Node flag 0x80 isn't
	// the mirror that setup_for_flip toggles (0x40) and nothing found sets it, so it's taken as 0.
	bool IsFlipped(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Body && Fighter->Body->IsMeshMirrored() ? 1 : 0);
		return true;
	}

	// 59.26 setup_for_flip(obj) -> object virtual +0x68 (FUN_00442160): the mesh's mirror and the playing animation node's
	// both toggle. Turnaround moves call it once their animation (which turns the fighter round by root motion) is done.
	bool SetupForFlip(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Fighter->Body)
		{
			Fighter->Body->SetupForFlip();
		}
		return true;
	}

	// 81.123 / 59.63 materials_restore(obj) -> object virtual +0xA4 (FUN_00454EB0): unless the fighter keeps its
	// materials (+0x1D0 flag 0x40000), puts its materials back (after a glow or a freeze) and returns 1.
	// [inf] Materials aren't swapped here yet, so only the result stands.
	bool MaterialsRestore(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && (ReadWord(Thread.GetRuntime(), Fighter->Handle + 0x1D0) & 0x40000) == 0 ? 1 : 0);
		return true;
	}

	// 81.204 (named command_state) obj: every button and command disabled (FUN_00448A40)
	bool DisableAllCommandsNative(FOKScriptThread& Thread)
	{
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			DisableAllCommands(Thread.GetRuntime(), *Fighter);
		}
		return true;
	}

	// 7.636 (named ai_jump_away) obj: 1 when the fighter's character info has flag 0x40 at +0xA0, or its side has
	// online flags 0x1000010 in an online mode (FUN_008F1BB0)
	bool CharacterFlag40(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Info = Fighter ? ReadWord(Runtime, Fighter->Handle + 0x2A0) : 0;
		Thread.SetResult(Fighter && Fighter->Player && Info && (ReadWord(Runtime, Info + 0xA0) & 0x40) ? 1 : 0);
		return true;
	}

	// 78.86 (info, code, allowRelease, sequence, window, late, maxAge): a 2-in-1 cancel's input (FUN_00455AF0)
	bool Input2in1(FOKScriptThread& Thread)
	{
		uint32 A[6] = {};
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		for (int32 i = 0; i < 6; ++i)
		{
			if (!Thread.ArgRaw(i + 1, A[i]))
			{
				return false;
			}
		}
		Thread.SetResult(Player && OKPlayerInput::TwoInOne(Thread.GetRuntime(), *Player, A[0], A[1] != 0, A[2], A[3], A[4], A[5]) ? 1 : 0);
		return true;
	}

	// 137.10 begin_special_move(obj, &command) (FUN_0089C920)
	bool BeginSpecialMoveNative(FOKScriptThread& Thread)
	{
		uint32 Record = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Record) || !Fighter || Record == 0)
		{
			return Fighter == nullptr || Record == 0;
		}
		BeginSpecialMove(Thread.GetRuntime(), *Fighter, Record);
		return true;
	}

	// 137.207 xfer_to_special_move(obj, &command, useMeter, meter) (FUN_008A75F0): with useMeter, the side gains the
	// meter first (a normal special's +0.02, from its command's +0x94 / +0x98). 137.208 xfer_to_ex_special_move(obj,
	// &command, cost, special) (FUN_008A76D0) spends the cost, unless it's 100 (free).
	bool XferToSpecialMoveNative(FOKScriptThread& Thread)
	{
		uint32 Record = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Record) || !Fighter || Record == 0)
		{
			return Fighter == nullptr || Record == 0;
		}
		if (ArgWord(Thread, 2) != 0 && Fighter->Player)
		{
			OKAddMeter(Thread.GetRuntime(), *Fighter->Player, ArgFloat(Thread, 3));
		}
		return XferToSpecialMove(Thread.GetRuntime(), *Fighter, Record, true);
	}

	bool XferToExSpecialMove(FOKScriptThread& Thread)
	{
		uint32 Record = 0, Cost = 0, Special = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Record) || !Thread.ArgRaw(2, Cost) || !Thread.ArgRaw(3, Special) || !Fighter || Record == 0)
		{
			return Fighter == nullptr || Record == 0;
		}
		if (BitsToFloat(Cost) != 100.0f && Fighter->Player)
		{
			OKAddMeter(Thread.GetRuntime(), *Fighter->Player, -BitsToFloat(Cost));
		}
		return XferToSpecialMove(Thread.GetRuntime(), *Fighter, Record, Special != 0);
	}

	/** The gate command_matched and its variants share: enabled (+8) and past both waits. */
	bool CommandReady(FOKScriptRuntime& Runtime, uint32 Record)
	{
		const uint32 Now = uint32(Runtime.GetSimFrame());
		return ReadWord(Runtime, Record + 8) != 0 && ReadWord(Runtime, Record + 0x38) < Now && ReadWord(Runtime, Record + 0x40) < Now;
	}

	/** The side's meter (player +0xC14; scripts compare it with 0.3333, a bar) covers Cost. */
	bool HasMeter(FOKScriptRuntime& Runtime, const FOKFighter& Fighter, float Cost)
	{
		return Fighter.Player && Cost <= BitsToFloat(ReadWord(Runtime, Fighter.Player->Handle + 0xC14));
	}

	// 7.640 command_matched_air(obj, height, &command) (FUN_008956D0): command_matched, while the fighter is higher
	// than height above the floor
	bool CommandMatchedAir(FOKScriptThread& Thread)
	{
		uint32 Height = 0, Record = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Height) || !Thread.ArgRaw(2, Record))
		{
			return false;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(Fighter && Fighter->Body && Record && CommandReady(Runtime, Record) && BitsToFloat(Height) < Fighter->Body->GetHeight() ? 1 : 0);
		return true;
	}

	// 7.641 command_matched_ex(obj, &command, cost) (FUN_00895750): command_matched, with the meter for it
	bool CommandMatchedEx(FOKScriptThread& Thread)
	{
		uint32 Record = 0, Cost = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Record) || !Thread.ArgRaw(2, Cost))
		{
			return false;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(Fighter && Record && HasMeter(Runtime, *Fighter, BitsToFloat(Cost)) && CommandReady(Runtime, Record) ? 1 : 0);
		return true;
	}

	// 7.642 command_matched_air_ex(obj, height, &command, cost) (FUN_008957C0): both
	bool CommandMatchedAirEx(FOKScriptThread& Thread)
	{
		uint32 Height = 0, Record = 0, Cost = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Height) || !Thread.ArgRaw(2, Record) || !Thread.ArgRaw(3, Cost))
		{
			return false;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(Fighter && Fighter->Body && Record && HasMeter(Runtime, *Fighter, BitsToFloat(Cost)) && CommandReady(Runtime, Record)
			&& BitsToFloat(Height) < Fighter->Body->GetHeight() ? 1 : 0);
		return true;
	}

	// 59.22 (named clone_mesh) obj: the mesh component's render flags (component +0x690, FUN_0042F7F0 ->
	// FUN_00760990), or -1 without a mesh. InitIceclone masks them for the clone it spawns.
	// [inf] OpenKombat's bodies keep no such flags, so 0.
	bool MeshRenderFlags(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Body ? 0 : 0xFFFFFFFFu);
		return true;
	}

	// 59.8 spawn_skel_actor(mesh, flags, ?, ?, ?): a new MKSkeletalObj (0x1D0) showing a skeletal mesh (FUN_004441B0);
	// 137.13 (named init_iceclone) obj gives the fighter's own mesh for it (actor +0x27C -> +0x280, FUN_008A10F0).
	// [inf] Spawning script objects is system W1 (MKKE-SYSTEM-MAP), not built: no mesh handle, no object. Callers
	// check for 0 (InitIceclone then goes without its clone).
	bool SpawnSkelActor(FOKScriptThread& Thread)
	{
		UE_LOG(LogOKVM, Warning, TEXT("frame %lld: spawn_skel_actor: skeletal objects aren't spawned yet (W1)"), Thread.GetRuntime().GetSimFrame());
		Thread.SetResult(0);
		return true;
	}

	// 59.71 set_hidden(obj, hidden), also 52.30 and 81.8 set_obj_hidden: object virtual +0x34 shows or hides it.
	// [inf] Only fighters have bodies here; other objects come with W1.
	bool SetHidden(FOKScriptThread& Thread)
	{
		uint32 bHidden = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, bHidden))
		{
			return false;
		}
		if (Fighter && Fighter->Body)
		{
			Fighter->Body->SetBodyHidden(bHidden != 0);
		}
		return true;
	}

	// 6.243 online_side(obj): 1 when the fighter's side plays online in an online mode (FUN_008F1930). Never, offline.
	bool OnlineSide(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 137.172 online_flag30(obj): bit 30 of the side's online flags (FUN_008954F0). Never set, offline.
	bool OnlineFlag30(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 59.91 morph_target_set(obj, weight, ?, name) (FUN_0042FF70): +0x110 = -1, +0x114 weight, +0x118 the second
	// value, and the named morph target ("Hair", "Boobs") of the body (+0x10C) blends to the weight.
	// [inf] Morph targets aren't imported yet, so only the fields are kept.
	bool MorphTargetSet(FOKScriptThread& Thread)
	{
		uint32 Weight = 0, Second = 0, Name = 0;
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Thread.ArgRaw(1, Weight) || !Thread.ArgRaw(2, Second) || !Thread.ArgRaw(3, Name) || !Fighter)
		{
			return Fighter == nullptr;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		WriteWord(Runtime, Fighter->Handle + 0x110, 0xFFFFFFFFu);
		WriteWord(Runtime, Fighter->Handle + 0x114, Weight);
		WriteWord(Runtime, Fighter->Handle + 0x118, Second);
		return true;
	}

	// 118.3 / 78.33 get_active_char(info, ?) -> the player's active fighter: the handle at +0x18, when it's still
	// alive (FUN_00895B70).
	bool GetActiveChar(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		Thread.SetResult(Player && Player->Fighter ? Player->Fighter->Handle : 0);
		return true;
	}

	// 137.173 online_flag_b(obj): 1 when the fighter's side is an online player (FUN_00895530). Never, offline.
	bool OnlineFlagB(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 136.31 get_player(obj, side) -> that side's player record (FUN_0080E540 indexes the records; the object isn't used,
	// and gamelogic passes GFGGameInfo)
	bool GetSidePlayer(FOKScriptThread& Thread)
	{
		uint32 Side = 0;
		Thread.ArgValue(1, Side);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptObject* Object = Runtime.ResolveHandle(Runtime.FindBinding(TEXT("GFGGameInfo")));
		FOKGameInfo* Game = Object ? Object->AsGameInfo() : nullptr;
		FOKPlayer* Player = nullptr;
		if (Game)
		{
			for (FOKPlayer* Each : Game->Players)
			{
				if (Each && ReadWord(Runtime, Each->Handle + FOKPlayer::Side) == Side)
				{
					Player = Each;
				}
			}
		}
		Thread.SetResult(Player ? Player->Handle : 0);
		return true;
	}

	// 6.230 (named start_player_proc) info, a, b, c: hands three values to the player's controller (+0x1C) when it
	// has a device (+0x10C) (FUN_004365E0 -> FUN_00751C30). [inf] Rumble, by the calls (on/off, 0, time). No pads here.
	bool ControllerFeedback(FOKScriptThread& Thread)
	{
		uint32 Unused = 0;
		return Thread.ArgRaw(3, Unused);
	}

	// 137.233 clear_attack(obj, ?): ends the fighter's attack (FUN_008967B0): its waiting presses go, the attack
	// records clear, and the state becomes 0x5200. The second argument is unused.
	// [inf] It also seeks the fighter's animation through +0x10C (FUN_00447A30); the actor owns animation here.
	bool ClearAttack(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		if (Fighter->Player)
		{
			OKInput::Flush(Runtime, ReadWord(Runtime, Fighter->Player->Handle + FOKPlayer::InputBuffer));
		}
		WriteWord(Runtime, Fighter->Handle + 0x2F58, 1);
		WriteWord(Runtime, Fighter->Handle + 0x79D4, 0);		// floats, 0.0
		WriteWord(Runtime, Fighter->Handle + 0x7BF0, 0);
		WriteWord(Runtime, Fighter->Handle + FOKFighter::LiveAttack, 0);
		WriteWord(Runtime, Fighter->Handle + FOKFighter::PendingAttack, 0);
		WriteWord(Runtime, Fighter->Handle + FOKFighter::PreviousState, ReadWord(Runtime, Fighter->Handle + FOKFighter::State));
		WriteWord(Runtime, Fighter->Handle + FOKFighter::State, 0x5200);
		return true;
	}

	// 43.24 get_player(gameinfo, side) -> the player record (GameInfo virtual +0x10). Also 136.26.
	bool GetPlayer(FOKScriptThread& Thread)
	{
		uint32 Info = 0, Side = 0;
		if (!Thread.ArgValue(0, Info) || !Thread.ArgRaw(1, Side))
		{
			return false;
		}
		FOKScriptObject* Object = Thread.GetRuntime().ResolveHandle(Info);
		FOKGameInfo* Game = Object ? Object->AsGameInfo() : nullptr;
		Thread.SetResult((Game && Side < 2 && Game->Players[Side]) ? Game->Players[Side]->Handle : 0);
		return true;
	}

	// 78.93 push_control(info, value): pushes a control state, up to 4 (FUN_00446710); 1 lets the player act
	bool PushControl(FOKScriptThread& Thread)
	{
		uint32 Value = 0;
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (!Thread.ArgRaw(1, Value) || !Player)
		{
			return Player == nullptr;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Count = ReadWord(Runtime, Player->Handle + FOKPlayer::ControlCount);
		if (Count < 4)
		{
			WriteWord(Runtime, Player->Handle + FOKPlayer::ControlStack + Count * 4, Value);
			WriteWord(Runtime, Player->Handle + FOKPlayer::ControlCount, Count + 1);
		}
		return true;
	}

	// 78.94 clear_control(info): empties the control stack (FUN_00446780)
	bool ClearControl(FOKScriptThread& Thread)
	{
		if (FOKPlayer* Player = ArgPlayer(Thread, 0))
		{
			WriteWord(Thread.GetRuntime(), Player->Handle + FOKPlayer::ControlCount, 0);
			WriteWord(Thread.GetRuntime(), Player->Handle + 0xB94, 0);
		}
		return true;
	}

	// --- Input scanners (FIGHT-RE-NOTES §6) ---------------------------------------------------------

	/** A character function slot's script (+4) and function (+0xC). */
	uint32 FuncSlot(const FOKFighter& Fighter, int32 Slot)
	{
		return Fighter.Handle + FOKFighter::FuncSlots + Slot * FOKFighter::FuncSlotBytes;
	}

	// 137.39 bind_char_func_slot(obj, slot, script, function): slot < 8. Slot 0 is also copied to 4, 5 and 6
	// (FUN_00894660). *_game_init binds a character's input scanners this way.
	bool BindCharFuncSlot(FOKScriptThread& Thread)
	{
		uint32 Slot = 0, Script = 0, Function = 0;
		if (!Thread.ArgRaw(1, Slot) || !Thread.ArgRaw(2, Script) || !Thread.ArgRaw(3, Function))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || int32(Slot) < 0 || int32(Slot) >= FOKFighter::MaxFuncSlots)
		{
			return true;
		}
		const uint32 Entry[3] = { Script, Script, Function };		// script handle {object, serial}: our serial is the value itself
		bool bOk = Thread.StoreBytes(FuncSlot(*Fighter, Slot) + 4, Entry, sizeof(Entry));
		if (Slot == 0)
		{
			for (const int32 Copy : { 4, 5, 6 })
			{
				bOk &= Thread.StoreBytes(FuncSlot(*Fighter, Copy) + 4, Entry, sizeof(Entry));
			}
		}
		return bOk;
	}

	// 137.40 clear_char_func_slots(obj): every slot's script handle released and its function 0 (FUN_008D76D0)
	bool ClearCharFuncSlots(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const uint32 Empty[3] = { 0, 0xFFFFFFFFu, 0 };
		for (int32 Slot = 0; Fighter && Slot < FOKFighter::MaxFuncSlots; ++Slot)
		{
			if (!Thread.StoreBytes(FuncSlot(*Fighter, Slot) + 4, Empty, sizeof(Empty)))
			{
				return false;
			}
		}
		return true;
	}

	// 137.38 check_special_moves(obj, slot, flag) (FUN_0089C9B0): runs the slot's input scanner at once, on a
	// thread of its own with @my_obj / @my_info bound. A scanner that matches a command switches the fighter's
	// main thread to the move; check_special_moves sees that and stops. It runs the scanner in up to two passes,
	// with the player's +0x940 set to 1, then 2:
	//   slots 0, 6, 7: one argument; with flag 0, only when a button is down (FUN_00451150 / FUN_004511F0),
	//                  and the argument is 1 when it was the first test that saw it;
	//   slot 4:        one argument, 1;
	//   other slots:   no arguments.
	bool CheckSpecialMoves(FOKScriptThread& Thread)
	{
		uint32 Slot = 0, Flag = 0;
		if (!Thread.ArgRaw(1, Slot) || !Thread.ArgRaw(2, Flag))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || Slot > 7)
		{
			return true;
		}
		uint32 Entry[3] = {};
		if (!Thread.LoadBytes(FuncSlot(*Fighter, Slot) + 4, Entry, sizeof(Entry)))
		{
			return false;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptObject* MainObject = Runtime.ResolveHandle(Fighter->MainThread);
		FOKScriptThread* Main = MainObject ? MainObject->AsThread() : nullptr;
		FOKScriptInstance* Scanner = (Entry[0] >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(Entry[0] & 0x00FFFFFF)) : nullptr;
		if (!Scanner || Entry[2] == 0 || !Main)
		{
			return true;		// no scanner bound, or no fighter thread for a move to take over
		}
		FOKScriptInstance* Target = nullptr;
		int32 Function = INDEX_NONE;
		if (!Runtime.ResolveFunction(*Scanner, Entry[2], Target, Function))
		{
			return Thread.NativeFail(FString::Printf(TEXT("check_special_moves: slot %u's function %u isn't in its script"), Slot, Entry[2]));
		}

		TArray<uint32, TInlineAllocator<1>> Args;
		if (Slot == 0 || Slot == 6 || Slot == 7)
		{
			// FUN_00451150 / FUN_004511F0: the current CollisionContext's outcome is a hit (0) or a block (1).
			uint32 Outcome = 5;
			if (Fighter->CollisionContext != 0)
			{
				Runtime.LoadHeapWord(Fighter->CollisionContext + FOKCollisionContext::Outcome, Outcome);
			}
			const bool bFirstTest = Outcome == 0, bSecondTest = Outcome == 1;
			if (Flag == 0 && !bFirstTest && !bSecondTest)
			{
				return true;
			}
			Args.Add(bFirstTest ? 1 : 0);
		}
		else if (Slot == 4)
		{
			Args.Add(1);
		}

		const uint32 Scanning = Fighter->Player ? Fighter->Player->Handle + FOKPlayer::Scanning : 0;
		const int32 Transfers = Main->GetTransfers();
		bool bOk = true;
		for (uint32 Pass = 1; Pass <= 2 && bOk && Main->GetTransfers() == Transfers; ++Pass)
		{
			if (Scanning)
			{
				Runtime.SetHeapWord(Scanning, Pass);
			}
			FOKScriptThread Scan(Runtime);
			Scan.BindFighter(*Fighter);
			uint32 Result = 0;
			FString Error;
			bOk = Scan.Run(*Target->File, Function, Args, Result, Error);
			if (!bOk)
			{
				Thread.NativeFail(TEXT("check_special_moves: ") + Error);
			}
		}
		if (Scanning)
		{
			Runtime.SetHeapWord(Scanning, 0);
		}
		return bOk;
	}

	// --- Animation (ANIM-RE-NOTES §4) -------------------------------------------------------

	// 81.52 blend_to_ani_dir(obj, anim, startFrame, rate, blendFrames, flags, direction)
	// 81.51 / 59.29 blend_to_ani(obj, anim, startFrame, rate, blendFrames, flags)
	// 81.53 blend_to_ani_frame(obj, anim, startFrame, rate, blendFrames, flags, ...)
	// All reach FUN_008A4080 with the flags fifth: bit 2 loops (node flag 0x20), bit 1 plays the node mirrored (0x40,
	// FUN_00435960). The animation is an asset value: the script's asset operand, bound when its instance was made.
	bool BlendToAni(FOKScriptThread& Thread)
	{
		uint32 Anim = 0, StartFrame = 0, Rate = 0, BlendFrames = 0;
		if (!Thread.ArgRaw(1, Anim) || !Thread.ArgRaw(2, StartFrame) || !Thread.ArgRaw(3, Rate) || !Thread.ArgRaw(4, BlendFrames))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || !Fighter->Body)
		{
			return true;		// nothing on screen to animate
		}
		const FOKMkoAsset* Asset = Thread.GetRuntime().FindAsset(Anim);
		if (!Asset)
		{
			UE_LOG(LogOKVM, Warning, TEXT("blend_to_ani: %08X isn't an animation"), Anim);
			return true;
		}
		const uint32 Flags = ArgWord(Thread, 5);
		Fighter->Body->PlayAnimation(Asset->Object, BitsToFloat(StartFrame), BitsToFloat(Rate), BitsToFloat(BlendFrames), (Flags & 2) != 0);
		Fighter->Body->SetAnimNodeMirrored((Flags & 1) != 0);
		Fighter->CurrentAnim = Anim;
		RunMoveCallback(Thread.GetRuntime(), *Fighter);
		return true;
	}

	// 59.42 anim_current_frame(obj) -> float
	bool AnimCurrentFrame(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(FloatToBits((Fighter && Fighter->Body) ? Fighter->Body->GetAnimFrame() : 0.0f));
		return true;
	}

	// 59.40 anim_last_frame(obj) -> float: the playing sequence's frame count - 1 (FUN_00443AF0 reads NumFrames
	// from the sequence's data). Was named anim_last_collision_frame.
	bool AnimLastFrame(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const float Count = (Fighter && Fighter->Body) ? float(Fighter->Body->GetAnimFrameCount()) : 0.0f;
		Thread.SetResult(FloatToBits(Count - 1.0f));
		return true;
	}

	// 6.112 moveset_anim_end_frame(anim) -> its last frame, an integer (FUN_00491910: the sequence's virtual +0x18).
	// [inf] NumFrames - 1, as 59.40 gives for the playing one. The anim is looked up on the thread's own fighter.
	bool MovesetAnimEndFrame(FOKScriptThread& Thread)
	{
		uint32 Anim = 0;
		const FOKMkoAsset* Asset = Thread.ArgValue(0, Anim) ? Thread.GetRuntime().FindAsset(Anim) : nullptr;
		FOKFighter* Fighter = Thread.MyFighter();
		const int32 Count = Asset && Fighter && Fighter->Body ? Fighter->Body->GetSequenceFrameCount(Asset->Object) : 0;
		Thread.SetResult(uint32(FMath::Max(0, Count - 1)));
		return true;
	}

	// 59.47 anim_rate(obj) -> float: the playing node's rate (node +0x84). Was named anim_ticks_left.
	bool AnimRate(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(FloatToBits((Fighter && Fighter->Body) ? Fighter->Body->GetAnimRate() : 0.0f));
		return true;
	}

	// 59.46 set_anim_rate(obj, rate)
	bool SetAnimRate(FOKScriptThread& Thread)
	{
		uint32 Rate = 0;
		if (!Thread.ArgRaw(1, Rate))
		{
			return false;
		}
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (Fighter && Fighter->Body)
		{
			Fighter->Body->SetAnimRate(BitsToFloat(Rate));
		}
		return true;
	}

	// 59.37 set_root_motion_scale(obj, scale): kept on the fighter until root motion is applied
	bool SetRootMotionScale(FOKScriptThread& Thread)
	{
		uint32 Scale = 0;
		if (!Thread.ArgRaw(1, Scale))
		{
			return false;
		}
		if (FOKFighter* Fighter = Thread.ArgFighter(0))
		{
			Fighter->RootMotionScale = BitsToFloat(Scale);
		}
		return true;
	}

	// 81.119 anim_flipping(obj) -> whether the animation plays mirrored. Mirroring comes later: never.
	bool AnimFlipping(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}
}


bool OKCanAct(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
{
	return CanAct(Runtime, Fighter);
}

void OKAddMeter(FOKScriptRuntime& Runtime, FOKPlayer& Player, float Delta)
{
	// A fighter whose character info has flag 0x1000 (+0xA0) gets none. [inf] Online checks and the cheats that keep
	// it full aren't here.
	if (Player.Fighter && (ReadWord(Runtime, ReadWord(Runtime, Player.Fighter->Handle + FOKFighter::CharacterInfo) + 0xA0) & 0x1000) != 0)
	{
		WriteWord(Runtime, Player.Handle + FOKPlayer::Meter, 0);
		return;
	}
	float Meter = FMath::Clamp(BitsToFloat(ReadWord(Runtime, Player.Handle + FOKPlayer::Meter)) + Delta, 0.0f, 1.0f);
	if (Delta < 0.0f && Meter < 0.001f)
	{
		Meter = 0.0f;		// what spending leaves under 0.001 is gone
	}
	WriteWord(Runtime, Player.Handle + FOKPlayer::Meter, FloatToBits(Meter));
}

// ResolveHit (FUN_008AC0F0, FIGHT-RE-NOTES §7.2). For now every contact is a clean hit: outcome 0 in the context.
// Damage, blocking and the victim's reaction come next.
void OKResolveContact(FOKScriptRuntime& Runtime, FOKFighter& Attacker, FOKFighter& Victim, uint32 Context)
{
	if ((Context >> 24) == FOKScriptRuntime::HeapTag)
	{
		WriteWord(Runtime, Context + FOKCollisionContext::Outcome, 0);
	}
	// A parry (ResolveHit step 5; set_parry, 81.232): the victim's +0x2E8 bit 0x10000, an attack without +0x17C, and
	// the attack's class for the parry's kind (+0x75FC): 0 attack flag 0x400 (and no armour, the attacker not
	// invulnerable), 1 flag 0x800. Then the parry function (+0x7600 script, +0x760C function) starts as process 0x55 in
	// group 6 for the victim, the contact ends with outcome 4, the victim is invulnerable for 100 ticks and the parry
	// is spent. [inf] MKKE's AttackFlag falls back to the contact's flags when the attack has none, and kind 1 also
	// wants a distance (FUN_00492790) under DAT_00C6724C; neither is kept here.
	const uint32 VictimFlags = ReadWord(Runtime, Victim.Handle + FOKFighter::Flags);
	const uint32 Now = uint32(Runtime.GetSimFrame());
	if ((VictimFlags & 0x10000) != 0 && ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + 0x17C) == 0)
	{
		const uint32 Kind = ReadWord(Runtime, Victim.Handle + 0x75FC);
		const uint32 Class = ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::AttackFlags);
		const bool bParried = Kind == 0 ? ((Class & 0x400) != 0 && FindArmor(Runtime, Victim) == 0 && ReadWord(Runtime, Attacker.Handle + FOKFighter::Invulnerable) < Now)
			: (Kind == 1 && (Class & 0x800) != 0);
		if (bParried)
		{
			const uint32 ScriptValue = ReadWord(Runtime, Victim.Handle + 0x7600);
			const uint32 FunctionValue = ReadWord(Runtime, Victim.Handle + 0x760C);
			const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
			FOKScriptInstance* Target = nullptr;
			int32 Function = INDEX_NONE;
			FString Error;
			if (From && FunctionValue != 0 && Runtime.ResolveFunction(*From, FunctionValue, Target, Function))
			{
				if (FOKScriptThread* Parry = Runtime.StartProcess(*Target->File, Function, 0x55, 6, true, Error))
				{
					Parry->BindFighter(Victim);
				}
			}
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s parries %s's attack (%s)"), Runtime.GetSimFrame(), *Victim.Name, *Attacker.Name,
				Target ? *Target->File->GetFunctions()[Function].Name : TEXT("no function"));
			if ((Context >> 24) == FOKScriptRuntime::HeapTag)
			{
				WriteWord(Runtime, Context + FOKCollisionContext::Outcome, 4);
			}
			WriteWord(Runtime, Victim.Handle + FOKFighter::Invulnerable, FMath::Max(ReadWord(Runtime, Victim.Handle + FOKFighter::Invulnerable), Now + 100));
			WriteWord(Runtime, Victim.Handle + FOKFighter::Flags, VictimFlags & ~0x10000u);
			return;
		}
	}
	// Damage is a fraction of the health bar (attack +0xCC), times the victim's combo scaling (player +0xB60, which
	// every reaction lowers with adjust_my_damage_multiplier). [inf] The player's virtual +0x4C scales it again; taken
	// as 1. Blocking, armour, breakers and the death path come later.
	FOKPlayer* VictimPlayer = Victim.Player;
	const float Scale = VictimPlayer ? BitsToFloat(ReadWord(Runtime, VictimPlayer->Handle + FOKPlayer::DamageMultiplier)) : 1.0f;
	const float Damage = BitsToFloat(ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::Damage)) * Scale;
	if (AbsorbWithArmor(Runtime, Attacker, Victim, Damage))
	{
		if ((Context >> 24) == FOKScriptRuntime::HeapTag)
		{
			WriteWord(Runtime, Context + FOKCollisionContext::Outcome, 1);
		}
		return;
	}
	if (BreakCombo(Runtime, Attacker, Victim, Context) || BlockContact(Runtime, Attacker, Victim, Context))
	{
		return;
	}

	// ApplyReaction (FUN_008A5490): the victim's side counts the hit (FUN_0089EBB0 -> FUN_008991E0). A hit on a fighter
	// that was free last frame (+0x2FCC clear) ends the combo before it, if it can end.
	uint32 Hits = 0;
	if (VictimPlayer)
	{
		// FUN_0089EBB0: by the attack's flags (+0xE8), the victim stays busy for combos (+0x2F5C, read by FightPlayers::Update):
		// 0x10000000 no, 0x40000000 no and its window (+0x2F60 / +0x2F64) cleared, 0x20000000 yes. Most attacks have
		// 0x20000000, so a combo goes on until the victim's reaction ends it (combo_message, 118.48).
		// [inf] Without those bits MKKE tests a second flags word of the contact; not kept here.
		const uint32 AttackFlags = ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::AttackFlags);
		if (AttackFlags & 0x10000000)
		{
			WriteWord(Runtime, Victim.Handle + 0x2F5C, 0);
		}
		else if (AttackFlags & 0x40000000)
		{
			WriteWord(Runtime, Victim.Handle + 0x2F5C, 0);
			WriteWord(Runtime, Victim.Handle + 0x2F60, 0);
			WriteWord(Runtime, Victim.Handle + 0x2F64, 0);
		}
		else if (AttackFlags & 0x20000000)
		{
			WriteWord(Runtime, Victim.Handle + 0x2F5C, 1);
		}
		WriteWord(Runtime, VictimPlayer->Handle + 0xC60, ReadWord(Runtime, VictimPlayer->Handle + 0xC60) + 1);
		WriteWord(Runtime, VictimPlayer->Handle + 0xBC8, ReadWord(Runtime, VictimPlayer->Handle + 0xBC8) + 1);
		if (ReadWord(Runtime, Victim.Handle + FOKFighter::NotFree) == 0 && ReadWord(Runtime, VictimPlayer->Handle + FOKPlayer::ComboHits) != 0)
		{
			EndCombo(Runtime, *VictimPlayer);
		}
		Hits = ReadWord(Runtime, VictimPlayer->Handle + FOKPlayer::ComboHits) + 1;
		WriteWord(Runtime, VictimPlayer->Handle + FOKPlayer::ComboHits, Hits);
	}
	// The attacker's side gains the damage x 0.7 in meter (DAT_00D46B8C).
	if (Attacker.Player)
	{
		OKAddMeter(Runtime, *Attacker.Player, Damage * 0.7f);
	}
	const TCHAR* HurtBone = OKCollision::BoneName(Attacker.LastContact.HurtBone);
	const TCHAR* AttackBone = OKCollision::BoneName(Attacker.LastContact.AttackBone);
	UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s hits %s on the %s (attack sphere %s), damage %g (x%.3f), combo hit %u, meter %.3f"), Runtime.GetSimFrame(),
		*Attacker.Name, *Victim.Name, HurtBone ? HurtBone : TEXT("?"), AttackBone ? AttackBone : TEXT("?"), Damage, Scale, Hits,
		Attacker.Player ? BitsToFloat(ReadWord(Runtime, Attacker.Player->Handle + FOKPlayer::Meter)) : 0.0f);

	ApplyDamage(Runtime, Victim, Damage);

	// The reaction (ResolveHit step 4 and FUN_00895910): the attack's standing reaction (+0xBC), a function of its
	// reaction script (+0xB8). MKKE picks one of four (+0xBC..+0xC8) by where the attacker is in the victim's frame,
	// falling back to +0xBC, but both setters (6.1 and the pending attack's FUN_00896EC0, whose one caller passes
	// zeros) leave the other three 0, so it's always +0xBC. A victim in the air (fighter virtual +0x1FC, Airborne)
	// takes the airborne reaction (+0xD4) of the script 6.1 kept at +0xDC instead, and while its state has 0x400 its
	// side counts a juggle hit (player +0xC64). [inf] Left out: the contact's flag 8, which keeps the standing one.
	uint32 ScriptValue = ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::ReactionScript);
	uint32 Reaction = ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::StandingReaction);
	const uint32 AirReaction = ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::AirborneReaction);
	const bool bAir = Airborne(Runtime, Victim);
	if (bAir)
	{
		if (VictimPlayer && (ReadWord(Runtime, Victim.Handle + FOKFighter::State) & 0x400) != 0)
		{
			WriteWord(Runtime, VictimPlayer->Handle + 0xC64, ReadWord(Runtime, VictimPlayer->Handle + 0xC64) + 1);
		}
		const uint32 AirScript = ReadWord(Runtime, Attacker.Handle + FOKFighter::LiveAttack + OKAttack::AirborneScript);
		if (AirReaction != 0)
		{
			Reaction = AirReaction;
			ScriptValue = AirScript != 0 ? AirScript : ScriptValue;
		}
	}
	const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
	FOKScriptInstance* Target = nullptr;
	int32 Function = INDEX_NONE;
	if (From && Reaction != 0 && Runtime.ResolveFunction(*From, Reaction, Target, Function))
	{
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s reacts with %s.%s%s, health %g"), Runtime.GetSimFrame(), *Victim.Name,
			*Target->File->GetName().Replace(TEXT(".mko"), TEXT("")), *Target->File->GetFunctions()[Function].Name,
			bAir ? (AirReaction != 0 ? TEXT(" (in the air)") : TEXT(" (in the air, no airborne reaction)")) : TEXT(""), Victim.Health);
		XferToReaction(Runtime, Victim, *Target, Function);
	}
}

bool OKTransferMainThread(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 ScriptValue, uint32 FunctionRaw)
{
	const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
	FOKScriptInstance* Target = nullptr;
	int32 Function = INDEX_NONE;
	if (!From || !Runtime.ResolveFunction(*From, FunctionRaw, Target, Function))
	{
		UE_LOG(LogOKVM, Warning, TEXT("%s: no function %08X in script %08X to transfer to"), *Fighter.Name, FunctionRaw, ScriptValue);
		return false;
	}
	return XferToReaction(Runtime, Fighter, *Target, Function);
}

void OKUpdateThrow(FOKScriptRuntime& Runtime, FOKFighter& Follower)
{
	FOKFighter* Leader = Follower.ThrowLeader;
	if (!Leader || !Leader->Body || !Follower.Body)
	{
		return;
	}
	// The link holds while the leader plays the same sequence (FUN_00490900).
	if (Leader->Body->GetAnimName() != Follower.ThrowSequence)
	{
		OKReleaseThrow(Runtime, Follower, true);
		return;
	}
	// Frame lock and root alignment
	Follower.Body->FollowThrow(*Leader->Body);
}

void OKReleaseThrow(FOKScriptRuntime& Runtime, FOKFighter& Follower, bool bCleanup)
{
	FOKFighter* Leader = Follower.ThrowLeader;
	Follower.ThrowLeader = nullptr;
	Follower.ThrowSequence.Reset();
	if (!Leader)
	{
		return;
	}
	UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s lets go of %s"), Runtime.GetSimFrame(), *Leader->Name, *Follower.Name);
	// FUN_004494A0: neither holds now, and both get their flags back
	WriteWord(Runtime, Leader->Handle + FOKFighter::Holding, 0);
	for (FOKFighter* Fighter : { Leader, &Follower })
	{
		if (ReadWord(Runtime, Fighter->Handle + FOKFighter::SavedCharFlags + 4) != 0)
		{
			WriteWord(Runtime, Fighter->Handle + FOKFighter::SavedCharFlags + 4, 0);
			WriteWord(Runtime, Fighter->Handle + FOKFighter::CharFlags, ReadWord(Runtime, Fighter->Handle + FOKFighter::SavedCharFlags));
		}
		else
		{
			WriteWord(Runtime, Fighter->Handle + FOKFighter::CharFlags, ReadWord(Runtime, Fighter->Handle + FOKFighter::CharFlags) & ~0x20u);
		}
	}
	// The victim's cleanup reaction, if the attack set one (two_player_rx_setup / two_player_rx_force: live attack +0x128 flag,
	// +0x130 script handle, +0x13C function). [inf] MKKE runs it when a reaction resets (FUN_008AB040); here the throw ending
	// is that moment.
	const uint32 Slot = Leader->Handle + FOKFighter::LiveAttack;
	uint32 Script = Follower.ThrowCleanupScript, Function = Follower.ThrowCleanupFunction;
	Follower.ThrowCleanupScript = Follower.ThrowCleanupFunction = 0;
	if (!bCleanup)
	{
		return;
	}
	if (ReadWord(Runtime, Slot + 0x128) != 0)
	{
		WriteWord(Runtime, Slot + 0x128, 0);
		Script = ReadWord(Runtime, Slot + 0x130 + 4);
		Function = ReadWord(Runtime, Slot + 0x13C);
	}
	if (Function != 0)
	{
		OKTransferMainThread(Runtime, Follower, Script, Function);
	}
}

bool OKTransferMainThreadTo(FOKScriptRuntime& Runtime, FOKFighter& Fighter, const FString& ScriptName, const FString& FunctionName)
{
	FOKScriptInstance* Target = Runtime.FindInstanceByName(ScriptName);
	const int32 Function = Target ? Target->File->FindFunctionByName(FunctionName) : INDEX_NONE;
	if (Function == INDEX_NONE)
	{
		UE_LOG(LogOKVM, Warning, TEXT("%s: no %s.%s to transfer to"), *Fighter.Name, *ScriptName, *FunctionName);
		return false;
	}
	return XferToReaction(Runtime, Fighter, *Target, Function);
}

void OKFireFrameEvents(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float PreviousFrame, float Frame, float Rate, bool bLooping)
{
	if (!Fighter.Body)
	{
		return;
	}
	auto Word = [&](uint32 Offset)
		{
			uint32 Value = 0;
			if (const uint8* Where = Runtime.HeapBytes((Fighter.Handle & 0x00FFFFFF) + Offset, 4))
			{
				FMemory::Memcpy(&Value, Where, 4);
			}
			return Value;
		};
	auto Float = [&](uint32 Offset) { const uint32 Bits = Word(Offset); float Value; FMemory::Memcpy(&Value, &Bits, 4); return Value; };
	auto SetWord = [&](uint32 Offset, uint32 Value) { Runtime.SetHeapWord(Fighter.Handle + Offset, Value); };

	const uint32 AnimId = Fighter.Body->GetAnimId();
	const int32 Count = FMath::Min(int32(Word(FOKFighter::FrameEventCount)), FOKFighter::MaxFrameEvents);
	for (int32 i = 0; i < Count; ++i)
	{
		const uint32 Base = FOKFighter::FrameEvents + uint32(i) * FOKFighter::FrameEventBytes;
		if (Word(Base + 4) != 0)
		{
			continue;		// not armed
		}
		if (Word(Base + 0x78) != AnimId)
		{
			SetWord(Base + 4, 2);		// armed during another animation: dropped
			continue;
		}
		// A one-shot (to = -1) fires when the animation crosses its frame; a range fires on every frame inside it.
		const uint32 Kind = Word(Base);
		const float From = Float(Base + 0x08);
		const float To = Float(Base + 0x0C);
		const bool bCrossed = Rate >= 0.0f ? (PreviousFrame < From && From <= Frame) : (From < PreviousFrame && Frame <= From);
		const bool bOneShot = To == -1.0f;
		const bool bFire = bOneShot ? (bCrossed || (Kind == 0 && From <= PreviousFrame)) : ((From <= Frame && Frame <= To) || bCrossed);
		if (!bFire)
		{
			continue;
		}

		// The event's process gets an MKAnimEventPdata with the slot's parameters (FUN_0044DCE0 -> FUN_004926C0).
		const uint32 ScriptValue = Word(Base + 0x14);
		const FOKScriptInstance* Instance = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		const int32 Function = int32(Word(Base + 0x10)) - 1;
		const uint32 Pdata = Instance ? Runtime.CreateOwned(MakeUnique<FOKAnimEventPdata>(), FOKAnimEventPdata::Bytes) : 0;
		if (Pdata != 0)
		{
			Runtime.SetHeapWord(Pdata + 4, Pdata);
			Runtime.SetHeapWord(Pdata + 0x08, Word(Base + 0x20));
			Runtime.SetHeapWord(Pdata + 0x0C, Word(Base + 0x24));
			Runtime.SetHeapWord(Pdata + 0x10, Fighter.Handle + Base + 0x28);
			if (Kind == 0)
			{
				Runtime.SetHeapWord(Pdata + 0x18, Word(Base + 0x80));
				Runtime.SetHeapWord(Pdata + 0x1C, Word(Base + 0x84));
			}
			FString Error;
			if (FOKScriptThread* Process = Runtime.StartProcess(*Instance->File, Function, 0, 14, true, Error))
			{
				Process->AttachObject(Pdata);
				Process->BindFighter(Fighter);		// the fighter adopts its events' processes (FUN_004926C0)
			}
			else
			{
				UE_LOG(LogOKVM, Warning, TEXT("frame event %d: %s"), i, *Error);
			}
		}
		if (bOneShot && !bLooping)
		{
			SetWord(Base + 4, 2);
		}
	}
}

/**
 * FightPlayers::Update (FUN_008ACD90), each frame before the scripts tick: for each player whose fighter is
 * active and human, the player update (virtual +0xC: input off unless alive), then the command records, and if
 * none matched, the plain buttons. [inf] The rotating start (frame & 3) and the AI path (character info +0x44)
 * are left out: one human per side.
 */
void OKUpdateFightPlayers(FOKScriptRuntime& Runtime, TConstArrayView<FOKFighter*> Fighters)
{
	for (FOKFighter* Fighter : Fighters)
	{
		FOKPlayer* Player = Fighter ? Fighter->Player : nullptr;
		if (!Player)
		{
			continue;
		}
		const uint32 Flags = ReadWord(Runtime, Fighter->Handle + FOKFighter::Flags);
		uint32 Buffer = 0;
		Runtime.LoadHeapWord(Player->Handle + FOKPlayer::InputBuffer, Buffer);
		WriteWord(Runtime, Player->Handle + FOKPlayer::InputOff, (Buffer != 0 && Fighter->Health > 0.0f) ? 0 : 1);
		// FUN_00448A00: frames spent in state bit 0x800, wound down afterwards.
		const uint32 Held = ReadWord(Runtime, Fighter->Handle + 0x2FFC);
		WriteWord(Runtime, Fighter->Handle + 0x2FFC, (ReadWord(Runtime, Fighter->Handle + FOKFighter::State) & 0x800) ? Held + 1 : (Held > 0x14 ? Held - 5 : 0));
		// FUN_008959D0: a fighter is free on the floor, out of state 0x4605, with +0x2F5C clear and past +0x2F64; on the
		// floor means no airborne flag and, unless it's still, within 160 of the floor (virtual +0x1FC). Combos go on
		// while the victim isn't free (+0x2FCC).
		if ((Flags & FOKFighter::Active) != 0)
		{
			const bool bStill = BitsToFloat(ReadWord(Runtime, Fighter->Handle + 0x74)) == 0.0f || BitsToFloat(ReadWord(Runtime, Fighter->Handle + 0x9C)) == 0.0f;
			const bool bOnFloor = (Flags & OKMotion::AirFlag) == 0 && ReadWord(Runtime, Fighter->Handle + FOKFighter::State) != 0x4605
				&& bStill && (!Fighter->Body || Fighter->Body->GetHeight() <= 160.0f);
			const bool bFree = ReadWord(Runtime, Fighter->Handle + 0x2F5C) == 0 && ReadWord(Runtime, Fighter->Handle + 0x2F64) <= uint32(Runtime.GetSimFrame()) && bOnFloor;
			if (CVarTraceSpecials.GetValueOnAnyThread() >= 3 && Runtime.GetSimFrame() % 10 == 0)
			{
				UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s free %d: +0x2F5C %u, +0x2F64 %u, air %d, state %04X, speeds %g %g, height %g"), Runtime.GetSimFrame(), *Fighter->Name, bFree,
					ReadWord(Runtime, Fighter->Handle + 0x2F5C), ReadWord(Runtime, Fighter->Handle + 0x2F64), (Flags & OKMotion::AirFlag) != 0, ReadWord(Runtime, Fighter->Handle + FOKFighter::State),
					BitsToFloat(ReadWord(Runtime, Fighter->Handle + 0x74)), BitsToFloat(ReadWord(Runtime, Fighter->Handle + 0x9C)), Fighter->Body ? Fighter->Body->GetHeight() : -1.0f);
			}
			WriteWord(Runtime, Fighter->Handle + FOKFighter::NotFree, bFree ? 0 : 1);
		}
		if ((Flags & FOKFighter::Active) == 0 || Buffer == 0 || (Flags & 0x80) != 0)
		{
			continue;
		}
		if (!CheckCommands(Runtime, *Fighter, *Player))
		{
			PlainButtons(Runtime, *Fighter, *Player);
		}
	}
}

/**
 * The load pass's attack names (FUN_00449630 -> FUN_00449990): each attack<steps> function goes into the move list
 * of its first button, with up to 5 steps (button << 16 | direction).
 */
static bool ParseAttackName(const FString& Name, uint32 (&OutSteps)[5])
{
	for (uint32& Step : OutSteps)
	{
		Step = 0xFFFFFFFFu;
	}
	const FString Text = Name.Mid(6);
	int32 At = 0, Count = 0;
	while (At < Text.Len())
	{
		if (!FChar::IsDigit(Text[At]))
		{
			return false;
		}
		uint32 Button = 0;
		const TCHAR First = Text[At];
		if (At + 2 < Text.Len() && Text[At + 1] == TEXT('n'))
		{
			const TCHAR Second = Text[At + 2];
			if (First == '1' && Second == '2') { Button = 0x80000014; }
			else if (First == '1' && Second == '3') { Button = 0x80000015; }
			else if (First == '1' && Second == '4') { Button = 0x80000016; }
			else if (First == '2' && Second == '3') { Button = 0x80000017; }
			else if (First == '2' && Second == '4') { Button = 0x80000018; }
			else if (First == '3' && Second == '4') { Button = 0x80000019; }
			At += 3;
		}
		if (Button == 0)
		{
			if (First < '1' || First > '5')
			{
				return false;
			}
			Button = 0x8000000Eu + uint32(First - '0');		// 1 = FP 0x8000000F ... 4 = BK 0x80000012
			At += 1;
		}
		uint32 Direction = 0;
		if (At < Text.Len() && Text[At] != '_')
		{
			int32 End = At;
			while (End < Text.Len() && Text[End] != '_')
			{
				++End;
			}
			const FString DirWord = Text.Mid(At, End - At);
			static const TPair<const TCHAR*, uint32> Words[] = {
				{ TEXT("upturnaround"), 0x8000003B }, { TEXT("awayturnaround"), 0x8000003A }, { TEXT("towardsturnaround"), 0x8000003C },
				{ TEXT("towards"), 0x80000001 }, { TEXT("away"), 0x80000002 }, { TEXT("up"), 0x80000003 }, { TEXT("downturnaround"), 0x80000029 },
				{ TEXT("down"), 0x80000004 }, { TEXT("jumpforward"), 0x80000024 }, { TEXT("jumpbackward"), 0x80000025 }, { TEXT("jumpup"), 0x80000026 },
				{ TEXT("turnaround"), 0x80000023 }, { TEXT("wakeupfront"), 0x80000027 }, { TEXT("wakeupback"), 0x80000028 }, { TEXT("hopup"), 0x8000002A } };
			for (const TPair<const TCHAR*, uint32>& W : Words)
			{
				if (DirWord.StartsWith(W.Key, ESearchCase::CaseSensitive))
				{
					Direction = W.Value;
					break;
				}
			}
			if (Direction == 0)
			{
				return false;
			}
			At = End;
		}
		if (Count >= 5)
		{
			return false;
		}
		OutSteps[Count++] = (Button << 16) | (Direction & 0xFFFF);
		++At;		// the '_'
	}
	return true;
}

static void BuildMoveLists(FOKScriptRuntime& Runtime, uint32 Moveset, const FOKMkoFile& Script)
{
	const TArray<FOKMkoFunction>& Functions = Script.GetFunctions();
	for (int32 i = 0; i < Functions.Num(); ++i)
	{
		uint32 Steps[5];
		if (!Functions[i].Name.StartsWith(TEXT("attack"), ESearchCase::CaseSensitive) || !ParseAttackName(Functions[i].Name, Steps))
		{
			continue;
		}
		const uint32 First = (Steps[0] >> 16) | 0x80000000u;
		const uint32 List = First - 0x8000000Fu < 4 ? First - 0x8000000Fu : 4;
		const uint32 Count = ReadWord(Runtime, Moveset + FOKMoveset::MoveListCounts + List * 4);
		if (Count >= uint32(FOKMoveset::MaxMoves))
		{
			continue;
		}
		const uint32 Move = Moveset + FOKMoveset::MoveLists + List * FOKMoveset::MoveListBytes + Count * FOKMoveset::MoveBytes;
		WriteWord(Runtime, Move, 0);
		WriteWord(Runtime, Move + 4, uint32(i + 1));
		for (int32 u = 0; u < 5; ++u)
		{
			WriteWord(Runtime, Move + 8 + u * 4, Steps[u]);
		}
		WriteWord(Runtime, Moveset + FOKMoveset::MoveListCounts + List * 4, Count + 1);
	}
}

/**
 * Starts a fighter's character script the way MKKE does when it spawns one: builds the moveset from every
 * moveset_<char> function (FUN_0044F850 -> FUN_008ECAB0), then runs <char>_game_init(fighter), which binds the
 * input scanners and the command records. [inf] MKKE's call to *_game_init isn't located yet: its name is in no
 * table the exe, the packages or the config hold, so OpenKombat finds it by its suffix.
 */
bool OKInitCharacter(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FString& OutError)
{
	if (!Fighter.CharacterScript)
	{
		OutError = TEXT("no character script");
		return false;
	}
	const FOKMkoFile& Script = *Fighter.CharacterScript;
	FOKScriptInstance* Instance = Runtime.GetInstance(Script);
	if (!Instance)
	{
		OutError = TEXT("too many script instances");
		return false;
	}
	uint32 Result = 0;
	const TArray<FOKMkoFunction>& Functions = Script.GetFunctions();
	// The fighter's reset (virtual +0xE0: MKFGCharacterObj FUN_0089B6A0, MKCharacterObj FUN_004567B0) leaves these
	// fields non-zero. +0xAA94 is the walk and step speed-up (137.146) that blend_to_moveset_anim_frame multiplies
	// their rate by. [inf] The others aren't named yet; the reset also releases handles and frame events, which a new
	// fighter doesn't have.
	const uint32 One = FloatToBits(1.0f);
	const uint32 Defaults[][2] = {
		{ 0x514, 1 }, { 0x218, 1 }, { 0x224, 1 }, { 0x220, 3 }, { 0x2F7C, 0xFFFFFFFFu }, { 0x114, One }, { 0x464, 4 },
		{ 0x7708, One }, { 0x770C, One }, { FOKFighter::MoveList, 5 }, { FOKFighter::Life, One }, { 0x78C4, One },
		{ 0xAA94, One }, { 0x77D0, One }, { 0xA974, 0xFFFFFFFFu }, { 0xAAAC, 0xFFFFFFFFu }, { 0x7F44, 0x80000000u },
	};
	for (const auto& Entry : Defaults)
	{
		Runtime.SetHeapWord(Fighter.Handle + Entry[0], Entry[1]);
	}
	uint32 CharFlags = 0;
	Runtime.LoadHeapWord(Fighter.Handle + FOKFighter::CharFlags, CharFlags);
	Runtime.SetHeapWord(Fighter.Handle + FOKFighter::CharFlags, CharFlags | 0x4000);		// constrain_enable_impl(0x4000)
	// FUN_004495C0 resets the moveset: counts and rows' animations 0, +0xC = -1.
	const uint32 Moveset = Fighter.Handle + FOKFighter::Moveset;
	for (uint32 Offset : { FOKMoveset::AltStanceCount, FOKMoveset::SecondaryFlag, FOKMoveset::MoveListCounts, FOKMoveset::MoveListCounts + 4,
		FOKMoveset::MoveListCounts + 8, FOKMoveset::MoveListCounts + 12, FOKMoveset::MoveListCounts + 16 })
	{
		Runtime.SetHeapWord(Moveset + Offset, 0);
	}
	for (int32 Row = 0; Row < FOKMoveset::MaxRows; ++Row)
	{
		Runtime.SetHeapWord(Moveset + FOKMoveset::Rows + Row * FOKMoveset::RowBytes, 0);
	}
	Runtime.SetHeapWord(Moveset + 0xC, 0xFFFFFFFFu);
	GBuildingMoveset = Moveset;
	for (int32 i = 0; i < Functions.Num(); ++i)
	{
		if (Functions[i].Name.StartsWith(TEXT("moveset_")))
		{
			FOKScriptThread Loader(Runtime);
			if (!Loader.Run(Script, i, {}, Result, OutError))
			{
				GBuildingMoveset = 0;
				OutError = Functions[i].Name + TEXT(": ") + OutError;
				return false;
			}
		}
	}
	GBuildingMoveset = 0;
	const uint32 CharacterScriptValue = (FOKScriptRuntime::InstanceTag << 24) | uint32(Instance->Id);
	Runtime.SetHeapWord(Moveset + FOKMoveset::Script, CharacterScriptValue);
	Runtime.SetHeapWord(Moveset + FOKMoveset::Script + 4, CharacterScriptValue);
	BuildMoveLists(Runtime, Moveset, Script);
	// [inf] Button slot +0x30BC must be non-zero for a button to act; nothing found sets it (78.13 clears it).
	for (int32 Slot = 0; Slot < FOKFighter::MaxButtonSlots; ++Slot)
	{
		Runtime.SetHeapWord(Fighter.Handle + FOKFighter::ButtonSlots + Slot * FOKFighter::ButtonSlotBytes + FOKButtonSlot::Allowed, 1);
	}
	// FUN_008983C0: the fighter becomes its player's active character.
	uint32 FighterFlags = 0;
	Runtime.LoadHeapWord(Fighter.Handle + FOKFighter::Flags, FighterFlags);
	Runtime.SetHeapWord(Fighter.Handle + FOKFighter::Flags, FighterFlags | FOKFighter::Active);
	// The character info is the script's global <script>_player_data (FUN_008A5270: FindGlobal on the character's script),
	// so +0x2A0 and +0x7620 point into the script's own memory and *_game_init's writes land in it. A script without one
	// gets a zeroed stand-in.
	uint32 Info = 0;
	const uint32 InfoHash = FOKMkoFile::Fnv1((Script.GetName().Replace(TEXT(".mko"), TEXT("")) + TEXT("_player_data")).ToLower());	// names hash in lower case
	for (const FOKMkoGlobal& Global : Script.GetGlobals())
	{
		if (Global.NameHash == InfoHash)
		{
			Info = FOKScriptRuntime::MemoryAddress(Instance->Id, Instance->PoolBase + Global.PoolOffset);
			UE_LOG(LogOKVM, Log, TEXT("%s: character info is its %s_player_data (%u bytes)"), *Fighter.Name, *Script.GetName().Replace(TEXT(".mko"), TEXT("")), Global.Size);
		}
	}
	if (Info == 0)
	{
		Info = Runtime.CreateOwned(MakeUnique<FOKCharacterInfo>(), FOKCharacterInfo::Bytes);
		UE_LOG(LogOKVM, Warning, TEXT("%s: no _player_data global; its character info is all zeros"), *Fighter.Name);
	}
	Runtime.SetHeapWord(Fighter.Handle + FOKFighter::CharacterInfo, Info);
	Runtime.SetHeapWord(Fighter.Handle + 0x7620, Info);
	// The handle {?, object +0x2A8, serial +0x2AC} at +0x2A4; our script instances use their value as the serial.
	Runtime.SetHeapWord(Fighter.Handle + FOKFighter::CharacterScriptHandle, (FOKScriptRuntime::InstanceTag << 24) | uint32(Instance->Id));
	Runtime.SetHeapWord(Fighter.Handle + FOKFighter::CharacterScriptHandle + 4, (FOKScriptRuntime::InstanceTag << 24) | uint32(Instance->Id));
	// FUN_00898130: FGFightEngineScript.p_player_created binds the buttons (register_moveset_commands_for_character),
	// then the character script's function named in the character info at +0x48, *_game_init.
	{
		const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
		const FOKMkoFile* Fight = (Scripts && Scripts->GetLibrary()) ? Scripts->GetLibrary()->FindScript(TEXT("FGFightEngineScript.mko")) : nullptr;
		const int32 Created = Fight ? Fight->FindFunctionByName(TEXT("p_player_created")) : INDEX_NONE;
		if (Created != INDEX_NONE)
		{
			FOKScriptThread Thread(Runtime);
			Thread.BindFighter(Fighter);
			if (!Thread.Run(*Fight, Created, {}, Result, OutError))
			{
				OutError = TEXT("p_player_created: ") + OutError;
				return false;
			}
		}
	}
	for (int32 i = 0; i < Functions.Num(); ++i)
	{
		// [inf] The one the character info names takes the fighter; tower variants (Kung Lao's show_hat_endurance_game_init)
		// take nothing and are skipped.
		if (Functions[i].Name.EndsWith(TEXT("_game_init")) && Functions[i].MaxArgs >= 1)
		{
			FOKScriptThread Init(Runtime);
			Init.BindFighter(Fighter);
			const uint32 Args[1] = { Fighter.Handle };
			if (!Init.Run(Script, i, Args, Result, OutError))
			{
				OutError = Functions[i].Name + TEXT(": ") + OutError;
				return false;
			}
			UE_LOG(LogOKVM, Log, TEXT("%s: %s ran (%d instructions)"), *Fighter.Name, *Functions[i].Name, Init.GetInstructionsRun());
		}
	}
	return true;
}

// --- Rounds and the match (system F11, FIGHT-RE-NOTES §8b) ---------------------------------------

namespace
{
	/** An argument's value, with locals loaded (ArgWord / ArgFloat take operands 0-2 as written). */
	uint32 ArgInt(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Value = 0;
		Thread.ArgValue(Index, Value);
		return Value;
	}

	float ArgReal(FOKScriptThread& Thread, int32 Index)
	{
		return BitsToFloat(ArgInt(Thread, Index));
	}

	FOKGameInfo* ArgGameInfo(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Handle = 0;
		FOKScriptObject* Object = Thread.ArgValue(Index, Handle) ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		return Object ? Object->AsGameInfo() : nullptr;
	}

	/** Rounds to Win and Round Time, from the game-settings record (FUN_0080EC10 / FUN_0080EB90, outside story, tower and online). */
	uint32 RoundsToWin(FOKScriptRuntime& Runtime, const FOKGameInfo& Game)
	{
		return ReadWord(Runtime, Game.Handle + FOKGameInfo::Settings + 8);
	}

	uint32 RoundTime(FOKScriptRuntime& Runtime, const FOKGameInfo& Game)
	{
		return ReadWord(Runtime, Game.Handle + FOKGameInfo::Settings + 4);
	}

	void SetLife(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Life)
	{
		Fighter.Health = Life;
		WriteWord(Runtime, Fighter.Handle + FOKFighter::Life, FloatToBits(Life));
		WriteWord(Runtime, Fighter.Handle + FOKFighter::Life + 4, FloatToBits(Life));
	}

	/** Runs the character script function the character info holds at Offset on the fighter (FUN_0044DCE0), if it has one. */
	void RunCharacterInfoFunction(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 Offset)
	{
		const uint32 Function = ReadWord(Runtime, ReadWord(Runtime, Fighter.Handle + FOKFighter::CharacterInfo) + Offset);
		const uint32 ScriptValue = ReadWord(Runtime, Fighter.Handle + FOKFighter::CharacterScriptHandle + 4);
		const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		FOKScriptInstance* Target = nullptr;
		int32 Index = INDEX_NONE;
		if (Function == 0 || !From || !Runtime.ResolveFunction(*From, Function, Target, Index))
		{
			return;
		}
		FOKScriptThread Call(Runtime);
		Call.BindFighter(Fighter);
		uint32 Result = 0;
		FString Error;
		if (!Call.Run(*Target->File, Index, {}, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: %s"), *Target->File->GetFunctions()[Index].Name, *Error);
		}
	}

	/**
	 * A fighter's round reset (FUN_0089A4C0): full life, the "match point" flag (fighter +0x2E8 bit 0x80000000, when its
	 * player has won all but one of the rounds it needs), some flags and timers cleared, then the round-start functions
	 * the character info names at +0x4C and +0x6C.
	 */
	void ResetFighterForRound(FOKScriptRuntime& Runtime, FOKFighter& Fighter, const FOKPlayer& Player, uint32 ToWin)
	{
		WriteWord(Runtime, Fighter.Handle + 0x2FDC, 0);
		WriteWord(Runtime, Fighter.Handle + 0x3034, 0);
		SetLife(Runtime, Fighter, 1.0f);		// DAT_00C5A698
		uint32 Flags = ReadWord(Runtime, Fighter.Handle + FOKFighter::Flags);
		if (ReadWord(Runtime, Player.Handle + FOKPlayer::RoundsWon) + 1 < ToWin)
		{
			Flags &= 0x7FFFFFFFu;
		}
		else
		{
			Flags |= 0x80000000u;
		}
		WriteWord(Runtime, Fighter.Handle + FOKFighter::Flags, Flags & 0xEEBDFFDFu);
		WriteWord(Runtime, Fighter.Handle + 0x2EC, ReadWord(Runtime, Fighter.Handle + 0x2EC) & 0xFFF9FFFFu);
		WriteWord(Runtime, Fighter.Handle + 0xAA98, 0);
		RunCharacterInfoFunction(Runtime, Fighter, 0x4C);
		RunCharacterInfoFunction(Runtime, Fighter, 0x6C);
		WriteWord(Runtime, Fighter.Handle + 0x788C, 0);
		WriteWord(Runtime, Fighter.Handle + 0x78B4, 0);
	}

	// 136.16 reset_round_clock(gameinfo) (FUN_0080FF30): the clock gets the round time (no time: the clock-stopped flag
	// 0x8000), flags 0x20 and 0x20000000 and the round winner +0x80F4 are cleared, and each player record resets (virtual +4, FUN_008A5840): its fighters,
	// and +0xC5C. [inf] The meter carries over outside the tower; character info flag 0x1000 forces it to 0.
	bool ResetRoundClock(FOKScriptThread& Thread)
	{
		FOKGameInfo* Game = ArgGameInfo(Thread, 0);
		if (!Game)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		uint32 Flags = ReadWord(Runtime, Game->Handle + FOKGameInfo::Flags) & ~0x20000020u;		// the win text and round over
		const uint32 Time = RoundTime(Runtime, *Game);
		WriteWord(Runtime, Game->Handle + FOKGameInfo::Clock, Time);
		if (Time == 0)
		{
			Flags |= 0x8000;
		}
		WriteWord(Runtime, Game->Handle + FOKGameInfo::Flags, Flags);
		WriteWord(Runtime, Game->Handle + FOKGameInfo::Winner, 0);
		const uint32 ToWin = RoundsToWin(Runtime, *Game);
		for (FOKPlayer* Player : Game->Players)
		{
			if (!Player || !Player->Fighter)
			{
				continue;
			}
			ResetFighterForRound(Runtime, *Player->Fighter, *Player, ToWin);
			WriteWord(Runtime, Player->Handle + 0xC5C, 0);
			if ((ReadWord(Runtime, ReadWord(Runtime, Player->Fighter->Handle + FOKFighter::CharacterInfo) + 0xA0) & 0x1000) != 0)
			{
				WriteWord(Runtime, Player->Handle + FOKPlayer::Meter, 0);
			}
		}
		UE_LOG(LogOKVM, Log, TEXT("Round %u: %u seconds, first to %u; rounds won %u - %u"), ReadWord(Runtime, Game->Handle + FOKGameInfo::Round), Time, ToWin,
			Game->Players[0] ? ReadWord(Runtime, Game->Players[0]->Handle + FOKPlayer::RoundsWon) : 0, Game->Players[1] ? ReadWord(Runtime, Game->Players[1]->Handle + FOKPlayer::RoundsWon) : 0);
		return true;
	}

	// 36.5 (settings) -> Rounds to Win; 36.3 (settings) -> Round Time (settings +8 / +4)
	bool SettingsRoundsToWin(FOKScriptThread& Thread)
	{
		Thread.SetResult(ReadWord(Thread.GetRuntime(), ArgInt(Thread, 0) + 8));
		return true;
	}

	bool SettingsRoundTime(FOKScriptThread& Thread)
	{
		Thread.SetResult(ReadWord(Thread.GetRuntime(), ArgInt(Thread, 0) + 4));
		return true;
	}

	// 43.6 (gameinfo) -> the game mode (FUN_00406850: the game info +8, whatever the argument)
	bool GameMode(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::GameMode));
		return true;
	}

	// 43.7 (gameinfo, mask) -> whether any of the bits are set in the game flags (+0x10, FUN_00406A70)
	bool GameFlagTest(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult((ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::Flags) & ArgInt(Thread, 1)) != 0 ? 1 : 0);
		return true;
	}

	// --- Vectors and the fight line, for FGCameraEngine.camera_lock_player_position --------------------------------

	bool StoreVector(FOKScriptThread& Thread, uint32 Address, const FVector& Value)
	{
		const float Parts[3] = { float(Value.X), float(Value.Y), float(Value.Z) };
		return Address == 0 || Thread.StoreBytes(Address, Parts, sizeof(Parts));
	}

	// 78.79 (player, &vector) (FUN_0044D4D0): its active fighter's position; 0 without one
	bool PlayerPosition(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		const bool bFound = Player && Player->Fighter && Player->Fighter->Body;
		Thread.SetResult(bFound ? 1 : 0);
		return !bFound || StoreVector(Thread, ArgAddress(Thread, 1), Player->Fighter->Body->GetWorldLocation());
	}

	// 78.80 (player, &rotator) (FUN_0044D510): its active fighter's rotation (pitch, yaw, roll; 65536 a turn)
	bool PlayerRotation(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		const bool bFound = Player && Player->Fighter && Player->Fighter->Body;
		Thread.SetResult(bFound ? 1 : 0);
		if (!bFound || ArgAddress(Thread, 1) == 0)
		{
			return true;
		}
		// [inf] Fighters stand upright, so only the yaw of their facing counts.
		const int32 Parts[3] = { 0, FRotator::CompressAxisToShort(Player->Fighter->Body->GetFacing().Rotation().Yaw), 0 };
		return Thread.StoreBytes(ArgAddress(Thread, 1), Parts, sizeof(Parts));
	}

	// 6.88 (named bone_rotation) (&a, &b) -> a . b (FUN_00491750)
	bool VecDot(FOKScriptThread& Thread)
	{
		FVector A, B;
		Thread.SetResult(FloatToBits(ArgVector(Thread, 0, A) && ArgVector(Thread, 1, B) ? float(FVector::DotProduct(A, B)) : 0.0f));
		return true;
	}

	// 6.91 (named socket_location) (&out, &a, &b): out = a - b (FUN_004917F0)
	bool VecSubtract(FOKScriptThread& Thread)
	{
		FVector A, B;
		return !(ArgVector(Thread, 1, A) && ArgVector(Thread, 2, B)) || StoreVector(Thread, ArgAddress(Thread, 0), A - B);
	}

	// 6.92 (named bone_dir) (&out, &in): out = in at length 1, or 0 when it's too short (FUN_00493940)
	bool VecNormal(FOKScriptThread& Thread)
	{
		FVector In;
		return !ArgVector(Thread, 1, In) || StoreVector(Thread, ArgAddress(Thread, 0), In.GetSafeNormal());
	}

	// 7.631 (&out) (FUN_008B3FD0): the fight line's direction, flat (DAT_00DF0C14)
	bool LineDirectionNative(FOKScriptThread& Thread)
	{
		FOKFighter* Mine = Thread.MyFighter();
		return StoreVector(Thread, ArgAddress(Thread, 0), Mine ? Mine->LineDirection.GetSafeNormal2D() : FVector::YAxisVector);
	}

	// 7.488 (&position) (FUN_008B4B70) -> 1 when the position is between the arena's ends along the fight line. With no
	// arena ends anything is.
	bool OnFightLine(FOKScriptThread& Thread)
	{
		FOKFighter* Mine = Thread.MyFighter();
		FVector Where;
		bool bInside = Mine && ArgVector(Thread, 0, Where);
		if (bInside && Mine->LineLength > 0.0)
		{
			const double Along = FVector::DotProduct(Where - Mine->LineStart, Mine->LineDirection.GetSafeNormal2D());
			bInside = Along >= 0.0 && Along <= Mine->LineLength;
		}
		Thread.SetResult(bInside ? 1 : 0);
		return true;
	}

	// 7.53 (FUN_008B3EE0) -> the furthest apart the fighters can be (DAT_00DF0BD8)
	bool MaxSeparationNative(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(float(OKMotion::MaxSeparation)));
		return true;
	}

	// 27.3 (named vec_edge) (&v) -> v . v, its squared length (as a float)
	bool VecLengthSquared(FOKScriptThread& Thread)
	{
		FVector V;
		Thread.SetResult(FloatToBits(ArgVector(Thread, 0, V) ? float(V.SizeSquared()) : 0.0f));
		return true;
	}

	// 6.46 (named actor_location) (&vector, &rotator) (FUN_00491A00): the vector's direction as a UE3 rotator
	bool VecToRotator(FOKScriptThread& Thread)
	{
		FVector V;
		const uint32 Out = ArgAddress(Thread, 1);
		if (!ArgVector(Thread, 0, V) || Out == 0)
		{
			return true;
		}
		const FRotator Rotation = V.Rotation();
		const int32 Parts[3] = { FRotator::CompressAxisToShort(Rotation.Pitch), FRotator::CompressAxisToShort(Rotation.Yaw), FRotator::CompressAxisToShort(Rotation.Roll) };
		return Thread.StoreBytes(Out, Parts, sizeof(Parts));
	}

	// 59.111 obj_rotation(obj, &vector) (FUN_00448020): the way the object faces, as a vector of length 1
	bool FacingVector(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !(Fighter && Fighter->Body) || StoreVector(Thread, ArgAddress(Thread, 1), Fighter->Body->GetFacing().GetSafeNormal());
	}

	// (&out): out = (0, 0, 0)
	bool VecZero3(FOKScriptThread& Thread)
	{
		return StoreVector(Thread, ArgAddress(Thread, 0), FVector::ZeroVector);
	}

	// --- More vector helpers, and MK object fields, for the roster's X-rays ---------------------------------------

	FString CStringAt(FOKScriptThread& Thread, uint32 Address);

	// 6.80 (named vec_size) (x) -> its square root
	bool FloatSqrt(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(FMath::Sqrt(FMath::Max(0.0f, ArgFloat(Thread, 0)))));
		return true;
	}

	// 6.87 (named bone_location) (&v) -> its length (FUN_00491730)
	bool VecLength(FOKScriptThread& Thread)
	{
		FVector V;
		Thread.SetResult(FloatToBits(ArgVector(Thread, 0, V) ? float(V.Size()) : 0.0f));
		return true;
	}

	// 6.89 (named bone_pos_3) (&out, &v, scale): out = v x scale (FUN_00491770)
	bool VecScale(FOKScriptThread& Thread)
	{
		FVector V;
		return !ArgVector(Thread, 1, V) || StoreVector(Thread, ArgAddress(Thread, 0), V * ArgFloat(Thread, 2));
	}

	// 6.90 (named bone_pos_3b) (&out, &a, &b): out = a + b (FUN_004917B0)
	bool VecAdd(FOKScriptThread& Thread)
	{
		FVector A, B;
		return !(ArgVector(Thread, 1, A) && ArgVector(Thread, 2, B)) || StoreVector(Thread, ArgAddress(Thread, 0), A + B);
	}

	// 27.7 (named vec_set) (&v, x, y, z)
	bool VecSet(FOKScriptThread& Thread)
	{
		return StoreVector(Thread, ArgAddress(Thread, 0), FVector(ArgFloat(Thread, 1), ArgFloat(Thread, 2), ArgFloat(Thread, 3)));
	}

	// 27.1 (named vec_from_hash) (&v, smallest) -> 0 when v . v is under smallest; else v is made length 1 and 1
	bool VecNormalizeAbove(FOKScriptThread& Thread)
	{
		FVector V;
		if (!ArgVector(Thread, 0, V))
		{
			return true;
		}
		const double Squared = V.SizeSquared();
		if (Squared != 1.0 && Squared < ArgFloat(Thread, 1))
		{
			Thread.SetResult(0);
			return true;
		}
		Thread.SetResult(1);
		return Squared == 1.0 || StoreVector(Thread, ArgAddress(Thread, 0), V / FMath::Sqrt(Squared));
	}

	// 7.440 (named ai_tick) -> the two fighters' squared distance, flat (FUN_00889C30)
	bool FightersApartSquared(FOKScriptThread& Thread)
	{
		FOKFighter* Mine = Thread.MyFighter();
		FOKFighter* His = Mine ? Mine->Opponent : nullptr;
		const float Squared = (Mine && Mine->Body && His && His->Body)
			? float(FVector::DistSquared2D(Mine->Body->GetWorldLocation(), His->Body->GetWorldLocation())) : 0.0f;
		Thread.SetResult(FloatToBits(Squared));
		return true;
	}

	// 22.4 (process) (FUN_00458360): the process ends
	bool KillProcessNative(FOKScriptThread& Thread)
	{
		uint32 Handle = 0;
		Thread.ArgValue(0, Handle);
		FOKScriptObject* Object = Handle ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		if (FOKScriptThread* Target = Object ? Object->AsThread() : nullptr)
		{
			Target->KillNow(TEXT("killed (22.4)"));
		}
		return true;
	}

	// 118.10 (named xray_check) (player, window) (FUN_00897A20): MK9's old X-ray input, FK + BK + block: each pair of
	// the three pressed together (FUN_00446500 -> FUN_00425CA0). [inf] The window isn't applied.
	bool OldXrayChord(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		uint32 Buffer = 0;
		if (Player)
		{
			Runtime.LoadHeapWord(Player->Handle + FOKPlayer::InputBuffer, Buffer);
		}
		const auto Pair = [&](uint32 A, uint32 B)
		{
			return OKInput::NextChord(Runtime, Buffer, OKInput::CodeToMask(A), OKInput::CodeToMask(B), 0xFFFFFFFFu, false, nullptr);
		};
		Thread.SetResult(Player && Pair(0x80000011, 0x80000012) && Pair(0x80000011, 0x8000001C) && Pair(0x8000001C, 0x80000012) ? 1 : 0);
		return true;
	}

	/** An object's handle from an argument, read through its mode; 0 if it isn't an object. */
	uint32 ArgObjectHandle(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Handle = 0;
		Thread.ArgValue(Index, Handle);
		return (Handle != 0 && Thread.GetRuntime().ResolveHandle(Handle)) ? Handle : 0;
	}

	/** A native that keeps its arguments in the object's fields: (obj, a, b, ...) -> obj + Offsets[i] = argument i + 1. */
	template <uint32... Offsets>
	bool StoreObjectFields(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle == 0)
		{
			return true;
		}
		int32 Index = 1;
		for (const uint32 Offset : { Offsets... })
		{
			uint32 Value = 0;
			Thread.ArgValue(Index++, Value);
			WriteWord(Thread.GetRuntime(), Handle + Offset, Value);
		}
		return true;
	}

	// 137.256 (obj) (FUN_00896A80): the pending attack's +0x28 (fighter +0x7B34) = 1
	bool SetPendingFlag28(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle)
		{
			WriteWord(Thread.GetRuntime(), Handle + 0x7B34, 1);
		}
		return true;
	}

	// 137.258 (obj, x) (FUN_00896A20): the pending attack's +0x20 (fighter +0x7B2C) = 1 and +0x164 (+0x7C70) = x
	bool SetPendingFlag20(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle)
		{
			WriteWord(Thread.GetRuntime(), Handle + 0x7B2C, 1);
			WriteWord(Thread.GetRuntime(), Handle + 0x7C70, ArgWord(Thread, 1));
		}
		return true;
	}

	// 121.22 col_capsule(obj, radius, length) (FUN_00895CE0): the collision object becomes a capsule (shape 1)
	bool SetCapsule(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle)
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			WriteWord(Runtime, Handle + OKCollisionObj::Radius, ArgWord(Thread, 1));
			WriteWord(Runtime, Handle + OKCollisionObj::Radius + 4, 0);
			WriteWord(Runtime, Handle + OKCollisionObj::Shape, 1);
			WriteWord(Runtime, Handle + OKCollisionObj::Radius + 8, ArgWord(Thread, 2));
		}
		return true;
	}

	// 59.117 (named land_check) (obj) -> its gravity's z (+0x9C) while gravity is on (+0x68), else 0 (FUN_00447C60)
	bool GravityZ(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(Handle && ReadWord(Runtime, Handle + OKObject::Gravity) != 0 ? ReadWord(Runtime, Handle + OKObject::GravityVector + 8) : 0);
		return true;
	}

	// 59.118 (named land_height) (obj, z) (FUN_00447C70): gravity on, straight down by z
	bool SetGravityZ(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle)
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			WriteWord(Runtime, Handle + OKObject::GravityVector, 0);
			WriteWord(Runtime, Handle + OKObject::GravityVector + 4, 0);
			WriteWord(Runtime, Handle + OKObject::Gravity, 1);
			WriteWord(Runtime, Handle + OKObject::GravityVector + 8, ArgWord(Thread, 1));
		}
		return true;
	}

	// 52.81 / 59.120 (obj, &out) (FUN_0042F050): its velocity (+0x6C)
	bool ObjectVelocity(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		const uint32 Out = ArgAddress(Thread, 1);
		if (!Handle || !Out)
		{
			return true;
		}
		uint32 Words[3];
		for (int32 i = 0; i < 3; ++i)
		{
			Words[i] = ReadWord(Thread.GetRuntime(), Handle + OKObject::Velocity + i * 4);
		}
		return Thread.StoreBytes(Out, Words, sizeof(Words));
	}

	// 52.102 (named obj_set_accel) (obj, pitch, yaw, roll) (FUN_0042F200): a spin in degrees a second, kept as UE3 rotation
	// units at +0x88..+0x90, with +0x84 set while any is non-zero. [inf] Our MK objects don't spin yet; the fields are kept.
	bool ObjectSpin(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (!Handle)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		bool bAny = false;
		for (int32 i = 0; i < 3; ++i)
		{
			const int32 Units = int32(ArgFloat(Thread, 1 + i) * (65536.0f / 360.0f));
			WriteWord(Runtime, Handle + 0x88 + i * 4, uint32(Units));
			bAny |= (Units & 0xFFFF) != 0;
		}
		WriteWord(Runtime, Handle + 0x84, bAny ? 1 : 0);
		return true;
	}

	// 52.110 (named obj_flag_b0) (obj, ticks) (FUN_0042EE90): the object hides that many ticks from now (+0xB0; the
	// object update calls virtual +0x34(1) once it passes); 0 cancels. [inf] Kept, not acted on yet.
	bool HideInTicks(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		const int32 Ticks = ArgInt(Thread, 1);
		if (Handle)
		{
			WriteWord(Thread.GetRuntime(), Handle + 0xB0, Ticks != 0 ? uint32(Thread.GetRuntime().GetSimFrame() + Ticks) : 0u);
		}
		return true;
	}

	// 81.108 (named launch_flags) (obj, x) (FUN_0044BC00): +0x334 = x, and +0x394 the frame when x is under
	// DAT_00C61FE0. [unk] what reads them; [inf] the frame is noted for any x below 0
	bool SetLaunchValue(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle)
		{
			WriteWord(Thread.GetRuntime(), Handle + 0x334, ArgWord(Thread, 1));
			if (ArgFloat(Thread, 1) < 0.0f)
			{
				WriteWord(Thread.GetRuntime(), Handle + 0x394, uint32(Thread.GetRuntime().GetSimFrame()));
			}
		}
		return true;
	}

	// 81.232 set_parry(obj, on, script, function) (FUN_00446A70): a parry: +0x2E8 bit 0x10000, +0x75FC (its kind) 0,
	// the script at +0x7600 (a handle) and the function to run when an attack is parried at +0x760C (OKResolveContact)
	bool SetParry(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (!Handle)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Flags = ReadWord(Runtime, Handle + FOKFighter::Flags);
		if (ArgInt(Thread, 1) == 0)
		{
			WriteWord(Runtime, Handle + FOKFighter::Flags, Flags & ~0x10000u);
			return true;
		}
		WriteWord(Runtime, Handle + FOKFighter::Flags, Flags | 0x10000u);
		WriteWord(Runtime, Handle + 0x75FC, 0);
		uint32 Process = 0, Data = 0;
		Thread.ArgValue(2, Process);
		Thread.ArgValue(3, Data);
		if (Process != 0)
		{
			WriteWord(Runtime, Handle + 0x7600, Process);
			WriteWord(Runtime, Handle + 0x7604, Process);	// our handles are their own serials
			WriteWord(Runtime, Handle + 0x760C, Data);
		}
		return true;
	}

	// 137.12 (named set_char_variant) (obj, name) (FUN_00897BE0) -> 1 when the name is the fighter's variant (+0x76EC),
	// ignoring case; with no variant, 1 for no name. [inf] OpenKombat's fighters have none.
	bool IsVariant(FOKScriptThread& Thread)
	{
		Thread.SetResult(ArgAddress(Thread, 1) == 0 ? 1 : 0);
		return true;
	}

	// 137.36 (named remove_buff) (obj) (FUN_00893530): constrain_disable(0x40000), the material effect's flag
	bool ClearMaterialEffectFlag(FOKScriptThread& Thread)
	{
		const uint32 Handle = ArgObjectHandle(Thread, 0);
		if (Handle)
		{
			WriteWord(Thread.GetRuntime(), Handle + FOKFighter::CharFlags, ReadWord(Thread.GetRuntime(), Handle + FOKFighter::CharFlags) & ~0x40000u);
		}
		return true;
	}

	// 6.141 (named spawn_particle_at) (obj, from, pitch, yaw, roll) (FUN_004370A0): obj turns to from's rotation plus
	// these degrees; 6.153 (named obj_detach) (obj, from) (FUN_00437680): to from's rotation. Only MK objects keep one here.
	bool TurnLike(FOKScriptThread& Thread, bool bWithDegrees)
	{
		FOKScriptObject* Target = ArgAnyObject(Thread, 0);
		FOKMkObject* Object = Target ? Target->AsMkObject() : nullptr;
		FOKScriptObject* From = ArgAnyObject(Thread, 1);
		if (!Object || !From)
		{
			return true;
		}
		FRotator Rotation = FRotator::ZeroRotator;
		if (FOKFighter* Fighter = From->AsFighter())
		{
			Rotation = Fighter->Body ? Fighter->Body->GetFacing().Rotation() : Rotation;
		}
		else if (FOKMkObject* Other = From->AsMkObject())
		{
			Rotation = Other->Rotation;
		}
		if (bWithDegrees)
		{
			Rotation += FRotator(ArgFloat(Thread, 2), ArgFloat(Thread, 3), ArgFloat(Thread, 4));
		}
		Object->Rotation = Rotation;
		return true;
	}
	bool TurnLikePlus(FOKScriptThread& Thread) { return TurnLike(Thread, true); }
	bool TurnLikeOnly(FOKScriptThread& Thread) { return TurnLike(Thread, false); }

	// 52.73 (named obj_set_pos_offset) (obj, from, x, y, z) (FUN_004341E0): obj placed at from plus the offset in from's
	// frame (x along its facing); y turns over when from faces left (virtual +0x19C, x DAT_00C5FDA4: -1)
	bool PlaceFromObject(FOKScriptThread& Thread)
	{
		FOKScriptObject* Target = ArgAnyObject(Thread, 0);
		FOKMkObject* Object = Target ? Target->AsMkObject() : nullptr;
		FOKScriptObject* From = ArgAnyObject(Thread, 1);
		FVector Base;
		if (!Object || !From || !ObjectLocation(From, Base))
		{
			return true;
		}
		FOKFighter* Fighter = From->AsFighter();
		const FRotator Facing = (Fighter && Fighter->Body) ? Fighter->Body->GetFacing().Rotation()
			: (From->AsMkObject() ? From->AsMkObject()->Rotation : FRotator::ZeroRotator);
		const float Side = (Fighter && !Fighter->bFacingRight) ? -1.0f : 1.0f;
		Object->Location = Base + FRotator(0.0, Facing.Yaw, 0.0).RotateVector(FVector(ArgFloat(Thread, 2), ArgFloat(Thread, 3) * Side, ArgFloat(Thread, 4)));
		if (Object->IsA(EOKScriptClass::Particle))
		{
			OKObjects::PlaceParticle(static_cast<FOKParticleObject&>(*Object));
		}
		return true;
	}

	// 82.42 / 82.43 (named fx_attach_bone, _bone2) (fx, owner, bone, flip) (FUN_00436990 / FUN_00436A00): the effect's
	// component onto the owner's bone, by id (flipped through virtual +0xB4 when flip is set). [inf] Both attach; MKKE's
	// two component calls may differ in whether the rotation follows.
	bool AttachEffectToBone(FOKScriptThread& Thread)
	{
		FOKParticleObject* Object = ArgParticle(Thread, 0);
		FOKScriptObject* Owner = ArgAnyObject(Thread, 1);
		FOKFighter* Fighter = Owner ? Owner->AsFighter() : nullptr;
		const TCHAR* Bone = OKCollision::BoneName(int32(ArgWord(Thread, 2)));
		if (Object && Fighter && Bone)
		{
			Object->AttachedTo = Fighter;
			Object->Bone = Bone;
			Object->Offset = FVector::ZeroVector;
			OKObjects::PlaceParticle(*Object);
		}
		return true;
	}

	// 52.55 (named fx_attach_bone_name) (fx, owner, &boneName) (FUN_0043E370 -> FUN_0043A6B0): as fx_attach, by name
	bool AttachEffectToBoneName(FOKScriptThread& Thread)
	{
		FOKParticleObject* Object = ArgParticle(Thread, 0);
		FOKScriptObject* Owner = ArgAnyObject(Thread, 1);
		FOKFighter* Fighter = Owner ? Owner->AsFighter() : nullptr;
		const FString Bone = CStringAt(Thread, ArgAddress(Thread, 2));
		if (Object && Fighter && !Bone.IsEmpty())
		{
			Object->AttachedTo = Fighter;
			Object->Bone = Bone;
			Object->Offset = FVector::ZeroVector;
			OKObjects::PlaceParticle(*Object);
		}
		return true;
	}

	// 137.89 scripted_hit(obj, ?, victim, script, function, context, word0, ?) (FUN_008AD6C0): the function sets the
	// pending attack up and it hits the victim at once (OKObjects::ScriptedHit). Script 0 is the running one.
	bool ScriptedHitNative(FOKScriptThread& Thread)
	{
		FOKFighter* Owner = Thread.ArgFighter(0);
		FOKFighter* Victim = Thread.ArgFighter(2);
		uint32 ScriptValue = 0, FunctionValue = 0, Context = 0, Word0 = 0;
		Thread.ArgValue(3, ScriptValue);
		Thread.ArgValue(4, FunctionValue);
		Thread.ArgValue(5, Context);
		Thread.ArgValue(6, Word0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const FOKScriptInstance* From = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag
			? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : Runtime.GetInstance(*Thread.GetCurrentScript());
		FOKScriptInstance* Target = nullptr;
		int32 Function = INDEX_NONE;
		if (!Owner || !Victim || !From || FunctionValue == 0 || !Runtime.ResolveFunction(*From, FunctionValue, Target, Function))
		{
			return true;
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s's scripted hit on %s (%s)"), Runtime.GetSimFrame(), *Owner->Name, *Victim->Name,
			*Target->File->GetFunctions()[Function].Name);
		OKObjects::ScriptedHit(Runtime, *Owner, *Victim, *Target->File, Function, Context, Word0);
		return true;
	}

	// 137.119 reset_two_player_state(obj, command, ?) (FUN_008AB040): sets up the fighter's two-player state from a
	// command (a throw's), or with none clears it. [inf] OpenKombat's two-player state is the throw link (F10): with no
	// command the fighter's link is let go, whichever side it is on. A command isn't handled here.
	bool ResetTwoPlayerState(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || ArgWord(Thread, 1) != 0)
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		if (Fighter->ThrowLeader)
		{
			OKReleaseThrow(Runtime, *Fighter, false);
		}
		if (Fighter->Opponent && Fighter->Opponent->ThrowLeader == Fighter)
		{
			OKReleaseThrow(Runtime, *Fighter->Opponent, false);
		}
		return true;
	}

	// 6.137 attach_obj(a, b, &rotator) (FUN_0043D800): the direction from a to b, flat, as a UE3 rotator (pitch, yaw, roll;
	// 65536 a turn). p_xray_cinema turns its scene by the yaw.
	bool AttachObj(FOKScriptThread& Thread)
	{
		FOKFighter* A = Thread.ArgFighter(0);
		FOKFighter* B = Thread.ArgFighter(1);
		int32 Rotator[3] = {};
		if (A && A->Body && B && B->Body)
		{
			const FVector Way = B->Body->GetWorldLocation() - A->Body->GetWorldLocation();
			if (Way.SizeSquared2D() > UE_KINDA_SMALL_NUMBER)
			{
				Rotator[1] = int32(FMath::RoundToInt(FMath::Atan2(Way.Y, Way.X) * 32768.0 / UE_DOUBLE_PI)) & 0xFFFF;
			}
		}
		const uint32 Out = ArgAddress(Thread, 2);
		return Out == 0 || Thread.StoreBytes(Out, Rotator, sizeof(Rotator));
	}

	// 136.33 slow_time(gameinfo, dilation) (FUN_0080DEC0): the world's time dilation (WorldInfo +0x280); 136.34 back to 1
	// (FUN_0080F9B0). FGFightEngineScript.p_slow_time holds it for a number of ticks (hit_pause: 0.0001).
	bool SlowTime(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Runtime.SetTimeDilation(FMath::Max(0.0f, ArgFloat(Thread, 1)));
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: time dilation %g"), Runtime.GetSimFrame(), Runtime.GetTimeDilation());
		return true;
	}

	bool SlowTimeEnd(FOKScriptThread& Thread)
	{
		Thread.GetRuntime().SetTimeDilation(1.0f);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: time dilation back to 1"), Thread.GetRuntime().GetSimFrame());
		return true;
	}

	// 136.35 char_anim_speed(gameinfo, speed) (FUN_0080DEF0): game info +0x82B4; 136.36 back to 1 (FUN_0080DF10).
	// [inf] No reader is found by offset; OpenKombat scales the fighters' animation by it.
	bool CharAnimSpeed(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const float Speed = FMath::Max(0.0f, ArgFloat(Thread, 1));
		Runtime.SetCharAnimSpeed(Speed);
		WriteWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + 0x82B4, FloatToBits(Speed));
		return true;
	}

	bool CharAnimSpeedEnd(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Runtime.SetCharAnimSpeed(1.0f);
		WriteWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + 0x82B4, FloatToBits(1.0f));
		return true;
	}

	// 4.24(n) (FUN_0080EB70 -> FUN_00406A90): game flag 0x4000 set when n is 0, cleared otherwise. The round clock stops
	// while it is set (43.7); p_xray_cinema holds it for the cinema.
	bool ClockStopFlag(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Flags = Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::Flags;
		const uint32 Old = ReadWord(Runtime, Flags);
		WriteWord(Runtime, Flags, ArgInt(Thread, 0) == 0 ? (Old | 0x4000u) : (Old & ~0x4000u));
		return true;
	}

	// 118.46 set_life(info, life) (FUN_00894300): the active fighter's life; at 0 a tag partner's too
	bool SetLifeNative(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		if (Player && Player->Fighter)
		{
			SetLife(Thread.GetRuntime(), *Player->Fighter, ArgReal(Thread, 1));
		}
		return true;
	}

	// 118.7 fight_started(info) (FUN_00897420): the input buffer forgets what was pressed before the fight: the newest
	// direction entry is cleared, every held button is swallowed (FUN_00425620) and the reader catches up (FUN_004259B0).
	// [inf] It also tells the AI drone.
	void ClearNewestDirection(uint8* Bytes)
	{
		const uint8 Head = Bytes[OKInput::DirHead];
		FMemory::Memzero(Bytes + OKInput::Directions + Head * OKInput::EntryBytes + 4, 4);
		if (Head != 0)
		{
			FMemory::Memzero(Bytes + OKInput::Directions + Head * OKInput::EntryBytes - 0xC, 4);
		}
	}

	void CatchUp(uint8* Bytes)
	{
		const uint8 Head = Bytes[OKInput::PressHead];
		Bytes[OKInput::Cursor] = Head;
		FMemory::Memcpy(Bytes + 0x5FC, Bytes + OKInput::Presses + Head * OKInput::EntryBytes, 8);
		for (int32 i = 0; i < OKInput::RingSize; ++i)
		{
			FMemory::Memzero(Bytes + OKInput::Releases + i * OKInput::EntryBytes + 4, 4);
		}
	}

	uint8* PlayerInputBytes(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Buffer = Player ? ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer) : 0;
		return Buffer ? Runtime.HeapBytes(Buffer & 0x00FFFFFF, OKInput::Bytes) : nullptr;
	}

	bool FightStarted(FOKScriptThread& Thread)
	{
		if (uint8* Bytes = PlayerInputBytes(Thread))
		{
			ClearNewestDirection(Bytes);
			uint32 Held = 0;
			FMemory::Memcpy(&Held, Bytes + OKInput::Held, 4);
			uint32 Previous = 0;
			FMemory::Memcpy(&Previous, Bytes + OKInput::PreviousHeld, 4);
			Previous |= Held & 0xFFFF;
			FMemory::Memcpy(Bytes + OKInput::PreviousHeld, &Previous, 4);
			CatchUp(Bytes);
		}
		return true;
	}

	// 78.77 (info) (FUN_00446000 -> FUN_00426120): the newest direction entry is cleared and the reader catches up
	bool ForgetDirection(FOKScriptThread& Thread)
	{
		if (uint8* Bytes = PlayerInputBytes(Thread))
		{
			ClearNewestDirection(Bytes);
			CatchUp(Bytes);
		}
		return true;
	}

	// 78.78 (info) (FUN_00446020 -> FUN_004259B0): the reader catches up with the newest press, releases cleared
	bool CatchUpInput(FOKScriptThread& Thread)
	{
		if (uint8* Bytes = PlayerInputBytes(Thread))
		{
			CatchUp(Bytes);
		}
		return true;
	}

	// 78.95 (info) -> pops the control stack (FUN_00446790), as utils.disable_controllers does
	bool PopControl(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		uint32 Top = 0;
		if (Player)
		{
			FOKScriptRuntime& Runtime = Thread.GetRuntime();
			const uint32 Count = ReadWord(Runtime, Player->Handle + FOKPlayer::ControlCount);
			if (Count != 0)
			{
				WriteWord(Runtime, Player->Handle + FOKPlayer::ControlCount, Count - 1);
				Top = ReadWord(Runtime, Player->Handle + FOKPlayer::ControlStack + (Count - 1) * 4);
			}
		}
		Thread.SetResult(Top);
		return true;
	}

	// 118.40 (named partner_change) (info) -> the opponent's player record, player +0xBE4 (virtual +0x38). [inf] On other
	// objects virtual +0x38 is something else; gamelogic only passes players.
	bool OpponentPlayer(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		Thread.SetResult(Player ? ReadWord(Thread.GetRuntime(), Player->Handle + FOKPlayer::Opponent) : 0);
		return true;
	}

	// 118.42 (named set_shadow_flag) (info, ?) -> on a player record (virtual +0x40, FUN_008922C0), the opponent's active
	// character: gamelogic.round_over takes the loser this way. [inf] On a fighter virtual +0x40 sets a shadow flag.
	bool OpponentCharacter(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		FOKScriptObject* Object = Player ? Thread.GetRuntime().ResolveHandle(ReadWord(Thread.GetRuntime(), Player->Handle + FOKPlayer::Opponent)) : nullptr;
		FOKPlayer* Opponent = Object ? Object->AsPlayer() : nullptr;
		Thread.SetResult(Opponent && Opponent->Fighter ? Opponent->Fighter->Handle : 0);
		return true;
	}

	// 78.54 (info, code) -> whether it's held (player virtual +0x24, FUN_00454310): towards (0x80000001) and away
	// (0x80000002) by the fighter's facing; other codes as their mask while the player can act. GenericThrow asks it for
	// towards, to tell a forward throw from a back one. [inf] Its diagonal codes (0x8000001F..22, 2E, 2F) go to
	// HeldCommand here.
	bool InputHeldFacing(FOKScriptThread& Thread);

	// 118.5 player_side(info) (FUN_008A4580) -> its side, 0 or 1 (+0xB3C)
	bool PlayerSide(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		Thread.SetResult(Player ? ReadWord(Thread.GetRuntime(), Player->Handle + FOKPlayer::Side) : 0);
		return true;
	}

	// 78.60 (info, code) (FUN_00446910): held, while the player can act (+0x944 isn't 1). The throw escapes ask it of the victim.
	bool InputHeldWhileActing(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(Player && ReadWord(Runtime, Player->Handle + FOKPlayer::InputOff) != 1
			&& OKInput::IsHeld(Runtime, ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer), OKInput::CodeToMask(ArgInt(Thread, 1))) ? 1 : 0);
		return true;
	}

	bool InputHeldFacing(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		const uint32 Code = ArgInt(Thread, 1);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		bool bHeld = false;
		if (Player && Player->Fighter && (Code == 0x80000001 || Code == 0x80000002))
		{
			uint32 Towards = 0, Away = 0;
			OKPlayerInput::FacingMasks(*Player->Fighter, Towards, Away);
			bHeld = OKInput::IsHeld(Runtime, ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer), Code == 0x80000001 ? Towards : Away);
		}
		else if (Player && ((Code >= 0x8000001F && Code <= 0x80000022) || Code == 0x8000002E || Code == 0x8000002F))
		{
			bHeld = OKPlayerInput::HeldCommand(Runtime, *Player, Code);
		}
		else if (Player && ReadWord(Runtime, Player->Handle + FOKPlayer::InputOff) != 1)
		{
			bHeld = OKInput::IsHeld(Runtime, ReadWord(Runtime, Player->Handle + FOKPlayer::InputBuffer), OKInput::CodeToMask(Code));
		}
		Thread.SetResult(bHeld ? 1 : 0);
		return true;
	}

	// 7.147 (round) (FUN_00850890): the HUD's "Round N"; 7.148 (FUN_00850920) its "Fight!". MKKE plays them in its
	// Scaleform HUD; ours are banners. [inf] How long each stays: Round until Fight, Fight for a second.
	void ShowBanner(FOKScriptRuntime& Runtime, FOKHudBanner::EKind Kind, int32 Value, int64 Frames)
	{
		FOKHudBanner Banner;
		Banner.Kind = Kind;
		Banner.Value = Value;
		Banner.Start = Runtime.GetSimFrame();
		Banner.Until = Banner.Start + Frames;
		Runtime.GetHud().ShowBanner(Banner);
	}

	bool ShowRoundText(FOKScriptThread& Thread)
	{
		UE_LOG(LogOKVM, Log, TEXT("Round %u"), ArgInt(Thread, 0));
		ShowBanner(Thread.GetRuntime(), FOKHudBanner::EKind::Round, int32(ArgInt(Thread, 0)), 180);
		return true;
	}

	bool ShowFightText(FOKScriptThread& Thread)
	{
		UE_LOG(LogOKVM, Log, TEXT("Fight!"));
		ShowBanner(Thread.GetRuntime(), FOKHudBanner::EKind::Fight, 0, 60);
		return true;
	}

	// 43.32 (gameinfo, brightness) (FUN_00406960): the fade to black, leaving the UI; 43.33 (FUN_00406980) fades the UI too
	bool SetFade(FOKScriptThread& Thread)
	{
		Thread.GetRuntime().SetFade(FMath::Clamp(ArgReal(Thread, 1), 0.0f, 1.0f));
		return true;
	}

	FString CStringAt(FOKScriptThread& Thread, uint32 Address)
	{
		FString Text;
		for (uint8 Char = 0; Address != 0 && Text.Len() < 80 && Thread.LoadBytes(Address + Text.Len(), &Char, 1) && Char != 0; )
		{
			Text.AppendChar(TCHAR(Char));
		}
		return Text;
	}

	FString ValueCString(FOKScriptThread& Thread, int32 Index)
	{
		const uint32 Address = ArgInt(Thread, Index);
		FString Text;
		for (uint8 Char = 0; Address != 0 && Text.Len() < 80 && Thread.LoadBytes(Address + Text.Len(), &Char, 1) && Char != 0; )
		{
			Text.AppendChar(TCHAR(Char));
		}
		return Text;
	}

	// 137.151 is_character(obj, name) (FUN_008A6F50): its character's name, ignoring case ("char_shaokahn")
	bool IsCharacter(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Name.Equals(ValueCString(Thread, 1), ESearchCase::IgnoreCase) ? 1 : 0);
		return true;
	}

	// 6.5 find_func(script, name, ?) (FUN_00458680) -> the script's function by name, as a function value (index + 1); 0 if none
	bool FindFunc(FOKScriptThread& Thread)
	{
		const uint32 ScriptValue = ArgWord(Thread, 0);
		const FOKScriptInstance* Instance = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Thread.GetRuntime().FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		const int32 Function = Instance ? Instance->File->FindFunctionByName(ValueCString(Thread, 1)) : INDEX_NONE;
		Thread.SetResult(uint32(Function + 1));
		return true;
	}

	/** The script instance a value names (a script's value, from 23.4 on a script handle), or null. */
	const FOKScriptInstance* ArgScriptInstance(FOKScriptThread& Thread, int32 Index)
	{
		uint32 Value = 0;
		Thread.ArgValue(Index, Value);
		return (Value >> 24) == FOKScriptRuntime::InstanceTag ? Thread.GetRuntime().FindInstanceById(int32(Value & 0x00FFFFFF)) : nullptr;
	}

	// 21.5 find_global(script, name, ?) -> the address of the script's global of that name (FindGlobal, FUN_00458620:
	// globals are found by the FNV-1 hash of their name); 0 if it has none. ck_fatality_available looks up "fatalities".
	bool FindGlobalNative(FOKScriptThread& Thread)
	{
		const FOKScriptInstance* Instance = ArgScriptInstance(Thread, 0);
		const FString Name = ValueCString(Thread, 1);
		uint32 Address = 0;
		for (const uint32 Hash : { FOKMkoFile::Fnv1(Name), FOKMkoFile::Fnv1(Name.ToLower()) })
		{
			const FOKMkoGlobal* Global = (Instance && Address == 0) ? Instance->File->GetGlobals().FindByPredicate([Hash](const FOKMkoGlobal& G) { return G.NameHash == Hash; }) : nullptr;
			Address = Global ? FOKScriptRuntime::MemoryAddress(Instance->Id, Instance->PoolBase + Global->PoolOffset) : Address;
		}
		Thread.SetResult(Address);
		return true;
	}

	// 21.6 global_count(script, address) -> the elements of the script's global at that address: size / element size
	// (GlobalCount, FUN_00458740); 0 if none is there
	bool GlobalCountNative(FOKScriptThread& Thread)
	{
		const FOKScriptInstance* Instance = ArgScriptInstance(Thread, 0);
		const uint32 Address = ArgWord(Thread, 1);
		uint32 Count = 0;
		if (Instance)
		{
			for (const FOKMkoGlobal& Global : Instance->File->GetGlobals())
			{
				if (Global.ElementSize != 0 && FOKScriptRuntime::MemoryAddress(Instance->Id, Instance->PoolBase + Global.PoolOffset) == Address)
				{
					Count = Global.Size / Global.ElementSize;
					break;
				}
			}
		}
		Thread.SetResult(Count);
		return true;
	}

	// --- Finisher cinemas (F8 on W4): play_fatality_cinema -> p_fatality_cinema -> fg_utils.PlayCinemaSceneEx ---------

	// 138.1: the FatalityCinemaPdata class; 138.3: a new one (FUN_00897EB0), which play_fatality_cinema fills in
	bool ClassFatalityCinemaPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::FatalityCinemaPdata));
		return true;
	}

	bool NewFatalityCinemaPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetRuntime().CreateOwned(MakeUnique<FOKFatalityCinemaPdata>(), FOKFatalityCinemaPdata::Bytes));
		return true;
	}

	/** Argument Index as a cinema scene (cinema_prepare's), with its handle; null if it isn't one. */
	FOKCinemaObject* ArgCinema(FOKScriptThread& Thread, int32 Index, uint32* OutHandle = nullptr)
	{
		uint32 Handle = 0;
		Thread.ArgValue(Index, Handle);
		FOKScriptObject* Object = Handle != 0 ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		if (OutHandle)
		{
			*OutHandle = Handle;
		}
		return Object && Object->IsA(EOKScriptClass::Cinema) ? static_cast<FOKCinemaObject*>(Object) : nullptr;
	}

	// 39.1 (named cinema_frames_left): the MKCinema class, for is_proc
	bool ClassCinema(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::Cinema));
		return true;
	}

	// 39.51 cinema_prepare(name, group, streamed) -> a cinema scene (FUN_004B4160: MKKE finds the SeqAct_Cinema of the
	// package's streamed level and wraps it in a 0x94-byte MKCinema). The package is read when it plays.
	bool CinemaPrepare(FOKScriptThread& Thread)
	{
		TUniquePtr<FOKCinemaObject> Scene = MakeUnique<FOKCinemaObject>();
		Scene->Package = ValueCString(Thread, 0);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: cinema_prepare %s"), Thread.GetRuntime().GetSimFrame(), *Scene->Package);
		Thread.SetResult(Thread.GetRuntime().CreateOwned(MoveTemp(Scene), FOKCinemaObject::Bytes));
		return true;
	}

	// 39.19 cinema_load(cinema) -> 1 while its level is still loading (FUN_004B4660; LoadandPrepareCinemaScene waits on
	// it); 0 here at once, since the package is read when the scene plays
	bool CinemaLoad(FOKScriptThread& Thread)
	{
		Thread.SetResult(0);
		return true;
	}

	// 39.35 cinema_bind_actor(cinema, obj, group, flag) (FUN_004B2C50): the object plays that group (Attacker, Victim)
	bool CinemaBindActor(FOKScriptThread& Thread)
	{
		FOKCinemaObject* Scene = ArgCinema(Thread, 0);
		FOKFighter* Fighter = Thread.ArgFighter(1);
		const FString Group = ValueCString(Thread, 2);
		if (Scene && Fighter && !Group.IsEmpty())
		{
			Scene->Actors.Add(Group, Fighter);
			UE_LOG(LogOKVM, Log, TEXT("cinema %s: %s plays %s"), *Scene->Package, *Fighter->Name, *Group);
		}
		return true;
	}

	/**
	 * The cinema's activated callback (FUN_004B2A30): a script (+0x54) and a function name (+0x58) that
	 * fg_utils.CommonPlayCinemaScene stores from the startup info (an X-ray's XRayPostActivatedCallback). MKKE runs it
	 * on a thread of its own, with the cinema and its two actors, when the Matinee first updates; here it is a process
	 * that starts with the cinema. It sends an X-ray's fighters to their recoveries (p_xray_victim, p_xray_attacker),
	 * which wait for the cinema to let go of them.
	 */
	void StartActivatedCallback(FOKScriptThread& Thread, uint32 Handle, const FOKCinemaObject& Scene)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 ScriptValue = ReadWord(Runtime, Handle + 0x54);
		const FString Name = CStringAt(Thread, ReadWord(Runtime, Handle + 0x58));
		const FOKScriptInstance* Instance = ((ScriptValue >> 24) == FOKScriptRuntime::InstanceTag)
			? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		const int32 Function = (Instance && !Name.IsEmpty()) ? Instance->File->FindFunctionByName(Name) : INDEX_NONE;
		if (Function == INDEX_NONE)
		{
			return;
		}
		const auto Actor = [&Scene](const TCHAR* Group)
		{
			FOKFighter* const* Fighter = Scene.Actors.Find(Group);
			return (Fighter && *Fighter) ? (*Fighter)->Handle : 0u;
		};
		const uint32 Args[3] = { Handle, Actor(TEXT("Attacker")), Actor(TEXT("Victim")) };
		FString Error;
		if (!Runtime.StartProcess(*Instance->File, Function, 0, 14, true, Error, Args))
		{
			UE_LOG(LogOKVM, Warning, TEXT("cinema %s: %s: %s"), *Scene.Package, *Name, *Error);
		}
	}

	// 39.11 cinema_play(cinema) (FUN_004B2770): the script world starts it on its next frame
	bool CinemaPlay(FOKScriptThread& Thread)
	{
		uint32 Handle = 0;
		FOKCinemaObject* Scene = ArgCinema(Thread, 0, &Handle);
		if (Scene && Scene->State == FOKCinemaObject::EState::Prepared)
		{
			Scene->State = FOKCinemaObject::EState::Requested;
			Thread.GetRuntime().SetActiveCinema(Handle);
			StartActivatedCallback(Thread, Handle, *Scene);
		}
		return true;
	}

	// 39.17 (named cinema_current_frame) -> 1 while it plays (FUN_004B2960: bit 1 of its interp actor's +0xFC)
	bool CinemaPlaying(FOKScriptThread& Thread)
	{
		const FOKCinemaObject* Scene = ArgCinema(Thread, 0);
		Thread.SetResult(Scene && (Scene->State == FOKCinemaObject::EState::Requested || Scene->State == FOKCinemaObject::EState::Playing) ? 1 : 0);
		return true;
	}

	// 39.31 cinema_wait(cinema, &frames) -> 1 while it plays, with the frames left at 60 a second (FUN_004B1CF0)
	bool CinemaWait(FOKScriptThread& Thread)
	{
		const FOKCinemaObject* Scene = ArgCinema(Thread, 0);
		uint32 Out = 0;
		Thread.ArgValue(1, Out);
		const bool bPlaying = Scene && Scene->State == FOKCinemaObject::EState::Playing;
		const uint32 Frames = bPlaying ? uint32(FMath::CeilToInt(FMath::Max(0.0f, Scene->Length - Scene->Position) * 60.0f)) : 0;
		if (Out != 0)
		{
			Thread.StoreBytes(Out, &Frames, 4);
		}
		Thread.SetResult(bPlaying ? 1 : 0);
		return true;
	}

	// 39.30 (named cinema_callback_at_frame) -> how far it has played, in seconds (FUN_004B1CD0)
	bool CinemaPosition(FOKScriptThread& Thread)
	{
		const FOKCinemaObject* Scene = ArgCinema(Thread, 0);
		Thread.SetResult(FloatToBits(Scene ? Scene->Position : 0.0f));
		return true;
	}

	// 39.14 cinema_skip(cinema) -> 1 if it was playing; it stops (FUN_004B27E0)
	bool CinemaSkip(FOKScriptThread& Thread)
	{
		FOKCinemaObject* Scene = ArgCinema(Thread, 0);
		const bool bPlaying = Scene && Scene->State == FOKCinemaObject::EState::Playing;
		if (bPlaying)
		{
			Scene->bSkip = true;
		}
		Thread.SetResult(bPlaying ? 1 : 0);
		return true;
	}

	// 6.8 vm_call2(script, function, a) / 6.9 vm_call4(script, function, a, b, c): calls a function (1-based) of a
	// script instance with arguments (FUN_0045C220 shifts them down for vm_call1_impl). utils.MKSCallback runs a
	// cinema's callbacks (Fatality1CinemaPostLoadCallback) this way.
	bool VmCallWith(FOKScriptThread& Thread, int32 ArgCount)
	{
		uint32 ScriptRef = 0, Function = 0, Args[3] = {};
		Thread.ArgValue(0, ScriptRef);
		Thread.ArgValue(1, Function);
		for (int32 i = 0; i < ArgCount; ++i)
		{
			Thread.ArgValue(2 + i, Args[i]);
		}
		FOKScriptInstance* Instance = ((ScriptRef >> 24) == FOKScriptRuntime::InstanceTag)
			? Thread.GetRuntime().FindInstanceById(int32(ScriptRef & 0x00FFFFFF)) : nullptr;
		if (!Instance || Function == 0)
		{
			return Thread.NativeFail(FString::Printf(TEXT("vm_call: %08X function %u isn't a script function"), ScriptRef, Function));
		}
		return Thread.CallFunction(*Instance, int32(Function) - 1, TConstArrayView<uint32>(Args, ArgCount));
	}

	bool VmCall2(FOKScriptThread& Thread) { return VmCallWith(Thread, 1); }
	bool VmCall4(FOKScriptThread& Thread) { return VmCallWith(Thread, 3); }

	// 7.217 fatality_effect(): the FATALITY logo, as the announcer says it (gamelogic.do_fatality_effect). [inf] MKKE's
	// effect isn't read yet; OpenKombat's HUD shows the word for three seconds.
	bool FatalityEffect(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKHudBanner Banner;
		Banner.Kind = FOKHudBanner::EKind::Message;
		Banner.Text = TEXT("FATALITY");
		Banner.Start = Runtime.GetSimFrame();
		Banner.Until = Banner.Start + 180;
		Runtime.GetHud().ShowBanner(Banner);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: FATALITY"), Runtime.GetSimFrame());
		return true;
	}

	// 137.81 win_effect_in_ticks(obj, name, ticks) (FUN_00893010): a new thread runs the named function of the fighter's
	// script slot 12, gamelogic, with ticks (do_win_effect_in_ticks: the win text after that many frames)
	bool WinEffectInTicks(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const FString Name = ValueCString(Thread, 1);
		uint32 Ticks = 0;
		Thread.ArgValue(2, Ticks);
		const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
		const FOKMkoFile* GameLogic = (Scripts && Scripts->GetLibrary()) ? Scripts->GetLibrary()->FindScript(TEXT("gamelogic.mko")) : nullptr;
		const int32 Function = GameLogic ? GameLogic->FindFunctionByName(Name) : INDEX_NONE;
		if (Function == INDEX_NONE)
		{
			return Thread.NativeFail(TEXT("win_effect_in_ticks: gamelogic has no ") + Name);
		}
		FString Error;
		const uint32 Args[1] = { Ticks };
		FOKScriptThread* Child = Thread.GetRuntime().StartProcess(*GameLogic, Function, 0, 14, true, Error, Args);
		if (!Child)
		{
			return Thread.NativeFail(TEXT("win_effect_in_ticks: ") + Error);
		}
		if (Fighter)
		{
			Child->BindFighter(*Fighter);
		}
		return true;
	}

	// 43.45 (gameinfo) -> the frame rate (FUN_005EAF30, which cinema_wait also counts frames by): 60
	bool FrameRate(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(60.0f));
		return true;
	}

	// 6.77 (named cinema_handle) acos(x) -> radians (FUN_008B8470); p_fatality_cinema turns a facing into the scene's yaw
	bool Acos(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(FMath::Acos(FMath::Clamp(ArgFloat(Thread, 0), -1.0f, 1.0f))));
		return true;
	}

	// 137.218 xfer_to_action(obj, function) (FUN_008A4D40): the fighter's main thread goes to that function (by
	// number) of the calling script; fatality_adjust_if_back_to_wall pushes the victim back with p_fatality_pushed_back
	bool XferToAction(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		uint32 Function = 0;
		Thread.ArgValue(1, Function);
		if (Fighter && Function != 0)
		{
			OKTransferMainThread(Thread.GetRuntime(), *Fighter, Thread.CurrentInstanceValue(), Function);
		}
		return true;
	}

	// 4.28 (info, ?, ?, text, ?, ?, slide, font, font) (FUN_00897510): a message on the player's half of the screen ("ESCAPE"
	// when a throw is broken). MKKE runs MKHUD's p_gameplay_slide_string_message (or ..._fade_in_...) with it; OpenKombat's HUD
	// draws it as a banner for a second and a half. [unk] the numbers (6, 1, 430, 17) and MKKE's fonts aren't used.
	bool HudMessage(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKHudBanner Banner;
		Banner.Kind = FOKHudBanner::EKind::Message;
		Banner.Text = ValueCString(Thread, 3);
		const FOKPlayer* Player = ArgPlayer(Thread, 0);
		Banner.Side = Player ? int32(ReadWord(Runtime, Player->Handle + FOKPlayer::Side)) : -1;
		Banner.Start = Runtime.GetSimFrame();
		Banner.Until = Banner.Start + 90;
		Runtime.GetHud().ShowBanner(Banner);
		UE_LOG(LogOKVM, Log, TEXT("HUD message: %s"), *Banner.Text);
		return true;
	}

	// 7.144 slide_message(edge, ?, text, ?, info) (FUN_0085EE80 -> FUN_00858560): text that slides in on the player's half of the
	// screen ("BREAKER"), from the edge chest_to_screen_flag picks. OpenKombat's HUD shows it as 4.28's banner. [unk] The
	// second and fourth numbers (1 from every caller).
	bool SlideMessage(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKHudBanner Banner;
		Banner.Kind = FOKHudBanner::EKind::Message;
		Banner.Text = ValueCString(Thread, 2);
		const FOKPlayer* Player = ArgPlayer(Thread, 4);
		Banner.Side = Player ? int32(ReadWord(Runtime, Player->Handle + FOKPlayer::Side)) : -1;
		Banner.Start = Runtime.GetSimFrame();
		Banner.Until = Banner.Start + 90;
		Runtime.GetHud().ShowBanner(Banner);
		UE_LOG(LogOKVM, Log, TEXT("HUD message: %s"), *Banner.Text);
		return true;
	}

	// 6.3 loc_string(section, key, fallback) -> the text from the localisation files (FUN_004920C0): "" + "SUB-ZERO" +
	// " WINS!". A key they don't have gives the key itself, which is easier to trace than MKKE's fallback ("BUG ME").
	bool LocString(FOKScriptThread& Thread)
	{
		const FString* Text = FOKLocalization::Get().Find(ValueCString(Thread, 0), ValueCString(Thread, 1));
		const uint32 Address = Text ? Thread.GetRuntime().InternString(*Text) : 0;
		Thread.SetResult(Address != 0 ? Address : ArgInt(Thread, 1));
		return true;
	}

	// 6.37 str_copy_n(dst, src, n) -> dst: strncpy, then dst[n - 1] = 0
	bool StrCopyN(FOKScriptThread& Thread)
	{
		const uint32 Dest = ArgInt(Thread, 0);
		const int32 Size = int32(ArgInt(Thread, 2));
		const FString Text = ValueCString(Thread, 1).Left(FMath::Max(0, Size - 1));
		for (int32 i = 0; i < Size; ++i)
		{
			const uint8 Char = i < Text.Len() ? uint8(Text[i]) : 0;
			Thread.StoreBytes(Dest + i, &Char, 1);
		}
		Thread.SetResult(Dest);
		return true;
	}

	// 6.35 str_concat3(dst, src, n) -> dst: src appended, the whole kept under n bytes (FUN_008B7E80)
	bool StrConcat(FOKScriptThread& Thread)
	{
		const uint32 Dest = ArgInt(Thread, 0);
		const int32 Size = int32(ArgInt(Thread, 2));
		const FString Text = (ValueCString(Thread, 0) + ValueCString(Thread, 1)).Left(FMath::Max(0, Size - 1));
		for (int32 i = 0; i <= Text.Len(); ++i)
		{
			const uint8 Char = i < Text.Len() ? uint8(Text[i]) : 0;
			Thread.StoreBytes(Dest + i, &Char, 1);
		}
		Thread.SetResult(Dest);
		return true;
	}

	// --- HUD objects: MKKE's render objects (module 15, FUN_00B90420 and the setters after it); our HUD draws them ---

	// 15.10 hud_text(text, id, font, font, layer, x, y, x, y) -> 1 (FUN_00B91A70): a text centred on (x, y) in HUD pixels,
	// such as the win text (id 6) and "FLAWLESS VICTORY" (7). MKKE uses the first font, or the second when DAT_00E04B04 is 0.
	bool HudText(FOKScriptThread& Thread)
	{
		const FString Text = ValueCString(Thread, 0);
		FOKScreenObject& Object = Thread.GetRuntime().GetHud().Add(int32(ArgInt(Thread, 1)), int32(ArgInt(Thread, 4)));
		Object.Text = Text;
		Object.Texture = ValueCString(Thread, 2);
		Object.Position = FVector2D(ArgReal(Thread, 5), ArgReal(Thread, 6));
		UE_LOG(LogOKVM, Log, TEXT("HUD text %d: %s"), Object.Id, *Text);
		Thread.SetResult(1);
		return true;
	}

	// 15.3 hud_sprite(texture, texture, id, layer, x, y, x, y) -> 1 (FUN_00B92390 -> FUN_00B91E90): a texture at its own
	// size, centred on (x, y), such as "Finish Him" and its flashes
	bool HudSprite(FOKScriptThread& Thread)
	{
		FOKScreenObject& Object = Thread.GetRuntime().GetHud().Add(int32(ArgInt(Thread, 2)), int32(ArgInt(Thread, 3)));
		Object.Texture = ValueCString(Thread, 0);
		Object.Position = FVector2D(ArgReal(Thread, 4), ArgReal(Thread, 5));
		UE_LOG(LogOKVM, Log, TEXT("HUD sprite %d: %s"), Object.Id, *Object.Texture);
		Thread.SetResult(1);
		return true;
	}

	// 15.0 (id) (FUN_00B92130): removes every object with that id
	bool HudFree(FOKScriptThread& Thread)
	{
		Thread.GetRuntime().GetHud().Free(int32(ArgInt(Thread, 0)));
		return true;
	}

	// 15.13 (id, alpha 0..255) (FUN_00B90710); larger values are ignored
	bool HudAlpha(FOKScriptThread& Thread)
	{
		FOKScreenObject* Object = Thread.GetRuntime().GetHud().Find(int32(ArgInt(Thread, 0)));
		const uint32 Alpha = ArgInt(Thread, 1);
		if (Object && Alpha < 0x100)
		{
			Object->Alpha = float(Alpha) / 255.0f;
		}
		return true;
	}

	// 15.16 (id, scale) its width (FUN_00B907F0), 15.18 its height (FUN_00B90880), 15.19 both (FUN_00B908D0)
	template <bool bX, bool bY>
	bool HudScale(FOKScriptThread& Thread)
	{
		if (FOKScreenObject* Object = Thread.GetRuntime().GetHud().Find(int32(ArgInt(Thread, 0))))
		{
			const float Scale = ArgReal(Thread, 1);
			Object->Scale = FVector2D(bX ? Scale : Object->Scale.X, bY ? Scale : Object->Scale.Y);
		}
		return true;
	}

	// 15.20 (id, x, y) (FUN_00B90990 -> FUN_00B90920): moves it
	bool HudPosition(FOKScriptThread& Thread)
	{
		if (FOKScreenObject* Object = Thread.GetRuntime().GetHud().Find(int32(ArgInt(Thread, 0))))
		{
			Object->Position = FVector2D(ArgReal(Thread, 1), ArgReal(Thread, 2));
		}
		return true;
	}

	// 15.24 (id, degrees) (FUN_00B90AC0): 0 to 360, others ignored
	bool HudRotation(FOKScriptThread& Thread)
	{
		FOKScreenObject* Object = Thread.GetRuntime().GetHud().Find(int32(ArgInt(Thread, 0)));
		const float Degrees = ArgReal(Thread, 1);
		if (Object && Degrees >= 0.0f && Degrees <= 360.0f)
		{
			Object->Rotation = Degrees;
		}
		return true;
	}

	// 7.154 / 7.155: the HUD's width and height (FUN_00830200 / 0210, the viewport's); ours is always 1280 x 720.
	// 7.157 (size): a text size at this height, size * height / 720 (FUN_00830240), so the size itself.
	bool HudWidth(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(OKHud::Width));
		return true;
	}

	bool HudHeight(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(OKHud::Height));
		return true;
	}

	bool HudTextSize(FOKScriptThread& Thread)
	{
		Thread.SetResult(FloatToBits(ArgReal(Thread, 0) * OKHud::Height / 720.0f));
		return true;
	}

	// --- The camera (system W3): one FOKCameraObject, at game info +0x180, that FOKFightCamera reads each frame ---

	uint32 CameraHandle(FOKScriptRuntime& Runtime)
	{
		return ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::Camera);
	}

	/** Argument Index if it's the camera object; 0 otherwise, so the setters below do nothing to anything else. */
	uint32 ArgCamera(FOKScriptThread& Thread, int32 Index)
	{
		const uint32 Camera = CameraHandle(Thread.GetRuntime());
		return Camera != 0 && ArgInt(Thread, Index) == Camera ? Camera : 0;
	}

	// 171.15 (manager) -> the fight camera (FUN_0086C7C0); 151.21 / 171.20 (manager) -> the current camera (virtual +0x28)
	bool CurrentCamera(FOKScriptThread& Thread)
	{
		Thread.SetResult(CameraHandle(Thread.GetRuntime()));
		return true;
	}

	// 169.93 set_camera_variable(camera, index, value) (FUN_0086B490): camera +0x178 + index * 4, for index < 0x44;
	// 169.94 (camera, index) reads one; 169.92 (camera) -> the address of variable 42 (camera +0x220, FUN_0086C0E0)
	bool SetCameraVariable(FOKScriptThread& Thread)
	{
		const uint32 Camera = ArgCamera(Thread, 0);
		const uint32 Index = ArgInt(Thread, 1);
		if (Camera && Index < uint32(FOKCameraObject::NumVariables))
		{
			WriteWord(Thread.GetRuntime(), Camera + FOKCameraObject::Variables + Index * 4, ArgInt(Thread, 2));
		}
		return true;
	}

	bool GetCameraVariable(FOKScriptThread& Thread)
	{
		const uint32 Camera = ArgCamera(Thread, 0);
		const uint32 Index = ArgInt(Thread, 1);
		Thread.SetResult(Camera && Index < uint32(FOKCameraObject::NumVariables) ? ReadWord(Thread.GetRuntime(), Camera + FOKCameraObject::Variables + Index * 4) : 0);
		return true;
	}

	// 171.5 / 171.7 (manager) (FUN_0086A750 / FUN_0086A870): the special camera's modes, +0x3C4 and +0x3C8; 3 is active.
	// Nothing sets them yet, so the fight camera is the one showing.
	template <uint32 Offset>
	bool CameraWord(FOKScriptThread& Thread)
	{
		const uint32 Camera = ArgCamera(Thread, 0);
		Thread.SetResult(Camera ? ReadWord(Thread.GetRuntime(), Camera + Offset) : 0);
		return true;
	}

	bool CameraVariable42(FOKScriptThread& Thread)
	{
		const uint32 Camera = ArgCamera(Thread, 0);
		Thread.SetResult(Camera ? Camera + FOKCameraObject::Variables + 42 * 4 : 0);
		return true;
	}

	// 171.11 (manager, a, b, c) (FUN_0086A6F0): the stage's camera data, +0x378.. (camera_set_bgnd_data). [unk] what the
	// three mean; the Courtyard passes 0, 0, 3.0. Kept, not used.
	bool SetCameraStageData(FOKScriptThread& Thread)
	{
		if (const uint32 Camera = ArgCamera(Thread, 0))
		{
			for (int32 i = 0; i < 3; ++i)
			{
				WriteWord(Thread.GetRuntime(), Camera + FOKCameraObject::BgndData + i * 4, ArgInt(Thread, 1 + i));
			}
		}
		return true;
	}

	// 149.23 (camera, &move, &turn) (FUN_0045E2A0): the shake's offsets, camera +0x124 and +0x130; a null pointer leaves
	// one as it is. The fight camera adds them each frame.
	bool SetCameraShake(FOKScriptThread& Thread)
	{
		const uint32 Camera = ArgCamera(Thread, 0);
		for (int32 Which = 0; Camera && Which < 2; ++Which)
		{
			const uint32 From = ArgInt(Thread, 1 + Which);
			uint32 Words[3] = {};
			if (From != 0 && Thread.LoadBytes(From, Words, sizeof(Words)))
			{
				for (int32 i = 0; i < 3; ++i)
				{
					WriteWord(Thread.GetRuntime(), Camera + (Which == 0 ? FOKCameraObject::ShakePosition : FOKCameraObject::ShakeRotation) + i * 4, Words[i]);
				}
			}
		}
		return true;
	}

	// 149.32 (camera, value) / 149.33 (camera) -> value: camera +0x154 (FUN_0045DC60 / FUN_0045DC80); the shaker clears it
	bool SetCameraShakePaused(FOKScriptThread& Thread)
	{
		if (const uint32 Camera = ArgCamera(Thread, 0))
		{
			WriteWord(Thread.GetRuntime(), Camera + FOKCameraObject::ShakePaused, ArgInt(Thread, 1));
		}
		return true;
	}

	bool GetCameraShakePaused(FOKScriptThread& Thread)
	{
		const uint32 Camera = ArgCamera(Thread, 0);
		Thread.SetResult(Camera ? ReadWord(Thread.GetRuntime(), Camera + FOKCameraObject::ShakePaused) : 0);
		return true;
	}

	// 29.1 (&rotator) -> its pitch, wrapped to -32768..32767 (FUN_0045DDD0)
	bool RotatorPitch(FOKScriptThread& Thread)
	{
		uint32 Pitch = 0;
		const uint32 From = ArgInt(Thread, 0);
		if (From != 0 && Thread.LoadBytes(From, &Pitch, 4))
		{
			Pitch = uint32(int32(int16(uint16(Pitch & 0xFFFF))));
		}
		Thread.SetResult(Pitch);
		return true;
	}

	// 91.1: the ScreenShakePdata class, for is_proc; 91.3: a new one (FUN_0044AA80: +8 and +0x24 are 1)
	bool ClassScreenShakePdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::ScreenShakePdata));
		return true;
	}

	bool NewScreenShakePdata(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Pdata = Runtime.CreateOwned(MakeUnique<FOKScreenShakePdata>(), FOKScreenShakePdata::Bytes);
		WriteWord(Runtime, Pdata + 0x08, 1);
		WriteWord(Runtime, Pdata + 0x24, 1);
		Thread.SetResult(Pdata);
		return true;
	}

	// 91.5 (pdata, process, count, &shake, &vector, flag) (FUN_0044AAF0): +0x24 count, +0x0C the shake, +0x18 the vector,
	// +0x28 the flag, +0x38 / +0x3C 1, +0x2C..+0x34 0; then attaches it to the process (CameraEngine.p_screen_shaker)
	bool SetupScreenShake(FOKScriptThread& Thread)
	{
		uint32 Pdata = 0;
		const FOKScriptObject* Object = Thread.ArgValue(0, Pdata) ? Thread.GetRuntime().ResolveHandle(Pdata) : nullptr;
		if (!Object || !Object->IsA(EOKScriptClass::ScreenShakePdata))
		{
			return true;
		}
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		uint32 Shake[3] = {}, Vector[3] = {};
		Thread.LoadBytes(ArgInt(Thread, 3), Shake, sizeof(Shake));
		Thread.LoadBytes(ArgInt(Thread, 4), Vector, sizeof(Vector));
		WriteWord(Runtime, Pdata + 0x24, ArgInt(Thread, 2));
		for (int32 i = 0; i < 3; ++i)
		{
			WriteWord(Runtime, Pdata + 0x0C + i * 4, Shake[i]);
			WriteWord(Runtime, Pdata + 0x18 + i * 4, Vector[i]);
		}
		WriteWord(Runtime, Pdata + 0x28, ArgInt(Thread, 5));
		WriteWord(Runtime, Pdata + 0x38, 1);
		WriteWord(Runtime, Pdata + 0x3C, 1);
		for (uint32 Offset = 0x2C; Offset <= 0x34; Offset += 4)
		{
			WriteWord(Runtime, Pdata + Offset, 0);
		}
		FOKScriptObject* Target = Runtime.ResolveHandle(ArgInt(Thread, 1));
		if (FOKScriptThread* Process = Target ? Target->AsThread() : nullptr)
		{
			Process->AttachObject(Pdata);
		}
		return true;
	}

	// 94.1: the ArmorPdata class, for is_proc; 94.3: a new one (FUN_00450220)
	bool ClassArmorPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(FOKScriptRuntime::ClassValue(EOKScriptClass::ArmorPdata));
		return true;
	}

	bool NewArmorPdata(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetRuntime().CreateOwned(MakeUnique<FOKArmorPdata>(), FOKArmorPdata::Bytes));
		return true;
	}

	// 94.5 setup_armor(armor, process, obj, value, points, scale, ticks) (FUN_0044A9F0): attaches the armour to the process
	// and fills it in; the points and ticks come as integers and are kept as floats
	bool SetupArmor(FOKScriptThread& Thread)
	{
		uint32 Pdata = 0;
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const FOKScriptObject* Object = Thread.ArgValue(0, Pdata) ? Runtime.ResolveHandle(Pdata) : nullptr;
		if (!Object || !Object->IsA(EOKScriptClass::ArmorPdata))
		{
			return true;
		}
		FOKScriptObject* Target = Runtime.ResolveHandle(ArgInt(Thread, 1));
		if (FOKScriptThread* Process = Target ? Target->AsThread() : nullptr)
		{
			Process->AttachObject(Pdata);
		}
		const int32 Ticks = int32(ArgInt(Thread, 6));
		WriteWord(Runtime, Pdata + 0x08, ArgInt(Thread, 3));
		WriteWord(Runtime, Pdata + FOKArmorPdata::Points, FloatToBits(float(int32(ArgInt(Thread, 4)))));
		WriteWord(Runtime, Pdata + FOKArmorPdata::DamageScale, ArgInt(Thread, 5));
		WriteWord(Runtime, Pdata + FOKArmorPdata::Ticks, FloatToBits(Ticks < 0 ? float(uint32(Ticks)) : float(Ticks)));
		WriteWord(Runtime, Pdata + FOKArmorPdata::Owner, ArgInt(Thread, 2));
		return true;
	}

	// 91.7 (pdata, a, b): +0x34 = a, +0x30 = b (FUN_0044ABA0); 91.8 (pdata, a, b): +0x38 = a, +0x3C = b (FUN_0044AB70)
	template <uint32 OffsetA, uint32 OffsetB>
	bool ScreenShakeWords(FOKScriptThread& Thread)
	{
		uint32 Pdata = 0;
		const FOKScriptObject* Object = Thread.ArgValue(0, Pdata) ? Thread.GetRuntime().ResolveHandle(Pdata) : nullptr;
		if (Object && Object->IsA(EOKScriptClass::ScreenShakePdata))
		{
			WriteWord(Thread.GetRuntime(), Pdata + OffsetA, ArgInt(Thread, 1));
			WriteWord(Thread.GetRuntime(), Pdata + OffsetB, ArgInt(Thread, 2));
		}
		return true;
	}

	// 43.3 (gameinfo) -> the game state (+4, FUN_004066D0); 43.4 (gameinfo, state) sets it: 11 while a fatality or
	// babality can play (MENU-RE-NOTES §3.1)
	bool GetGameState(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult(ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::State));
		return true;
	}

	bool SetGameState(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		WriteWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::State, ArgInt(Thread, 1));
		UE_LOG(LogOKVM, Log, TEXT("Game state %u"), ArgInt(Thread, 1));
		return true;
	}

	// 131.13 (obj) -> whether the object's +0xD88 is set (FUN_008D51D0). [unk] what +0xD88 is; ck_do_fatality asks it
	// before it ends the finishing phase
	bool ObjectD88(FOKScriptThread& Thread)
	{
		Thread.SetResult(ReadWord(Thread.GetRuntime(), ArgInt(Thread, 0) + 0xD88) != 0 ? 1 : 0);
		return true;
	}

	// 137.26 victory_asset(obj) -> the package the character's victory needs, character info +0x88, or 0 when it names
	// none (FUN_00892710)
	bool VictoryAsset(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Name = Fighter ? ReadWord(Runtime, ReadWord(Runtime, Fighter->Handle + FOKFighter::CharacterInfo) + 0x88) : 0;
		uint8 First = 0;
		Thread.SetResult(Name != 0 && Thread.LoadBytes(Name, &First, 1) && First != 0 ? Name : 0);
		return true;
	}

	// 22.8 (process, group, script, function, ...) (FUN_008B64E0): starts a function in a process 22.1 made. 22.1 (?, id)
	// (FUN_0045CC80) makes one. [inf] The scripts use the pair for background package loads (fg_utils), which OpenKombat
	// doesn't need: 22.1 makes none and 22.8 skips a null process.
	bool StartMadeProcess(FOKScriptThread& Thread)
	{
		return ArgInt(Thread, 0) == 0 || Thread.NativeFail(TEXT("22.8: processes made with 22.1 aren't supported yet"));
	}

	// The post-match prompt (msg_boxes.AskToContinueFighting, for a human loser). 122.3 (&response) sets the response to -1
	// (FUN_008D3B60); 37.3 makes the prompt's 0xC-byte data (FUN_00406730: +8 the player); 43.14 asks whether the message box
	// process (id 0x50) is still up (FUN_00406710). The box itself is a menu (M2).
	bool ResetResponse(FOKScriptThread& Thread)
	{
		WriteWord(Thread.GetRuntime(), ArgInt(Thread, 0), 0xFFFFFFFFu);
		return true;
	}

	bool NewMessageBoxData(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetRuntime().CreateOwned(MakeUnique<FOKPdata>(), 0xC));
		return true;
	}

	bool MessageBoxUp(FOKScriptThread& Thread)
	{
		Thread.SetResult(Thread.GetRuntime().FindProcess(0x50) ? 1 : 0);
		return true;
	}

	// 6.110 (FUN_00492D30) -> game flag 0x20, the round is over (gamelogic.round_over sets it, reset_round_clock clears it).
	// The fighters' end_of_round_check, combo_test and check_special_moves stop once it is set. 6.106 (FUN_00492D40) ->
	// flag 0x10, set with 0x400 when the match is won; the winner's victory waits for it.
	bool RoundOverFlag(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult((ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::Flags) >> 5) & 1);
		return true;
	}

	bool MatchOverFlag(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		Thread.SetResult((ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + FOKGameInfo::Flags) >> 4) & 1);
		return true;
	}

	// 7.432 ring_out(obj) (FUN_00893410): 1 when the stage's +0x830 is 1 ([inf] a ring-out stage); otherwise only the tower
	// and story conditions (7.433) can make it 1, and they are 0 in versus
	bool RingOut(FOKScriptThread& Thread)
	{
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		const uint32 Stage = ReadWord(Runtime, Runtime.FindBinding(TEXT("GFGGameInfo")) + 0x1C);
		Thread.SetResult(ReadWord(Runtime, Stage + 0x830) == 1 ? 1 : 0);
		return true;
	}

	// --- After the round: the winner's walk-away and taunt, the loser's getup (FGFightEngineScript.end_of_round_*) ---

	/**
	 * FUN_0089C770: whether a point Distance along a direction from the fighter lies inside the arena (FUN_008B4B70
	 * projects it onto the fight line). The direction is towards the opponent, or the fighter's facing when bFacing is set
	 * or the opponent is straight above.
	 */
	bool PointInArena(const FOKFighter& Fighter, float Distance, bool bFacing)
	{
		if (!Fighter.Body || Fighter.LineLength <= 0.0)
		{
			return true;
		}
		const FVector Here = Fighter.Body->GetWorldLocation();
		FVector Direction = FVector::ZeroVector;
		if (!bFacing && Fighter.Opponent && Fighter.Opponent->Body)
		{
			Direction = (Fighter.Opponent->Body->GetWorldLocation() - Here).GetSafeNormal2D();
		}
		if (Direction.IsNearlyZero())
		{
			Direction = Fighter.Body->GetFacing().GetSafeNormal2D();
		}
		const double Along = FVector::DotProduct(Here + Direction * Distance - Fighter.LineStart, Fighter.LineDirection);
		return Along >= 0.0 && Along <= Fighter.LineLength;		// [inf] MKKE's bounds may carry margins (DAT_00DF0C04 / 0C0C)
	}

	// 137.15 (named is_within_dist) (obj, distance, facing) (FUN_0089CCE0): 1 when the point that far behind is outside
	// the arena, a wall within reach behind; 137.16 (named is_beyond_dist) (FUN_0089C8F0) the same ahead
	bool WallBehind(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && !PointInArena(*Fighter, -ArgReal(Thread, 1), ArgInt(Thread, 2) != 0) ? 1 : 0);
		return true;
	}

	bool WallAhead(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && !PointInArena(*Fighter, ArgReal(Thread, 1), ArgInt(Thread, 2) != 0) ? 1 : 0);
		return true;
	}

	// 137.130 am_i_facing_him(obj, degrees) (fighter virtual +0x194): 1 when the opponent is within that angle of the
	// fighter's facing
	bool FacingOpponent(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		bool bFacing = false;
		if (Fighter && Fighter->Body && Fighter->Opponent && Fighter->Opponent->Body)
		{
			const FVector ToHim = (Fighter->Opponent->Body->GetWorldLocation() - Fighter->Body->GetWorldLocation()).GetSafeNormal2D();
			const FVector Facing = Fighter->Body->GetFacing().GetSafeNormal2D();
			bFacing = FVector::DotProduct(ToHim, Facing) >= FMath::Cos(FMath::DegreesToRadians(ArgReal(Thread, 1)));
		}
		Thread.SetResult(bFacing ? 1 : 0);
		return true;
	}

	// 81.137 (named is_winner_state) (obj) -> fighter +0x2E8 bit 29 (FUN_00446DC0)
	bool FlagBit29(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter ? (ReadWord(Thread.GetRuntime(), Fighter->Handle + FOKFighter::Flags) >> 29) & 1 : 0);
		return true;
	}

	// 81.120 getup_type(obj) (FUN_00455D00): which getup suits how the fighter lies (states 0x200C / 0x200D) and where
	// the opponent is, 0-7 (7 with no opponent). [inf] The angle buckets aren't reproduced yet: 1, lying on the back.
	bool GetupType(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && Fighter->Opponent ? 1 : 7);
		return true;
	}

	// 137.98 xfer_to_new_action(obj, name) (FUN_008A1BE0): the fighter's main thread goes to the named function of its
	// character script. [inf] FUN_008A0090 also clears the attack and frame events and flushes the input first.
	bool XferToNewAction(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		FOKScriptRuntime& Runtime = Thread.GetRuntime();
		FOKScriptInstance* Script = Fighter && Fighter->CharacterScript ? Runtime.GetInstance(*Fighter->CharacterScript) : nullptr;
		const int32 Function = Script ? Script->File->FindFunctionByName(ValueCString(Thread, 1)) : INDEX_NONE;
		if (Function != INDEX_NONE)
		{
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s's thread goes to %s"), Runtime.GetSimFrame(), *Fighter->Name, *ValueCString(Thread, 1));
			OKTransferMainThread(Runtime, *Fighter, (FOKScriptRuntime::InstanceTag << 24) | uint32(Script->Id), uint32(Function + 1));
		}
		return true;
	}

	// 137.103 getup_fast(obj, name) (FUN_0089B590): the main thread goes to the named FGFightEngineScript function;
	// 137.104 / 105 / 106 to getup_with_laydown_check, force_smart_getup and smart_getup_with_death_check
	bool GetupNamed(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		const FString Name = ValueCString(Thread, 1);
		return !Fighter || TransferMainThread(Thread, *Fighter, *Name);
	}

	bool GetupWithLaydown(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || TransferMainThread(Thread, *Fighter, TEXT("getup_with_laydown_check"));
	}

	bool ForceSmartGetup(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || TransferMainThread(Thread, *Fighter, TEXT("force_smart_getup"));
	}

	bool SmartGetupDeathCheck(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		return !Fighter || TransferMainThread(Thread, *Fighter, TEXT("smart_getup_with_death_check"));
	}

	// 81.48 anim_flip_for_state(obj, node) (FUN_00446A00): whether an animation should start mirrored. With node set, the
	// playing node's mirror (FUN_004423A0); otherwise 1 when the mesh is drawn mirrored (+0x758 bit 0) and the opponent is
	// on the right (virtual +0x19C), or neither: a fighter turned away from how it is drawn.
	bool AnimFlipForState(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		if (!Fighter || !Fighter->Body)
		{
			Thread.SetResult(0);
			return true;
		}
		if (ArgInt(Thread, 1) != 0)
		{
			Thread.SetResult(Fighter->Body->IsAnimNodeMirrored() ? 1 : 0);
			return true;
		}
		bool bRight = true;
		if (Fighter->Opponent && Fighter->Opponent->Body)
		{
			const FVector& Line = Fighter->LineDirection;
			bRight = FVector::DotProduct(Fighter->Body->GetWorldLocation(), Line) < FVector::DotProduct(Fighter->Opponent->Body->GetWorldLocation(), Line);
		}
		const bool bMirrored = Fighter->Body->IsMeshMirrored();
		Thread.SetResult(bRight == bMirrored ? 1 : 0);
		return true;
	}

	// 151.2 (obj) -> the object's +8 (FUN_005048E0; also 213.14, 244.32). p_winner_winpose waits while the camera object at
	// game info +0x180 has it set. [inf] a camera move in progress; there is no camera object yet (W3), so 0.
	bool ObjectWord8(FOKScriptThread& Thread)
	{
		const uint32 Object = ArgInt(Thread, 0);
		Thread.SetResult(Object ? ReadWord(Thread.GetRuntime(), Object + 8) : 0);
		return true;
	}

	// 7.650 (obj) -> FUN_00895A90 (CanMove): Moving.game_check_bufferred_moves_upon_exit asks it before a buffered walk or
	// dash, once the round is live
	bool CanMoveNative(FOKScriptThread& Thread)
	{
		FOKFighter* Fighter = Thread.ArgFighter(0);
		Thread.SetResult(Fighter && CanMove(Thread.GetRuntime(), *Fighter) ? 1 : 0);
		return true;
	}

	// 78.32 (info, ?) -> the active fighter's actor (player +0x1C handle, then the object's +8; FUN_00454810). Our fighters
	// are their own actors, so the fighter stands in.
	bool ActiveActor(FOKScriptThread& Thread)
	{
		FOKPlayer* Player = ArgPlayer(Thread, 0);
		Thread.SetResult(Player && Player->Fighter ? Player->Fighter->Handle : 0);
		return true;
	}

	// 22.3 (process) -> its proc id (FUN_0049FFD0); utils.clear_existing_fade asks for its own (@active_vm)
	bool ProcessId(FOKScriptThread& Thread)
	{
		uint32 Handle = 0;
		FOKScriptObject* Object = Thread.ArgValue(0, Handle) ? Thread.GetRuntime().ResolveHandle(Handle) : nullptr;
		FOKScriptThread* Process = Object ? Object->AsThread() : nullptr;
		Thread.SetResult(Process ? uint32(Process->GetProcId()) : 0);
		return true;
	}

	// 6.30 str_equal(a, b) -> _stricmp: 0 when the strings match, ignoring case
	bool StrCompare(FOKScriptThread& Thread)
	{
		Thread.SetResult(uint32(ValueCString(Thread, 0).Compare(ValueCString(Thread, 1), ESearchCase::IgnoreCase)));
		return true;
	}
}

const FOKNativeTable& FOKNativeTable::Get()
{
	// Built once, the first time anything asks. C++ guarantees this runs exactly once.
	static const FOKNativeTable Table = []()
		{
			FOKNativeTable T;

			// Empty functions in the shipped exe (most point at one empty function, 0x8BEBF0).
			const int32 Empty[][2] = {
				{ 1, 11 },{ 1, 13 },{ 1, 14 },{ 1, 15 },{ 1, 16 },{ 1, 17 },{ 1, 18 },{ 1, 20 },
			{ 2, 14 },{ 4, 16 },
			{ 6, 10 },{ 6, 11 },{ 6, 12 },{ 6, 13 },{ 6, 14 },{ 6, 15 },{ 6, 16 },{ 6, 17 },
			{ 7, 32 },{ 7, 33 },{ 7, 35 },{ 7, 73 },{ 7, 74 },{ 7, 75 },{ 7, 76 },{ 7, 439 },
			{ 7, 587 },{ 7, 588 },{ 7, 601 },{ 7, 624 },{ 7, 670 },{ 7, 672 },{ 7, 673 },
			{ 10, 60 },{ 10, 154 },{ 10, 209 },{ 10, 250 },{ 10, 253 },
			{ 12, 10 },{ 12, 11 },{ 14, 25 },
			};
			for (const auto& Entry : Empty)
			{
				T.Add(Entry[0], Entry[1], &NoOp);
			}
			
			T.Add(1, 0, &Sleep);
			T.Add(1, 1, &Jump);
			T.Add(1, 3, &Die);
			T.Add(1, 5, &LoopSleep);
			T.Add(1, 10, &CreateProc);
			T.Add(1, 12, &IsProc);
			T.Add(1, 22, &FindProc);
			T.Add(6, 1, &SetAttackReactions);
			T.Add(6, 70, &StartProc);
			T.Add(6, 73, &StartProc);			// start_obj_proc (FUN_00492630): the same, the object keeping the process in its own
												// list (FUN_00447E40, virtual +0xEC). [inf] Adopting it as start_proc does serves
			T.Add(6, 71, &KillProc);
			T.Add(6, 72, &ProcExists);
			T.Add(6, 81, &Pow);
			T.Add(6, 95, &Random);
			T.Add(7, 16, &SetAttackFoley);

			// Script logging and traces: they only print when debug output is on.
			T.Add(10, 94, &NoOp);		// mks_log
			T.Add(10, 251, &NoOp);		// trace_int
			T.Add(10, 252, &NoOp);		// trace_float

			// "Destroy object": native 0 of every module that shares FUN_008D1080.
			const int32 DestroyModules[] = {
				20, 21, 22, 37, 39, 44, 45, 49, 50, 51, 52, 53, 54, 55, 56, 59, 60, 64, 65, 67, 79, 80, 81, 82,
			83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105,
			106, 107, 109, 110, 111, 112, 114, 116, 117, 121, 132, 133, 137, 138, 139, 140, 141, 142, 144,
			145, 148, 149, 152, 153, 156, 157, 158, 159, 160, 161, 167, 169, 172, 175, 176, 177, 178, 179,
			180, 181, 194, 195, 196, 197, 198, 199, 214, 216, 217, 218, 246, 247, 248, 249, 251, 252,
			};
			for (const int32 Module : DestroyModules)
			{
				T.Add(Module, 0, &DestroyObject);
			}
			
			// "vec_zero" and eight natives that share its function: they read argument 0 and discard it.
			const int32 ReadsAndDiscards[][2] = { { 27, 5 },{ 28, 2 },{ 29, 3 },{ 36, 8 },{ 62, 19 },{ 63, 3 },{ 70, 3 },{ 73, 3 },{ 173, 5 } };
			for (const auto& Entry : ReadsAndDiscards)
			{
				T.Add(Entry[0], Entry[1], &NoOp);
			}

			T.Add(43, 34, &SetColorScale);

			// set_head_track_target(obj, ticks): MKKE does nothing without a mesh component (+0x10C).
			T.Add(59, 138, &NoOp);

			T.Add(59, 137, &NoOp);		// set_head_tracking(obj, frames): head tracking comes with the camera and opponent

			T.Add(59, 29, &BlendToAni);
			T.Add(59, 37, &SetRootMotionScale);
			T.Add(59, 40, &AnimLastFrame);
			T.Add(59, 42, &AnimCurrentFrame);
			T.Add(59, 46, &SetAnimRate);
			T.Add(59, 47, &AnimRate);
			T.Add(81, 51, &BlendToAni);
			T.Add(81, 52, &BlendToAni);
			T.Add(81, 119, &AnimFlipping);

			T.Add(81, 166, &NewCollisionContext);

			T.Add(95, 1, &ClassMiscPdata);
			T.Add(95, 3, &NewMiscPdata);
			T.Add(95, 5, &AttachToProcess);

			T.Add(137, 118, &LaunchAttack);
			T.Add(137, 226, &ResetFrameEventsNative);
			T.Add(52, 87, &ObjGetPos);
			// Reaction tags (rx_tag_*): metadata for MK's tools; they do nothing in the shipped game.
			for (const int32 Index : { 206, 207, 208, 209, 211, 212, 213, 215, 219, 222, 223, 225, 226 })
			{
				T.Add(6, Index, &NoOp);
			}
			T.Add(81, 182, &RegisterMiss);
			T.Add(7, 401, &Silent);		// sound_event2
			for (const int32 Index : { 3, 5, 6, 7, 9, 10, 12, 13, 17, 18, 20, 23, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 36, 41, 42, 43, 44 })
			{
				T.Add(193, Index, &Silent);
			}
			T.Add(193, 1, &PlaySound);
			T.Add(193, 2, &PlaySound);
			T.Add(6, 67, &VmSelf);
			// GameInfo's callback tables; on other objects these virtuals are something else, still 0
			T.Add(43, 40, &FightEngineTable);
			T.Add(136, 57, &FightEngineTable);
			T.Add(43, 41, &SoundTable);
			T.Add(136, 58, &SoundTable);
			T.Add(43, 42, &CameraTable);
			T.Add(136, 59, &CameraTable);
			T.Add(43, 43, &EffectsTable);
			T.Add(136, 60, &EffectsTable);
			T.Add(43, 44, &MovingTable);
			T.Add(136, 61, &MovingTable);
			T.Add(248, 14, &ObjectCallbackTable);
			T.Add(252, 15, &ObjectCallbackTable);
			T.Add(81, 167, &CurrentCollisionContext);
			T.Add(7, 400, &NoOp);		// sound_event(obj, hash): FMOD events come with audio
			T.Add(7, 654, &IsOnline);
			T.Add(6, 251, &LastContactHit);
			T.Add(6, 252, &LastContactMissed);
			T.Add(6, 253, &LastContactBlocked);
			T.Add(23, 4, &ResolveHandle);
			T.Add(79, 1, &ClassCollisionContext);
			T.Add(81, 162, &CollisionScanFlag);
			T.Add(92, 1, &ClassAnimEventPdata);
			T.Add(137, 90, &CollisionScan);
			T.Add(137, 92, &CheckAttackCollision);
			T.Add(137, 93, &CheckAttackCollisionSimple);
			T.Add(137, 152, &CanHit);
			T.Add(137, 232, &CancelFrameEvents);

			T.Add(137, 227, &AddFrameEvent);
			T.Add(137, 228, &AddArmedFrameEvent);
			T.Add(137, 229, &ArmFrameEvents);
			T.Add(137, 230, &SetFrameEventParam);
			// Effects, the camera, rumble, AI events and face animation: no system yet, so they return 0.
			const int32 ZeroForNow[][2] = {
				{ 1, 25 },{ 5, 26 },{ 6, 84 },{ 6, 195 },{ 6, 205 },{ 22, 22 },{ 22, 23 },
			{ 59, 34 },{ 59, 116 },{ 78, 47 },{ 78, 104 },{ 81, 224 },
			{ 131, 9 },
			// particles and FX objects (EffectsEngine): spawning returns no object, so these act on none
			{ 6, 115 },{ 6, 120 },{ 6, 121 },{ 81, 234 },{ 52, 39 },{ 52, 44 },{ 52, 45 },{ 52, 46 },{ 52, 53 },{ 52, 66 },{ 52, 94 },
			{ 82, 9 },{ 82, 14 },{ 137, 200 },	// 137.200 fx_proc(obj, function) -> an effect object (FUN_00896230)
			// the quick-blur trail (81.131 quickblur_clear is already a no-op)
			{ 81, 128 },{ 81, 129 },{ 81, 130 },{ 81, 132 },{ 81, 133 },{ 81, 134 },{ 81, 135 },
			};
			// Particle effects (W2)
			T.Add(82, 3, &FxCreate);
			T.Add(6, 123, &PlayFxAtPos);
			T.Add(137, 191, &PlayCharFx);
			T.Add(52, 54, &FxAttach);
			T.Add(52, 61, &FxStart);
			T.Add(52, 95, &ObjSetPosVec);
			T.Add(52, 41, &FxSetScale);
			T.Add(82, 19, &FxScale);
			T.Add(52, 96, &FxSetToFacing);
			T.Add(52, 89, &FxGetFacing);
			T.Add(82, 21, &FxStopEmitter);
			T.Add(82, 20, &FxActivate);
			T.Add(82, 17, &FxBloodTint);
			T.Add(30, 2, &MakeName);
			T.Add(137, 64, &FaceLike);
			T.Add(52, 91, &ObjGetRot);
			T.Add(52, 98, &ObjCopyRotation);
			T.Add(137, 231, &SetFrameEventText);

			// Reactions (SharedReactions)
			T.Add(59, 41, &AnimFrameCount);
			T.Add(81, 1, &ClassCharacterObj);

			// combo_test (FightEngineScript): what it asks before chaining into the next attack.
			T.Add(6, 110, &RoundOverFlag);
			T.Add(6, 112, &MovesetAnimEndFrame);
			T.Add(24, 4, &NoOp);				// (list): empties an object list (FUN_0042D6A0), e.g. the player's at +0xC78 that the ice clone
												// uses; 24.6 adds nothing to one yet, so there is nothing to empty
			T.Add(78, 54, &InputHeldFacing);
			T.Add(137, 174, &ReturnZero);		// has_buff_zombie (FUN_00895560): fighter +0x7744 is "zombie", only in the Challenge Tower or with
												// game info +0x14 bit 0x20; neither here
			T.Add(7, 432, &RingOut);
			T.Add(137, 15, &WallBehind);
			T.Add(81, 48, &AnimFlipForState);
			// FGFightEngineScript.p_anim_event_scanner: for each active fighter not held by 52.16, 137.225 scans its frame events.
			// OpenKombat fires them from the fighter actor as its animation steps (OKFireFrameEvents), so 137.225 does nothing.
			T.Add(52, 16, &ReturnZero);			// anim_event_obj(obj) (FUN_00433830): the actor's +0x138 > 0. [inf] a cinema holds it
			T.Add(78, 32, &ActiveActor);
			T.Add(137, 225, &NoOp);
			T.Add(7, 650, &CanMoveNative);
			T.Add(151, 2, &ObjectWord8);
			T.Add(6, 199, &ReturnZero);			// is_animating(info) (FUN_008B4190): 1 while the active fighter plays the animation its virtual
												// +0x1A8 record names (+0x10). [inf] a scripted cinema; none yet, so wait_for_me_to_finish_animating waits
			T.Add(137, 16, &WallAhead);
			T.Add(137, 130, &FacingOpponent);
			T.Add(81, 137, &FlagBit29);
			T.Add(81, 120, &GetupType);
			T.Add(137, 98, &XferToNewAction);
			T.Add(137, 103, &GetupNamed);
			T.Add(137, 104, &GetupWithLaydown);
			T.Add(137, 105, &ForceSmartGetup);
			T.Add(137, 106, &SmartGetupDeathCheck);
			T.Add(137, 65, &NoOp);				// force_reaction(obj, n) (FUN_008A2540): the KO handler for tag partners, ring-outs and stage
												// fatalities; in a 1-on-1 fight without them it does nothing
			T.Add(7, 431, &ReturnZero);			// FUN_00893350: the tower or story mode 5; 0 in versus
			T.Add(6, 69, &ReturnZero);			// (obj, a, b) (FUN_008F8F60): the stage script's bgnd_bodyslam_effects; no stage scripts yet (W5)
			T.Add(137, 186, &NoOp);				// game_event_babality (FUN_0089F830): babalities (F8)
			T.Add(6, 106, &MatchOverFlag);
			T.Add(6, 201, &NoOp);				// combo_test(info): tells the player's AI drone (FUN_00890010); no AI yet
			// The player's input buffer (player +0xB40) answers these; until it exists nothing is pressed.
			T.Add(78, 67, &ReturnZero);			// input_pressed(info, code, ?, consume): FUN_00451FB0
			T.Add(78, 69, &ReturnZero);			// input_pressed_dir(info, code, dir, ?, consume): FUN_00446180
			T.Add(78, 24, &ReturnZero);			// last_input(info, &button, &dir): FUN_00426EC0
			// combo_branch(obj, n, &button, &dir) -> the move's n-th follow-up (FUN_00449AC0, from the move lists
			// built from the attack function names, ANIM-RE-NOTES §3). No lists yet, so no follow-ups.
			T.Add(137, 236, &ReturnZero);

			// The character bootstrap
			for (const auto& Entry : MovesetNatives)
			{
				T.Add(7, Entry[0], &MovesetEntry);
			}
			T.Add(7, 534, &MovesetAltStance);
			T.Add(7, 535, &MovesetSecondary);
			T.Add(137, 95, &Moveset);
			T.Add(81, 136, &Moveset);
			T.Add(81, 138, &MovesetAnimFlag);
			T.Add(81, 215, &BindCommand);
			T.Add(66, 2, &CommandInit);
			T.Add(28, 4, &SetFour);
			T.Add(147, 5, &ZeroSix);
			// Constructors __global__ runs on other kinds of globals. They write C++ vtables, handles ({0, -1}) and
			// counts the file already holds as zero, so nothing here needs them. 30.2 makes names (MakeName).
			const int32 Constructors[][2] = { { 108, 2 }, { 115, 2 }, { 26, 2 }, { 23, 2 }, { 69, 14 }, { 61, 2 }, { 25, 2 }, { 46, 5 },
				{ 146, 2 }, { 71, 2 }, { 127, 2 } };
			for (const auto& Entry : Constructors)
			{
				T.Add(Entry[0], Entry[1], &NoOp);
			}
			T.Add(66, 10, &RegisterSpecial);
			T.Add(66, 11, &RegisterSpecialDist);
			T.Add(66, 12, &RegisterExSpecial);
			T.Add(66, 13, &RegisterExSpecialDist);
			T.Add(66, 15, &RegisterFatality);
			T.Add(66, 16, &RegisterBabality);
			T.Add(66, 17, &RegisterTagAssist);
			T.Add(66, 18, &RegisterTagSpecial);
			T.Add(66, 19, &RegisterXray);
			T.Add(66, 20, &RegisterXrayAir);
			T.Add(66, 23, &CommandWords11C);
			// Props, art-tool settings and looks set up by *_game_init: nothing to show them on yet.
			T.Add(6, 232, &NoOp);				// tweak_var: an empty function in the shipped exe
			T.Add(7, 589, &ReturnZero);			// a debug flag (DAT_00E0B8D8 != 0)
			for (const int32 Index : { 590, 591, 592, 593, 594, 595, 597, 598, 599, 600, 603, 604 })
			{
				T.Add(7, Index, &ReturnZero);	// arttool_loop and friends: all return 0 in the shipped exe (FUN_008BEBC0)
			}
			T.Add(8, 11, &NoOp);				// reveal_add_receiver: blood decals
			T.Add(14, 0, &ReturnZero);			// (name): is that package loaded (FUN_00810840); the alternate costumes aren't
			T.Add(52, 56, &ReturnZero);			// fx_attach_socket
			T.Add(59, 101, &NoOp);				// set_physics
			T.Add(59, 103, &NoOp);				// set_cast_shadow
			T.Add(61, 6, &NoOp);				// (list, weapon, socket): adds a "Weapon" prop entry (FUN_008B59F0)
			T.Add(137, 202, &ReturnZero);			// play_anim_fx(obj, &anim) -> the FX object: none until W1, and callers test for 0

			// Leaving an attack
			T.Add(81, 235, &SetBreakoutFrames);
			T.Add(137, 169, &BreakoutTest);
			T.Add(137, 97, &ExitToStanceNow);
			T.Add(81, 189, &ExitToStanceNow);
			T.Add(137, 187, &GameEventExit);
			T.Add(137, 224, &TakeControl);
			T.Add(137, 82, &InvalidStateRecover);
			T.Add(81, 227, &InvalidStateRecover);
			T.Add(137, 83, &DuckReturnFromEntry);
			T.Add(81, 229, &DuckReturnFromEntry);
			T.Add(137, 84, &DuckToStand);
			T.Add(81, 228, &DuckToStand);
			T.Add(137, 157, &ReturnZero);		// is_holding_block: asks the input buffer (FUN_00895310); nothing is held
			T.Add(131, 8, &NoOp);				// online only: tells the network component (FUN_008F0E90)

			// Stance
			T.Add(81, 213, &EnableCommands);
			T.Add(81, 111, &StanceAnim);
			T.Add(59, 50, &CurrentAnimation);
			T.Add(81, 196, &BlendToCorrectStance);
			T.Add(137, 120, &BlendToStanceNative);
			T.Add(81, 65, &InitGroundMove);
			T.Add(81, 35, &IsAirborne);
			T.Add(81, 41, &StopNative);
			T.Add(81, 67, &InitAirMove);
			T.Add(81, 68, &InitStillMove);
			T.Add(81, 69, &Init3dMove);
			T.Add(81, 87, &AfterLandInput);
			T.Add(81, 91, &LaunchAndLandEx);
			T.Add(81, 92, &LaunchAndLand);
			T.Add(81, 93, &LaunchToHeight);
			T.Add(81, 94, &LaunchToHeightGuaranteed);
			T.Add(81, 95, &ForceAwayFromOpponent);
			T.Add(81, 97, &StopForceAway);
			T.Add(81, 98, &SetAirborne);
			T.Add(81, 103, &IsLanded);
			T.Add(81, 107, &SetAirSpeed);
			T.Add(81, 246, &SnapToGround);
			T.Add(81, 247, &HipsOnFloor);
			T.Add(137, 146, &SetMovingSpeedup);
			T.Add(81, 117, &OpponentOnRight);
			T.Add(59, 51, &AnimNode);
			T.Add(58, 43, &SetAnimFrameNative);
			T.Add(2, 8, &GameEventMine);
			T.Add(81, 249, &BoneIdWithFlip);
			T.Add(59, 78, &BoneIdWithFlip);
			T.Add(81, 243, &BoneWorldPosition);
			T.Add(59, 82, &BoneWorldPosition);
			T.Add(81, 244, &BoneWorldPosition);
			T.Add(42, 1, &StageFloorHeight);
			T.Add(137, 252, &SetAttackCallback);
			// Checks that only act with systems OpenKombat doesn't have yet:
			T.Add(6, 203, &NoOp);			// check_buffered_moves(info): the side's AI drone only (FUN_0088BC60)
			T.Add(137, 154, &NoOp);			// tag_swap_flag(obj): a call through character info +0x40 (FUN_0089DBC0); 0 for every fighter here
			T.Add(118, 32, &NoOp);			// xray_check_ground(info): an X-ray breaker when the meter allows one (FUN_008A9680); no meter yet (F6)
			T.Add(137, 221, &NoOp);			// back_to_normal_input(obj): the attack buffered in the air (+0x39C), started on landing
											// (FUN_008A7A20). [inf] Air buffering isn't recorded yet, so there is never one.
			T.Add(6, 249, &NoOp);				// debug_obj: an empty function in the shipped exe
			T.Add(7, 426, &ReturnZero);			// a mode test (FUN_00893300): 0 unless DAT_00E91C64 is set

			T.Add(81, 236, &Opponent);
			T.Add(137, 287, &PlayerEntry);

			// The idle loop
			T.Add(6, 231, &GameEvent);
			T.Add(81, 184, &GetState);
			T.Add(81, 179, &OpponentFlag);
			T.Add(2, 10, &NoOp);				// (obj, move hash) (FUN_0080E0D0): a finisher's stats and telemetry event (0xCC)
			T.Add(81, 231, &SetInvulnerable);
			T.Add(81, 70, &SetCharFlags);
			T.Add(81, 241, &ConstrainEnable);
			T.Add(81, 242, &ConstrainDisable);
			T.Add(137, 112, &SetThrowFlag);
			T.Add(137, 274, &ClearFlag40);
			T.Add(137, 159, &AmIDucking);
			T.Add(6, 200, &PlyrBackToNormal);
			T.Add(68, 3, &ResetAttack);
			T.Add(6, 74, &KillObjProc);
			T.Add(118, 48, &ComboMessage);
			T.Add(118, 47, &LifeQuery);
			T.Add(137, 7, &FighterPlayer);
			T.Add(137, 173, &OnlineFlagB);
			T.Add(1, 6, &XferThread);
			T.Add(6, 243, &OnlineSide);
			T.Add(78, 86, &Input2in1);
			T.Add(137, 10, &BeginSpecialMoveNative);
			T.Add(137, 207, &XferToSpecialMoveNative);
			T.Add(137, 208, &XferToExSpecialMove);
			T.Add(7, 640, &CommandMatchedAir);
			T.Add(7, 641, &CommandMatchedEx);
			T.Add(7, 642, &CommandMatchedAirEx);
			// 6.244 (named tag_assist_blocked) obj: online flags 0x1040000 in an online mode (FUN_008F1990). Never, offline.
			T.Add(6, 244, &OnlineAbort2in1);
			// 7.58 partner_alive(obj) (FUN_00892BD0): the tag partner can come in. [inf] One fighter a side here.
			T.Add(7, 58, &ReturnZero);
			T.Add(59, 49, &AnimAtEnd);
			T.Add(59, 22, &MeshRenderFlags);
			T.Add(137, 13, &ReturnZero);		// see SpawnSkelActor
			T.Add(59, 8, &SpawnSkelActor);
			T.Add(59, 71, &SetHidden);
			T.Add(52, 30, &SetHidden);
			T.Add(81, 8, &SetHidden);
			T.Add(81, 131, &NoOp);		// quickblur_clear(obj): ends the fighter's motion-blur trail (FUN_00447550). [inf] No trails yet.
			T.Add(81, 123, &MaterialsRestore);
			T.Add(81, 122, &NoOp);			// materials_override(obj, ...): swaps the fighter's materials for an effect (the fire hand); no material effects yet
			T.Add(59, 63, &MaterialsRestore);
			T.Add(81, 204, &DisableAllCommandsNative);
			T.Add(7, 636, &CharacterFlag40);
			// force_away_facing(obj, speed, type, ?, ticks) -> object virtual +0x168 (FUN_0044BA10): pushes the fighter along
			// the line to its opponent, away when speed is negative (virtual +0x1F4). [inf] Fighters don't move yet.
			T.Add(81, 96, &ForceAwayFacing);
			T.Add(6, 26, &VecRotation);
			T.Add(59, 114, &ObjYaw);
			T.Add(59, 69, &IsFlipped);
			T.Add(59, 26, &SetupForFlip);
			T.Add(64, 5, &NoOp);
			T.Add(59, 48, &AnimIsLooping);
			T.Add(81, 207, &EnableAttacksOnly);
			T.Add(78, 52, &InputHeldCommand);
			T.Add(78, 50, &InputJustPressed);
			T.Add(78, 83, &InputMotion);
			T.Add(78, 75, &InputSwallowPress);
			T.Add(7, 637, &OnlineBufferedExit);
			T.Add(81, 116, &IsOpponentBehind);
			T.Add(7, 674, &OnlineAbort2in1);
			T.Add(6, 245, &ComboInputState);
			T.Add(6, 202, &AIDroneStep);
			T.Add(6, 7, &VmCall1);
			T.Add(7, 639, &CommandMatched);
			T.Add(137, 172, &OnlineFlag30);
			T.Add(59, 91, &MorphTargetSet);
			T.Add(118, 3, &GetActiveChar);
			T.Add(78, 33, &GetActiveChar);
			T.Add(136, 31, &GetSidePlayer);
			T.Add(6, 230, &ControllerFeedback);
			T.Add(137, 233, &ClearAttack);
			T.Add(118, 4, &GetPartnerChar);
			T.Add(137, 9, &SetCharLayer);
			T.Add(81, 256, &NoOp);				// (obj, value): sets +0x58 of every mesh material entry (FUN_0044EA70)
			T.Add(137, 277, &NoOp);				// virtual +0xE4 (FUN_00897310): releases tag partners (ReleaseCharacters)
			T.Add(81, 18, &NoOp);

			// Input scanners
			T.Add(137, 38, &CheckSpecialMoves);
			T.Add(137, 39, &BindCharFuncSlot);
			T.Add(137, 40, &ClearCharFuncSlots);
			T.Add(81, 53, &BlendToAni);			// blend_to_ani_frame(obj, anim, startFrame, rate, blendFrames, ...): virtual +0x104
			T.Add(81, 61, &SetMoveFlag);
			T.Add(81, 185, &SetState);
			T.Add(81, 214, &DisableCommands);
			T.Add(118, 45, &TeamLife);
			T.Add(23, 6, &ReleaseHandle);
			T.Add(23, 3, &DestroyHandle);
			T.Add(23, 5, &SetHandle);
			T.Add(137, 158, &ReturnZero);		// is_ducking_state: nobody ducks yet (FUN_00895330 tests states 4, 0x1F, 0x20)
			T.Add(137, 149, &NoOp);				// set_reaction_sounds(obj, table): the sounds come with audio
			T.Add(78, 88, &AdjustDamageMultiplier);
			T.Add(43, 6, &GameMode);			// game mode (GGameInfo +8): IsBloodOn tests 6, 12 and 13
			// Rounds and the match (gamelogic.ExecuteFight, FIGHT-RE-NOTES §8b)
			T.Add(136, 16, &ResetRoundClock);
			T.Add(36, 5, &SettingsRoundsToWin);
			T.Add(36, 3, &SettingsRoundTime);
			T.Add(43, 7, &GameFlagTest);
			T.Add(118, 46, &SetLifeNative);
			T.Add(118, 7, &FightStarted);
			T.Add(78, 77, &ForgetDirection);
			T.Add(78, 78, &CatchUpInput);
			T.Add(78, 95, &PopControl);
			T.Add(118, 40, &OpponentPlayer);
			T.Add(118, 42, &OpponentCharacter);
			T.Add(6, 30, &StrCompare);
			T.Add(6, 64, &ReturnZero);			// game_flag_set(mask), also 6.65: FUN_0041DD10 is 0 on PC
			T.Add(6, 65, &ReturnZero);
			T.Add(9, 1, &ReturnZero);			// (mask) -> whether the boot flags have all those bits (FUN_004D4560); none are set
			T.Add(43, 20, &ReturnZero);			// DAT_00E43054: when set, the round clock doesn't run. [inf] a debug pause
			T.Add(43, 12, &ReturnZero);			// FUN_00406760: an online session's state isn't 1; 0 offline
			T.Add(7, 421, &ReturnZero);			// DAT_00E89BD8 & 1. [inf] a replay or no-stats fight; 0, so the stats run
			T.Add(10, 56, &ReturnZero);			// FUN_0081C430: online, ends the fight loop early
			T.Add(10, 27, &ReturnZero);			// FUN_0081C240: online
			T.Add(10, 52, &ReturnZero);			// FUN_0081C340: online
			T.Add(78, 35, &ReturnZero);			// (info) -> the handle at player +0x28 (FUN_0044CB60). [inf] none in a 1-on-1 fight
			T.Add(137, 99, &ReturnZero);		// start_death_ender(obj) -> the character's p_death_ender, when its info names one (FUN_0089B270)
			T.Add(2, 0, &NoOp);					// debug text (FUN_008BEBF0 is empty on PC); also 9.5, 14.14
			T.Add(9, 5, &NoOp);
			T.Add(14, 14, &NoOp);
			T.Add(6, 233, &NoOp);				// endurance_head_tracking (FUN_00492580)
			// The HUD (system W7) isn't built: 7.128 and 7.218 make and free its objects, 7.131 places it, 7.135 / 7.136 read where.
			T.Add(7, 128, &NoOp);
			T.Add(7, 218, &NoOp);
			T.Add(7, 131, &NoOp);
			T.Add(7, 135, &ReturnZero);
			T.Add(7, 136, &ReturnZero);
			// Stats, trophies, the win streak, profiles and saves (system M4) aren't kept. The stats path asks whether a player is
			// human (78.30) and for its profile (78.31, 78.45, 174.11, 174.33); with 0 it records nothing.
			const int32 NoStats[][2] = {
				{ 7, 34 }, { 7, 78 }, { 7, 79 }, { 7, 80 }, { 7, 81 }, { 7, 83 }, { 7, 84 }, { 7, 86 }, { 7, 91 }, { 7, 92 }, { 7, 95 },
				{ 7, 98 }, { 7, 100 }, { 7, 107 }, { 7, 109 }, { 7, 111 }, { 7, 112 }, { 7, 116 }, { 7, 656 }, { 7, 415 }, { 7, 418 },
				{ 10, 202 }, { 10, 205 }, { 10, 206 }, { 10, 214 }, { 10, 114 }, { 10, 227 }, { 14, 32 }, { 78, 30 }, { 78, 31 }, { 78, 45 },
				{ 118, 6 }, { 136, 10 }, { 201, 2 }, { 201, 13 }, { 201, 14 }, { 201, 17 }, { 4, 53 }, { 137, 23 }, { 137, 25 }, { 7, 436 },
				{ 174, 11 }, { 174, 16 }, { 174, 33 }, { 7, 369 }, { 200, 1 }, { 7, 376 }, { 6, 39 }, { 2, 1 }, { 2, 3 } };
			for (const auto& Entry : NoStats)
			{
				T.Add(Entry[0], Entry[1], &ReturnZero);
			}
			// Online (system M5): announcements and round results go nowhere.
			T.Add(10, 19, &ReturnZero);
			T.Add(10, 73, &NoOp);
			T.Add(10, 221, &NoOp);
			T.Add(7, 433, &ReturnZero);			// FUN_008933A0: 1 in the tower, story and a mode flag; 0 in versus
			T.Add(7, 228, &ReturnZero);			// ring_out_flag (FUN_0091FEC0): no ring-outs yet
			// The HUD's Scaleform movies (W7): none are loaded, so the scripts skip them.
			T.Add(44, 3, &ReturnZero);
			T.Add(44, 5, &NoOp);
			T.Add(129, 14, &NoOp);
			T.Add(44, 1, &ReturnZero);			// the MoveMKHUDdata class, for is_proc; with none, MKHUD.p_slide_MKHUD_on_Screen ends
			T.Add(7, 464, &NoOp);				// drone_round_init(info) (FUN_00891EB0): the AI drone's round start (F9)
			T.Add(6, 55, &ReturnZero);
			T.Add(6, 56, &NoOp);
			T.Add(7, 147, &ShowRoundText);
			T.Add(7, 148, &ShowFightText);
			T.Add(7, 623, &NoOp);				// (n) (FUN_00893B80). [inf] an arttool hook
			T.Add(7, 602, &ReturnZero);			// arttool story auto-kill: off
			T.Add(16, 4, &ReturnZero);			// FUN_0041DF00: a storage device check; 16.1 runs when it says so
			T.Add(16, 1, &NoOp);
			T.Add(22, 3, &ProcessId);
			// The finishing phase (gamelogic.ck_do_fatality, FIGHT-RE-NOTES §8b.5)
			T.Add(43, 3, &GetGameState);
			T.Add(43, 4, &SetGameState);
			T.Add(131, 13, &ObjectD88);
			T.Add(137, 26, &VictoryAsset);
			T.Add(20, 1, &ClassScript);			// ck_fatality_available: is_proc(the winner's script, 20.1), then its "fatalities"
			T.Add(21, 5, &FindGlobalNative);	// global (21.5) must have elements (21.6)
			T.Add(21, 6, &GlobalCountNative);
			// A finisher's cinema (F8, FIGHT-RE-NOTES §8b.5): play_fatality_cinema, p_fatality_cinema, fg_utils' scene calls
			T.Add(138, 1, &ClassFatalityCinemaPdata);
			T.Add(138, 3, &NewFatalityCinemaPdata);
			T.Add(22, 5, &FindProc);			// the running process's attached object (FUN_004584C0), here its request
			T.Add(39, 1, &ClassCinema);
			T.Add(39, 51, &CinemaPrepare);
			T.Add(39, 19, &CinemaLoad);
			T.Add(39, 35, &CinemaBindActor);
			T.Add(39, 11, &CinemaPlay);
			T.Add(39, 17, &CinemaPlaying);
			T.Add(39, 31, &CinemaWait);
			T.Add(39, 30, &CinemaPosition);
			T.Add(39, 14, &CinemaSkip);
			T.Add(39, 16, &ReturnZero);			// (cinema): nonzero while the scene is still being torn down (FUN_004B1B00)
			T.Add(39, 13, &NoOp);				// cinema_unload: unloads its level when it ends (FUN_004B20B0)
			// [design] FOKCinemaPlayer puts the scene on the attacker, facing its way, so the scene's own placement goes unused:
			T.Add(39, 20, &NoOp);				// cinema_orient(cinema, &rotation): +0x14 (FUN_004B20D0)
			T.Add(39, 21, &NoOp);				// cinema_position(cinema, &position): +0x08 (FUN_004B1B50)
			T.Add(39, 25, &NoOp);				// cinema_mirror(cinema, a, b): +0x20, +0x24 (FUN_004B1C00)
			T.Add(39, 27, &NoOp);				// cinema_common_play(cinema, value): +0x44
			T.Add(39, 40, &NoOp);				// cinema_actor_flags(cinema, obj, n): the binding's +0x14 = 1, +0x18 = n (FUN_004B3180)
			T.Add(39, 39, &NoOp);				// cinema_duplicate_actor(cinema): GenericFatalityPostloadCallback; MKSeqAct_DuplicateActor
			T.Add(7, 408, &NoOp);				// cinema_asset(obj, &vector) (FUN_00491D30); the vector stays zero
			T.Add(6, 8, &VmCall2);
			T.Add(6, 9, &VmCall4);
			T.Add(6, 77, &Acos);
			T.Add(137, 218, &XferToAction);
			T.Add(10, 36, &ReturnOne);			// WaitForFatalityAssets: the finisher's packages are in (OpenKombat reads them when needed)
			T.Add(10, 37, &ReturnOne);			// localasyncloadcomplete
			T.Add(10, 38, &ReturnOne);			// isasyncloadcomplete (FUN_0081C100)
			T.Add(40, 2, &NoOp);				// (&info): constructs the cinema startup info on the frame (FUN_004B2180); InitializeCinemaStartupInfo fills it
			T.Add(151, 8, &NoOp);				// (camera, &vector, 0, ?): camera virtual +0x10. [inf] where the fight camera looks
			T.Add(169, 58, &NoOp);				// (camera, ...) (FUN_0086EF40): fills the scene placement vectors. [unk]
			T.Add(171, 17, &ReturnZero);		// (GGameInfo+0x180) -> the camera blender (FUN_0086C940); 170.9 blends it
			T.Add(170, 9, &NoOp);
			T.Add(81, 89, &NoOp);				// ring_out_flag(obj): fighter virtual +0x14C. [unk]
			T.Add(81, 193, &NoOp);				// chunk_shadows(obj, on) (FUN_0044EE70): the cut pieces' shadows
			T.Add(89, 3, &ReturnZero);			// (id, mesh, scale): the red FatalityCube backdrop (a 0xFC-byte static mesh object). Not drawn yet
			T.Add(89, 6, &NoOp);
			T.Add(6, 173, &NoOp);				// objmic_scalar_interp(obj, param, ...): its material's "Transition"
			T.Add(101, 3, &ReturnZero);			// a fade's parameters (0x1C bytes); utils.fade_to_black does the fade
			T.Add(101, 5, &NoOp);
			T.Add(193, 6, &NoOp);				// sound_signal(proc, n): music cues
			T.Add(5, 31, &NoOp);				// Fatality1CinemaPostPlayCallback. [unk]
			T.Add(7, 217, &FatalityEffect);
			T.Add(43, 45, &FrameRate);
			T.Add(7, 626, &FrameRate);			// to_seconds' divisor: the same function
			// A victory cinema (FGFightEngineScript.p_victory_cinema), on the same scene natives
			T.Add(7, 241, &NoOp);				// (1) (FUN_008879A0): a global flag, set while a victory cinema runs. [unk] its readers
			T.Add(7, 627, &ReturnOne);			// load_character(package) (FUN_00887DB0): whether it's loaded; OpenKombat reads it when it plays
			T.Add(151, 4, &NoOp);				// (camera, 1): camera virtual. [inf] hands the view over
			T.Add(6, 196, &NoOp);				// victory_cinema_time(120f) (FUN_00B8FDF0): a global time. [unk] what reads it
			T.Add(137, 81, &WinEffectInTicks);
			T.Add(6, 111, &ReturnZero);			// moveset_anim_index(anim) (FUN_0044AD80): 1 for an anim with more than one variant
												// (+0xAC, +0x1C > 1). [inf] 0: blend_to_moveset_anim_frame takes the plain blend
			T.Add(7, 127, &ReturnZero);			// curve_value(name): a HUD animation curve (Finish_TopText_Scale); without one,
												// p_FinishHimEffect holds its sprites still and removes them after 130 frames (W7)
			T.Add(10, 254, &ReturnOne);			// FUN_0087FED0: 1
			T.Add(10, 39, &NoOp);				// endasyncload: online (FUN_0081C130)
			// DoFinishHimEffect: 195.3 a parameter block, 195.5 attaches it to p_FinishHimEffect with the winner's side at +8,
			// and that process finds it (find_proc, is_proc 195.1). [inf] a FinishHimPdata; a MiscPdata serves.
			T.Add(195, 1, &ClassMiscPdata);
			T.Add(195, 3, &NewMiscPdata);
			// Package slots (A2): fatality and victory art loads in the background; OpenKombat loads what it needs when asked.
			T.Add(7, 65, &ReturnZero);
			T.Add(7, 66, &NoOp);
			T.Add(22, 1, &ReturnZero);
			T.Add(22, 8, &StartMadeProcess);
			T.Add(122, 3, &ResetResponse);
			T.Add(37, 3, &NewMessageBoxData);
			T.Add(43, 14, &MessageBoxUp);
			// The save icon and its clean-up list (msg_boxes.auto_save, M2 / M4): OpenKombat saves nothing, so none shows.
			for (const auto& Entry : { std::pair<int32, int32>{ 6, 254 }, { 7, 44 }, { 24, 3 }, { 24, 6 }, { 24, 8 }, { 176, 3 } })
			{
				T.Add(Entry.first, Entry.second, &ReturnZero);
			}
			T.Add(6, 3, &LocString);
			T.Add(6, 37, &StrCopyN);
			T.Add(6, 35, &StrConcat);
			// HUD objects (W7)
			T.Add(15, 10, &HudText);
			T.Add(15, 3, &HudSprite);
			T.Add(15, 0, &HudFree);
			T.Add(15, 13, &HudAlpha);
			T.Add(15, 16, &HudScale<true, false>);
			T.Add(15, 18, &HudScale<false, true>);
			T.Add(15, 19, &HudScale<true, true>);
			T.Add(15, 20, &HudPosition);
			T.Add(15, 24, &HudRotation);
			T.Add(6, 262, &ReturnZero);			// FUN_0058DD90 (also 7.608, 10.61). [inf] whether the message font is a texture
			// The camera and its shakes (W3)
			T.Add(171, 15, &CurrentCamera);
			T.Add(171, 20, &CurrentCamera);
			T.Add(151, 21, &CurrentCamera);
			T.Add(169, 93, &SetCameraVariable);
			T.Add(169, 94, &GetCameraVariable);
			T.Add(169, 92, &CameraVariable42);
			T.Add(171, 11, &SetCameraStageData);
			T.Add(149, 23, &SetCameraShake);
			T.Add(149, 24, &NoOp);				// (camera, &move): moves the camera for good (FUN_0045E300). [design] Ours frames the
												// fighters again each frame, so a move would be undone anyway
			T.Add(149, 32, &SetCameraShakePaused);
			T.Add(149, 33, &GetCameraShakePaused);
			T.Add(29, 1, &RotatorPitch);
			T.Add(91, 1, &ClassScreenShakePdata);
			T.Add(91, 3, &NewScreenShakePdata);
			T.Add(91, 5, &SetupScreenShake);
			T.Add(94, 1, &ClassArmorPdata);
			T.Add(94, 3, &NewArmorPdata);
			T.Add(94, 5, &SetupArmor);
			T.Add(91, 7, &ScreenShakeWords<0x34, 0x30>);
			T.Add(91, 8, &ScreenShakeWords<0x38, 0x3C>);
			T.Add(7, 154, &HudWidth);
			T.Add(7, 155, &HudHeight);
			T.Add(7, 157, &HudTextSize);
			T.Add(43, 32, &SetFade);
			T.Add(43, 33, &SetFade);
			T.Add(10, 0, &ReturnZero);			// (bool) -> "TRUE" or "FALSE" (FUN_0081A0A0), for mks_log, which logs nothing
			T.Add(174, 61, &NoOp);				// FUN_0087F040: matches controllers to profiles for the menus (M2)
			for (const auto& Entry : ZeroForNow)
			{
				T.Add(Entry[0], Entry[1], &ReturnZero);
			}

			T.Add(137, 249, &SetAttackBlockA);
			T.Add(137, 250, &SetAttackBlockA2);
			T.Add(137, 264, &ThrowEntry);
			T.Add(81, 21, &StartThrow);
			T.Add(78, 60, &InputHeldWhileActing);
			T.Add(118, 5, &PlayerSide);
			T.Add(137, 86, &DamageChar);
			T.Add(137, 243, &SetDuckReaction);
			T.Add(4, 28, &HudMessage);
			T.Add(7, 144, &SlideMessage);
			T.Add(4, 2, &ReturnZero);			// () -> the object at DAT_00DF0BBC, or 0 (FUN_008AF430); the armour effects hand its +4 to the
											// flash (6.51). [unk] None here, so they skip it
			// 5.4 / 5.0 (FUN_00879D60 / FUN_00877890): [inf] end a special camera on DAT_00E89CF0 and give the fight camera back; the
			// throw cleanup calls them. There are no special cameras yet (W3).
			T.Add(5, 4, &NoOp);
			T.Add(5, 0, &NoOp);
			T.Add(171, 5, &CameraWord<0x3C4>);
			T.Add(171, 7, &CameraWord<0x3C8>);
			T.Add(81, 198, &NoOp);			// wakeup_window(obj, n) (FUN_0044EEC0): the same, a wakeup attack window, for the drone
			T.Add(7, 467, &NoOp);			// (info) (FUN_008919C0): tells the player's AIDrone a getup roll is open (event 2); OpenKombat's AI has no drone (F9)
			T.Add(137, 151, &IsCharacter);
			T.Add(6, 5, &FindFunc);
			T.Add(137, 271, &TwoPlayerRxSetup);
			T.Add(137, 272, &TwoPlayerRxClear);
			T.Add(137, 283, &CountThrow<0x2FF0>);
			T.Add(137, 282, &CountThrow<0x2FEC>);
			T.Add(137, 109, &FirstHitDetected);
			T.Add(137, 102, &GetCharScript);
			T.Add(81, 186, &GetCharScript);
			T.Add(137, 111, &NoOp);			// throw_reversed (FUN_008D8580): the fighter's unused collision context +0x14 = 1
			T.Add(137, 278, &SetThrowId);
			T.Add(7, 59, &ReturnZero);		// is_tag_team_player (FUN_00892B70): no tag team yet (F7)
			T.Add(7, 110, &NoOp);			// throw_escape_off (FUN_009099F0): [inf] the tutorial's throw-escape prompt
			// Character effects (137.191-193) and the objects they would return (W2): none yet, so these act on none
			for (const auto& Entry : { std::pair<int32, int32>{ 137, 191 }, { 137, 192 }, { 137, 193 }, { 52, 101 }, { 52, 42 }, { 52, 69 }, { 82, 37 }, { 82, 38 }, { 6, 160 }, { 193, 41 },
				// Sub-Zero's throw: play_fx_id2, a prop (56.7 makes one), and what acts on them
				{ 137, 196 }, { 56, 7 }, { 137, 122 }, { 6, 135 }, { 52, 111 }, { 52, 112 }, { 52, 113 }, { 52, 49 }, { 52, 57 }, { 82, 11 } })
			{
				T.Add(Entry.first, Entry.second, &ReturnZero);
			}
			T.Add(137, 246, &AirThrow);
			T.Add(137, 156, &IsBlocking);
			T.Add(137, 257, &SetAttackBlockB);
			T.Add(137, 259, &SetAttackBlockC);

			// Input (FIGHT-RE-NOTES §6.4). These replace the earlier stubs.
			T.Add(78, 67, &InputPressed);
			T.Add(78, 69, &InputPressedDir);
			T.Add(78, 24, &LastInput);
			T.Add(137, 157, &IsHoldingBlock);
			T.Add(78, 13, &DisallowButton);
			T.Add(81, 219, &BindButton);
			T.Add(81, 221, &BindButtonFunc);
			T.Add(81, 222, &BindButtonNamed);
			T.Add(81, 203, &SetButtonDelay);
			T.Add(81, 217, &ClearButtons);
			T.Add(6, 102, &MovingScript);
			T.Add(6, 103, &JumpingScript);
			T.Add(6, 104, &BlockingDuckingScript);
			T.Add(6, 105, &GenericThrowScript);
			T.Add(137, 113, &EnableAllCommands);
			T.Add(81, 205, &EnableAllCommands);
			T.Add(137, 236, &ComboBranch);
			T.Add(43, 24, &GetPlayer);
			T.Add(136, 26, &GetPlayer);
			T.Add(78, 93, &PushControl);
			T.Add(78, 94, &ClearControl);
			T.Add(195, 5, &AttachToProcess);
			// MK objects (W1)
			T.Add(137, 188, &CreateCollisionObj);
			T.Add(137, 189, &CreateCollisionObj);	// create_collision_obj2 (FUN_008A4C70): the constructor 137.188 calls once fighter +0xAAB8
													// checks out, which ours doesn't look at
			T.Add(121, 1, &ClassCollisionObj);
			T.Add(121, 13, &ColRescheduleCleanup);
			T.Add(121, 21, &ColSphere);
			T.Add(121, 30, &ColWord<0x1D4>);
			T.Add(121, 28, &ColWord<0x1BC>);	// col_wall_flag (FUN_00895EC0): +0x1BC
			T.Add(121, 29, &ColWord<0x1C0>);	// (FUN_00895ED0): +0x1C0
			T.Add(137, 155, &ReactionRerouter);
			T.Add(121, 31, &ColWord<OKCollisionObj::DeleteAfterCallback>);
			T.Add(121, 32, &ColHitCallback);
			T.Add(121, 33, &ColBlockCallback);
			T.Add(121, 36, &ColWallCallback);
			T.Add(6, 148, &PlaceAtBone);
			T.Add(6, 146, &PlaceAtObject);
			T.Add(52, 78, &KillObjInTicks);
			T.Add(52, 79, &ObjSetVelocity);
			T.Add(52, 80, &ObjGetVelocity);
			T.Add(52, 88, &ObjGetPos);
			T.Add(6, 138, &AddFramePoint);
			T.Add(6, 139, &AddFrameVector);
			T.Add(81, 23, &XferToScript);
			T.Add(6, 99, &FightersDistance);
			T.Add(6, 250, &ContactValid);
			T.Add(81, 169, &DeleteContext);
			T.Add(1, 2, &JumpVm);
			T.Add(137, 270, &TwoPlayerRxForce);
			T.Add(137, 238, &SetProps);
			T.Add(137, 242, &SetPropsParams);
			T.Add(66, 7, &SetCommandWindow);
			T.Add(81, 50, &GlitchToAni);
			T.Add(137, 70, &IsArmorActive);
			T.Add(118, 14, &AddMeterNative);
			T.Add(78, 70, &ConsumeComboPress);
			T.Add(81, 71, &RunOnFighter);
			T.Add(81, 62, &RunMoveCallbackNative);
			T.Add(81, 63, &SetMoveCallback);
			T.Add(137, 261, &SetHitCallback);
			// The EX flash (anim_event_assert): its effects come later.
			T.Add(6, 51, &ReturnZero);			// (obj, ...) -> a flash object (0x110 bytes, FUN_00492310); was named anim_event_assert
			T.Add(22, 9, &AttachObjectToProcess);
			T.Add(137, 1, &ClassFGCharacterObj);
			T.Add(59, 1, &ClassSkeletalObj);
			T.Add(59, 65, &NoOp);				// set_materials_color(obj, colour, ?): virtual +0xAC tints the materials; no material effects yet
			T.Add(81, 126, &NoOp);				// the same function
			T.Add(59, 64, &NoOp);				// scale_materials_timing(obj, x): virtual +0xA8, the timing of a material effect
			T.Add(81, 125, &NoOp);				// the same function
			T.Add(6, 147, &NoOp);				// obj_set_visible(obj, ...) (FUN_004373E0): our MK objects aren't drawn yet
			T.Add(78, 71, &ConsumePress);
			// The rest of the roster's X-rays
			T.Add(6, 80, &FloatSqrt);
			T.Add(6, 87, &VecLength);
			T.Add(6, 89, &VecScale);
			T.Add(6, 90, &VecAdd);
			T.Add(27, 7, &VecSet);
			T.Add(27, 1, &VecNormalizeAbove);
			T.Add(7, 440, &FightersApartSquared);
			T.Add(22, 4, &KillProcessNative);
			T.Add(118, 10, &OldXrayChord);
			T.Add(59, 117, &GravityZ);
			T.Add(59, 118, &SetGravityZ);
			T.Add(52, 81, &ObjectVelocity);
			T.Add(59, 120, &ObjectVelocity);	// the same function
			T.Add(52, 102, &ObjectSpin);
			T.Add(52, 110, &HideInTicks);
			T.Add(52, 109, &StoreObjectFields<0x64>);		// (obj, x) (FUN_0042EE20). [unk] what +0x64 does
			T.Add(52, 68, &StoreObjectFields<0x1C>);		// (named obj_culling) (obj, x) (FUN_0093C730). [unk]
			T.Add(52, 31, &ReturnZero);			// (obj) (FUN_0042F190): its component's +0x80 bit 1. Never set here
			T.Add(81, 108, &SetLaunchValue);
			T.Add(81, 232, &SetParry);
			T.Add(137, 256, &SetPendingFlag28);
			T.Add(137, 258, &SetPendingFlag20);
			T.Add(137, 262, &StoreObjectFields<0x7C24, 0x7C30>);	// (obj, process, data) (FUN_00896930): the pending attack's
														// reaction callback (+0x118 handle, +0x124)
			T.Add(82, 23, &StoreObjectFields<0xFC>);		// (named fx_lifetime) (fx, x) (FUN_00430570). [unk] what +0xFC does
			T.Add(121, 22, &SetCapsule);
			T.Add(137, 12, &IsVariant);
			T.Add(137, 36, &ClearMaterialEffectFlag);
			T.Add(6, 141, &TurnLikePlus);
			T.Add(6, 153, &TurnLikeOnly);
			T.Add(52, 73, &PlaceFromObject);
			T.Add(82, 42, &AttachEffectToBone);
			T.Add(82, 43, &AttachEffectToBone);
			T.Add(52, 55, &AttachEffectToBoneName);
			T.Add(137, 89, &ScriptedHitNative);
			T.Add(137, 119, &ResetTwoPlayerState);
			// Skeletal props, weapon objects, damage layers and modes OpenKombat doesn't have: these act on none
			T.Add(59, 4, &ReturnZero);			// start_track_matcher(obj, prop, anim, ...) (FUN_00447830): a prop follows the fighter's
												// animation. No skeletal props are spawned (W1)
			T.Add(59, 38, &NoOp);				// (obj, a, b, c) (FUN_0042FC70): its mesh component's animation parameters
			T.Add(59, 106, &NoOp);				// (&name, x) (FUN_004300E0): by name. [unk]
			T.Add(59, 127, &NoOp);				// (obj, ...) (FUN_007668E0): the mesh's teleport meshes. [unk]
			T.Add(59, 128, &NoOp);				// (obj, x) (FUN_00447980): mesh flag 0x40000 and a linked skeletal object. [unk]
			T.Add(81, 99, &NoOp);				// (obj, ...5) fighter virtual +0x174. [unk]
			T.Add(81, 142, &NoOp);				// (obj) (FUN_0044D8B0): hides the weapon object at +0x4D0. None here
			T.Add(81, 152, &NoOp);				// (obj, layer, amount) (FUN_00447240): a damage layer's wear (+0x4D8), which redraws
												// the wounds (W8). Not drawn yet
			T.Add(6, 52, &ReturnZero);			// (obj, anim, ...) (FUN_004923B0) -> a new 0x110-byte animation object. Not made
			T.Add(5, 11, &NoOp);				// (...9) (FUN_0087B7A0): reads +0x50 of its first and hands it on. [unk]
			T.Add(99, 3, &ReturnZero);			// () -> a new 0x20-byte block; none, so 99.5 gets none
			T.Add(99, 5, &NoOp);				// (block, process, ...) (FUN_0044A970): fills it and attaches it to the process
			T.Add(52, 72, &NoOp);				// (named obj_set_pos_rel) (obj, from, ...) (FUN_0043E3B0): placed against a bone of from.
												// [unk] its arguments
			T.Add(137, 138, &NoOp);				// (obj) (FUN_00899FA0): a mode's side flags (towers, online). [unk]
			T.Add(137, 201, &ReturnZero);		// (obj, function) (FUN_0089FD40): a process on the fighter's FX list (+0xAAB8). None here
			T.Add(137, 203, &NoOp);				// (obj) (FUN_0089FDB0): clears that FX list
			// The characters' game_init setup: meshes, cloth, bone layers and unlocks OpenKombat doesn't model yet
			T.Add(7, 422, &ReturnZero);			// (FUN_00887B70) -> 1 in game state 3. [unk] which state; ours is 8
			T.Add(7, 660, &NoOp);				// (a, b, c, d) (FUN_008F3E50): looks through unlocks 0x9C-0xAB (M4)
			T.Add(8, 13, &NoOp);				// (obj, x) (FUN_008AE380): the fighter's materials (W8). [unk]
			T.Add(21, 1, &NoOp);				// (script, player) (FUN_004587B0): adds the player to the script's list at +0x60. [unk]
			T.Add(59, 25, &NoOp);				// set_bone_mask(obj, layer, bones): an animation layer's bone mask. Layers aren't
												// ported (F4)
			T.Add(59, 98, &NoOp);				// cloth_by_name(obj, name, x) (FUN_00430090): cloth (W9)
			T.Add(81, 14, &NoOp);				// bone_remap(obj, a, b) (FUN_00458010 -> FUN_00744150): the four-armed meshes' extra
												// arms. [unk]
			T.Add(108, 4, &ReturnZero);			// (obj) (FUN_00494B90): starts a stage object's effect. [unk]
			T.Add(111, 3, &ReturnZero);			// (x) -> a new 0x2F4-byte object (FUN_0097F400). [unk] Not made
			T.Add(59, 3, &NoOp);				// cache_obj_meshes(obj, ...) (FUN_0042F7C0): keeps meshes loaded
			T.Add(52, 11, &ReturnZero);			// spawn_dynamic_sm (...) -> a static mesh object. Not spawned (W1)
			T.Add(59, 12, &ReturnZero);			// spawn_skel_actor2 (...) -> a skeletal object. Not spawned (W1)
			T.Add(59, 21, &ReturnZero);			// (obj) (FUN_0042F7D0) -> its skeletal component. None
			T.Add(52, 37, &NoOp);				// (obj, ...): a mesh parameter. [unk]
			T.Add(52, 48, &NoOp);				// (obj, x): an object flag. [unk]
			T.Add(59, 58, &NoOp);				// set_material_by_name and set_material_slot (59.60): materials (W8)
			T.Add(59, 60, &NoOp);
			T.Add(59, 93, &NoOp);				// (obj) (FUN_0042FFF0). [unk]
			T.Add(81, 58, &NoOp);				// (obj, x) (FUN_00446E10). [unk]
			T.Add(81, 139, &NoOp);				// attach_weapons_to_limbs(obj, ...): weapon props. None (W1)
			T.Add(137, 136, &NoOp);				// (obj, x): fighter virtual +0xDC. [unk]
			T.Add(137, 137, &NoOp);				// (obj) (FUN_00899EB0): a mode's side flags, as 137.138. [unk]
			T.Add(7, 634, &ReturnZero);			// add_status(obj, name) and has_status (FUN_008F2A10, FUN_008F2A60): 1 when the name is
			T.Add(7, 635, &ReturnZero);			// the fighter's material effect (+0x7744, "zombie"), then clears or keeps it. None here
			T.Add(78, 25, &CatchUpPresses);
			T.Add(78, 74, &RecordComboStep);
			T.Add(118, 58, &RecordMoveKind);
			T.Add(137, 211, &XferToMovesetScript);
			// Objects OpenKombat doesn't make yet: effects (W2), lights, skeletal props. Spawning returns none.
			T.Add(6, 140, &NoOp);				// (obj, info, yaw, pitch, roll): turns the object to face the opponent (FUN_0043D990)
			T.Add(137, 199, &ReturnZero);		// (obj) -> a light (class 0x3F3, FUN_0089FC70); was named kill_projectile
			T.Add(83, 3, &ReturnZero);			// (r, g, b, brightness, radius) -> a new point light (0x104 bytes, FUN_0043D010)
			T.Add(109, 7, &NoOp);				// light radius (FUN_00436F00)
			T.Add(109, 8, &NoOp);				// light flag (+0x1CC)
			T.Add(109, 9, &NoOp);				// light brightness (+0x1D0)
			T.Add(59, 27, &NoOp);				// weapon_anim(obj, &anim, frame, rate, ?): animates a skeletal prop (the swords)
			for (const int32 Index : { 191, 192, 193, 197, 198 })
			{
				T.Add(137, Index, &ReturnZero);	// play_char_fx, play_char_fx_light, play_particle_actor, char_light_fx(2)
			}
			for (const int32 Index : { 22, 42, 69, 101 })
			{
				T.Add(52, Index, &ReturnZero);	// object flags, sound and offsets, on effects
			}
			for (const int32 Index : { 10, 16, 36, 37, 38 })
			{
				T.Add(82, Index, &ReturnZero);	// fx_light, fx_color, fx_link, fx_link2, fx_attach_char
			}
			T.Add(6, 191, &NoOp);				// light_fade_in_hold
			T.Add(6, 192, &NoOp);				// light_fade_out
			// X-rays (FIGHT §8c.5-6)
			T.Add(7, 138, &NoOp);				// play_hud_supermeter_effect(1) (FUN_00852CC0): the meter bar's flash. [inf] HUD only
			T.Add(137, 75, &TeleportInFront);
			T.Add(137, 288, &NoOp);				// teleport_partner (FUN_008AAE70): the tag partner (+0x10C), none here
			T.Add(65, 1, &ReturnZero);			// the class of the fighter's FX list (+0xAAB4), for is_proc: none here, so
			T.Add(65, 7, &NoOp);				// attach_fx_at_bone_pos_only doesn't add the effect to it (65.7, FUN_0042DFE0)
			T.Add(4, 24, &ClockStopFlag);
			T.Add(39, 41, &NoOp);				// cinema_actor_blend(cinema, actor, from, to) (FUN_004B3AB0): the actor blends out of
												// the cinema's pose over those frames. [inf] Our hold ends at once
			T.Add(137, 17, &ReturnZero);		// dist_to_vec(obj, distance, &vector) (FUN_00894640): 1 when a point that far along
												// the facing is out of the arena, with the way back; then the X-ray scene is moved in.
												// [inf] Our stage has no walls
			T.Add(154, 2, &NoOp);				// (camera) (FUN_004B1E90), and 154.6-154.8 (camera, frames, blend, amount): the
			T.Add(154, 6, &NoOp);				// X-ray's screen effects. [unk] which post-process values
			T.Add(154, 7, &NoOp);
			T.Add(154, 8, &NoOp);
			T.Add(170, 13, &NoOp);				// (camera, camera) (FUN_0086DC00). [unk]
			T.Add(81, 73, &NoOp);				// cinema_flag(obj, 1) (FUN_00447640): a flag on the fighter's skeletal mesh. [unk]
			T.Add(7, 222, &NoOp);				// (n) (FUN_0083F5D0): the HUD's +0x874, set to 1 when an X-ray ends. [unk] what reads it
			T.Add(137, 37, &NoOp);				// clear_buffs(obj) (FUN_008935B0): puts a fighter's own materials back after an effect
												// swapped them (+0x1D0 & 0x40000). Materials aren't swapped here
			T.Add(137, 51, &NoOp);				// xray_cinema_flag(obj) (FUN_008ADF10): destroys the skeletal object at +0xA954. None here
			T.Add(174, 12, &ReturnZero);		// (pad), 174.24 (pad, button), 6.48 ui_input_held(pad): a player's input to skip a
			T.Add(174, 24, &ReturnZero);		// cinema. [inf] Cinemas aren't skipped here
			T.Add(6, 48, &ReturnZero);
			T.Add(6, 137, &AttachObj);
			T.Add(78, 79, &PlayerPosition);
			T.Add(78, 80, &PlayerRotation);
			T.Add(6, 88, &VecDot);
			T.Add(6, 91, &VecSubtract);
			T.Add(6, 92, &VecNormal);
			T.Add(7, 631, &LineDirectionNative);
			T.Add(7, 488, &OnFightLine);
			T.Add(7, 53, &MaxSeparationNative);
			T.Add(120, 15, &StageFloorHeight);	// the same function as 42.1
			// fatality_adjust_if_winner_near_edge (a fatality's activated callback): the scene moved in from an arena edge
			T.Add(27, 3, &VecLengthSquared);
			T.Add(6, 46, &VecToRotator);
			T.Add(59, 111, &FacingVector);
			T.Add(7, 61, &VecZero3);			// (&out) (FUN_008F8D00): the global fatality_background_pos_adjust, if a script has one.
												// [unk] whose: FindGlobal(name, 0). None here
			T.Add(39, 32, &NoOp);				// cinema_adjust_edge(cinema, &position, &?, ...) (FUN_004B3080) and cinema_adjust_edge2
			T.Add(39, 34, &NoOp);				// (FUN_004B2BE0): move the scene. [inf] Not yet: the scene stays at the attacker
			T.Add(137, 237, &NoOp);				// moveset_anim_frame(obj, anim, frame, ...) (FUN_008A0790): poses the fighter's skeletal
												// prop (+0x4C0) at a frame of a moveset animation. Fighters have none here
			T.Add(136, 33, &SlowTime);
			T.Add(136, 34, &SlowTimeEnd);
			T.Add(136, 35, &CharAnimSpeed);
			T.Add(136, 36, &CharAnimSpeedEnd);
			T.Add(131, 17, &NoOp);				// (player, player) (FUN_008F1050): tells a mode's listener (+0xCB8) of a combo hit. [unk]
			return T;
		}();
	return Table;
}

void FOKNativeTable::Add(int32 Module, int32 Index, FOKNativeFn Fn)
{
	Natives.Add((uint32(Module) << 16) | uint32(Index), Fn);
}

namespace
{
	TSet<uint32>& NativesRun()
	{
		static TSet<uint32> Run;
		return Run;
	}
}

void FOKNativeTable::NoteRun(uint32 NativeId)
{
	NativesRun().Add(NativeId);
}

void FOKNativeTable::WriteReport() const
{
	const FString Path = FPaths::ProjectSavedDir() / TEXT("OKNatives.tsv");
	TSet<FString> RanBefore;
	TArray<FString> Lines;
	if (FFileHelper::LoadFileToStringArray(Lines, *Path))
	{
		for (const FString& Line : Lines)
		{
			TArray<FString> Cells;
			Line.ParseIntoArray(Cells, TEXT("\t"));
			if (Cells.Num() >= 3 && Cells[2] == TEXT("ran"))
			{
				RanBefore.Add(Cells[0]);
			}
		}
	}
	const FOKNativeFn Stubs[] = { &NoOp, &Silent, &ReturnZero, &ReturnOne };
	TArray<uint32> Ids;
	Natives.GetKeys(Ids);
	Ids.Sort();
	FString Out = TEXT("native\tkind\trun\n");
	for (const uint32 Id : Ids)
	{
		const FString Key = FString::Printf(TEXT("%u.%u"), Id >> 16, Id & 0xFFFF);
		const FOKNativeFn Fn = Natives[Id];
		const bool bStub = Algo::Find(Stubs, Fn) != nullptr;
		const bool bRan = NativesRun().Contains(Id) || RanBefore.Contains(Key);
		Out += FString::Printf(TEXT("%s\t%s\t%s\n"), *Key, bStub ? TEXT("stub") : TEXT("real"), bRan ? TEXT("ran") : TEXT("-"));
	}
	FFileHelper::SaveStringToFile(Out, *Path);
}

FOKNativeFn FOKNativeTable::Find(int32 Module, int32 Index) const
{
	const FOKNativeFn* Found = Natives.Find((uint32(Module) << 16) | uint32(Index));
	return Found ? *Found : nullptr;
}