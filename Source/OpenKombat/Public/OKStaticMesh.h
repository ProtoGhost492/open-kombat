#pragma once

#include "CoreMinimal.h"

class FOKPackage;
class UMaterialInterface;
class UStaticMesh;

/** One draw call of a static mesh: a run of triangles with one material. */
struct FOKStaticMeshElement
{
	int32 MaterialRef = 0;		// object reference to the material
	int32 FirstIndex = 0;
	int32 NumTriangles = 0;
};

/**
 * A StaticMesh's LOD 0 as MKKE stores it (MK-FORMAT-NOTES, "StaticMesh"): after the property list, bounds, the
 * collision kDOP tree, then per LOD the elements and the position, colour, tangent, UV and index streams.
 * Unreal 3 and 5 share axes and units, so values are used as stored.
 */
struct OPENKOMBAT_API FOKStaticMeshData
{
	FString Name;
	TArray<FOKStaticMeshElement> Elements;
	TArray<FVector3f> Positions;
	TArray<FVector3f> TangentX;
	TArray<FVector3f> TangentY;
	TArray<FVector3f> TangentZ;		// the normal
	TArray<TArray<FVector2f>> UVs;	// one array per UV set
	TArray<FColor> Colors;			// empty when the mesh has none
	TArray<uint32> Indices;
	int32 LodCount = 0;

	/** Reads export ExportRef (1-based). False, with OutError, if it isn't a readable StaticMesh. */
	bool Read(FOKPackage& Package, int32 ExportRef, FString& OutError);
};

/** Builds Unreal static meshes from MKKE data at runtime (the fast build path, which packaged games need too). */
namespace OKStaticMesh
{
	/** A static mesh for Data, one material slot per element. Null with OutError on failure. */
	OPENKOMBAT_API UStaticMesh* Build(const FOKStaticMeshData& Data, const TArray<UMaterialInterface*>& Materials, FString& OutError);
}
