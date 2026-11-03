#include "OKFighterActor.h"
#include "OKCharacterMesh.h"
#include "OKPackage.h"
#include "OKStage.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PlayerController.h"
#include "Components/CapsuleComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "OKFightClockSubsystem.h"
#include "OKFighterMotion.h"
#include "OKObjects.h"
#include "OKCollision.h"
#include "OKNatives.h"
#include "DrawDebugHelpers.h"
#include "OKMkoFile.h"
#include "OKScriptLibrary.h"
#include "OKScriptSubsystem.h"
#include "OKScriptWorld.h"
#include "GameFramework/Character.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

AOKFighterActor::AOKFighterActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
}

void AOKFighterActor::BeginPlay()
{
	Super::BeginPlay();
	FloorZ = GetActorLocation().Z;
	if (UOKFightClockSubsystem* Clock = GetWorld()->GetSubsystem<UOKFightClockSubsystem>())
	{
		Clock->Register(this);
	}
}

void AOKFighterActor::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UOKFightClockSubsystem* Clock = GetWorld()->GetSubsystem<UOKFightClockSubsystem>())
	{
		Clock->Unregister(this);
	}
	if (ScriptFighter && ScriptFighter->Body == this)
	{
		ScriptFighter->Body = nullptr;
	}
	ScriptFighter = nullptr;
	Super::EndPlay(Reason);
}

bool AOKFighterActor::LoadCharacter(const FString& InPackageName, FString& OutError, int32 FighterIndex)
{
	USkeletalMesh* Loaded = OKCharacterMesh::Load(InPackageName, OutError);
	TUniquePtr<FOKPackage> Opened = Loaded ? FOKPackage::OpenGamePackage(InPackageName, OutError) : nullptr;
	if (!Opened)
	{
		return false;
	}
	PackageName = InPackageName;
	Package = MakeShareable(Opened.Release());
	SequenceCache.Reset();
	CharacterMesh = Loaded;
	// The skeleton's root-motion bone and the bone it is taken off (SkeletalMesh +0xD0 / +0xD4, set by name in FUN_00783520)
	RootBone = Loaded->GetRefSkeleton().FindBoneIndex(TEXT("position_locator"));
	RootPairBone = Loaded->GetRefSkeleton().FindBoneIndex(TEXT("Hips"));
	BuildMirrorTable();
	Mesh->SetSkinnedAssetAndUpdate(Loaded);

	// Become the body of the script world's @my_obj. Its character script (CHAR_Scorpion.mko)
	// is what the scripts' animation numbers refer to.
	UOKScriptWorld* ScriptWorld = GetWorld()->GetSubsystem<UOKScriptWorld>();
	const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
	if (ScriptWorld && ScriptWorld->GetRuntime())
	{
		ScriptFighter = &ScriptWorld->GetFighter(FighterIndex);
		ScriptFighter->Index = FighterIndex;
		ScriptFighter->Name = InPackageName;
		ScriptFighter->Body = this;
		ScriptFighter->CharacterScript = (Scripts && Scripts->GetLibrary()) ? Scripts->GetLibrary()->FindScript(InPackageName + TEXT(".mko")) : nullptr;
		if (!ScriptFighter->CharacterScript)
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: no %s.mko in the script library; scripts can't name its animations"), *InPackageName, *InPackageName);
		}
		else if (!OKCollision::ReadTable(*ScriptFighter->CharacterScript, ScriptFighter->CollisionRows))
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: no collision table in its script; it can't hit or be hit"), *InPackageName);
		}
		FString InitError;
		if (ScriptFighter->CharacterScript && !OKInitCharacter(*ScriptWorld->GetRuntime(), *ScriptFighter, InitError))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: starting its character script failed: %s"), *InPackageName, *InitError);
		}
	}
	return true;
}

/**
 * Startup holds the animations every fighter shares (the Reactions and Airborne sets the engine scripts name).
 * It's 50 MB once inflated, so it's opened once and kept for the whole run.
 */
static FOKPackage* SharedAnimationPackage()
{
	static TUniquePtr<FOKPackage> Startup;
	static bool bTried = false;
	if (!bTried)
	{
		bTried = true;
		FString Error;
		Startup = FOKPackage::OpenGamePackage(TEXT("Startup"), Error);
		if (!Startup)
		{
			UE_LOG(LogOKAsset, Error, TEXT("Shared animations: %s"), *Error);
		}
	}
	return Startup.Get();
}

const FOKAnimSequence* AOKFighterActor::FindSequence(const FString& SequenceName, FString& OutError)
{
	if (const FOKAnimSequence* Cached = SequenceCache.Find(SequenceName))
	{
		return Cached;
	}
	// The character's own package first, then the shared one.
	FOKAnimSequence Loaded;
	FString SharedError;
	FOKPackage* Shared = nullptr;
	if (!Package || !Loaded.Find(*Package, SequenceName, OutError))
	{
		Shared = SharedAnimationPackage();
		if (!Shared || !Loaded.Find(*Shared, SequenceName, SharedError))
		{
			// Then the opponent's: a victim plays its attacker's reactions (Supermove_Spear_Victim_Reaction_Into02 is
			// in Anims_Scorpion). The fighter actor is the only body there is.
			const AOKFighterActor* Other = ScriptFighter && ScriptFighter->Opponent ? static_cast<const AOKFighterActor*>(ScriptFighter->Opponent->Body) : nullptr;
			FString OtherError;
			if (!Other || !Other->Package || !Loaded.Find(*Other->Package, SequenceName, OtherError))
			{
				return nullptr;
			}
		}
		OutError.Reset();
	}
	return &SequenceCache.Add(SequenceName, MoveTemp(Loaded));
}

bool AOKFighterActor::Play(const FString& SequenceName, bool bInLoop, float StartFrame, bool bHold, FString& OutError)
{
	if (!CharacterMesh)
	{
		OutError = TEXT("no character loaded");
		return false;
	}
	const FOKAnimSequence* Found = FindSequence(SequenceName, OutError);
	if (!Found)
	{
		return false;
	}

	// Tracks name their bones; find each in the mesh. Bones without a track keep the reference pose.
	TrackToBone.Reset();
	int32 Missing = 0;
	for (const FString& Bone : Found->TrackBones)
	{
		TrackToBone.Add(CharacterMesh->GetRefSkeleton().FindBoneIndex(FName(*Bone)));
		Missing += TrackToBone.Last() == INDEX_NONE ? 1 : 0;
	}
	Sequence = *Found;
	TrackGroup = 0;
	Frame = FMath::Clamp(StartFrame, 0.0f, float(FMath::Max(0, Sequence.NumFrames - 1)));
	RootTransform = SampleRoot(Frame);
	bNodeMirrored = false;		// a new animation node starts unmirrored (node flag 0x40)
	EventFrame = Frame - 1.0f;		// so events on the start frame fire (FUN_008A4080 sets +0x2D4 = start - 1)
	bLoop = bInLoop;
	bPlaying = !bHold;
	Rate = 1.0f;
	BlendFrames = 0.0f;
	ApplyPose();
	UE_LOG(LogOKAsset, Log, TEXT("%s: playing %s, %d frames, %.3f s, %d tracks%s"), *PackageName, *Sequence.Name, Sequence.NumFrames, Sequence.Length,
		Sequence.Tracks.Num(), Missing ? *FString::Printf(TEXT(" (%d track bones not in the mesh)"), Missing) : TEXT(""));
	return true;
}

bool AOKFighterActor::PlayAnimation(const FString& SequenceName, float StartFrame, float InRate, float InBlendFrames, bool bInLoop)
{
	if (CinemaPackage)
	{
		// The cinema has the body: the request waits until it lets go (Queued).
		FString Error;
		const FOKAnimSequence* Found = CharacterMesh ? FindSequence(SequenceName, Error) : nullptr;
		if (!Found)
		{
			UE_LOG(LogOKAsset, Error, TEXT("%s"), *Error);
			return false;
		}
		Queued.Sequence = *Found;
		Queued.TrackToBone.Reset();
		for (const FString& Bone : Found->TrackBones)
		{
			Queued.TrackToBone.Add(CharacterMesh->GetRefSkeleton().FindBoneIndex(FName(*Bone)));
		}
		Queued.Frame = FMath::Clamp(StartFrame, 0.0f, float(FMath::Max(0, Found->NumFrames - 1)));
		Queued.Rate = InRate > 0.0f ? InRate : 1.0f;
		Queued.BlendFrames = InBlendFrames;
		Queued.bLoop = bInLoop;
		Queued.bSet = true;
		return true;
	}
	// Crossfade from whatever is showing now (ANIM-RE-NOTES §4.2: linear over BlendFrames / 60 s).
	const TArray<FTransform> Previous = Mesh->BoneSpaceTransforms;
	FString Error;
	if (!Play(SequenceName, bInLoop, StartFrame, false, Error))
	{
		UE_LOG(LogOKAsset, Error, TEXT("%s"), *Error);
		return false;
	}
	Rate = InRate > 0.0f ? InRate : 1.0f;
	if (InBlendFrames > 0.0f && Previous.Num() == Mesh->BoneSpaceTransforms.Num())
	{
		BlendFrom = Previous;
		BlendFrames = InBlendFrames;
		BlendDone = 0.0f;
		ApplyPose();
	}
	return true;
}

void AOKFighterActor::SimTick(int64 SimFrame)
{
	if (CinemaPackage)
	{
		return;		// the cinema poses and places it (FOKCinemaPlayer)
	}
	// MKKE's frame (ENGINE-RE-NOTES §4): the object update, then the actor tick (animation and root motion), then
	// the fighter's step (jumps, force-aways).
	UOKScriptWorld* MotionWorld = ScriptFighter ? GetWorld()->GetSubsystem<UOKScriptWorld>() : nullptr;
	FOKScriptRuntime* MotionRuntime = MotionWorld ? MotionWorld->GetRuntime() : nullptr;
	if (MotionRuntime)
	{
		OKMotion::PreStep(*MotionRuntime, *ScriptFighter);
	}

	const float FrameBefore = Frame;
	bool bWrapped = false;
	// A hit pause (slow_time) or char_anim_speed slows the animation and its blends; the frame still runs.
	const float TimeScale = MotionRuntime ? MotionRuntime->GetTimeDilation() * MotionRuntime->GetCharAnimSpeed() : 1.0f;
	if (BlendFrames > 0.0f)
	{
		BlendDone += TimeScale;
		if (BlendDone >= BlendFrames)
		{
			BlendFrames = 0.0f;
		}
	}
	if (bPlaying && Sequence.NumFrames > 1)
	{
		// One sim frame is 1/60 s; MK sequences are 60 frames a second, so at rate 1 one frame per tick.
		const float LastFrame = float(Sequence.NumFrames - 1);
		Frame += Sequence.FramesPerSecond() / 60.0f * Rate * TimeScale;
		if (Frame > LastFrame)
		{
			Frame = bLoop ? FMath::Fmod(Frame, LastFrame) : LastFrame;
			bPlaying = bLoop;
		}
		bWrapped = bLoop && Frame < FrameBefore;
	}
	ApplyPose();
	ApplyRootMotion(bWrapped);
	if (MotionRuntime)
	{
		OKMotion::Step(*MotionRuntime, *ScriptFighter);
		// The second fighter steps last, so both have moved: then they push each other apart.
		if (ScriptFighter->Index == 1 && ScriptFighter->Opponent)
		{
			// A throw's victim follows its attacker (F10); the two overlap on purpose, so they don't push apart.
			OKUpdateThrow(*MotionRuntime, *ScriptFighter);
			OKUpdateThrow(*MotionRuntime, *ScriptFighter->Opponent);
			if (!ScriptFighter->ThrowLeader && !ScriptFighter->Opponent->ThrowLeader)
			{
				OKMotion::PushApart(*MotionRuntime, *ScriptFighter->Opponent, *ScriptFighter);
			}
			// Then the MK objects (projectiles): they move and test for contact against where the fighters are now.
			OKObjects::Tick(*MotionRuntime);
		}
	}

	if (ScriptFighter)
	{
		// The frame events this frame's animation reached start their processes (FUN_008A0400). Those run in
		// the next frame's script phase, like MKKE's.
		UOKScriptWorld* ScriptWorld = GetWorld()->GetSubsystem<UOKScriptWorld>();
		if (ScriptWorld && ScriptWorld->GetRuntime())
		{
			OKFireFrameEvents(*ScriptWorld->GetRuntime(), *ScriptFighter, EventFrame, Frame, Rate, bLoop);
		}
		EventFrame = Frame;

		// This frame's attack spheres become last frame's, for filling gaps (FUN_008AFDD0).
		DrawCollision();
		ScriptFighter->PreviousAttackSpheres = MoveTemp(ScriptFighter->AttackSpheres);
		ScriptFighter->AttackSpheres.Reset();
	}
}

bool AOKFighterActor::GetBoneWorldLocation(const FString& BoneName, FVector& OutLocation) const
{
	const FName Bone(*BoneName);
	if (!CharacterMesh)
	{
		return false;
	}
	if (CharacterMesh->GetRefSkeleton().FindBoneIndex(Bone) == INDEX_NONE)
	{
		// Scripts name sockets the same way (Spear_Left, Sword_Right): a point on a bone (FUN_007866C0).
		if (!Mesh->DoesSocketExist(Bone))
		{
			return false;
		}
		OutLocation = Mesh->GetSocketLocation(Bone);
		return true;
	}
	OutLocation = Mesh->GetBoneLocationByName(Bone, EBoneSpaces::WorldSpace);
	return true;
}

void AOKFighterActor::SetOpponents(AOKFighterActor& A, AOKFighterActor& B)
{
	if (A.ScriptFighter && B.ScriptFighter)
	{
		A.ScriptFighter->Opponent = B.ScriptFighter;
		B.ScriptFighter->Opponent = A.ScriptFighter;
		if (UOKScriptWorld* ScriptWorld = A.GetWorld()->GetSubsystem<UOKScriptWorld>())
		{
			// Each fighter's handle to its opponent (+0x763C), which MKKE binds @his_obj to. Our serial is the handle.
			FOKScriptRuntime* Runtime = ScriptWorld->GetRuntime();
			for (FOKFighter* F : { A.ScriptFighter, B.ScriptFighter })
			{
				Runtime->SetHeapWord(F->Handle + FOKFighter::OpponentHandle + 4, F->Opponent->Handle);
				Runtime->SetHeapWord(F->Handle + FOKFighter::OpponentHandle + 8, F->Opponent->Handle);
			}
		}
	}
}

void AOKFighterActor::ApplyPose()
{
	if (!CharacterMesh || Mesh->BoneSpaceTransforms.Num() != CharacterMesh->GetRefSkeleton().GetNum())
	{
		return;
	}
	TArray<FTransform> Pose = CharacterMesh->GetRefSkeleton().GetRefBonePose();
	Sequence.Sample(Frame, TrackToBone, Pose, TrackGroup);
	// The root-motion bone's translation is extracted (MKKE's keep factors are 0) and taken off the hips too, so the body
	// stays over the actor: it moves the actor instead (FUN_0049CF50).
	if (Pose.IsValidIndex(RootBone))
	{
		const FTransform Extracted = Pose[RootBone];
		Pose[RootBone].SetTranslation(FVector::ZeroVector);
		Pose[RootBone].SetRotation(FQuat::Identity);
		if (Pose.IsValidIndex(RootPairBone) && RootPairBone != RootBone && RootPairBone != 0)
		{
			// in the locator's frame, so a turnaround's yaw is taken off the body too
			Pose[RootPairBone] = Pose[RootPairBone].GetRelativeTransform(Extracted);
		}
	}
	if (bNodeMirrored)
	{
		MirrorPose(Pose);
	}
	if (BlendFrames > 0.0f && BlendFrom.Num() == Pose.Num())
	{
		const float Alpha = FMath::Clamp(BlendDone / BlendFrames, 0.0f, 1.0f);
		for (int32 Bone = 0; Bone < Pose.Num(); ++Bone)
		{
			const FTransform Target = Pose[Bone];
			Pose[Bone].Blend(BlendFrom[Bone], Target, Alpha);		// lerp, and a normalised lerp for rotations, like MK
		}
	}
	Mesh->BoneSpaceTransforms = Pose;
	Mesh->MarkRefreshTransformDirty();
}

FTransform AOKFighterActor::SampleRoot(float AtFrame) const
{
	return SampleRoot(AtFrame, TrackGroup);
}

FTransform AOKFighterActor::SampleRoot(float AtFrame, int32 Group) const
{
	if (!CharacterMesh)
	{
		return FTransform::Identity;
	}
	TArray<FTransform> Pose = CharacterMesh->GetRefSkeleton().GetRefBonePose();
	if (!Pose.IsValidIndex(RootBone))
	{
		return FTransform::Identity;
	}
	Sequence.Sample(AtFrame, TrackToBone, Pose, Group);
	FTransform Root = Pose[RootBone];
	Root.SetScale3D(FVector::OneVector);
	return Root;
}

/** The reflection across the component's XZ plane (y -> -y), as a proper rotation: what a mirror does to a transform. */
static FTransform Reflect(const FTransform& In)
{
	const FVector T = In.GetTranslation();
	const FQuat Q = In.GetRotation();
	return FTransform(FQuat(-Q.X, Q.Y, -Q.Z, Q.W), FVector(T.X, -T.Y, T.Z), In.GetScale3D());
}

void AOKFighterActor::BuildMirrorTable()
{
	const FReferenceSkeleton& Skeleton = CharacterMesh->GetRefSkeleton();
	const int32 Num = Skeleton.GetNum();
	MirrorBone.SetNum(Num);
	MirrorCorrection.SetNum(Num);
	TArray<FTransform> Ref;
	ComponentSpace(Skeleton.GetRefBonePose(), Ref);
	for (int32 Bone = 0; Bone < Num; ++Bone)
	{
		// LeftArm <-> RightArm; bones on the middle line are their own mirror
		const FString Name = Skeleton.GetBoneName(Bone).ToString();
		FString Other = Name.Contains(TEXT("Left")) ? Name.Replace(TEXT("Left"), TEXT("Right")) : Name.Replace(TEXT("Right"), TEXT("Left"));
		const int32 Found = Other == Name ? INDEX_NONE : Skeleton.FindBoneIndex(FName(*Other));
		MirrorBone[Bone] = Found == INDEX_NONE ? Bone : Found;
		// Bone axes aren't mirror images of each other, so a mirrored bone is corrected by how its own reference pose
		// differs from the mirrored reference pose of its pair.
		MirrorCorrection[Bone] = Reflect(Ref[MirrorBone[Bone]]).GetRotation().Inverse() * Ref[Bone].GetRotation();
	}
}

void AOKFighterActor::ComponentSpace(const TArray<FTransform>& Local, TArray<FTransform>& Out) const
{
	const FReferenceSkeleton& Skeleton = CharacterMesh->GetRefSkeleton();
	Out.SetNum(Local.Num());
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		const int32 Parent = Skeleton.GetParentIndex(Bone);
		Out[Bone] = Parent == INDEX_NONE ? Local[Bone] : Local[Bone] * Out[Parent];
	}
}

void AOKFighterActor::MirrorPose(TArray<FTransform>& Pose) const
{
	if (MirrorBone.Num() != Pose.Num())
	{
		return;
	}
	TArray<FTransform> Space, Mirrored;
	ComponentSpace(Pose, Space);
	Mirrored.SetNum(Pose.Num());
	for (int32 Bone = 0; Bone < Pose.Num(); ++Bone)
	{
		const FTransform From = Reflect(Space[MirrorBone[Bone]]);
		Mirrored[Bone] = FTransform((From.GetRotation() * MirrorCorrection[Bone]).GetNormalized(), From.GetTranslation(), Space[Bone].GetScale3D());
	}
	const FReferenceSkeleton& Skeleton = CharacterMesh->GetRefSkeleton();
	for (int32 Bone = 0; Bone < Pose.Num(); ++Bone)
	{
		const int32 Parent = Skeleton.GetParentIndex(Bone);
		const FVector Scale = Pose[Bone].GetScale3D();
		Pose[Bone] = Parent == INDEX_NONE ? Mirrored[Bone] : Mirrored[Bone].GetRelativeTransform(Mirrored[Parent]);
		Pose[Bone].SetScale3D(Scale);
	}
}

void AOKFighterActor::SetupForFlip()
{
	// FUN_00442160: the mesh's mirror and the playing node's both toggle, so the pose on screen doesn't jump; the
	// animations that start from now on play on the mirrored mesh.
	bNodeMirrored = !bNodeMirrored;
	SetMeshMirrored(!bMeshMirrored);
}

void AOKFighterActor::SetMeshMirrored(bool bMirrored)
{
	bMeshMirrored = bMirrored;
	SetActorScale3D(FVector(1.0, bMeshMirrored ? -1.0 : 1.0, 1.0));
	ApplyPose();
}

void AOKFighterActor::ApplyRootMotion(bool bWrapped)
{
	// The locator's move since last frame, in its own previous frame (so a step taken while turning stays a step)
	const FTransform Now = SampleRoot(Frame);
	FTransform Delta = Now.GetRelativeTransform(RootTransform);
	if (bWrapped)
	{
		// The rest of the loop, then from its start
		const FTransform ToEnd = SampleRoot(float(Sequence.NumFrames - 1)).GetRelativeTransform(RootTransform);
		Delta = Now.GetRelativeTransform(SampleRoot(0.0f)) * ToEnd;
	}
	RootTransform = Now;
	if (bNodeMirrored)
	{
		Delta = Reflect(Delta);
	}
	// A throw victim's own tracks (group 1) face the other way (FollowThrow); once let go, it moves by them itself.
	if (TrackGroup == 1 && (!ScriptFighter || (ScriptFighter->ThrowFlags & 1) == 0))
	{
		Delta.SetTranslation(FVector(-Delta.GetTranslation().X, -Delta.GetTranslation().Y, Delta.GetTranslation().Z));
	}
	// During a crossfade the new sequence moves the fighter by its blend weight; the old pose is held, so adds nothing.
	const float Weight = BlendFrames > 0.0f ? FMath::Clamp(BlendDone / BlendFrames, 0.0f, 1.0f) : 1.0f;
	const FVector Move = Delta.GetTranslation() * Weight * (ScriptFighter ? ScriptFighter->RootMotionScale : 1.0f);
	FQuat Turn = FQuat::Slerp(FQuat::Identity, Delta.GetRotation(), Weight);
	if (Move.IsNearlyZero() && Turn.Equals(FQuat::Identity))
	{
		return;
	}
	// UE3's root-motion task (FUN_00762C10): the move into the world by the component (its mirror included), then
	// SetLocation with no sweep; with rotation mode on (+0x650) the turn is applied too. A mirrored mesh turns the
	// other way.
	if (bMeshMirrored)
	{
		Turn = Reflect(FTransform(Turn)).GetRotation();
	}
	SetActorLocationAndRotation(GetActorLocation() + Mesh->GetComponentTransform().TransformVector(Move), (GetActorQuat() * Turn).GetNormalized());
}

void AOKFighterActor::SetAnimFrame(float InFrame)
{
	if (IsQueued())
	{
		Queued.Frame = FMath::Clamp(InFrame, 0.0f, float(FMath::Max(0, Queued.Sequence.NumFrames - 1)));
		return;
	}
	Frame = FMath::Clamp(InFrame, 0.0f, float(FMath::Max(0, Sequence.NumFrames - 1)));
	RootTransform = SampleRoot(Frame);
}

void AOKFighterActor::FollowThrow(const IOKFighterBody& LeaderBody)
{
	// The fighter actor is the only body there is.
	const AOKFighterActor& Leader = static_cast<const AOKFighterActor&>(LeaderBody);
	if (Sequence.Name != Leader.Sequence.Name)
	{
		return;
	}
	TrackGroup = 1;
	Rate = Leader.Rate;
	SetAnimFrame(Leader.Frame);		// the leader's frame drives it
	ApplyPose();

	// Both groups share one origin, where the leader's actor stood when its root was at the identity: its root (group 0) is
	// kept out of its pose and moves its actor, turns included. The follower's root (group 1) is written facing the other way,
	// so MKKE negates its X/Y, unless start_throw's flag bit 0 says not to (FUN_00490900). Seen from the leader's root, that
	// is where the follower's actor goes, through the leader's mesh (its facing and mirror).
	FVector Position = RootTransform.GetTranslation();
	if (!ScriptFighter || (ScriptFighter->ThrowFlags & 1) == 0)
	{
		Position.X = -Position.X;
		Position.Y = -Position.Y;
	}
	const FTransform Rel(Leader.RootTransform.GetRotation().Inverse() * RootTransform.GetRotation(), Leader.RootTransform.InverseTransformPositionNoScale(Position));
	const FTransform& LeaderMesh = Leader.Mesh->GetComponentTransform();
	const FVector Offset = LeaderMesh.TransformPosition(Rel.GetTranslation()) - LeaderMesh.GetLocation();
	const FQuat Turn = bMeshMirrored ? Reflect(FTransform(Rel.GetRotation())).GetRotation() : Rel.GetRotation();
	SetActorLocationAndRotation(Leader.GetActorLocation() + Offset, (Leader.GetActorQuat() * Turn).GetNormalized());
}

void AOKFighterActor::BeginCinema(TSharedPtr<FOKPackage> InCinemaPackage)
{
	if (!CinemaPackage)
	{
		bMirroredBeforeCinema = bMeshMirrored;
	}
	CinemaPackage = InCinemaPackage;
	Queued = FQueuedAnim();
	bNodeMirrored = false;
	BlendFrames = 0.0f;
	SetMeshMirrored(false);
}

void AOKFighterActor::EndCinema()
{
	if (CinemaPackage)
	{
		CinemaPackage.Reset();
		SetMeshMirrored(bMirroredBeforeCinema);
		if (Queued.bSet)
		{
			// what the scripts asked for meanwhile starts now, blending from the cinema's last pose
			BlendFrom = Mesh->BoneSpaceTransforms;
			Sequence = MoveTemp(Queued.Sequence);
			TrackToBone = MoveTemp(Queued.TrackToBone);
			TrackGroup = 0;
			Frame = Queued.Frame;
			RootTransform = SampleRoot(Frame);
			bNodeMirrored = false;
			EventFrame = Frame - 1.0f;
			bLoop = Queued.bLoop;
			bPlaying = true;
			Rate = Queued.Rate;
			BlendFrames = Queued.BlendFrames;
			BlendDone = 0.0f;
			ApplyPose();
			UE_LOG(LogOKAsset, Log, TEXT("%s: after the cinema, playing %s from frame %.0f"), *PackageName, *Sequence.Name, Frame);
		}
		Queued = FQueuedAnim();
	}
}

void AOKFighterActor::CinemaPose(const FString& SequenceName, float Seconds, const FTransform& Scene)
{
	if (!CharacterMesh || !CinemaPackage)
	{
		return;
	}
	if (Sequence.Name != SequenceName)
	{
		// The cinema's package first (Anims_Scorpion_Fatality is in Scorpion_Fatality1), then the fighter's own.
		const FString Key = TEXT("cinema:") + SequenceName;
		const FOKAnimSequence* Found = SequenceCache.Find(Key);
		FString Error;
		if (!Found)
		{
			FOKAnimSequence Loaded;
			if (Loaded.Find(*CinemaPackage, SequenceName, Error))
			{
				Found = &SequenceCache.Add(Key, MoveTemp(Loaded));
			}
			else
			{
				Found = FindSequence(SequenceName, Error);
			}
		}
		if (!Found)
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: cinema sequence %s: %s"), *PackageName, *SequenceName, *Error);
			SequenceCache.Add(Key, FOKAnimSequence());		// not looked for again
			return;
		}
		if (Found->NumFrames == 0)
		{
			return;
		}
		TrackToBone.Reset();
		for (const FString& Bone : Found->TrackBones)
		{
			TrackToBone.Add(CharacterMesh->GetRefSkeleton().FindBoneIndex(FName(*Bone)));
		}
		Sequence = *Found;
		TrackGroup = 0;
		UE_LOG(LogOKAsset, Log, TEXT("%s: cinema plays %s, %d frames"), *PackageName, *Sequence.Name, Sequence.NumFrames);
	}
	Frame = FMath::Clamp(Seconds * Sequence.FramesPerSecond(), 0.0f, float(FMath::Max(0, Sequence.NumFrames - 1)));
	ApplyPose();
	RootTransform = SampleRoot(Frame);

	// The root kept out of the pose moves the actor from where the cinema puts it, as a throw's leader moves its follower.
	SetActorLocationAndRotation(Scene.GetLocation(), Scene.GetRotation());
	const FTransform& Component = Mesh->GetComponentTransform();
	const FVector Offset = Component.TransformPosition(RootTransform.GetTranslation()) - Component.GetLocation();
	SetActorLocationAndRotation(Scene.GetLocation() + Offset, (Scene.GetRotation() * RootTransform.GetRotation()).GetNormalized());
}

FVector AOKFighterActor::GetRootOffset() const
{
	return Mesh->GetComponentTransform().TransformVector(RootTransform.GetTranslation());
}

int32 AOKFighterActor::GetSequenceFrameCount(const FString& SequenceName)
{
	FString Error;
	const FOKAnimSequence* Found = CharacterMesh ? FindSequence(SequenceName, Error) : nullptr;
	return Found ? Found->NumFrames : 0;
}

float AOKFighterActor::GetSequenceBoneHeight(const FString& BoneName, float AtFrame) const
{
	const int32 Bone = CharacterMesh ? CharacterMesh->GetRefSkeleton().FindBoneIndex(FName(*BoneName)) : INDEX_NONE;
	if (Bone == INDEX_NONE)
	{
		return 0.0f;
	}
	const FReferenceSkeleton& Skeleton = CharacterMesh->GetRefSkeleton();
	TArray<FTransform> Pose = Skeleton.GetRefBonePose();
	Sequence.Sample(AtFrame, TrackToBone, Pose, TrackGroup);
	FTransform At = Pose[Bone];
	for (int32 Parent = Skeleton.GetParentIndex(Bone); Parent != INDEX_NONE; Parent = Skeleton.GetParentIndex(Parent))
	{
		At = At * Pose[Parent];
	}
	return float(Mesh->GetComponentQuat().RotateVector(At.GetTranslation()).Z);
}

bool AOKFighterActor::RunScript(const FString& Function, FString& OutError)
{
	UOKScriptWorld* ScriptWorld = GetWorld()->GetSubsystem<UOKScriptWorld>();
	const FOKMkoFile* Script = ScriptFighter ? ScriptFighter->CharacterScript : nullptr;
	const int32 Index = Script ? Script->FindFunctionByName(Function) : INDEX_NONE;
	if (!ScriptWorld || !ScriptWorld->GetRuntime() || Index == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s has no script function %s"), *PackageName, *Function);
		return false;
	}
	FOKScriptThread* Process = ScriptWorld->GetRuntime()->StartProcess(*Script, Index, 0, 14, true, OutError);
	if (!Process)
	{
		return false;
	}
	Process->BindFighter(*ScriptFighter);
	ScriptFighter->MainThread = Process->GetHandle();		// for now the script we run is the fighter's own thread
	return true;
}

FVector AOKFighterActor::GetBoneLocation(const FName& Bone) const
{
	return Mesh->GetBoneLocationByName(Bone, EBoneSpaces::ComponentSpace);
}

// --- Console tools -----------------------------------------

static TAutoConsoleVariable<float> CVarFighterYaw(
	TEXT("ok.Fighter.Yaw"),
	0.0f,
	TEXT("Extra yaw (degrees) for fighters spawned by ok.Fighter.Show, to match the MK mesh's forward axis."));

static TAutoConsoleVariable<bool> CVarDebugCollision(
	TEXT("ok.Debug.Collision"),
	false,
	TEXT("Draw fighters' hurtboxes (green) and attack spheres (red)."));

void AOKFighterActor::DrawCollision() const
{
	if (!CVarDebugCollision.GetValueOnGameThread() || !ScriptFighter)
	{
		return;
	}
	for (const FOKCollisionRow& Row : ScriptFighter->CollisionRows)
	{
		const TCHAR* Name = OKCollision::BoneName(Row.BoneId);
		FVector Where = FVector::ZeroVector;
		if (Row.HurtRadius > 0.0f && Name && GetBoneWorldLocation(Name, Where))
		{
			DrawDebugSphere(GetWorld(), Where, Row.HurtRadius, 12, FColor::Green, false, 1.0f / 60.0f);
		}
	}
	for (const FOKCollisionSphere& Sphere : ScriptFighter->AttackSpheres)
	{
		DrawDebugSphere(GetWorld(), Sphere.Center, Sphere.Radius, 12, FColor::Red, false, 1.0f);		// kept a second, so a punch can be seen
	}
}

namespace
{
	/** The world of the game that's playing (Play in Editor or a standalone game). */
	UWorld* FindPlayingWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** Where the player stands on the ground, and their facing and right vectors. */
	void PlayerGround(APawn* Pawn, FVector& OutFeet, FVector& OutForward, FVector& OutRight)
	{
		float FeetBelowCentre = 0.0f;
		if (const ACharacter* Character = Cast<ACharacter>(Pawn))
		{
			FeetBelowCentre = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		}
		OutFeet = Pawn->GetActorLocation() - FVector(0.0f, 0.0f, FeetBelowCentre);
		OutForward = Pawn->GetActorForwardVector();
		OutRight = Pawn->GetActorRightVector();
	}

	/** Tags what ok.Fight spawns for a stage (the stage, its camera), so the next ok.Fight can clear it. */
	const FName StageTag(TEXT("OKStage"));

	/**
	 * Views the fight through a camera on the near side of the fight line. [inf] MKKE's fight camera (MKFGCameraMgr,
	 * system W3) isn't ported: this is a fixed framing of the two fighters.
	 */
	void ViewFight(UWorld* World, const FVector& Centre, const FVector& Forward)
	{
		const FVector Eye = Centre - Forward * 650.0 + FVector(0.0, 0.0, 150.0);
		const FVector Target = Centre + FVector(0.0, 0.0, 110.0);
		ACameraActor* Camera = World->SpawnActor<ACameraActor>(Eye, (Target - Eye).Rotation());
		Camera->Tags.Add(StageTag);
		Camera->GetCameraComponent()->SetFieldOfView(45.0f);
		Camera->GetCameraComponent()->SetConstraintAspectRatio(false);
		if (APlayerController* Controller = World->GetFirstPlayerController())
		{
			Controller->SetViewTarget(Camera);
		}
	}

	AOKFighterActor* SpawnFighter(UWorld* World, const FString& PackageName, int32 FighterIndex, const FVector& Location, float Yaw)
	{
		AOKFighterActor* Fighter = World->SpawnActor<AOKFighterActor>(Location, FRotator(0.0f, Yaw, 0.0f));
		FString Error;
		if (!Fighter || !Fighter->LoadCharacter(PackageName, Error, FighterIndex))
		{
			UE_LOG(LogOKAsset, Error, TEXT("Can't show %s: %s"), *PackageName, *Error);
			if (Fighter)
			{
				Fighter->Destroy();
			}
			return nullptr;
		}
		UE_LOG(LogOKAsset, Log, TEXT("Showing %s at %s"), *PackageName, *Location.ToString());
		return Fighter;
	}

	/**
	 * Runs Spawn once there's a player (commands may run from -ExecCmds before the map has loaded). With bShot,
	 * waits for shader compiling and takes a screenshot (Saved/Screenshots/.../OKFighter.png) ShotDelay frames
	 * later; with bQuit, exits afterwards. For automated checks.
	 */
	void WhenPlayerReady(TFunction<bool(UWorld*, APawn*)> Spawn, bool bShot, bool bQuit, int32 ShotDelay)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Spawn, bShot, bQuit, ShotDelay, Frames = 0, bSpawned = false, ShotFrame = -1](float) mutable -> bool
			{
				++Frames;
				UWorld* World = FindPlayingWorld();
				if (!bSpawned)
				{
					APawn* Pawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
					if (!Pawn)
					{
						if (Frames > 1200)
						{
							UE_LOG(LogOKAsset, Error, TEXT("No player to spawn fighters next to. Press Play first."));
							return false;
						}
						return true;
					}
					bSpawned = Spawn(World, Pawn);
					Frames = 0;
					return bSpawned && (bShot || bQuit);
				}
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				if (bShot && ShotFrame < 0 && Frames > ShotDelay && (!bCompiling || Frames > 6000))
				{
					FScreenshotRequest::RequestScreenshot(TEXT("OKFighter"), false, false);
					ShotFrame = Frames;
				}
				const bool bDone = !bShot || (ShotFrame >= 0 && Frames > ShotFrame + 60);
				if (bDone && bQuit)
				{
					GEngine->Exec(World, TEXT("quit"));
					return false;
				}
				return !bDone;
			}));
	}

	/** "shot=N" in a command's arguments: take the screenshot N frames after spawning (default 60). */
	int32 ShotDelayFrom(const TArray<FString>& Args)
	{
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(TEXT("shot=")))
			{
				return FCString::Atoi(*Arg.RightChop(5));
			}
		}
		return 60;
	}

	bool WantsShot(const TArray<FString>& Args)
	{
		return Args.ContainsByPredicate([](const FString& Arg) { return Arg == TEXT("shot") || Arg.StartsWith(TEXT("shot=")); });
	}
}

static FAutoConsoleCommand CmdFighterShow(
	TEXT("ok.Fighter.Show"),
	TEXT("ok.Fighter.Show <Package> [shot[=frames]] [quit]: spawn a character in front of the player, e.g. ok.Fighter.Show CHAR_Scorpion. ")
	TEXT("'shot' takes a screenshot once shaders have compiled, 'quit' exits afterwards (for automated checks)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Fighter.Show CHAR_Scorpion"));
				return;
			}
			const FString PackageName = Args[0];
			WhenPlayerReady([PackageName](UWorld* World, APawn* Pawn)
				{
					// 3 m in front of the player and a little to the side (so the player doesn't hide them), facing them.
					FVector Feet, Forward, Right;
					PlayerGround(Pawn, Feet, Forward, Right);
					const FVector Location = Feet + Forward * 300.0f + Right * 150.0f;
					const float Yaw = (Pawn->GetActorLocation() - Location).Rotation().Yaw + CVarFighterYaw.GetValueOnGameThread();
					return SpawnFighter(World, PackageName, 0, Location, Yaw) != nullptr;
				}, WantsShot(Args), Args.Contains(TEXT("quit")), ShotDelayFrom(Args));
		}));

static FAutoConsoleCommand CmdCamera(
	TEXT("ok.Camera"),
	TEXT("ok.Camera <x> <y> <z> <pitch> <yaw> [fov]: move the camera ok.Fight views a stage through, e.g. ok.Camera -2500 0 1200 -25 0"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 5)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Camera -2500 0 1200 -25 0 [fov]"));
				return;
			}
			// Waits (up to ~20 s) for ok.Fight's camera, so both can go in one -ExecCmds.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Args, Frames = 0](float) mutable -> bool
				{
					UWorld* World = FindPlayingWorld();
					for (TActorIterator<ACameraActor> It(World); World && It; ++It)
					{
						if (It->ActorHasTag(StageTag))
						{
							It->SetActorLocationAndRotation(FVector(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2])),
								FRotator(FCString::Atod(*Args[3]), FCString::Atod(*Args[4]), 0.0));
							if (Args.Num() > 5)
							{
								It->GetCameraComponent()->SetFieldOfView(FCString::Atof(*Args[5]));
							}
							return false;
						}
					}
					return ++Frames < 1200;
				}));
		}));

static FAutoConsoleCommand CmdFight(
	TEXT("ok.Fight"),
	TEXT("ok.Fight <Player1> <Player2> [distance] [stage=<BGND_...>|stage=none] [shot[=frames]] [quit]: two fighters facing each other, ")
	TEXT("e.g. ok.Fight CHAR_Scorpion CHAR_SubZero 150. Player 1 is @my_obj, player 2 @his_obj. The fight is on Shang Tsung's Courtyard ")
	TEXT("(BGND_Courtyard) unless stage= names another; stage=none fights in front of the player in the current map."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Fight CHAR_Scorpion CHAR_SubZero [distance]"));
				return;
			}
			const FString Player1 = Args[0];
			const FString Player2 = Args[1];
			const float Distance = (Args.Num() > 2 && FCString::IsNumeric(*Args[2])) ? FCString::Atof(*Args[2]) : 150.0f;
			FString StageName = TEXT("BGND_Courtyard");
			for (const FString& Arg : Args)
			{
				if (Arg.StartsWith(TEXT("stage=")))
				{
					StageName = Arg.Mid(6);
				}
			}
			WhenPlayerReady([Player1, Player2, Distance, StageName](UWorld* World, APawn* Pawn)
				{
					for (TActorIterator<AOKFighterActor> It(World); It; ++It)
					{
						It->Destroy();
					}
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if (It->ActorHasTag(StageTag))
						{
							It->Destroy();		// a stage or camera from an earlier ok.Fight
						}
					}
					// Side by side 3 m in front of the player, so the camera sees the fight from the side.
					FVector Feet, Forward, Right;
					PlayerGround(Pawn, Feet, Forward, Right);
					FVector Centre = Feet + Forward * 300.0f;
					FVector ArenaLeft = FVector::ZeroVector;
					double ArenaLength = 0.0;
					if (StageName != TEXT("none"))
					{
						FOKStage Stage;
						FString Error;
						if (!OKStage::Spawn(World, StageName, Stage, Error) || !Stage.bHasArena)
						{
							UE_LOG(LogOKAsset, Error, TEXT("%s"), Error.IsEmpty() ? *FString::Printf(TEXT("%s has no arena points"), *StageName) : *Error);
							return false;
						}
						Stage.Actor->Tags.Add(StageTag);
						// The map's own geometry would show through the stage. Hidden, it still holds up the (hidden) player.
						for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
						{
							It->SetActorHiddenInGame(true);
						}
						// The fight starts in the middle of the stage's fight line; the camera looks across it.
						Centre = (Stage.Left + Stage.Right) / 2.0;
						Right = (Stage.Right - Stage.Left).GetSafeNormal2D();
						ArenaLeft = Stage.Left;
						ArenaLength = FVector::DotProduct(Stage.Right - Stage.Left, Right);
						Forward = FVector::CrossProduct(Right, FVector::UpVector);
						ViewFight(World, Centre, Forward);
					}
					AOKFighterActor* A = SpawnFighter(World, Player1, 0, Centre - Right * (Distance / 2.0f), Right.Rotation().Yaw);
					AOKFighterActor* B = SpawnFighter(World, Player2, 1, Centre + Right * (Distance / 2.0f), (-Right).Rotation().Yaw);
					if (!A || !B)
					{
						return false;
					}
					// Player 2 faces left: turned round and mirrored, as a turnaround leaves a fighter, so both show the camera
					// the same side. [inf] Where MKKE sets this up at the round start isn't traced.
					B->SetMeshMirrored(true);
					AOKFighterActor::SetOpponents(*A, *B);
					if (UOKScriptWorld* ScriptWorld = World->GetSubsystem<UOKScriptWorld>())
					{
						ScriptWorld->StartFight(Right);
						// The arena ends player pushing keeps the fighters between (FUN_008B4BF0)
						for (AOKFighterActor* Each : { A, B })
						{
							Each->GetScriptFighter()->LineStart = ArenaLeft;
							Each->GetScriptFighter()->LineLength = ArenaLength;
						}
					}
					Pawn->SetActorHiddenInGame(true);		// the player would stand in front of the fight
					return true;
				}, WantsShot(Args), Args.Contains(TEXT("quit")), ShotDelayFrom(Args));
		}));

namespace
{
	/** Runs Action on every fighter in the playing world, waiting (up to ~20 s) until there is one */
	void WithFighters(const TCHAR* Command, TFunction<void(AOKFighterActor&)> Action)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Command, Action, Frames = 0](float) mutable -> bool
			{
				TArray<AOKFighterActor*> Fighters;
				if (UWorld* World = FindPlayingWorld())
				{
					for (TActorIterator<AOKFighterActor> It(World); It; ++It)
					{
						Fighters.Add(*It);
					}
				}
				if (Fighters.Num() == 0)
				{
					if (++Frames > 1200)
					{
						UE_LOG(LogOKAsset, Error, TEXT("%s: no fighter to use. Run ok.Fighter.Show first."), Command);
						return false;
					}
					return true;
				}
				for (AOKFighterActor* Fighter : Fighters)
				{
					Action(*Fighter);
				}
				return false;
			}));
	}
}

static FAutoConsoleCommand CmdFighterPlay(
	TEXT("ok.Fighter.Play"),
	TEXT("ok.Fighter.Play <Sequence> [once]: play one of the character's sequences on every fighter, e.g. ok.Fighter.Play FrontPunch01"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Fighter.Play FrontPunch01 [once]"));
				return;
			}
			const FString SequenceName = Args[0];
			const bool bLoop = !Args.Contains(TEXT("once"));
			WithFighters(TEXT("ok.Fighter.Play"), [SequenceName, bLoop](AOKFighterActor& Fighter)
				{
					FString Error;
					if (!Fighter.Play(SequenceName, bLoop, 0.0f, false, Error))
					{
						UE_LOG(LogOKAsset, Error, TEXT("ok.Fighter.Play: %s"), *Error);
					}
				});
		}));

static FAutoConsoleCommand CmdFighterPose(
	TEXT("ok.Fighter.Pose"),
	TEXT("ok.Fighter.Pose <Sequence> <Frame> [Bone...]: hold a frame and log where bones are, e.g. ok.Fighter.Pose FrontPunch01 12 RightHand Head"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Fighter.Pose FrontPunch01 12 RightHand Head"));
				return;
			}
			const FString SequenceName = Args[0];
			const float AtFrame = FCString::Atof(*Args[1]);
			const TArray<FString> Bones(Args.GetData() + 2, Args.Num() - 2);
			WithFighters(TEXT("ok.Fighter.Pose"), [SequenceName, AtFrame, Bones](AOKFighterActor& Fighter)
				{
					FString Error;
					if (!Fighter.Play(SequenceName, false, AtFrame, true, Error))
					{
						UE_LOG(LogOKAsset, Error, TEXT("ok.Fighter.Pose: %s"), *Error);
						return;
					}
					TArray<FString> Parts;
					for (const FString& Bone : Bones)
					{
						const FVector Where = Fighter.GetBoneLocation(FName(*Bone));
						Parts.Add(FString::Printf(TEXT("%s (%.1f, %.1f, %.1f)"), *Bone, Where.X, Where.Y, Where.Z));
					}
					UE_LOG(LogOKAsset, Log, TEXT("%s frame %g: %s"), *SequenceName, AtFrame, *FString::Join(Parts, TEXT(", ")));
				});
		}));

static FAutoConsoleCommand CmdAnimList(
	TEXT("ok.Anim.List"),
	TEXT("ok.Anim.List <Package> [Filter]: the package's animation sequences with their frame counts, e.g. ok.Anim.List CHAR_Scorpion Punch"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Anim.List CHAR_Scorpion [Filter]"));
				return;
			}
			FString Error;
			TUniquePtr<FOKPackage> Package = FOKPackage::OpenGamePackage(Args[0], Error);
			if (!Package)
			{
				UE_LOG(LogOKAsset, Error, TEXT("%s"), *Error);
				return;
			}
			const TArray<TPair<FString, int32>> Sequences = FOKAnimSequence::List(*Package);
			int32 Shown = 0;
			for (const TPair<FString, int32>& Entry : Sequences)
			{
				if (Args.Num() < 2 || Entry.Key.Contains(Args[1]))
				{
					UE_LOG(LogOKAsset, Log, TEXT("  %s (%d frames)"), *Entry.Key, Entry.Value);
					++Shown;
				}
			}
			UE_LOG(LogOKAsset, Log, TEXT("%s: %d sequences, %d shown"), *Package->GetName(), Sequences.Num(), Shown);
		}));

static FAutoConsoleCommand CmdFighterRun(
	TEXT("ok.Fighter.Run"),
	TEXT("ok.Fighter.Run <Function>: run a function of the fighter's character script as a script process, e.g. ok.Fighter.Run attack1"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Fighter.Run attack1"));
				return;
			}
			const FString Function = Args[0];
			WithFighters(TEXT("ok.Fighter.Run"), [Function](AOKFighterActor& Fighter)
				{
					if (Fighter.GetScriptFighter() && Fighter.GetScriptFighter()->Index != 0)
					{
						return;		// only player 1 runs scripts so far: @my_obj is bound to it
					}
					FString Error;
					if (!Fighter.RunScript(Function, Error))
					{
						UE_LOG(LogOKAsset, Error, TEXT("ok.Fighter.Run: %s"), *Error);
					}
				});
		}));