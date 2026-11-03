#include "OKSkeletalMeshData.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"

namespace
{
	constexpr int32 VertexStride = 48;

	/** A packed tangent component: a byte mapped to [-1, 1]. */
	float Unpack(uint8 Byte)
	{
		return Byte / 127.5f - 1.0f;
	}

	FVector3f UnpackVector(const uint8* Bytes)
	{
		return FVector3f(Unpack(Bytes[0]), Unpack(Bytes[1]), Unpack(Bytes[2]));
	}

	/** Reads an int32 count and checks it against a limit and the bytes left (each element at least MinBytes). */
	int32 ReadCount(FOKByteReader& R, int32 Limit, int32 MinBytes, bool& bBad)
	{
		const int32 Count = R.I32();
		if (Count < 0 || Count > Limit || int64(Count) * MinBytes > R.Remaining())
		{
			bBad = true;
			return 0;
		}
		return Count;
	}
}

bool FOKSkeletalMeshData::Read(FOKPackage& Package, int32 ExportRef, FString& OutError)
{
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	if (!Exports.IsValidIndex(ExportRef - 1) || Exports[ExportRef - 1].ClassName != TEXT("SkeletalMesh"))
	{
		OutError = FString::Printf(TEXT("%s isn't a SkeletalMesh stored in %s"), *Package.ObjectPath(ExportRef), *Package.GetName());
		return false;
	}
	const FOKPackageExport& Export = Exports[ExportRef - 1];
	Name = Export.Name;
	TArray<uint8> Bytes;
	if (!Package.ExportData(Export, Bytes))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *Export.Path);
		return false;
	}

	FOKByteReader R(Bytes);
	FOKPropertyList Properties;
	if (!Properties.Read(R, Package.GetNames()))
	{
		OutError = FString::Printf(TEXT("%s: the property list is damaged"), *Export.Path);
		return false;
	}
	ReadSockets(Package, Properties);
	const TArray<FString>& Names = Package.GetNames();
	bool bBad = false;

	BoundsOrigin.X = R.F32(); BoundsOrigin.Y = R.F32(); BoundsOrigin.Z = R.F32();
	BoundsExtent.X = R.F32(); BoundsExtent.Y = R.F32(); BoundsExtent.Z = R.F32();
	BoundsRadius = R.F32();

	const int32 MaterialCount = ReadCount(R, 4096, 4, bBad);
	for (int32 i = 0; i < MaterialCount; ++i)
	{
		MaterialRefs.Add(R.I32());
	}

	// The skeleton is stored as separate arrays: transforms, parents, names.
	const int32 BoneCount = ReadCount(R, 4096, 28, bBad);
	Bones.SetNum(BoneCount);
	for (FOKBone& Bone : Bones)
	{
		Bone.Rotation.X = R.F32(); Bone.Rotation.Y = R.F32(); Bone.Rotation.Z = R.F32(); Bone.Rotation.W = R.F32();
		Bone.Rotation.Normalize();
		Bone.Position.X = R.F32(); Bone.Position.Y = R.F32(); Bone.Position.Z = R.F32();
	}
	if (ReadCount(R, 4096, 2, bBad) != BoneCount)
	{
		OutError = FString::Printf(TEXT("%s: the parent array doesn't match the bone count"), *Export.Path);
		return false;
	}
	for (int32 i = 0; i < BoneCount; ++i)
	{
		const int32 Parent = R.I16();
		// The root refers to itself. Parents always come first in the files seen; be defensive anyway.
		Bones[i].Parent = (i == 0) ? INDEX_NONE : ((Parent >= 0 && Parent < i) ? Parent : 0);
	}
	if (ReadCount(R, 4096, 8, bBad) != BoneCount)
	{
		OutError = FString::Printf(TEXT("%s: the bone name array doesn't match the bone count"), *Export.Path);
		return false;
	}
	for (FOKBone& Bone : Bones)
	{
		Bone.Name = OKReadName(R, Names, bBad);
	}

	// Bone sets {FName, 32-byte mask}, per-bone names, 5-byte per-bone records, 2 bytes, depth, 0.
	const int32 SetCount = ReadCount(R, 1024, 40, bBad);
	R.Skip(int64(SetCount) * 40);
	const int32 ExtraNameCount = ReadCount(R, 4096, 8, bBad);
	R.Skip(int64(ExtraNameCount) * 8);
	const int32 RecordCount = ReadCount(R, 4096, 5, bBad);
	R.Skip(int64(RecordCount) * 5);
	R.Skip(2);
	R.Skip(4);		// SkeletalDepth
	R.Skip(4);		// always 0
	if (bBad || R.HasError())
	{
		OutError = FString::Printf(TEXT("%s: the skeleton is damaged (at byte %lld)"), *Export.Path, R.Tell());
		return false;
	}

	LodCount = R.I32();
	if (LodCount < 1 || LodCount > 16)
	{
		OutError = FString::Printf(TEXT("%s: bad LOD count %d"), *Export.Path, LodCount);
		return false;
	}
	if (!ReadLod(R, OutError))
	{
		OutError = FString::Printf(TEXT("%s: %s (at byte %lld)"), *Export.Path, *OutError, R.Tell());
		return false;
	}
	AssignMaterialsAndLayers();
	ReadRegions(Package, Properties);
	return true;
}

void FOKSkeletalMeshData::ReadSockets(FOKPackage& Package, const FOKPropertyList& Properties)
{
	// MK keeps sockets in sets the mesh lists (SocketSets: its own set, then shared ones such as
	// MK9_SocketLibrary.BaseMaleSet). The first socket of a name wins. Sets that are imports aren't read.
	const FOKProperty* Sets = Properties.Find(TEXT("SocketSets"));
	if (!Sets)
	{
		return;
	}
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	const auto ReadExport = [&Package, &Exports](int32 Ref, const TCHAR* ClassName, FOKPropertyList& Out)
	{
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(Ref - 1) || Exports[Ref - 1].ClassName != ClassName || !Package.ExportData(Exports[Ref - 1], Bytes))
		{
			return false;
		}
		FOKByteReader R(Bytes);
		return Out.Read(R, Package.GetNames());
	};
	for (const int32 SetRef : FOKPropertyList::ReadIntArray(*Sets))
	{
		FOKPropertyList Set;
		const FOKProperty* List = ReadExport(SetRef, TEXT("SkeletalMeshSocketSet"), Set) ? Set.Find(TEXT("Sockets")) : nullptr;
		if (!List)
		{
			continue;
		}
		for (const int32 Ref : FOKPropertyList::ReadIntArray(*List))
		{
			FOKPropertyList Socket;
			if (!ReadExport(Ref, TEXT("SkeletalMeshSocket"), Socket))
			{
				continue;
			}
			const FString SocketName = Socket.GetText(TEXT("m_SocketName"));
			if (SocketName.IsEmpty() || Sockets.ContainsByPredicate([&SocketName](const FOKMeshSocket& Other) { return Other.Name == SocketName; }))
			{
				continue;
			}
			FOKMeshSocket& Out = Sockets.AddDefaulted_GetRef();
			Out.Name = SocketName;
			Out.Bone = Socket.GetText(TEXT("BoneName"));
			// Vectors are three floats; a Rotator is three int32s (pitch, yaw, roll; 65536 a turn).
			const auto Floats = [&Socket](const TCHAR* Property, FVector3f& Value)
			{
				const FOKProperty* Found = Socket.Find(Property);
				if (Found && Found->Raw.Num() >= 12)
				{
					FMemory::Memcpy(&Value, Found->Raw.GetData(), 12);
				}
			};
			Floats(TEXT("m_RelativeLocation"), Out.Location);
			Floats(TEXT("m_RelativeScale"), Out.Scale);
			if (const FOKProperty* Found = Socket.Find(TEXT("m_RelativeRotation")); Found && Found->Raw.Num() >= 12)
			{
				int32 Parts[3];
				FMemory::Memcpy(Parts, Found->Raw.GetData(), 12);
				Out.Rotation = FRotator3f(Parts[0] * (360.0f / 65536.0f), Parts[1] * (360.0f / 65536.0f), Parts[2] * (360.0f / 65536.0f));
			}
		}
	}
}

bool FOKSkeletalMeshData::ReadLod(FOKByteReader& R, FString& OutError)
{
	bool bBad = false;

	const int32 SectionCount = ReadCount(R, 65535, 10, bBad);
	Sections.SetNum(SectionCount);
	for (FOKMeshSection& Section : Sections)
	{
		Section.StoredMaterial = R.U16();
		Section.Chunk = R.U16();
		Section.BaseIndex = int32(R.U32());
		Section.NumTriangles = R.U16();
	}

	const int32 IndexCount = ReadCount(R, 50000000, 2, bBad);
	Indices.SetNum(IndexCount);
	for (uint32& Index : Indices)
	{
		Index = R.U16();
	}
	R.Skip(int64(ReadCount(R, 50000000, 2, bBad)) * 2);		// a second index array (empty on bodies)
	R.Skip(int64(ReadCount(R, 65535, 2, bBad)) * 2);		// ActiveBoneIndices
	R.Skip(ReadCount(R, 50000000, 1, bBad));				// ShadowTriangleDoubleSided (empty)

	const int32 ChunkCount = ReadCount(R, 65535, 32, bBad);
	Chunks.SetNum(ChunkCount);
	for (int32 i = 0; i < ChunkCount && !bBad; ++i)
	{
		FOKMeshChunk& Chunk = Chunks[i];
		Chunk.BaseVertex = int32(R.U32());
		for (int32 k = 0; k < 4; ++k)
		{
			if (R.I32() != 0)
			{
				OutError = FString::Printf(TEXT("chunk %d has un-stripped vertex data; the layout is unknown"), i);
				return false;
			}
		}
		const int32 BoneMapCount = ReadCount(R, 4096, 2, bBad);
		for (int32 k = 0; k < BoneMapCount; ++k)
		{
			const uint16 Bone = R.U16();
			if (Bone >= Bones.Num())
			{
				OutError = FString::Printf(TEXT("chunk %d uses bone %d of %d"), i, Bone, Bones.Num());
				return false;
			}
			Chunk.BoneMap.Add(Bone);
		}
		Chunk.NumRigid = R.I32();
		Chunk.NumSoft = R.I32();
		Chunk.MaxInfluences = R.I32();
	}

	R.Skip(4);		// Size
	const int32 NumVertices = R.I32();
	R.Skip(int64(ReadCount(R, 50000000, 16, bBad)) * 16);		// edges (usually empty)
	R.Skip(ReadCount(R, 65535, 1, bBad));						// RequiredBones
	R.Skip(4);		// RawPointIndices bulk data: flags
	R.Skip(4);		// element count
	const int32 RawSize = R.I32();
	R.Skip(4);		// offset in the file
	if (RawSize > 0)
	{
		R.Skip(RawSize);
	}

	const int32 VertexCount = ReadCount(R, 10000000, VertexStride, bBad);
	if (bBad || R.HasError())
	{
		OutError = TEXT("the LOD tables are damaged");
		return false;
	}
	if (VertexCount != NumVertices)
	{
		OutError = FString::Printf(TEXT("the vertex buffer has %d vertices, NumVertices says %d"), VertexCount, NumVertices);
		return false;
	}
	const uint8* Data = R.Take(int64(VertexCount) * VertexStride);
	if (!Data)
	{
		OutError = TEXT("the vertex buffer is truncated");
		return false;
	}
	Vertices.SetNum(VertexCount);
	for (int32 v = 0; v < VertexCount; ++v)
	{
		const uint8* In = Data + v * VertexStride;
		FOKMeshVertex& Out = Vertices[v];
		FMemory::Memcpy(&Out.Position, In, 12);
		Out.TangentX = UnpackVector(In + 12);
		Out.TangentY = UnpackVector(In + 16);
		Out.TangentZ = UnpackVector(In + 20);
		FMemory::Memcpy(&Out.UV0, In + 24, 8);
		FMemory::Memcpy(&Out.UV1, In + 32, 8);
		FMemory::Memcpy(Out.Bones, In + 40, 4);
		FMemory::Memcpy(Out.Weights, In + 44, 4);
	}

	for (const FOKMeshSection& Section : Sections)
	{
		if (int64(Section.BaseIndex) + int64(Section.NumTriangles) * 3 > IndexCount)
		{
			OutError = TEXT("a section runs past the index buffer");
			return false;
		}
		if (Section.Chunk >= ChunkCount)
		{
			OutError = FString::Printf(TEXT("a section uses chunk %d of %d"), Section.Chunk, ChunkCount);
			return false;
		}
	}
	for (const uint32 Index : Indices)
	{
		if (int32(Index) >= VertexCount)
		{
			OutError = FString::Printf(TEXT("index %u is out of range (%d vertices)"), Index, VertexCount);
			return false;
		}
	}
	return true;
}

void FOKSkeletalMeshData::ReadRegions(FOKPackage& Package, const FOKPropertyList& Properties)
{
	// The dismemberment regions are on the mesh's physics asset (SubZero_PhysicsD): each body's convex pieces name a
	// GenericDismembermentState whose StateMask has the piece's region bit (the head's four quarters are 0-3, the
	// torso 4-9, the arms 10-11, hips and thighs 12-13, the legs 14-17). A vertex takes the region of the piece
	// nearest it on its main bone's body, or on the nearest parent's that has pieces. [inf] MKKE cuts the mesh
	// along prepared sections and caps instead (the region table after the vertices; FIGHT-RE-NOTES §8k).
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	const TArray<FString>& Names = Package.GetNames();
	const auto ReadObject = [&Package, &Exports, &Names](int32 Ref, FOKPropertyList& Out)
	{
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(Ref - 1) || !Package.ExportData(Exports[Ref - 1], Bytes))
		{
			return false;
		}
		FOKByteReader R(Bytes);
		return Out.Read(R, Names);
	};
	struct FPiece
	{
		FBox3f Box;			// ElemBox, in the bone's space
		uint8 Region = NoRegion;
	};
	TMap<FString, TArray<FPiece>> Pieces;		// by bone name
	FOKPropertyList Asset;
	const FOKProperty* Bodies = ReadObject(Properties.GetObject(TEXT("PhysicsAsset")), Asset) ? Asset.Find(TEXT("BodySetup")) : nullptr;
	int32 PieceCount = 0;
	for (const int32 BodyRef : Bodies ? FOKPropertyList::ReadIntArray(*Bodies) : TArray<int32>())
	{
		FOKPropertyList Body;
		const FOKProperty* Geometry = ReadObject(BodyRef, Body) ? Body.Find(TEXT("AggGeom")) : nullptr;
		if (!Geometry)
		{
			continue;
		}
		FOKByteReader GeometryReader(Geometry->Raw);
		FOKPropertyList Aggregate;
		TArray<FOKPropertyList> Convexes;
		const FOKProperty* List = Aggregate.Read(GeometryReader, Names) ? Aggregate.Find(TEXT("ConvexElems")) : nullptr;
		if (!List || !FOKPropertyList::ReadStructArray(*List, Names, Convexes))
		{
			continue;
		}
		for (const FOKPropertyList& Convex : Convexes)
		{
			FOKPropertyList State;
			const FOKProperty* Box = Convex.Find(TEXT("ElemBox"));
			const uint32 Mask = ReadObject(Convex.GetObject(TEXT("DismemberState")), State) ? uint32(State.GetNumber(TEXT("StateMask"))) : 0u;
			if (!Box || Box->Raw.Num() < 24 || !FMath::IsPowerOfTwo(Mask))
			{
				continue;		// pieces in two regions at once (12+13: the hips) go with neither
			}
			float Corners[6];
			FMemory::Memcpy(Corners, Box->Raw.GetData(), sizeof(Corners));
			FPiece& Piece = Pieces.FindOrAdd(Body.GetText(TEXT("BoneName"))).AddDefaulted_GetRef();
			Piece.Box = FBox3f(FVector3f(Corners[0], Corners[1], Corners[2]), FVector3f(Corners[3], Corners[4], Corners[5]));
			Piece.Region = uint8(FMath::FloorLog2(Mask));
			++PieceCount;
		}
	}
	if (PieceCount == 0)
	{
		return;
	}

	// The reference pose in the mesh's space, and for each bone the bone whose pieces its vertices use.
	TArray<FTransform3f> RefPose;
	TArray<int32> Owner;
	RefPose.SetNum(Bones.Num());
	Owner.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const FOKBone& Bone = Bones[i];
		const FTransform3f Local(Bone.Rotation, Bone.Position);
		RefPose[i] = Bone.Parent == INDEX_NONE ? Local : Local * RefPose[Bone.Parent];
		Owner[i] = Pieces.Contains(Bone.Name) ? i : (Bone.Parent == INDEX_NONE ? INDEX_NONE : Owner[Bone.Parent]);
	}
	VertexRegions.Init(NoRegion, Vertices.Num());
	TArray<int32> PerRegion;
	PerRegion.SetNumZeroed(32);
	for (const FOKMeshChunk& Chunk : Chunks)
	{
		const int32 End = FMath::Min(Vertices.Num(), Chunk.BaseVertex + Chunk.NumRigid + Chunk.NumSoft);
		for (int32 v = Chunk.BaseVertex; v < End; ++v)
		{
			const FOKMeshVertex& Vertex = Vertices[v];
			int32 Main = 0;
			for (int32 k = 1; k < 4; ++k)
			{
				Main = Vertex.Weights[k] > Vertex.Weights[Main] ? k : Main;
			}
			const int32 Bone = Chunk.BoneMap.IsValidIndex(Vertex.Bones[Main]) ? Chunk.BoneMap[Vertex.Bones[Main]] : INDEX_NONE;
			const int32 Body = Bone == INDEX_NONE ? INDEX_NONE : Owner[Bone];
			if (Body == INDEX_NONE)
			{
				continue;
			}
			const FVector3f Local = RefPose[Body].InverseTransformPosition(Vertex.Position);
			float Nearest = MAX_flt;
			for (const FPiece& Piece : Pieces[Bones[Body].Name])
			{
				const float Distance = Piece.Box.ComputeSquaredDistanceToPoint(Local);
				if (Distance < Nearest)
				{
					Nearest = Distance;
					VertexRegions[v] = Piece.Region;
				}
			}
			++PerRegion[VertexRegions[v]];
		}
	}
	FString Counts;
	for (int32 Region = 0; Region < PerRegion.Num(); ++Region)
	{
		Counts += PerRegion[Region] > 0 ? FString::Printf(TEXT(" %d:%d"), Region, PerRegion[Region]) : FString();
	}
	UE_LOG(LogOKAsset, Log, TEXT("%s: %d dismemberment pieces; vertices by region%s"), *Name, PieceCount, *Counts);
}

uint8 FOKSkeletalMeshData::TriangleRegion(uint32 A, uint32 B, uint32 C) const
{
	if (VertexRegions.Num() != Vertices.Num())
	{
		return NoRegion;
	}
	const uint8 RA = VertexRegions[A];
	const uint8 RB = VertexRegions[B];
	return (RA == RB || RA == VertexRegions[C]) ? RA : (RB == VertexRegions[C] ? RB : RA);
}

void FOKSkeletalMeshData::AssignMaterialsAndLayers()
{
	// On multi-material character bodies the stored "material" u16 is a sort index. The material is
	// then the U tile of the section's UVs, and the sort index splits the sections into damage states
	// (MK-FORMAT-NOTES, "SkeletalMesh": material index quirk, damage states).
	const int32 MaterialCount = MaterialRefs.Num();
	bool bStoredIsSortIndex = false;
	bool bHasDamageStates = false;
	int32 OtherMax = -1;
	for (FOKMeshSection& Section : Sections)
	{
		// The UV tile holding most of the section's triangle centres.
		TMap<FIntPoint, int32> Votes;
		int32 BestVotes = 0;
		for (int32 i = Section.BaseIndex; i < Section.BaseIndex + Section.NumTriangles * 3; i += 3)
		{
			const FVector2f& A = Vertices[Indices[i]].UV0;
			const FVector2f& B = Vertices[Indices[i + 1]].UV0;
			const FVector2f& C = Vertices[Indices[i + 2]].UV0;
			const FIntPoint Tile(FMath::FloorToInt((A.X + B.X + C.X) / 3.0f), FMath::FloorToInt((A.Y + B.Y + C.Y) / 3.0f));
			const int32 Count = ++Votes.FindOrAdd(Tile);
			if (Count > BestVotes)
			{
				BestVotes = Count;
				Section.UvTile = Tile;
			}
		}
		bStoredIsSortIndex |= Section.StoredMaterial >= FMath::Max(1, MaterialCount);
		bHasDamageStates |= Section.UvTile.X < 0 || Section.UvTile.Y != 0;
		if (Section.UvTile.X < 0)
		{
			OtherMax = FMath::Max(OtherMax, Section.StoredMaterial);
		}
	}

	for (FOKMeshSection& Section : Sections)
	{
		if (!bStoredIsSortIndex)
		{
			Section.Material = FMath::Clamp(Section.StoredMaterial, 0, FMath::Max(0, MaterialCount - 1));
			Section.Layer = EOKSectionLayer::Always;
			continue;
		}
		Section.Material = (Section.UvTile.X < 0 || MaterialCount <= 1) ? 0 : FMath::Min(Section.UvTile.X, MaterialCount - 1);
		if (!bHasDamageStates)
		{
			Section.Layer = EOKSectionLayer::Always;
		}
		else if (Section.UvTile.X < 0)
		{
			Section.Layer = EOKSectionLayer::Other;
		}
		else if (OtherMax >= 0 && Section.StoredMaterial > OtherMax)
		{
			Section.Layer = EOKSectionLayer::Always;
		}
		else
		{
			Section.Layer = Section.UvTile.Y == 0 ? EOKSectionLayer::Undamaged : EOKSectionLayer::Damage;
		}
	}
}
