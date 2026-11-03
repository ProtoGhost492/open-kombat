#pragma once

#include "CoreMinimal.h"

class FOKMkoFile;
class FOKScriptRuntime;

/**
 * The fight camera (system W3). MKKE's MKFGCameraObj is a large state machine (special, cinema, tag and Krypt cameras;
 * FUN_008789D0 and the functions under it). Ours does the fight's everyday job, for any game: it frames the midpoint
 * of the two fighters from the near side of the fight line, pulls back as they separate, stops at the arena's ends,
 * follows with MKKE's travel rates, and adds the shake scripts set on the camera object (149.23).
 *
 * The camera object (FOKCameraObject) holds MKKE's 68 camera variables, copied from FGCameraEngine's CameraVariables
 * global as MKKE does (FUN_0086DFB0 -> FUN_0086B510), so scripts can read and change them (169.93 / 169.94).
 */
class OPENKOMBAT_API FOKFightCamera
{
public:
	/** Where the fighters are this frame, and the fight line. */
	struct FInput
	{
		FVector Fighters[2] = { FVector::ZeroVector, FVector::ZeroVector };
		FVector ScreenRight = FVector::YAxisVector;	// the fight line, left to right on screen
		FVector LineStart = FVector::ZeroVector;	// its left end
		double LineLength = 0.0;					// 0 = no ends
	};

	/** Makes the camera object in Runtime, with FGCameraEngine's variables. Returns its handle. */
	uint32 Create(FOKScriptRuntime& Runtime, const FOKMkoFile* CameraScript);
	uint32 GetHandle() const { return Handle; }

	/** One sim frame: moves towards the framing of Input; Snap jumps straight there (the first frame). */
	void Update(const FOKScriptRuntime& Runtime, const FInput& Input, bool bSnap);

	const FVector& GetEye() const { return Eye; }
	const FRotator& GetRotation() const { return Rotation; }
	float GetFieldOfView() const { return FieldOfView; }

private:
	float Variable(const FOKScriptRuntime& Runtime, int32 Index) const;

	uint32 Handle = 0;
	FVector Focus = FVector::ZeroVector;	// the point on the fight line the eye is placed from
	FVector Target = FVector::ZeroVector;	// the point it looks at, which catches up faster than the eye
	double Distance = 0.0;
	FVector Eye = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float FieldOfView = 45.0f;
};
