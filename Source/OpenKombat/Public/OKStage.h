#pragma once

#include "CoreMinimal.h"

class AActor;
class FOKPackage;
class UStaticMesh;
class UWorld;

/** A stage spawned into a world, and where its fight takes place. */
struct FOKStage
{
	AActor* Actor = nullptr;		// owns one static mesh component per placed mesh
	int32 Placements = 0;
	int32 Meshes = 0;
	int32 Skipped = 0;				// hidden, unreadable or stored in another package

	/**
	 * The fight line, from the stage's MKArenaConstrainPoint actors (tags _Left and _Right): the fighters stay
	 * between them, and screen right runs from Left to Right.
	 */
	bool bHasArena = false;
	FVector Left = FVector::ZeroVector;
	FVector Right = FVector::ZeroVector;
};

/**
 * Stages (MK-FORMAT-NOTES, "Levels / stages"): a BGND_* package holds several Levels, each owning StaticMeshActors.
 * Loading one builds every static mesh actor of its non-cinematic levels at its own place: Unreal 3 and 5 share
 * axes and units, so the stage keeps MKKE's coordinates.
 */
namespace OKStage
{
	/** Spawns PackageName (e.g. BGND_Courtyard) into World. False, with OutError, if nothing could be built. */
	OPENKOMBAT_API bool Spawn(UWorld* World, const FString& PackageName, FOKStage& OutStage, FString& OutError);
	/** One StaticMesh export of a package, with its base-colour materials (a cinema's set piece). Null, with OutError, if unreadable. */
	OPENKOMBAT_API UStaticMesh* LoadMesh(FOKPackage& Package, int32 MeshRef, FString& OutError);
}
