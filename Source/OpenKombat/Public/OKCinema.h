#pragma once

#include "CoreMinimal.h"

class AOKCinemaProp;
class AOKFighterActor;
class AOKParticleActor;
struct FOKAnimSequence;
class FOKPackage;
class FOKPropertyList;
class UWorld;

/** A UE3 InterpCurveFloat: points with tangents. InterpMode 0 is linear, 2 constant, the rest cubic (Hermite). */
struct OPENKOMBAT_API FOKInterpCurve
{
	struct FPoint
	{
		float In = 0.0f;
		float Out = 0.0f;
		float Arrive = 0.0f;
		float Leave = 0.0f;
		uint8 Mode = 1;
	};
	TArray<FPoint> Points;

	bool IsEmpty() const { return Points.Num() == 0; }
	float Eval(float Time, float Default = 0.0f) const;
};

/** One group of a cinema (CinemaGroupSpawnedActor): an actor, a camera or an emitter, with its tracks. */
struct OPENKOMBAT_API FOKCinemaGroup
{
	enum class EKind : uint8 { Actor, Camera, Emitter, Static, Other };

	struct FAnimKey
	{
		float Start = 0.0f;			// cinema time it starts at
		FString Sequence;			// AnimSeqName, in the group's AnimSets
		float StartOffset = 0.0f;	// seconds into the sequence it starts from
		float EndOffset = 0.0f;		// seconds left off its end
		float Rate = 1.0f;
		bool bLooping = false;
	};

	FString Name;					// GroupName: Attacker, Victim, Camera1 ...
	EKind Kind = EKind::Other;
	FString ProxyMesh;				// the mesh the cinema was made with (CHAR_Kitana.Meshes.Kitana for a victim)
	bool bHasMove = false;
	FOKInterpCurve X, Y, Z;			// CinemaTrackMove: the position from the scene's origin (bUseMKSceneOffset)
	FOKInterpCurve Roll, Pitch, Yaw;	// its EulerTrack, in degrees
	FOKInterpCurve FocalLength;		// CinemaTrackFocalLength, a camera's
	TArray<FAnimKey> Anims;			// CinemaTrackBlendAnim's body subtrack (0); the face subtracks aren't played

	struct FAttachment
	{
		float Time = 0.0f;
		FString Group;				// AttachmentName: the group it goes onto (Victim)
		FString Bone;				// AttachedBoneName (Neck)
		FTransform Relative = FTransform::Identity;
	};
	struct FParameter
	{
		FName Name;					// ParamNameOut (FX_Blood_Color)
		FOKInterpCurve R, G, B;
	};

	int32 MeshRef = 0;				// a prop's SkeletalMesh (its factory's), in the cinema's package
	int32 StaticMeshRef = 0;		// a set piece's StaticMesh (ActorFactoryMover's): the victory's Netherrealm wall
	FString ParticleSystem;			// an emitter's (ActorFactoryEmitter), by name
	TArray<TPair<float, bool>> Hide;		// CinemaTrackHideActor: hidden or shown from each time
	TArray<TPair<float, uint8>> Toggles;	// InterpTrackToggle: 0 off, 1 on, 2 toggle, 3 trigger
	TArray<FAttachment> Attachments;		// CinemaTrackAttachment
	TArray<FParameter> Parameters;			// InterpTrackVectorParticleParam
	TArray<TPair<float, uint32>> Dismemberment;	// CinemaTrackCharDismemberment: the regions hidden from each time, a bit each

	bool IsHiddenAt(float Time) const;
	/** The dismemberment regions hidden at Time (none before the first key). */
	uint32 HiddenRegionsAt(float Time) const;
	/** The attachment in force at Time, or null. */
	const FAttachment* AttachmentAt(float Time) const;

	/** The group's transform from the scene's origin at Time. */
	FTransform MoveAt(float Time) const;
	/** The body animation playing at Time and how far into it, in seconds; false before the first. */
	bool AnimAt(float Time, FString& OutSequence, float& OutSeconds) const;
};

/** A cinema's InterpData (CinemaData, a Matinee of Midway's): its length, groups, camera cuts and slow motion. */
struct OPENKOMBAT_API FOKCinemaData
{
	FString Name;
	float Length = 0.0f;
	FString ScenePrefix;		// "Scorpion_XRAY." when the cinema is a sub-package, else empty
	TArray<FOKCinemaGroup> Groups;
	TArray<TPair<float, FString>> Cuts;		// the director track: from this time, this camera group
	FOKInterpCurve Slomo;

	/** A PointLight of the cinema's level. */
	struct FLight
	{
		FVector Offset = FVector::ZeroVector;		// in the scene, as its moves are
		FColor Color = FColor::White;
		float Brightness = 1.0f;
		float Radius = 1024.0f;
	};
	TArray<FLight> Lights;

	/**
	 * Reads the cinema a package plays: its SeqAct_Cinema named after the package (Scorpion_Fatality1), else the longest.
	 * With a Scene, only that sub-package's: an X-ray (Scorpion_XRAY) is cooked into the character's package.
	 */
	bool Read(FOKPackage& Package, FString& OutError, const FString& Scene = FString());
	void ReadLights(FOKPackage& Package);
	/** Whether an export belongs to the cinema read: all of them, or its sub-package's. */
	bool InScene(const struct FOKPackageExport& Export) const;
	const FOKCinemaGroup* FindGroup(const FString& GroupName) const;
};

/**
 * Plays a cinema (W4) with the fighters as its Attacker and Victim. The scene's origin is where the attacker stands,
 * turned to its facing; every group is placed from there. The fighters play the cinema's animations with their
 * root motion in place, and the view follows the director's cuts. [inf] MKKE places the scene from gameInfo +0x54.
 */
class OPENKOMBAT_API FOKCinemaPlayer
{
public:
	/** bHoldAtEnd: when it ends it keeps its last frame (the fighters, pieces and view) until Stop or the next Start, as a finisher does. */
	bool Start(const FString& PackageName, AOKFighterActor* Attacker, AOKFighterActor* Victim, FString& OutError, bool bHoldAtEnd = false);
	bool IsPlaying() const { return bPlaying; }
	/** Playing, or holding its last frame: the view and the fighters are still the cinema's. */
	bool IsShowing() const { return bPlaying || bHolding; }
	float GetTime() const { return Time; }
	float GetLength() const { return Data.Length; }
	/** One sim frame of Seconds (the slow-motion track scales it), then the fighters' poses and the view. */
	void Step(UWorld* World, float Seconds);
	void Stop();

private:
	void Pose(AOKFighterActor* Actor, const FOKCinemaGroup* Group);
	void View(UWorld* World);
	/** The props (skeletal groups that aren't a character, and the fighters' cut pieces) and emitters, from Before to Time. */
	void StepProps(UWorld* World);
	void StepEmitters(float Before);
	/** Where a group's actor is now, or a bone of it; false if the group has no actor. */
	bool GroupBone(const FString& GroupName, const FString& Bone, FTransform& Out) const;
	const FOKAnimSequence* PropSequence(const FString& SequenceName);

	TMap<int32, TWeakObjectPtr<AOKCinemaProp>> Props;		// by group index
	TMap<int32, TWeakObjectPtr<AOKParticleActor>> Emitters;
	TMap<int32, TWeakObjectPtr<AActor>> SetPieces;		// static groups, by group index
	TArray<TWeakObjectPtr<AActor>> LightActors;
	TArray<TWeakObjectPtr<AActor>> HiddenStage;		// the stage, while a cinema with its own set shows
	TMap<FString, TSharedPtr<struct FOKAnimSequence>> PropSequences;

	TSharedPtr<FOKPackage> Package;
	FOKCinemaData Data;
	float Time = 0.0f;
	bool bPlaying = false;
	bool bHoldAtEnd = false;
	bool bHolding = false;
	FTransform Scene = FTransform::Identity;
	TWeakObjectPtr<AOKFighterActor> Attacker;
	TWeakObjectPtr<AOKFighterActor> Victim;
};
