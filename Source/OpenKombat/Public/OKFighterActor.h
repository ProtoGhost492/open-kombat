#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OKAnimation.h"
#include "OKScriptObjects.h"
#include "OKSimTickable.h"
#include "OKFighterActor.generated.h"

class UPoseableMeshComponent;
class USkeletalMesh;
class FOKPackage;

/**
* A fighter on screen: its MKKE character mesh, loaded at runtime from the game's package, posed
* by our own animation sampler on the 60 Hz fight clock. 
*/
UCLASS()
class OPENKOMBAT_API AOKFighterActor : public AActor, public IOKSimTickable, public IOKFighterBody
{
	GENERATED_BODY()

public:
	AOKFighterActor();

	/** Loads the character's mesh from its package (e.g. CHAR_Scorpion) and shows it. */
	bool LoadCharacter(const FString& InPackageName, FString& OutError, int32 FighterIndex = 0);
	/** The character's package (CHAR_Scorpion), once loaded. */
	const FString& GetPackageName() const { return PackageName; }

	/** The script fighter this actor is the body of (player 1 = @my_obj, player 2 = @his_obj). */
	FOKFighter* GetScriptFighter() const { return ScriptFighter; }

	/** Makes two fighters each other's opponent, for hit detection. */
	static void SetOpponents(AOKFighterActor& A, AOKFighterActor& B);

	/** Plays one of the character's sequences from StartFrame. bHold freezes it on that frame. */
	bool Play(const FString& SequenceName, bool bInLoop, float StartFrame, bool bHold, FString& OutError);

	/** Starts a function of the character's script (e.g. attack1) as a script process driving this fighter. */
	bool RunScript(const FString& Function, FString& OutError);
	
	/** A bone's position relative to the mesh (component space), or zero if there's no such bone. */
	FVector GetBoneLocation(const FName& Bone) const;

	const FOKAnimSequence& GetSequence() const { return Sequence; }
	float GetFrame() const { return Frame; }
	UPoseableMeshComponent* GetMesh() const { return Mesh; }

	// IOKSimTickable
	virtual void SimTick(int64 SimFrame) override;

	// IOKFighterBody
	virtual bool PlayAnimation(const FString& SequenceName, float StartFrame, float InRate, float BlendFrames, bool bInLoop) override;
	virtual float GetAnimFrame() const override { return IsQueued() ? Queued.Frame : Frame; }
	virtual float GetAnimRate() const override { return IsQueued() ? Queued.Rate : Rate; }
	virtual void SetAnimRate(float InRate) override { (IsQueued() ? Queued.Rate : Rate) = InRate; }
	virtual void SetAnimLooping(bool bLooping) override { (IsQueued() ? Queued.bLoop : bLoop) = bLooping; }
	virtual bool IsAnimLooping() const override { return IsQueued() ? Queued.bLoop : bLoop; }
	virtual void SetBodyHidden(bool bHide) override { SetActorHiddenInGame(bHide); }
	virtual int32 GetAnimFrameCount() const override { return IsQueued() ? Queued.Sequence.NumFrames : Sequence.NumFrames; }
	virtual int32 GetSequenceFrameCount(const FString& SequenceName) override;
	virtual uint32 GetAnimId() const override { return GetTypeHash(GetAnimName()) | 1; }
	virtual FString GetAnimName() const override { return IsQueued() ? Queued.Sequence.Name : Sequence.Name; }
	virtual bool GetBoneWorldLocation(const FString& BoneName, FVector& OutLocation) const override;
	virtual FVector GetWorldLocation() const override { return GetActorLocation(); }
	virtual float GetHeight() const override { return float(GetActorLocation().Z - FloorZ); }
	virtual FVector GetFacing() const override { return GetActorForwardVector(); }
	virtual void SetAnimFrame(float InFrame) override;
	virtual void SetWorldLocation(const FVector& Location) override { SetActorLocation(Location); }
	virtual double GetFloorZ() const override { return FloorZ; }
	virtual FVector GetRootOffset() const override;
	virtual float GetSequenceBoneHeight(const FString& BoneName, float AtFrame) const override;
	virtual void SetupForFlip() override;
	virtual bool IsMeshMirrored() const override { return bMeshMirrored; }
	virtual void SetAnimNodeMirrored(bool bMirrored) override { bNodeMirrored = bMirrored; ApplyPose(); }
	virtual bool IsAnimNodeMirrored() const override { return bNodeMirrored; }
	virtual void FollowThrow(const IOKFighterBody& Leader) override;

	/** Draws the fighter mirrored or not, leaving the playing animation as it is. */
	void SetMeshMirrored(bool bMirrored);

	/**
	 * Cinema (W4): from now the cinema poses this fighter, with sequences from its package, and the fight stops moving
	 * it. EndCinema hands it back, mirrored as it was.
	 */
	void BeginCinema(TSharedPtr<FOKPackage> InCinemaPackage);
	void EndCinema();
	bool IsInCinema() const { return CinemaPackage.IsValid(); }
	/** Shows a cinema sequence Seconds in, the actor placed at Scene and moved by the sequence's root. */
	void CinemaPose(const FString& SequenceName, float Seconds, const FTransform& Scene);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	/** A decoded sequence of this character, from the cache or the package; null with OutError. */
	const FOKAnimSequence* FindSequence(const FString& SequenceName, FString& OutError);
	
	/** Poses the mesh: the reference pose with the sequence's tracks at the current frame. */
	void ApplyPose();

	/** The root-motion bone's transform at a frame (component space); identity if the skeleton has none. */
	FTransform SampleRoot(float AtFrame) const;
	/** The same for one group of a two-player sequence. */
	FTransform SampleRoot(float AtFrame, int32 Group) const;

	/** Each bone's mirror image (LeftArm <-> RightArm) and the rotation that corrects for their bone axes. */
	void BuildMirrorTable();
	/** Local bone transforms to component space. */
	void ComponentSpace(const TArray<FTransform>& Local, TArray<FTransform>& Out) const;
	/** The animation node's mirror: the pose reflected across the component's XZ plane, left and right swapped. */
	void MirrorPose(TArray<FTransform>& Pose) const;

	/**
	 * Root motion (ANIM-RE-NOTES §4.3): the root's translation since last frame, scaled by the move's root-motion
	 * scale, moves the actor. bWrapped: a looping sequence went past its end this frame.
	 */
	void ApplyRootMotion(bool bWrapped);

	/** ok.Debug.Collision: hurtboxes in green, attack spheres in red. */
	void DrawCollision() const;

	UPROPERTY(VisibleAnywhere, Category = "OpenKombat")
	TObjectPtr<UPoseableMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> CharacterMesh;

	FString PackageName;
	FOKAnimSequence Sequence;
	TArray<int32> TrackToBone;		// sequence track bone -> mesh bone
	int32 TrackGroup = 0;			// the sequence's tracks this fighter shows: 1 while it's a throw's victim
	float Frame = 0.0f;
	bool bPlaying = false;
	bool bLoop = true;

	float EventFrame = -1.0f;		// the frame frame events were last checked at (MKKE's fighter +0x2D4)
	float Rate = 1.0f;				// playback speed (block A's rate, set_anim_rate)

	TArray<FTransform> BlendFrom;	// the pose a blend started from
	float BlendFrames = 0.0f;		// how long the crossfade lasts, in frames (MK blends linearly)
	float BlendDone = 0.0f;
	FTransform RootTransform = FTransform::Identity;	// the root-motion bone at the current frame, kept out of the pose
	int32 RootBone = INDEX_NONE;					// position_locator: its translation is the root motion
	int32 RootPairBone = INDEX_NONE;				// Hips: the root motion is taken off it as well
	bool bMeshMirrored = false;						// mesh +0x758 bit 0: the fighter is drawn mirrored (actor scale y = -1)
	bool bNodeMirrored = false;						// the playing animation node's mirror (node flag 0x40)
	TArray<int32> MirrorBone;
	TArray<FQuat> MirrorCorrection;
	double FloorZ = 0.0;				// where the fighter was placed: its floor

	TSharedPtr<FOKPackage> Package;					// kept open: animations are read from it as scripts ask for them
	TMap<FString, FOKAnimSequence> SequenceCache;
	TSharedPtr<FOKPackage> CinemaPackage;			// while a cinema plays this fighter (W4)
	/**
	 * The animation the fight's scripts ask for while a cinema has the body: it waits at its start frame, and the
	 * scripts see it as playing, until the cinema lets go (EndCinema). [inf] MKKE's node stack is kept, paused, while
	 * the Matinee drives the mesh: an X-ray's p_xray_victim starts its _Out animation at the cinema's start.
	 */
	struct FQueuedAnim
	{
		FOKAnimSequence Sequence;
		TArray<int32> TrackToBone;
		float Frame = 0.0f;
		float Rate = 1.0f;
		float BlendFrames = 0.0f;
		bool bLoop = false;
		bool bSet = false;
	};
	FQueuedAnim Queued;
	bool IsQueued() const { return CinemaPackage.IsValid() && Queued.bSet; }
	bool bMirroredBeforeCinema = false;
	FOKFighter* ScriptFighter = nullptr;			// the script fighter this actor is the body of
};