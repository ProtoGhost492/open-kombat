#include "OKStaticMesh.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace
{
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

	/**
	 * A vertex stream: {int32 stride, int32 numVertices, int32 count, count × stride bytes}. Count may be 0 (no colours)
	 * or more than numVertices (some FX meshes); only the first numVertices are used.
	 */
	struct FStream
	{
		int32 Stride = 0;
		int32 NumVertices = 0;
		int32 Count = 0;
		const uint8* Bytes = nullptr;
	};

	bool ReadStream(FOKByteReader& R, FStream& Out)
	{
		Out.Stride = R.I32();
		Out.NumVertices = R.I32();
		Out.Count = R.I32();
		if (R.HasError() || Out.Stride <= 0 || Out.Stride > 256 || Out.Count < 0 || int64(Out.Count) * Out.Stride > R.Remaining())
		{
			return false;
		}
		Out.Bytes = R.Take(int64(Out.Count) * Out.Stride);
		return Out.Count == 0 || Out.Bytes != nullptr;
	}
}

bool FOKStaticMeshData::Read(FOKPackage& Package, int32 ExportRef, FString& OutError)
{
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	if (!Exports.IsValidIndex(ExportRef - 1) || Exports[ExportRef - 1].ClassName != TEXT("StaticMesh"))
	{
		OutError = FString::Printf(TEXT("%s isn't a StaticMesh stored in %s"), *Package.ObjectPath(ExportRef), *Package.GetName());
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
	bool bBad = false;
	R.Skip(28);			// bounds: origin, extent, radius
	R.I32();			// BodySetup
	R.Skip(int64(ReadCount(R, 10000000, 36, bBad)) * 36);		// kDOP nodes {min, max, isLeaf, a, b}
	R.Skip(int64(ReadCount(R, 10000000, 16, bBad)) * 16);		// kDOP triangles {v0, v1, v2, material}
	R.I32();			// InternalVersion (15)
	LodCount = ReadCount(R, 16, 0, bBad);
	if (bBad || LodCount < 1)
	{
		OutError = FString::Printf(TEXT("%s: no LOD models"), *Export.Path);
		return false;
	}

	// LOD 0
	R.Skip(16);			// RawTriangles bulk data header (empty when cooked)
	const int32 ElementCount = ReadCount(R, 4096, 32, bBad);
	Elements.SetNum(ElementCount);
	for (FOKStaticMeshElement& Element : Elements)
	{
		Element.MaterialRef = R.I32();
		R.I32();
		R.I32();
		Element.FirstIndex = R.I32();
		Element.NumTriangles = R.I32();
		R.I32();		// min vertex
		R.I32();		// max vertex
		R.Skip(int64(ReadCount(R, 100000, 16, bBad)) * 16);		// fragments
	}

	FStream Position, Color, Tangent, UV;
	if (bBad || !ReadStream(R, Position) || Position.Stride != 12 || Position.Count != Position.NumVertices)
	{
		OutError = FString::Printf(TEXT("%s: bad position stream"), *Export.Path);
		return false;
	}
	const int32 N = Position.NumVertices;
	const int32 NumUVSets = (ReadStream(R, Color) && ReadStream(R, Tangent)) ? R.I32() : 0;
	if (Color.Stride != 4 || Tangent.Stride != 12 || Tangent.Count < N || NumUVSets < 1 || NumUVSets > 8 || !ReadStream(R, UV) || UV.Count < N)
	{
		OutError = FString::Printf(TEXT("%s: bad colour, tangent or UV stream"), *Export.Path);
		return false;
	}
	const bool bHalfUVs = UV.Stride == NumUVSets * 4;		// otherwise two floats per set
	if (!bHalfUVs && UV.Stride != NumUVSets * 8)
	{
		OutError = FString::Printf(TEXT("%s: UV stride %d doesn't fit %d sets"), *Export.Path, UV.Stride, NumUVSets);
		return false;
	}
	R.I32();			// [unk] the vertex count again
	const int32 IndexCount = ReadCount(R, 50000000, 2, bBad);
	Indices.SetNum(IndexCount);
	for (uint32& Index : Indices)
	{
		Index = R.U16();
	}
	if (bBad || R.HasError())
	{
		OutError = FString::Printf(TEXT("%s: truncated"), *Export.Path);
		return false;
	}
	for (const uint32 Index : Indices)
	{
		if (Index >= uint32(N))
		{
			OutError = FString::Printf(TEXT("%s: index %u past %d vertices"), *Export.Path, Index, N);
			return false;
		}
	}
	for (const FOKStaticMeshElement& Element : Elements)
	{
		if (Element.FirstIndex < 0 || Element.NumTriangles < 0 || int64(Element.FirstIndex) + int64(Element.NumTriangles) * 3 > IndexCount)
		{
			OutError = FString::Printf(TEXT("%s: an element runs past the index buffer"), *Export.Path);
			return false;
		}
	}

	Positions.SetNum(N);
	FMemory::Memcpy(Positions.GetData(), Position.Bytes, int64(N) * 12);
	TangentX.SetNum(N);
	TangentY.SetNum(N);
	TangentZ.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		const uint8* T = Tangent.Bytes + i * 12;
		TangentX[i] = UnpackVector(T);
		TangentY[i] = UnpackVector(T + 4);
		TangentZ[i] = UnpackVector(T + 8);
	}
	UVs.SetNum(NumUVSets);
	for (int32 Set = 0; Set < NumUVSets; ++Set)
	{
		UVs[Set].SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			const uint8* At = UV.Bytes + i * UV.Stride + Set * (bHalfUVs ? 4 : 8);
			if (bHalfUVs)
			{
				FFloat16 U, V;
				FMemory::Memcpy(&U.Encoded, At, 2);
				FMemory::Memcpy(&V.Encoded, At + 2, 2);
				UVs[Set][i] = FVector2f(U.GetFloat(), V.GetFloat());
			}
			else
			{
				FMemory::Memcpy(&UVs[Set][i], At, 8);
			}
		}
	}
	// FColor is B, G, R, A in memory, as stored.
	if (Color.Count >= N)
	{
		Colors.SetNum(N);
		FMemory::Memcpy(Colors.GetData(), Color.Bytes, int64(N) * 4);
	}
	return true;
}

UStaticMesh* OKStaticMesh::Build(const FOKStaticMeshData& Data, const TArray<UMaterialInterface*>& Materials, FString& OutError)
{
	const int32 N = Data.Positions.Num();
	if (N == 0 || Data.Indices.Num() < 3 || Data.UVs.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s has no triangles"), *Data.Name);
		return nullptr;
	}

	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(Data.UVs.Num());

	// One vertex instance per stored vertex: MKKE's buffers are already split where normals or UVs differ.
	Description.ReserveNewVertices(N);
	Description.ReserveNewVertexInstances(N);
	Description.ReserveNewTriangles(Data.Indices.Num() / 3);
	TArray<FVertexInstanceID> Instances;
	Instances.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		const FVertexID Vertex = Description.CreateVertex();
		Positions[Vertex] = Data.Positions[i];
		const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
		Instances[i] = Instance;
		const FVector3f X = Data.TangentX[i], Y = Data.TangentY[i], Z = Data.TangentZ[i];
		Normals[Instance] = Z.GetSafeNormal();
		Tangents[Instance] = X.GetSafeNormal();
		// Unreal rebuilds the bitangent as cross(normal, tangent) × sign.
		BinormalSigns[Instance] = FVector3f::DotProduct(FVector3f::CrossProduct(Z, X), Y) < 0.0f ? -1.0f : 1.0f;
		Colors[Instance] = Data.Colors.IsValidIndex(i) ? FVector4f(Data.Colors[i].ReinterpretAsLinear()) : FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		for (int32 Set = 0; Set < Data.UVs.Num(); ++Set)
		{
			UVs.Set(Instance, Set, Data.UVs[Set][i]);
		}
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), FName(*Data.Name)), RF_Transient);
	for (int32 e = 0; e < Data.Elements.Num(); ++e)
	{
		const FOKStaticMeshElement& Element = Data.Elements[e];
		const FName Slot(*FString::Printf(TEXT("Element%d"), e));
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		SlotNames[Group] = Slot;
		for (int32 t = 0; t < Element.NumTriangles; ++t)
		{
			const int32 At = Element.FirstIndex + t * 3;
			const uint32 A = Data.Indices[At], B = Data.Indices[At + 1], C = Data.Indices[At + 2];
			if (A == B || B == C || A == C)
			{
				continue;		// degenerate: strip stitching
			}
			const FVertexInstanceID Corners[3] = { Instances[A], Instances[B], Instances[C] };
			Description.CreateTriangle(Group, MakeArrayView(Corners, 3));
		}
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials.IsValidIndex(e) ? Materials[e] : nullptr, Slot, Slot));
	}

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;			// the only path outside the editor
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	const FMeshDescription* Descriptions[] = { &Description };
	if (!Mesh->BuildFromMeshDescriptions(TArray<const FMeshDescription*>(Descriptions, 1), Params))
	{
		OutError = FString::Printf(TEXT("%s: Unreal couldn't build the mesh"), *Data.Name);
		return nullptr;
	}
	return Mesh;
}
