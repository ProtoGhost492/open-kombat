#pragma once

#include "CoreMinimal.h"

class FOKPackage;

/** One track: a bone's rotation, translation or scale keys (MK-FORMAT-NOTES, "Animation"). */
struct FOKAnimTrack
{
	enum class EType : uint8 { Rotation, Translation, Scale };

	int32 TrackBone = 0;		// index into the AnimSet's TrackBoneNames
	int32 Group = 0;			// 0 the fighter playing it; 1 the other fighter, in two-player ("Attached") sequences
	EType Type = EType::Rotation;
	TArray<int32> Frames;		// ascending frame numbers
	TArray<FVector4f> Values; // a quaternion (x, y, z, w), or x, y, z (cm, or scale)
};

/** A decoded AnimSequence. Keys are in Unreal's own axes, like the mesh's reference pose. */
struct OPENKOMBAT_API FOKAnimSequence
{
	FString Name;
	TArray<FString> TrackBones;		// the AnimSet's TrackBoneNames
	int32 NumFrames = 0;
	float Length = 0.0f;			// seconds; 60 frames a second in every file seen
	TArray<FOKAnimTrack> Tracks;		// every group's: group 1 animates the other fighter in two-player ("Attached") sequences

	/** Finds a sequence by its SequenceName in the package's AnimSets and decodes it. */
	bool Find(FOKPackage& Package, const FString& SequenceName, FString& OutError);

	/** Frames per second (frames are numbered 0 .. NumFrames - 1). */
	float FramesPerSecond() const { return (Length > 0.0f && NumFrames > 1) ? (NumFrames - 1) / Length : 60.0f; }

	/**
	 * Applies the tracks at Frame (fractional frames interpolate) to InOutPose, a skeleton's local
	 * transforms. TrackToBone maps each track bone to a skeleton bone (INDEX_NONE = not in it).
	 */
	void Sample(float Frame, const TArray<int32>& TrackToBone, TArray<FTransform>& InOutPose, int32 Group = 0) const;

	/** Every sequence name in the package, with its frame count. */
	static TArray<TPair<FString, int32>> List(FOKPackage& Package);

private:
	bool Decode(TConstArrayView<uint8> Native, bool bFlaggedExtra, FString& OutError);
};