#include "OKObjects.h"
#include "OKCollision.h"
#include "OKMkoFile.h"
#include "OKNatives.h"
#include "OKParticles.h"
#include "OKScriptRuntime.h"
#include "OKScriptThread.h"

namespace
{
	uint32 Word(FOKScriptRuntime& Runtime, uint32 Address)
	{
		uint32 Value = 0;
		Runtime.LoadAnyWord(Address, Value);
		return Value;
	}

	float Float(FOKScriptRuntime& Runtime, uint32 Address)
	{
		const uint32 Bits = Word(Runtime, Address);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, 4);
		return Value;
	}

	FVector Vector(FOKScriptRuntime& Runtime, uint32 Address)
	{
		return FVector(Float(Runtime, Address), Float(Runtime, Address + 4), Float(Runtime, Address + 8));
	}

	void SetVector(FOKScriptRuntime& Runtime, uint32 Address, const FVector& Value)
	{
		const float Parts[3] = { float(Value.X), float(Value.Y), float(Value.Z) };
		for (int32 i = 0; i < 3; ++i)
		{
			uint32 Bits = 0;
			FMemory::Memcpy(&Bits, &Parts[i], 4);
			Runtime.StoreAnyWord(Address + i * 4, Bits);
		}
	}

	/** A script and function as scripts pass them, resolved; false if either is missing. */
	bool ResolveCallback(FOKScriptRuntime& Runtime, const FOKCollisionObject::FCallback& Callback, FOKScriptInstance*& OutScript, int32& OutFunction)
	{
		const FOKScriptInstance* From = (Callback.Script >> 24) == FOKScriptRuntime::InstanceTag
			? Runtime.FindInstanceById(int32(Callback.Script & 0x00FFFFFF)) : nullptr;
		return From && Callback.Function != 0 && Runtime.ResolveFunction(*From, Callback.Function, OutScript, OutFunction);
	}

	/**
	 * MKCollisionObj::ExecuteCallback (FUN_0089F570): the function runs at once, on a thread with no stack of its own
	 * (it can't sleep), with @my_obj the owner and the collision object as its process data.
	 */
	void RunCallback(FOKScriptRuntime& Runtime, FOKCollisionObject& Object, const FOKCollisionObject::FCallback& Callback)
	{
		FOKScriptInstance* Script = nullptr;
		int32 Function = INDEX_NONE;
		if (!Object.Owner || !ResolveCallback(Runtime, Callback, Script, Function))
		{
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: the collision object has no callback for that (%08X, %u)"), Runtime.GetSimFrame(), Callback.Script, Callback.Function);
			return;
		}
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: the collision object runs %s.%s"), Runtime.GetSimFrame(),
			*Script->File->GetName().Replace(TEXT(".mko"), TEXT("")), *Script->File->GetFunctions()[Function].Name);
		// Started, then given its process data (Start clears it), then run at once.
		FOKScriptThread Thread(Runtime);
		Thread.SetCanSleep(false);
		FString Error;
		if (Thread.Start(*Script->File, Function, {}, Error))
		{
			Thread.BindFighter(*Object.Owner);
			Thread.AttachObject(Object.Handle);
			Thread.Tick();
			Error = Thread.GetError();
		}
		if (!Error.IsEmpty())
		{
			UE_LOG(LogOKVM, Warning, TEXT("collision callback %s: %s"), *Script->File->GetFunctions()[Function].Name, *Error);
		}
	}

	/** A CollisionContext of the object's own (+0x114, new_collision_context_impl): player slot at +0x18, outcome 5. */
	uint32 NewContext(FOKScriptRuntime& Runtime, const FOKFighter& Owner)
	{
		const uint32 Context = Runtime.CreateOwned(MakeUnique<FOKCollisionContext>(), FOKCollisionContext::Bytes);
		if (Context != 0)
		{
			Runtime.SetHeapWord(Context + 4, Context);
			Runtime.SetHeapWord(Context + 0x18, uint32(Owner.Index));
			Runtime.SetHeapWord(Context + FOKCollisionContext::Outcome, 5);
		}
		return Context;
	}

	/** The object's sphere against the opponent's hurt spheres, in the fight plane (FUN_008B0910). */
	bool TouchesOpponent(FOKCollisionObject& Object, FOKContact& OutContact, int64 Frame, float Radius)
	{
		const FOKFighter* Victim = Object.Owner->Opponent;
		const FOKCollisionSphere Sphere = { 0, Object.Location, Radius };
		for (const FOKCollisionRow& Row : Victim->CollisionRows)
		{
			const TCHAR* Name = OKCollision::BoneName(Row.BoneId);
			FVector Where = FVector::ZeroVector;
			if (Row.HurtRadius > 0.0f && Name && Victim->Body->GetBoneWorldLocation(Name, Where)
				&& OKCollision::Touch(Sphere, { Row.BoneId, Where, Row.HurtRadius }, Object.Owner->LineDirection))
			{
				OutContact = { 0, Row.BoneId, Object.Location, Frame };
				return true;
			}
		}
		return false;
	}

	/**
	 * CollisionObj::TestHit (FUN_008A46E0), for a sphere: once it touches the opponent it stops testing, and
	 * FightEngineScript.p_reschedule_col_reaction_to_top_of_frame resolves the contact at the top of the next frame.
	 * [inf] can_hit isn't asked (no victim states filter hits yet), and the miss timer (+0x12C) isn't kept.
	 */
	void TestHit(FOKScriptRuntime& Runtime, FOKCollisionObject& Object)
	{
		FOKFighter* Owner = Object.Owner;
		const uint32 Base = Object.Handle;
		if (!Owner || !Owner->Body || !Owner->Opponent || !Owner->Opponent->Body || Object.Props.Function == 0
			|| Word(Runtime, Base + OKCollisionObj::Touched) != 0 || Word(Runtime, Base + OKCollisionObj::Shape) != 0)
		{
			return;
		}
		const int64 Frame = Runtime.GetSimFrame();
		FOKContact Contact;
		if (!TouchesOpponent(Object, Contact, Frame, Float(Runtime, Base + OKCollisionObj::Radius)))
		{
			return;
		}
		Runtime.StoreAnyWord(Base + OKCollisionObj::Touched, 1);
		Owner->LastContact = Contact;
		const TCHAR* Bone = OKCollision::BoneName(Contact.HurtBone);
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s's collision object touches %s on the %s"), Frame, *Owner->Name, *Owner->Opponent->Name, Bone ? Bone : TEXT("?"));

		FOKScriptInstance* Fight = Runtime.FindInstanceByName(TEXT("FightEngineScript"));
		const int32 Function = Fight ? Fight->File->FindFunctionByName(TEXT("p_reschedule_col_reaction_to_top_of_frame")) : INDEX_NONE;
		FString Error;
		FOKScriptThread* Process = Function != INDEX_NONE ? Runtime.StartProcess(*Fight->File, Function, 0xCE, 2, true, Error) : nullptr;
		if (!Process)
		{
			UE_LOG(LogOKVM, Warning, TEXT("collision object: no p_reschedule_col_reaction_to_top_of_frame (%s)"), *Error);
			return;
		}
		Process->AttachObject(Object.Handle);
		Process->BindFighter(*Owner);
		if (Word(Runtime, Base + OKObject::DieFlag) != 0)
		{
			Runtime.StoreAnyWord(Base + OKObject::DieFrame, uint32(Frame + 90));
		}
	}

	/** MKObj::vf20 and vf24: the move this frame from the velocity and acceleration, then the new velocity. */
	void Integrate(FOKScriptRuntime& Runtime, FOKMkObject& Object)
	{
		const uint32 Base = Object.Handle;
		const float Dt = Runtime.GetTimeDilation() / 60.0f;		// vf14: the world's time dilation x 1/60
		const bool bGravity = Word(Runtime, Base + OKObject::Gravity) != 0;
		const FVector Acceleration = Vector(Runtime, Base + OKObject::Acceleration) + (bGravity ? Vector(Runtime, Base + OKObject::GravityVector) : FVector::ZeroVector);
		FVector Velocity = Vector(Runtime, Base + OKObject::Velocity);
		Object.Location += Velocity * Dt + Acceleration * (0.5f * Dt * Dt);
		if (!Acceleration.IsZero())
		{
			Velocity += Acceleration * Dt;
			if (bGravity && Word(Runtime, Base + OKObject::TerminalFlag) != 0)
			{
				const float Terminal = Float(Runtime, Base + OKObject::Terminal);
				Velocity.Z = Terminal > 0.0f ? FMath::Min(Velocity.Z, double(Terminal)) : FMath::Max(Velocity.Z, double(Terminal));
			}
			SetVector(Runtime, Base + OKObject::Velocity, Velocity);
		}
	}
}

FOKParticleObject::~FOKParticleObject()
{
	// MKKE deletes the component with the object; here the particles already in the air live out their lives.
	if (Actor.IsValid())
	{
		Actor->Finish();
	}
}

uint32 OKObjects::CreateParticle(FOKScriptRuntime& Runtime, const FString& SystemName, bool bDeleteWhenDone)
{
	AOKParticleActor* Actor = FOKParticles::Get().Spawn(SystemName, FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Actor)
	{
		return 0;
	}
	TUniquePtr<FOKParticleObject> Created = MakeUnique<FOKParticleObject>();
	FOKParticleObject& Object = *Created;
	const uint32 Handle = Runtime.CreateOwned(MoveTemp(Created), FOKParticleObject::Bytes);
	if (Handle == 0)
	{
		Actor->Destroy();
		return 0;
	}
	Object.Handle = Handle;
	Object.Actor = Actor;
	Object.bDeleteWhenDone = bDeleteWhenDone;
	Runtime.SetHeapWord(Handle + 4, Handle);		// serial, for handles
	Runtime.GetMkObjects().Add(Handle);
	return Handle;
}

void OKObjects::PlaceParticle(FOKParticleObject& Object)
{
	FOKFighter* Fighter = Object.AttachedTo;
	if (Fighter && Fighter->Body)
	{
		FVector Where = FVector::ZeroVector;
		if (Object.Bone.IsEmpty() || !Fighter->Body->GetBoneWorldLocation(Object.Bone, Where))
		{
			Where = Fighter->Body->GetWorldLocation();
		}
		const FVector Facing = Fighter->Body->GetFacing().GetSafeNormal2D();
		const FVector Right(-Facing.Y, Facing.X, 0.0);
		Object.Location = Where + Facing * Object.Offset.X + Right * (Fighter->bFacingRight ? Object.Offset.Y : -Object.Offset.Y) + FVector::UpVector * Object.Offset.Z;
	}
	if (AOKParticleActor* Actor = Object.Actor.Get())
	{
		// On a bone its rotation is the bone's frame's; here the fighter's facing stands for that frame.
		const FQuat Turn = Fighter && Fighter->Body ? FRotator(0.0, Fighter->Body->GetFacing().Rotation().Yaw, 0.0).Quaternion() * Object.Rotation.Quaternion() : Object.Rotation.Quaternion();
		Actor->SetActorLocationAndRotation(Object.Location, Turn);
		Actor->SetEffectScale(Object.DrawScale * Object.ComponentScale);
	}
}

uint32 OKObjects::CreateCollisionObject(FOKScriptRuntime& Runtime, FOKFighter& Owner, uint32 Script, uint32 Function)
{
	TUniquePtr<FOKCollisionObject> Created = MakeUnique<FOKCollisionObject>();
	FOKCollisionObject& Object = *Created;
	const uint32 Handle = Runtime.CreateOwned(MoveTemp(Created), OKCollisionObj::Bytes);
	if (Handle == 0)
	{
		return 0;
	}
	Object.Handle = Handle;
	Object.Owner = &Owner;
	Object.Props = { Script, Function };
	Object.Location = Owner.Body ? Owner.Body->GetWorldLocation() : FVector::ZeroVector;
	Object.Context = NewContext(Runtime, Owner);
	Runtime.SetHeapWord(Handle + 4, Handle);		// serial, for handles
	Runtime.SetHeapWord(Handle + OKCollisionObj::FromAir, (Word(Runtime, Owner.Handle + 0x2EC) & 0x80000) != 0 ? 1 : 0);
	Runtime.SetHeapWord(Handle + OKCollisionObj::DeleteAfterCallback, 1);
	Runtime.SetHeapWord(Handle + OKCollisionObj::PlayerInfo, Owner.Player ? Owner.Player->Handle : 0);
	Runtime.SetHeapWord(Handle + OKCollisionObj::Owner, Owner.Handle);
	Runtime.SetHeapWord(Handle + OKCollisionObj::PropsFunction, Function);
	Runtime.GetMkObjects().Add(Handle);
	UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s creates a collision object"), Runtime.GetSimFrame(), *Owner.Name);
	return Handle;
}

FOKMkObject* OKObjects::Find(FOKScriptRuntime& Runtime, uint32 Handle)
{
	FOKScriptObject* Object = Handle != 0 ? Runtime.ResolveHandle(Handle) : nullptr;
	return Object ? Object->AsMkObject() : nullptr;
}

void OKObjects::Tick(FOKScriptRuntime& Runtime)
{
	TArray<uint32>& Objects = Runtime.GetMkObjects();
	for (int32 i = 0; i < Objects.Num(); )
	{
		const uint32 Handle = Objects[i];
		FOKMkObject* Object = Find(Runtime, Handle);
		if (Object && Word(Runtime, Handle + OKObject::DieFlag) != 0 && Runtime.GetSimFrame() >= int64(Word(Runtime, Handle + OKObject::DieFrame)))
		{
			Runtime.DestroyObject(Handle);
			Object = nullptr;
		}
		if (!Object)
		{
			Objects.RemoveAt(i);
			continue;
		}
		Integrate(Runtime, *Object);
		if (Object->IsA(EOKScriptClass::CollisionObj))
		{
			TestHit(Runtime, static_cast<FOKCollisionObject&>(*Object));
		}
		else if (Object->IsA(EOKScriptClass::Particle))
		{
			// [inf] +0x100: an effect left to finish on its own goes when its particles have.
			FOKParticleObject& Particle = static_cast<FOKParticleObject&>(*Object);
			PlaceParticle(Particle);
			if (!Particle.Actor.IsValid() || (Particle.bDeleteWhenDone && Particle.Actor->IsComplete()))
			{
				Runtime.DestroyObject(Handle);
				Objects.RemoveAt(i);
				continue;
			}
		}
		++i;
	}
}

uint32 OKObjects::ScriptedHit(FOKScriptRuntime& Runtime, FOKFighter& Owner, FOKFighter& Victim, const FOKMkoFile& Script,
	int32 Function, uint32 Context, uint32 Word0)
{
	if (!Runtime.ResolveHandle(Context))
	{
		Context = NewContext(Runtime, Owner);
	}
	const uint32 Live = (Owner.Handle & 0x00FFFFFF) + FOKFighter::LiveAttack;
	const uint32 Pending = (Owner.Handle & 0x00FFFFFF) + FOKFighter::PendingAttack;
	uint8 Saved[OKAttack::Bytes];
	uint8* LiveBytes = Runtime.HeapBytes(Live, OKAttack::Bytes);
	if (!LiveBytes)
	{
		return Context;
	}
	FMemory::Memcpy(Saved, LiveBytes, OKAttack::Bytes);
	{
		FOKScriptThread Thread(Runtime);
		Thread.BindFighter(Owner);
		uint32 Result = 0;
		FString Error;
		if (!Thread.Run(Script, Function, {}, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("hit props %s: %s"), *Script.GetFunctions()[Function].Name, *Error);
		}
	}
	if (const uint8* PendingBytes = Runtime.HeapBytes(Pending, OKAttack::Bytes))
	{
		// MKKE's attack copy (FUN_00899370) leaves the handle words alone.
		uint8 Attack[OKAttack::Bytes];
		FMemory::Memcpy(Attack, PendingBytes, OKAttack::Bytes);
		for (const uint32 Offset : OKAttack::HandleWords)
		{
			FMemory::Memcpy(Attack + Offset, Saved + Offset, 4);
		}
		FMemory::Memcpy(Attack, &Word0, 4);
		FMemory::Memcpy(LiveBytes, Attack, OKAttack::Bytes);
	}
	OKResolveContact(Runtime, Owner, Victim, Context);
	FMemory::Memcpy(Runtime.HeapBytes(Live, OKAttack::Bytes), Saved, OKAttack::Bytes);
	return Context;
}

void OKObjects::Cleanup(FOKScriptRuntime& Runtime, FOKCollisionObject& Object)
{
	FOKFighter* Owner = Object.Owner;
	FOKFighter* Victim = Owner ? Owner->Opponent : nullptr;
	FOKScriptInstance* PropsScript = nullptr;
	int32 PropsFunction = INDEX_NONE;
	if (!Victim || !ResolveCallback(Runtime, Object.Props, PropsScript, PropsFunction))
	{
		return;
	}
	Object.Context = ScriptedHit(Runtime, *Owner, *Victim, *PropsScript->File, PropsFunction, Object.Context,
		Word(Runtime, Object.Handle + OKCollisionObj::FromAir));

	// By the outcome: 0 hit, 1 blocked. [inf] 4 (callback 3, +0x160) and 6 (reflected) need blocking and reflection.
	const uint32 Outcome = Word(Runtime, Object.Context + FOKCollisionContext::Outcome);
	if (Outcome == 0 || Outcome == 1)
	{
		const uint32 Handle = Object.Handle;
		RunCallback(Runtime, Object, Outcome == 0 ? Object.OnHit : Object.OnBlock);
		if (Find(Runtime, Handle) && Word(Runtime, Handle + OKCollisionObj::DeleteAfterCallback) != 0)
		{
			Runtime.DestroyObject(Handle);
		}
	}
}
