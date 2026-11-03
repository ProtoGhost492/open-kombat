#pragma once

#include "CoreMinimal.h"

class FOKPackage;

/** One bone of the reference pose. Unreal 3 and 5 share axes and units, so values are used as stored. */
struct FOKBone
{
	FString Name;
	int32 Parent = INDEX_NONE;
	FQuat4f Rotation = FQuat4f::Identity;		// relative to the parent
	FVector3f Position = FVector3f::ZeroVector;
};

/** A SkeletalMeshSocket: a named point on a bone (Spear_Left, Sword_Right). Scripts place objects there. */
struct FOKMeshSocket
{
	FString Name;
	FString Bone;
	FVector3f Location = FVector3f::ZeroVector;		// relative to the bone
	FRotator3f Rotation = FRotator3f::ZeroRotator;
	FVector3f Scale = FVector3f::OneVector;
};

/** A run of vertices skinned with one bone map (UE3's skin "chunk"). */
struct FOKMeshChunk
{
	int32 BaseVertex = 0;
	TArray<uint16> BoneMap;		// chunk-local bone index -> skeleton bone
	int32 NumRigid = 0;
	int32 NumSoft = 0;
	int32 MaxInfluences = 0;
};

/** Which damage state a section belongs to (MK-FORMAT-NOTES, "SkeletalMesh", damage states). */
enum class EOKSectionLayer : uint8
{
	Always,			// drawn with and without battle damage
	Undamaged,		// the undamaged version of a swappable pair
	Damage,			// its battle-damaged twin
	Other,			// drawn in neither state (X-ray, fatalities, ...)
};

struct FOKMeshSection
{
	int32 StoredMaterial = 0;	// the u16 in the file: a material index, or on character bodies a sort index
	int32 Chunk = 0;
	int32 BaseIndex = 0;
	int32 NumTriangles = 0;
	int32 Material = 0;			// the material actually used (from the UV tile on character bodies)
	EOKSectionLayer Layer = EOKSectionLayer::Always;
	FIntPoint UvTile = FIntPoint(0, 0);

	bool IsDrawnUndamaged() const { return Layer == EOKSectionLayer::Always || Layer == EOKSectionLayer::Undamaged; }
};

/** One 48-byte vertex. */
struct FOKMeshVertex
{
	FVector3f Position;
	FVector3f TangentX;
	FVector3f TangentY;
	FVector3f TangentZ;			// the normal
	FVector2f UV0;
	FVector2f UV1;
	uint8 Bones[4];				// chunk-local, through the chunk's bone map
	uint8 Weights[4];			// sum to 255
};

/** A SkeletalMesh's LOD 0 as MKKE stores it (MK-FORMAT-NOTES, "SkeletalMesh"). */
struct OPENKOMBAT_API FOKSkeletalMeshData
{
	FString Name;
	FVector3f BoundsOrigin = FVector3f::ZeroVector;
	FVector3f BoundsExtent = FVector3f::ZeroVector;
	float BoundsRadius = 0.0f;
	TArray<int32> MaterialRefs;		// object references to the materials
	TArray<FOKBone> Bones;
	TArray<FOKMeshSocket> Sockets;
	TArray<FOKMeshSection> Sections;
	TArray<FOKMeshChunk> Chunks;
	TArray<uint32> Indices;
	TArray<FOKMeshVertex> Vertices;
	int32 LodCount = 0;

	static constexpr uint8 NoRegion = 0xFF;
	/** Each vertex's dismemberment region (0-31; a cinema hides regions by number), NoRegion if it has none. Empty without a physics asset. */
	TArray<uint8> VertexRegions;

	/** Reads export ExportRef (1-based). False, with OutError, if it isn't a readable SkeletalMesh. */
	bool Read(FOKPackage& Package, int32 ExportRef, FString& OutError);
	/** The region most of a triangle's vertices are in. */
	uint8 TriangleRegion(uint32 A, uint32 B, uint32 C) const;

private:
	bool ReadLod(class FOKByteReader& R, FString& OutError);
	void ReadSockets(FOKPackage& Package, const class FOKPropertyList& Properties);
	void ReadRegions(FOKPackage& Package, const class FOKPropertyList& Properties);
	void AssignMaterialsAndLayers();
};
