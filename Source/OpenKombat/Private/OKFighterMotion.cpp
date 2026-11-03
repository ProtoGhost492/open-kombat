#include "OKFighterMotion.h"
#include "OKNatives.h"
#include "OKScriptObjects.h"
#include "OKScriptRuntime.h"
#include "OKScriptThread.h"
#include "OKCollision.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<bool> CVarTraceMotion(
	TEXT("ok.Fighter.TraceMotion"),
	false,
	TEXT("Log each fighter's animation, position and air path every frame."));

namespace
{
	using namespace OKMotion;

	constexpr float TickSeconds = 1.0f / 60.0f;	// DAT_00C66EEC
	constexpr float Tiny = 0.0001f;				// DAT_00C61FE0
	constexpr float NoValue = -123456.0f;		// DAT_00C681E0: "use the current one"
	const TCHAR* const Hips = TEXT("Hips");		// MK bone id 1, what the air path tracks (virtual +0xC4 with 1)

	/** A fighter's fields in the runtime's heap. */
	struct FFields
	{
		FOKScriptRuntime& Runtime;
		uint32 Base;

		uint32 U(uint32 Offset) const
		{
			uint32 Value = 0;
			Runtime.LoadHeapWord(Base + Offset, Value);
			return Value;
		}
		float F(uint32 Offset) const
		{
			const uint32 Bits = U(Offset);
			float Value;
			FMemory::Memcpy(&Value, &Bits, 4);
			return Value;
		}
		void SetU(uint32 Offset, uint32 Value) const { Runtime.SetHeapWord(Base + Offset, Value); }
		void SetF(uint32 Offset, float Value) const
		{
			uint32 Bits;
			FMemory::Memcpy(&Bits, &Value, 4);
			SetU(Offset, Bits);
		}
	};

	FVector HipsWorld(const FOKFighter& Fighter)
	{
		FVector Location = Fighter.Body->GetWorldLocation();
		Fighter.Body->GetBoneWorldLocation(Hips, Location);
		return Location;
	}

	/** Virtual +0xD4 (FUN_0044E580): the object's own velocity and acceleration zeroed. */
	void ZeroObjectMotion(const FFields& M)
	{
		for (uint32 Offset = ObjectVelocity; Offset < ObjectAcceleration + 12; Offset += 4)
		{
			M.SetU(Offset, 0);
		}
		M.SetU(0x68, 0);
	}

	/**
	 * Virtual +0x12C (FUN_00457CA0): the hips' height above the floor at Frame of the landing animation, with its
	 * root translation. [inf] MKKE samples the animation the launch recorded (+0x34C); we sample the one playing,
	 * which is the same one unless the script changed it in the air.
	 */
	float LandHeight(const FOKFighter& Fighter, float Frame)
	{
		return Fighter.Body->GetSequenceBoneHeight(Hips, Frame);
	}

	/**
	 * Virtual +0x128 (FUN_0044C0C0): the ticks until the path comes down to Height above the floor, rounded up to a
	 * whole tick. Gravity is then adjusted so the path lands exactly on that tick.
	 */
	float TicksToHeight(const FFields& M, const FOKFighter& Fighter, float Height)
	{
		const float V0 = M.F(StartVelocityZ);
		const float Drop = (M.F(StartZ) - Fighter.Body->GetFloorZ()) - (M.F(LandOffset) + Height);
		float G = M.F(Gravity);
		if (FMath::Abs(G) < Tiny)
		{
			G = Tiny;
		}
		const float Root = FMath::Sqrt(FMath::Max(Tiny, V0 * V0 - G * Drop * 2.0f));
		float Seconds = (Root - V0) * (1.0f / G);
		const float Other = (-V0 - Root) * (1.0f / G);
		if (Seconds < 0.0f || (Other > 0.0f && Seconds < Other))
		{
			Seconds = Other;
		}
		const int32 Ticks = FMath::CeilToInt(Seconds * 60.0f);
		const float Whole = float(Ticks) * TickSeconds;
		if (Whole > 0.0f)
		{
			M.SetF(Gravity, -((Whole * V0 * 2.0f + Drop * 2.0f) / (Whole * Whole)));
		}
		return float(Ticks);
	}

	/**
	 * Virtual +0x11C (FUN_0044B340): the height on the path at the air clock, z = g t^2 / 2 + v0 t + z0. The fighter
	 * is moved up or down so its hips (with the animation's root translation) are there.
	 */
	void FollowPath(const FFields& M, FOKFighter& Fighter)
	{
		const float T = M.F(AirClock) * TickSeconds;
		const float Z = M.F(Gravity) * 0.5f * T * T + M.F(StartVelocityZ) * T + M.F(StartZ);
		M.SetF(VelocityZ, M.F(Gravity) * T + M.F(StartVelocityZ));
		M.SetF(PathZ, Z);
		FVector Location = Fighter.Body->GetWorldLocation();
		Location.Z += Z - (HipsWorld(Fighter).Z + Fighter.Body->GetRootOffset().Z);
		Fighter.Body->SetWorldLocation(Location);
	}

	/** Virtual +0x134 (FUN_0044B630): the animation's rate set so it reaches the landing frame at touchdown. */
	void RetimeToLanding(const FFields& M, FOKFighter& Fighter)
	{
		float Ticks = 3.4e38f;
		if (M.F(Speed) > 0.0f)
		{
			Ticks = (M.F(LandTicks) - M.F(AirClock)) / M.F(Speed);
		}
		Ticks = FMath::Max(1.0f, FMath::CeilToFloat(Ticks));
		const float Rate = (M.F(LandFrame) - Fighter.Body->GetAnimFrame()) / Ticks;
		M.SetF(LandRate, Rate);
		Fighter.Body->SetAnimRate(Rate);
	}

	/**
	 * Virtual +0x124 (FUN_0044B280): touchdown. [inf] MKKE leaves the fighter where the path ended (the hips at the
	 * landing frame's height, so the feet on the floor); we also put it on the floor, so the small difference between
	 * our pose and MKKE's doesn't add up over many jumps.
	 */
	void Touchdown(const FFields& M, FOKFighter& Fighter)
	{
		FVector Location = Fighter.Body->GetWorldLocation();
		Location.Z = Fighter.Body->GetFloorZ() + M.F(LandOffset);
		Fighter.Body->SetWorldLocation(Location);
		M.SetU(MoveKind, 8);
		M.SetU(InAir, 0);
	}

	/** On landing the fighter's thread goes to the function the launch named (XferToReaction with +0x368, +0x364). */
	void TransferToLanding(const FFields& M, FOKFighter& Fighter)
	{
		if (M.U(LandScript) != 0)
		{
			OKTransferMainThread(M.Runtime, Fighter, M.U(LandScript), M.U(LandFunction));
		}
	}

	/**
	 * Virtual +0x118 (FUN_0044B820), the first frame of a launch: when it lands, and the animation's rate that reaches
	 * the landing frame then. Already below the landing height and falling, it waits for the landing frame instead.
	 */
	void StartAirPath(const FFields& M, FOKFighter& Fighter)
	{
		const float Land = LandHeight(Fighter, M.F(LandFrame));
		if (M.F(PathZ) - Fighter.Body->GetFloorZ() < Land && M.F(VelocityZ) <= 0.0f && M.F(Gravity) <= 0.0f)
		{
			// [inf] With +0x388 clear MKKE also restarts the landing animation (virtual +0x74); ours is still playing.
			M.SetU(MoveKind, 6);
			return;
		}
		M.SetF(LandTicks, FMath::Max(1.0f, TicksToHeight(M, Fighter, Land)));
		if (M.U(KeepRate) == 0 && M.U(LandAnim) == Fighter.Body->GetAnimId())
		{
			const float Rate = (M.F(LandFrame) - Fighter.Body->GetAnimFrame()) / M.F(LandTicks);
			M.SetF(LandRate, Rate);
			Fighter.Body->SetAnimRate(Rate);
		}
		else
		{
			M.SetF(LandRate, Fighter.Body->GetAnimRate());
		}
		M.SetF(AirClock, 0.0f);
		M.SetU(MoveKind, 5);
		M.SetU(MoveTick, 0);
	}

	/** Virtual +0x120 (FUN_0044B420): the force-away moves the fighter along x and y. */
	void Push(const FFields& M, FOKFighter& Fighter, float TickStep)
	{
		FVector Location = Fighter.Body->GetWorldLocation();
		Location.X += M.F(PushVelocity) * TickStep * TickSeconds;
		Location.Y += M.F(PushVelocity + 4) * TickStep * TickSeconds;
		Fighter.Body->SetWorldLocation(Location);
		const FVector Tracked3 = HipsWorld(Fighter);
		M.SetF(Tracked, float(Tracked3.X));
		M.SetF(Tracked + 4, float(Tracked3.Y));
	}

	/** Starts a push of Velocity (virtual +0x1F4's second half). */
	void StartPush(const FFields& M, const FOKFighter& Fighter, const FVector& Velocity, float DecayStart, float Decay, float Extra)
	{
		const FVector Tracked3 = HipsWorld(Fighter);
		M.SetF(Tracked, float(Tracked3.X));
		M.SetF(Tracked + 4, float(Tracked3.Y));
		M.SetF(PushVelocity, float(Velocity.X));
		M.SetF(PushVelocity + 4, float(Velocity.Y));
		M.SetF(PushDecayStart, DecayStart);
		M.SetF(PushStop, DecayStart + Extra);
		M.SetF(PushDecay, Decay);
		M.SetF(PushClock, 0.0f);
		M.SetU(ForceAway, 5);
	}
}

void OKMotion::InitGroundMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter, bool bResetRate)
{
	const FFields M{ Runtime, Fighter.Handle };
	// virtual +0x110 (FUN_00447D90): flag 0x80000 off, and the animation's rate back to 1 when asked
	M.SetU(FOKFighter::Flags, M.U(FOKFighter::Flags) & ~(AirFlag | StillFlag));
	if (bResetRate && Fighter.Body)
	{
		Fighter.Body->SetAnimRate(1.0f);
	}
	Runtime.KillProcesses(11, Fighter.Handle);
	ZeroObjectMotion(M);
	M.SetU(ForceAway, 0);
	if (M.U(MoveKind) != 8)
	{
		M.SetF(Speed, 0.0f);
		M.SetU(MoveTick, uint32(Runtime.GetSimFrame()));
	}
}

void OKMotion::InitAirMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
{
	const FFields M{ Runtime, Fighter.Handle };
	M.SetU(FOKFighter::Flags, (M.U(FOKFighter::Flags) | AirFlag) & ~StillFlag);
}

void OKMotion::InitStillMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter, bool bResetRate)
{
	InitGroundMove(Runtime, Fighter, bResetRate);
	const FFields M{ Runtime, Fighter.Handle };
	M.SetU(FOKFighter::Flags, M.U(FOKFighter::Flags) | StillFlag);
}

void OKMotion::Stop(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
{
	const FFields M{ Runtime, Fighter.Handle };
	Runtime.KillProcesses(11, Fighter.Handle);
	ZeroObjectMotion(M);
	M.SetU(ForceAway, 0);
	if (M.U(MoveKind) != 8)
	{
		M.SetF(Speed, 0.0f);
		M.SetU(MoveTick, uint32(Runtime.GetSimFrame()));
	}
}

bool OKMotion::IsStoppedMidMove(FOKScriptRuntime& Runtime, const FOKFighter& Fighter)
{
	const FFields M{ Runtime, Fighter.Handle };
	const uint32 Kind = M.U(MoveKind);
	return Kind != 0 && Kind != 3 && Kind != 4 && M.U(MoveTick) != 0 && M.F(Speed) < Tiny;
}

void OKMotion::StopMove(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
{
	const FFields M{ Runtime, Fighter.Handle };
	for (const uint32 Offset : { MoveKind, ForceAway, PushVelocity, PushVelocity + 4, VelocityZ, Gravity, InAir })
	{
		M.SetU(Offset, 0);
	}
}

void OKMotion::Launch(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float VelocityZ0, float Gravity0, float InLandFrame, float InEarlyTicks,
	uint32 Script, uint32 Function, uint32 Anim, float InStartFrame, float Blend, uint32 InKeepRate)
{
	const FFields M{ Runtime, Fighter.Handle };
	const float V0 = VelocityZ0 == NoValue ? M.F(ObjectVelocity + 8) : VelocityZ0;
	M.SetF(StartVelocityZ, V0);
	M.SetF(VelocityZ, V0);
	if (Anim == 0)
	{
		M.SetU(LandAnim, Fighter.Body ? Fighter.Body->GetAnimId() : 0);
		M.SetF(LandBlend, 0.0f);
		M.SetF(StartFrame, Fighter.Body ? Fighter.Body->GetAnimFrame() : 0.0f);
	}
	else
	{
		M.SetU(LandAnim, Anim);
		M.SetF(LandBlend, Blend);
		M.SetF(StartFrame, InStartFrame);
	}
	M.SetU(LandFunction, Function);
	M.SetU(LandScript, Script);
	M.SetF(EarlyTicks, InEarlyTicks);
	M.SetF(LandFrame, InLandFrame);
	M.SetF(Speed, 1.0f);
	M.SetF(Gravity, Gravity0);
	M.SetU(SolveStart, 1);
	M.SetU(MoveTick, 0);
	M.SetU(MoveKind, 1);
	M.SetU(NoLandTransfer, 0);
	M.SetU(KeepRate, InKeepRate);
	M.SetU(AfterLandInput, 0);
	M.SetF(LandOffset, 0.0f);
	M.SetU(InAir, 1);
}

void OKMotion::LaunchAndLand(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float VelocityZ0, float Gravity0, float InLandFrame, float InLandOffset)
{
	const FFields M{ Runtime, Fighter.Handle };
	const float V0 = VelocityZ0 == NoValue ? M.F(ObjectVelocity + 8) : VelocityZ0;
	M.SetF(StartVelocityZ, V0);
	M.SetF(VelocityZ, V0);
	M.SetF(LandOffset, InLandOffset);
	M.SetU(LandAnim, Fighter.Body ? Fighter.Body->GetAnimId() : 0);
	M.SetU(LandScript, 0);
	M.SetF(LandFrame, InLandFrame);
	M.SetF(Speed, 1.0f);
	M.SetU(MoveTick, 0);
	M.SetU(KeepRate, 0);
	M.SetU(AfterLandInput, 0);
	M.SetF(Gravity, Gravity0);
	M.SetU(MoveKind, 1);
	M.SetU(SolveStart, 1);
	M.SetU(NoLandTransfer, 1);
	M.SetU(InAir, 1);
}

void OKMotion::LaunchToHeight(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Height, float Frames, float InLandFrame, float InEarlyTicks,
	uint32 Script, uint32 Function, uint32 Anim, float InStartFrame, float Blend, uint32 InKeepRate)
{
	const FFields M{ Runtime, Fighter.Handle };
	M.SetF(TargetHeight, Height);
	M.SetF(TargetTime, Frames * TickSeconds);
	M.SetU(SolveLaunch, 1);
	Launch(Runtime, Fighter, 1.0f, 1.0f, InLandFrame, InEarlyTicks, Script, Function, Anim, InStartFrame, Blend, InKeepRate);
}

void OKMotion::LaunchToHeightGuaranteed(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Height, float Frames, float InLandFrame)
{
	const FFields M{ Runtime, Fighter.Handle };
	M.SetF(TargetHeight, Height);
	M.SetF(TargetTime, Frames * TickSeconds);
	M.SetU(SolveLaunch, 1);
	LaunchAndLand(Runtime, Fighter, 1.0f, 1.0f, InLandFrame, 0.0f);
}

void OKMotion::SetAirborne(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float VelocityZ0)
{
	const FFields M{ Runtime, Fighter.Handle };
	M.SetF(VelocityZ, VelocityZ0);
	M.SetF(StartVelocityZ, VelocityZ0);
	M.SetU(SolveStart, 1);
	M.SetF(Gravity, 0.0f);
	M.SetF(LandTicks, -1.0f);
	M.SetF(AirClock, 0.0f);
	M.SetF(Speed, 1.0f);
	M.SetU(MoveTick, 0);
	M.SetU(MoveKind, 2);
	M.SetF(LandOffset, 0.0f);
	M.SetU(InAir, 1);
}

void OKMotion::SetAirSpeed(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float Scale)
{
	const FFields M{ Runtime, Fighter.Handle };
	if (Scale == 0.0f)
	{
		if (Fighter.Body)
		{
			Fighter.Body->SetAnimRate(0.0f);
		}
		M.SetF(Speed, Scale);
		M.SetU(MoveTick, uint32(Runtime.GetSimFrame()));
		return;
	}
	if (Fighter.Body)
	{
		Fighter.Body->SetAnimRate(M.F(LandRate) * Scale);
	}
	M.SetF(Speed, Scale);
}

void OKMotion::ForceAwayFromOpponent(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float InSpeed, float DecayStart, float Decay, float Extra)
{
	if (!Fighter.Body || !Fighter.Opponent || !Fighter.Opponent->Body)
	{
		return;
	}
	// Along the fight line (FUN_008B4230), away from the opponent: the speed's sign flips when the fighter is on
	// the line's lower side. [inf] MKKE then turns the velocity by the call's second argument (FUN_004936D0); the
	// native always passes 0.
	const FFields M{ Runtime, Fighter.Handle };
	const FVector& Line = Fighter.LineDirection;
	const double Mine = FVector::DotProduct(HipsWorld(Fighter), Line);
	const double Theirs = FVector::DotProduct(HipsWorld(*Fighter.Opponent), Line);
	const float Signed = Mine - Theirs < 0.0 ? -InSpeed : InSpeed;
	StartPush(M, Fighter, Line * Signed, DecayStart, Decay, Extra);
}

void OKMotion::ForceAwayFacing(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float InSpeed, float DecayStart, float Decay, float Extra)
{
	if (!Fighter.Body)
	{
		return;
	}
	const FFields M{ Runtime, Fighter.Handle };
	FVector Line = Fighter.LineDirection;
	if (FVector::DotProduct(Line, Fighter.Body->GetFacing()) < 0.0)
	{
		Line = -Line;
	}
	StartPush(M, Fighter, Line * -InSpeed, DecayStart, Decay, Extra);
}

void OKMotion::PreStep(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
{
	if (!Fighter.Body)
	{
		return;
	}
	const FFields M{ Runtime, Fighter.Handle };
	if (M.U(SolveLaunch) != 0)
	{
		// launch_to_height: gravity brings the hips from the target height down to the landing frame's height in the
		// target time, and the start speed reaches the target height in it.
		M.SetU(SolveLaunch, 0);
		const float T = M.F(TargetTime);
		if (M.U(LandAnim) != 0 && T > 0.0f)
		{
			const float Height = M.F(TargetHeight);
			const float G = -((Height - LandHeight(Fighter, M.F(LandFrame))) / (T * T * 0.5f));
			const float HipsHeight = float(HipsWorld(Fighter).Z - Fighter.Body->GetFloorZ());
			const float V0 = ((Height - HipsHeight) - G * T * T * 0.5f) / T;
			M.SetF(Gravity, G);
			M.SetF(VelocityZ, V0);
			M.SetF(StartVelocityZ, V0);
		}
	}
	if (M.U(SolveStart) != 0)
	{
		// The path starts where the hips are, with the animation's root translation (virtual +0x138).
		M.SetU(SolveStart, 0);
		const FVector At = HipsWorld(Fighter);
		const float Z = float(At.Z) + Fighter.Body->GetRootOffset().Z;
		M.SetF(Tracked, float(At.X));
		M.SetF(Tracked + 4, float(At.Y));
		M.SetF(PathZ, Z);
		M.SetF(StartZ, Z);
	}
}

void OKMotion::Step(FOKScriptRuntime& Runtime, FOKFighter& Fighter)
{
	if (!Fighter.Body)
	{
		return;
	}
	const FFields M{ Runtime, Fighter.Handle };
	// The world's time dilation (a hit pause). [inf] The object's rate (+0xF4 x 0xE91E24) is 1 here.
	const float TickStep = Runtime.GetTimeDilation();
	const float Advance = M.F(Speed) * TickStep;
	switch (M.U(MoveKind))
	{
	case 1:
		StartAirPath(M, Fighter);
		FollowPath(M, Fighter);
		break;
	case 2:
	case 3:
		M.SetF(AirClock, M.F(AirClock) + Advance);
		FollowPath(M, Fighter);
		break;
	case 5:
		if (M.F(Speed) > Tiny)
		{
			M.SetF(AirClock, FMath::Min(M.F(AirClock) + Advance, M.F(LandTicks)));
			FollowPath(M, Fighter);
			if (M.U(NoLandTransfer) == 0)
			{
				// EarlyTicks before touchdown the landing begins: the thread sleeps until then (state 0x200F) and the
				// animation is timed to the landing frame.
				const float Early = M.F(EarlyTicks);
				if ((M.F(LandTicks) - M.F(AirClock)) / M.F(Speed) - Early <= Tiny && Early != -1.0f)
				{
					OKTransferMainThreadTo(Runtime, Fighter, TEXT("FightEngineScript"), TEXT("sleep_forever"));
					M.SetU(FOKFighter::State, 0x200F);
					// [inf] When the animation changed in the air MKKE restarts the landing animation (virtual +0x130).
					RetimeToLanding(M, Fighter);
					M.SetU(MoveKind, 7);
				}
			}
			else if (M.F(LandTicks) <= M.F(AirClock))
			{
				Touchdown(M, Fighter);
			}
		}
		else
		{
			FollowPath(M, Fighter);
		}
		break;
	case 6:
		if (Fighter.Body->GetAnimFrame() >= M.F(LandFrame))
		{
			Touchdown(M, Fighter);
			if (M.U(NoLandTransfer) == 0)
			{
				TransferToLanding(M, Fighter);
			}
		}
		[[fallthrough]];		// as in MKKE: the vertical terms are cleared while it waits, too
	case 8:
		for (const uint32 Offset : { Gravity, VelocityZ, StartVelocityZ, MoveTick })
		{
			M.SetU(Offset, 0);
		}
		break;
	case 7:
		M.SetF(AirClock, FMath::Min(M.F(AirClock) + Advance, M.F(LandTicks)));
		FollowPath(M, Fighter);
		if (M.F(AirClock) - M.F(LandTicks) >= -Tiny)
		{
			Touchdown(M, Fighter);
			TransferToLanding(M, Fighter);
		}
		break;
	default:
		break;
	}
	const uint32 Kind = M.U(MoveKind);
	if ((Kind == 5 || Kind == 7) && M.F(LandTicks) <= M.F(AirClock) + Advance)
	{
		Fighter.Body->SetAnimFrame(M.F(LandFrame));
	}

	if (M.U(ForceAway) == 5)
	{
		Push(M, Fighter, TickStep);
		const float Clock = M.F(PushClock) + TickStep;
		M.SetF(PushClock, Clock);
		if (Clock > M.F(PushDecayStart))
		{
			// [inf] MKKE's pow call lost its arguments in the decompile; one decay factor a tick is assumed.
			const float Factor = FMath::Pow(M.F(PushDecay), TickStep);
			M.SetF(PushVelocity, M.F(PushVelocity) * Factor);
			M.SetF(PushVelocity + 4, M.F(PushVelocity + 4) * Factor);
			if (Clock > M.F(PushStop))
			{
				M.SetU(ForceAway, 8);
			}
		}
	}
	else if (M.U(ForceAway) == 8)
	{
		M.SetU(PushVelocity, 0);
		M.SetU(PushVelocity + 4, 0);
	}

	if (CVarTraceMotion.GetValueOnGameThread())
	{
		const FVector At = Fighter.Body->GetWorldLocation();
		UE_LOG(LogOKVM, Log, TEXT("frame %lld %s: %s %.1f x%.2f at (%.1f, %.1f, %.1f) move %u clock %.1f/%.1f push %u (%.0f, %.0f) state %X yaw %.0f%s"),
			Runtime.GetSimFrame(), *Fighter.Name, *Fighter.Body->GetAnimName(), Fighter.Body->GetAnimFrame(), Fighter.Body->GetAnimRate(),
			At.X, At.Y, At.Z - Fighter.Body->GetFloorZ(), Kind, M.F(AirClock), M.F(LandTicks), M.U(ForceAway), M.F(PushVelocity),
			M.F(PushVelocity + 4), M.U(FOKFighter::State), Fighter.Body->GetFacing().Rotation().Yaw, Fighter.Body->IsMeshMirrored() ? TEXT(" mirrored") : TEXT(""));
	}
}

void OKMotion::PushApart(FOKScriptRuntime& Runtime, FOKFighter& A, FOKFighter& B)
{
	if (!A.Body || !B.Body)
	{
		return;
	}
	// Player pushing (FUN_008B4BF0) works on each fighter's position along the fight line, counted from its left end
	// (FUN_008B4320), and writes only that back: the push, the separation window, then the arena ends.
	const FFields MA{ Runtime, A.Handle }, MB{ Runtime, B.Handle };
	const FVector& Line = A.LineDirection;
	const FVector StartA = A.Body->GetWorldLocation(), StartB = B.Body->GetWorldLocation();
	const double OldA = FVector::DotProduct(StartA - A.LineStart, Line), OldB = FVector::DotProduct(StartB - A.LineStart, Line);
	double PosA = OldA, PosB = OldB;
	const uint32 FlagsA = MA.U(FOKFighter::CharFlags), FlagsB = MB.U(FOKFighter::CharFlags);

	// 1. The push. The overlap (FUN_008B1080) is the largest (rA + rB)^2 - d^2 over the two push sphere lists
	// (PLAYER_COL_INFO +0x188: the collision table's second radii), d measured along the line and in height
	// (FUN_008AF580); the feet count a tenth when either fighter is in a 0x1000 state. Being in the air changes
	// nothing. FUN_008B4000 gives each half of it times 0.01 (none while either is in state 0x460D, unless the other
	// is in 0x2000 or 0x2010), then x0.75 and the fighter's +0x77D0. Flags 0x420 keep a fighter in place.
	// [inf] The 0x1000-state split by +0x2F0, and the +0x76F8 hold-off, aren't done.
	const uint32 StateA = MA.U(FOKFighter::State), StateB = MB.U(FOKFighter::State);
	const bool bFeetSmall = ((StateA | StateB) & 0x1000) != 0;
	const auto Radius = [bFeetSmall](const FOKCollisionRow& Row)
	{
		return (bFeetSmall && (Row.BoneId == 0x73 || Row.BoneId == 0x79)) ? Row.SecondaryRadius * 0.1f : Row.SecondaryRadius;
	};
	double Overlap = 0.0;
	for (const FOKCollisionRow& RowA : A.CollisionRows)
	{
		FVector PA = FVector::ZeroVector;
		const TCHAR* BoneA = OKCollision::BoneName(RowA.BoneId);
		if (RowA.SecondaryRadius <= 0.0f || !BoneA || !A.Body->GetBoneWorldLocation(BoneA, PA))
		{
			continue;
		}
		for (const FOKCollisionRow& RowB : B.CollisionRows)
		{
			FVector PB = FVector::ZeroVector;
			const TCHAR* BoneB = OKCollision::BoneName(RowB.BoneId);
			if (RowB.SecondaryRadius <= 0.0f || !BoneB || !B.Body->GetBoneWorldLocation(BoneB, PB))
			{
				continue;
			}
			const double Reach = double(Radius(RowA)) + Radius(RowB);
			const double Along = FVector::DotProduct(PA - PB, Line), Height = PA.Z - PB.Z;
			Overlap = FMath::Max(Overlap, Reach * Reach - (Along * Along + Height * Height));
		}
	}
	const bool bNoPush = (StateA == 0x460D || StateB == 0x460D) && StateA != 0x2000 && StateB != 0x2000 && StateA != 0x2010 && StateB != 0x2010;
	if (Overlap > 0.0 && !bNoPush)
	{
		const double Each = Overlap * 0.01 * 0.5 * 0.75;
		const double Sign = PosA < PosB ? -1.0 : 1.0;
		if ((FlagsA & 0x420) == 0)
		{
			PosA += MA.F(0x77D0) * Each * Sign;
		}
		if ((FlagsB & 0x420) == 0)
		{
			PosB -= MB.F(0x77D0) * Each * Sign;
		}
	}

	// 2. The separation window: while the fighters are at most 600 apart it is their midpoint +-300; further apart it
	// stays where it was, so neither gets more than 600 from the other. Flags 0x100010 on either turn it off.
	// [inf] The second window (DAT_00DF0BDC, flag DAT_00ED556C) belongs to modes not traced.
	if ((FlagsA & 0x100010) == 0 && (FlagsB & 0x100010) == 0)
	{
		const double Apart = FMath::Abs(PosB - PosA);
		if (Apart <= MaxSeparation || !A.bSeparationWindow)
		{
			const double Middle = (PosA + PosB) * 0.5;
			A.SeparationWindow[0] = Middle - MaxSeparation * 0.5;
			A.SeparationWindow[1] = Middle + MaxSeparation * 0.5;
			A.bSeparationWindow = true;
		}
		const double Low = A.SeparationWindow[0], High = A.SeparationWindow[1];
		if (PosB >= PosA)
		{
			PosB = FMath::Min(PosB, High);
			PosA = FMath::Max(PosA, Low);
		}
		else
		{
			PosB = FMath::Max(PosB, Low);
			PosA = FMath::Min(PosA, High);
		}
	}

	// 3. The arena ends: 0 to the line's length. [inf] FUN_0048FE70's modes, where one fighter at an end carries
	// the other along, aren't done.
	if (A.LineLength > 0.0)
	{
		PosA = FMath::Clamp(PosA, 0.0, A.LineLength);
		PosB = FMath::Clamp(PosB, 0.0, A.LineLength);
	}

	if (PosA != OldA)
	{
		A.Body->SetWorldLocation(StartA + Line * (PosA - OldA));
	}
	if (PosB != OldB)
	{
		B.Body->SetWorldLocation(StartB + Line * (PosB - OldB));
	}
}
