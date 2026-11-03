#pragma once

#include "CoreMinimal.h"

class FOKPackage;
class UMaterialInterface;
class USkeletalMesh;
class USkinnedMeshComponent;
struct FOKSkeletalMeshData;

/**
 * Builds Unreal skeletal meshes from MKKE packages at runtime. The mesh is made straight from
 * render data (vertex, skin-weight and index buffers), so it doesn't need the editor's mesh
 * builder and works the same in a packaged game.
 */
namespace OKCharacterMesh
{
	/** The character's main mesh (the package's largest SkeletalMesh), with its diffuse textures. Null with OutError on failure. */
	OPENKOMBAT_API USkeletalMesh* Load(const FString& PackageName, FString& OutError);
	/** One SkeletalMesh export of a package (a cinema's prop: FX_Scorpion.Meshes.SC_Sword_Sk). */
	OPENKOMBAT_API USkeletalMesh* LoadExport(FOKPackage& Package, int32 ExportRef, FString& OutError);

	/** A skeletal mesh for Data, drawing the sections a fight starts with (no battle damage). */
	OPENKOMBAT_API USkeletalMesh* Build(const FOKSkeletalMeshData& Data, const TArray<UMaterialInterface*>& Materials, FString& OutError);

	/** Hides the dismemberment regions whose bits are set in HiddenRegions (a cinema's CharDismemberment key) and shows the rest. */
	OPENKOMBAT_API void ShowRegions(USkinnedMeshComponent* Component, uint32 HiddenRegions);

	/** A material for one of the mesh's MK materials: the base material with the MK material's DiffuseMap. */
	OPENKOMBAT_API UMaterialInterface* MakeMaterial(FOKPackage& Package, int32 MaterialRef);

	/** The base material with a texture of the package as its diffuse map; the base material alone if TextureRef is 0 or unreadable. */
	OPENKOMBAT_API UMaterialInterface* MakeMaterialFromTexture(FOKPackage& Package, int32 TextureRef);
}
