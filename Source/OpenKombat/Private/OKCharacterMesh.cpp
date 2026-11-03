#include "OKCharacterMesh.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "OKSkeletalMeshData.h"
#include "OKTexture.h"
#include "Animation/Skeleton.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#if WITH_EDITOR
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#endif

namespace
{
	const FName DiffuseParameter(TEXT("DiffuseMap"));

	/**
	 * The material every MK material becomes for now: lit, with the diffuse map as base colour.
	 * Built in code while there's no material asset in the project; outside the editor it falls
	 * back to the engine's default material.
	 */
	UMaterialInterface* BaseMaterial()
	{
		static TStrongObjectPtr<UMaterialInterface> Cached;
		if (Cached.IsValid())
		{
			return Cached.Get();
		}
#if WITH_EDITOR
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_OKCharacter"), RF_Transient);
		UMaterialExpressionTextureSampleParameter2D* Diffuse = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		Diffuse->ParameterName = DiffuseParameter;
		Diffuse->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		Diffuse->SamplerType = SAMPLERTYPE_Color;
		Material->GetExpressionCollection().AddExpression(Diffuse);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Diffuse);

		UMaterialExpressionConstant* Roughness = NewObject<UMaterialExpressionConstant>(Material);
		Roughness->R = 0.6f;
		Material->GetExpressionCollection().AddExpression(Roughness);
		Material->GetEditorOnlyData()->Roughness.Connect(0, Roughness);

		Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		Material->PostEditChange();		// compiles the shaders
		Cached.Reset(Material);
#else
		Cached.Reset(UMaterial::GetDefaultMaterial(MD_Surface));
#endif
		return Cached.Get();
	}

	/** The texture an MK material instance uses for a parameter (e.g. DiffuseMap), or 0. */
	int32 TextureParameter(FOKPackage& Package, int32 MaterialRef, const FString& Parameter)
	{
		const TArray<FOKPackageExport>& Exports = Package.GetExports();
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(MaterialRef - 1) || !Package.ExportData(Exports[MaterialRef - 1], Bytes))
		{
			return 0;
		}
		FOKByteReader R(Bytes);
		FOKPropertyList Properties;
		const FOKProperty* Values = Properties.Read(R, Package.GetNames()) ? Properties.Find(TEXT("TextureParameterValues")) : nullptr;
		TArray<FOKPropertyList> Elements;
		if (!Values || !FOKPropertyList::ReadStructArray(*Values, Package.GetNames(), Elements))
		{
			return 0;
		}
		for (const FOKPropertyList& Element : Elements)
		{
			if (Element.GetText(TEXT("ParameterName")) == Parameter)
			{
				return Element.GetObject(TEXT("ParameterValue"));
			}
		}
		return 0;
	}
}

UMaterialInterface* OKCharacterMesh::MakeMaterial(FOKPackage& Package, int32 MaterialRef)
{
	UMaterialInterface* Base = BaseMaterial();
	const int32 TextureRef = TextureParameter(Package, MaterialRef, DiffuseParameter.ToString());
	if (TextureRef <= 0)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("%s: no DiffuseMap stored in this package"), *Package.ObjectPath(MaterialRef));
		return Base;
	}
	return MakeMaterialFromTexture(Package, TextureRef);
}

UMaterialInterface* OKCharacterMesh::MakeMaterialFromTexture(FOKPackage& Package, int32 TextureRef)
{
	UMaterialInterface* Base = BaseMaterial();
	if (TextureRef <= 0)
	{
		return Base;
	}
	FOKTextureData TextureData;
	FString Error;
	UTexture2D* Texture = TextureData.Read(Package, TextureRef, Error) ? TextureData.CreateTexture(true, Error) : nullptr;
	if (!Texture)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("%s"), *Error);
		return Base;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	Instance->SetTextureParameterValue(DiffuseParameter, Texture);
	return Instance;
}

USkeletalMesh* OKCharacterMesh::Build(const FOKSkeletalMeshData& Data, const TArray<UMaterialInterface*>& Materials, FString& OutError)
{
	if (Data.Bones.Num() == 0 || Data.Vertices.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s has no bones or no vertices"), *Data.Name);
		return nullptr;
	}
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), FName(*Data.Name)), RF_Transient);

	// The reference skeleton. Bone transforms are relative to the parent, in Unreal's own axes.
	{
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
		for (const FOKBone& Bone : Data.Bones)
		{
			Modifier.Add(FMeshBoneInfo(FName(*Bone.Name), Bone.Name, Bone.Parent), FTransform(FQuat(Bone.Rotation), FVector(Bone.Position)));
		}
	}
	Mesh->CalculateInvRefMatrices();
	USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), USkeleton::StaticClass(), FName(*(Data.Name + TEXT("_Skeleton")))), RF_Transient);
	Skeleton->MergeAllBonesToBoneTree(Mesh, false);
	Mesh->SetSkeleton(Skeleton);
	for (const FOKMeshSocket& From : Data.Sockets)
	{
		USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh);
		Socket->SocketName = FName(*From.Name);
		Socket->BoneName = FName(*From.Bone);
		Socket->RelativeLocation = FVector(From.Location);
		Socket->RelativeRotation = FRotator(From.Rotation);
		Socket->RelativeScale = FVector(From.Scale);
		Mesh->AddSocket(Socket);
	}

	// Render data for one LOD.
	Mesh->AllocateResourceForRendering();
	FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	FSkeletalMeshLODRenderData* Lod = new FSkeletalMeshLODRenderData();
	RenderData->LODRenderData.Add(Lod);
	RenderData->NumInlinedLODs = 1;
	RenderData->NumNonOptionalLODs = 1;

	const int32 NumVertices = Data.Vertices.Num();
	Lod->StaticVertexBuffers.PositionVertexBuffer.Init(NumVertices);
	Lod->StaticVertexBuffers.StaticMeshVertexBuffer.Init(NumVertices, 2);
	TArray<FSkinWeightInfo> Weights;
	Weights.SetNumZeroed(NumVertices);
	for (int32 v = 0; v < NumVertices; ++v)
	{
		const FOKMeshVertex& Vertex = Data.Vertices[v];
		Lod->StaticVertexBuffers.PositionVertexBuffer.VertexPosition(v) = Vertex.Position;
		Lod->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(v, Vertex.TangentX.GetSafeNormal(), Vertex.TangentY.GetSafeNormal(), Vertex.TangentZ.GetSafeNormal());
		Lod->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(v, 0, Vertex.UV0);
		Lod->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(v, 1, Vertex.UV1);

		// MK weights are bytes summing to 255; Unreal's 16-bit weights sum to 65535 (255 x 257).
		int32 Sum = 0;
		for (int32 k = 0; k < 4; ++k)
		{
			Sum += Vertex.Weights[k];
		}
		int32 Given = 0;
		int32 Largest = 0;
		for (int32 k = 0; k < 4; ++k)
		{
			Weights[v].InfluenceBones[k] = Vertex.Bones[k];
			const int32 Weight = Sum > 0 ? Vertex.Weights[k] * 65535 / Sum : (k == 0 ? 65535 : 0);
			Weights[v].InfluenceWeights[k] = uint16(Weight);
			Given += Weight;
			if (Weight > Weights[v].InfluenceWeights[Largest])
			{
				Largest = k;
			}
		}
		Weights[v].InfluenceWeights[Largest] += uint16(65535 - Given);		// rounding goes to the biggest influence
	}
	Lod->SkinWeightVertexBuffer.SetMaxBoneInfluences(4);
	Lod->SkinWeightVertexBuffer.SetUse16BitBoneIndex(false);
	Lod->SkinWeightVertexBuffer.SetUse16BitBoneWeight(true);
	Lod->SkinWeightVertexBuffer = Weights;

	// One render section per section a fight starts with and dismemberment region in it. Each uses its chunk's bone
	// map, and the vertex bone indices are chunk-local, as Unreal's sections expect. Every (material, region) has a
	// material slot of its own ("Material2_R5"), so a region is hidden by hiding its slots (ShowRegions).
	TArray<uint32> Indices;
	TArray<TPair<int32, uint8>> SlotKeys;		// material, region
	int32 OutsideChunk = 0;
	for (const FOKMeshSection& Section : Data.Sections)
	{
		if (!Section.IsDrawnUndamaged() || Section.NumTriangles == 0)
		{
			continue;
		}
		const FOKMeshChunk& Chunk = Data.Chunks[Section.Chunk];
		const int32 ChunkVertices = Chunk.NumRigid + Chunk.NumSoft;
		TArray<TPair<uint8, TArray<uint32>>> ByRegion;		// in the order regions first appear
		for (int32 i = Section.BaseIndex; i < Section.BaseIndex + Section.NumTriangles * 3; i += 3)
		{
			const uint8 Region = Data.TriangleRegion(Data.Indices[i], Data.Indices[i + 1], Data.Indices[i + 2]);
			TPair<uint8, TArray<uint32>>* Run = ByRegion.FindByPredicate([Region](const TPair<uint8, TArray<uint32>>& Each) { return Each.Key == Region; });
			if (!Run)
			{
				Run = &ByRegion.Emplace_GetRef(Region, TArray<uint32>());
			}
			for (int32 k = 0; k < 3; ++k)
			{
				const uint32 Index = Data.Indices[i + k];
				OutsideChunk += (int32(Index) < Chunk.BaseVertex || int32(Index) >= Chunk.BaseVertex + ChunkVertices) ? 1 : 0;
				Run->Value.Add(Index);
			}
		}
		for (const TPair<uint8, TArray<uint32>>& Run : ByRegion)
		{
			const TPair<int32, uint8> Key(Section.Material, Run.Key);
			int32 Slot = SlotKeys.IndexOfByKey(Key);
			if (Slot == INDEX_NONE)
			{
				Slot = SlotKeys.Add(Key);
			}
			FSkelMeshRenderSection& Out = Lod->RenderSections.AddDefaulted_GetRef();
			Out.MaterialIndex = uint16(Slot);
			Out.BaseIndex = Indices.Num();
			Out.NumTriangles = Run.Value.Num() / 3;
			Out.BaseVertexIndex = Chunk.BaseVertex;
			Out.NumVertices = ChunkVertices;
			Out.BoneMap = Chunk.BoneMap;
			Out.MaxBoneInfluences = 4;
			Out.DuplicatedVerticesBuffer.Init(Out.NumVertices, TMap<int32, TArray<int32>>());
			Indices.Append(Run.Value);
		}
	}
	if (OutsideChunk > 0)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("%s: %d indices point outside their section's chunk; those vertices may be skinned to the wrong bones"), *Data.Name, OutsideChunk);
	}
	Lod->MultiSizeIndexContainer.RebuildIndexBuffer(NumVertices < 65536 ? sizeof(uint16) : sizeof(uint32), Indices);

	for (int32 Bone = 0; Bone < Data.Bones.Num(); ++Bone)
	{
		Lod->ActiveBoneIndices.Add(FBoneIndexType(Bone));
		Lod->RequiredBones.Add(FBoneIndexType(Bone));
	}

	FSkeletalMeshLODInfo& LodInfo = Mesh->AddLODInfo();
	LodInfo.ScreenSize = 1.0f;
	LodInfo.LODHysteresis = 0.02f;

	TArray<FSkeletalMaterial> Slots;
	if (SlotKeys.Num() == 0)
	{
		SlotKeys.Emplace(0, FOKSkeletalMeshData::NoRegion);
	}
	for (const TPair<int32, uint8>& Key : SlotKeys)
	{
		UMaterialInterface* Material = Materials.IsValidIndex(Key.Key) ? Materials[Key.Key] : UMaterial::GetDefaultMaterial(MD_Surface);
		const FString SlotName = Key.Value == FOKSkeletalMeshData::NoRegion ? FString::Printf(TEXT("Material%d"), Key.Key)
			: FString::Printf(TEXT("Material%d_R%d"), Key.Key, Key.Value);
		FSkeletalMaterial& Slot = Slots.Add_GetRef(FSkeletalMaterial(Material, FName(*SlotName)));
		Slot.UVChannelData.bInitialized = true;
	}
	Mesh->SetMaterials(Slots);
	Mesh->SetImportedBounds(FBoxSphereBounds(FVector(Data.BoundsOrigin), FVector(Data.BoundsExtent), Data.BoundsRadius));
	Mesh->SetHasVertexColors(false);
	Mesh->InitResources();
	return Mesh;
}

void OKCharacterMesh::ShowRegions(USkinnedMeshComponent* Component, uint32 HiddenRegions)
{
	const USkeletalMesh* Mesh = Component ? Cast<USkeletalMesh>(Component->GetSkinnedAsset()) : nullptr;
	if (!Mesh)
	{
		return;
	}
	const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		FString Left;
		FString Right;
		if (!Slots[i].MaterialSlotName.ToString().Split(TEXT("_R"), &Left, &Right))
		{
			continue;
		}
		const int32 Region = FCString::Atoi(*Right);
		const bool bShow = Region < 0 || Region > 31 || (HiddenRegions & (1u << Region)) == 0;
		if (Component->IsMaterialSectionShown(i, 0) != bShow)
		{
			Component->ShowMaterialSection(i, 0, bShow, 0);
		}
	}
}

USkeletalMesh* OKCharacterMesh::Load(const FString& PackageName, FString& OutError)
{
	TUniquePtr<FOKPackage> Package = FOKPackage::OpenGamePackage(PackageName, OutError);
	if (!Package)
	{
		return nullptr;
	}
	const FOKPackageExport* Biggest = nullptr;
	for (const FOKPackageExport* Export : Package->FindExports(TEXT("SkeletalMesh")))
	{
		if (!Biggest || Export->SerialSize > Biggest->SerialSize)
		{
			Biggest = Export;
		}
	}
	if (!Biggest)
	{
		OutError = FString::Printf(TEXT("%s has no SkeletalMesh"), *PackageName);
		return nullptr;
	}

	return LoadExport(*Package, Biggest->Index, OutError);
}

USkeletalMesh* OKCharacterMesh::LoadExport(FOKPackage& Package, int32 ExportRef, FString& OutError)
{
	const FString PackageName = Package.GetName();
	const FOKPackageExport* Biggest = Package.GetExports().IsValidIndex(ExportRef - 1) ? &Package.GetExports()[ExportRef - 1] : nullptr;
	FOKSkeletalMeshData Data;
	if (!Biggest || !Data.Read(Package, ExportRef, OutError))
	{
		return nullptr;
	}
	TArray<UMaterialInterface*> Materials;
	for (const int32 MaterialRef : Data.MaterialRefs)
	{
		Materials.Add(MaterialRef > 0 ? MakeMaterial(Package, MaterialRef) : BaseMaterial());
	}

	int32 Drawn = 0;
	for (const FOKMeshSection& Section : Data.Sections)
	{
		Drawn += Section.IsDrawnUndamaged() ? 1 : 0;
	}
	UE_LOG(LogOKAsset, Log, TEXT("%s: %s, %d bones, %d sockets, %d vertices, %d of %d sections drawn, %d materials, %d LOD(s) stored"),
		*PackageName, *Biggest->Path, Data.Bones.Num(), Data.Sockets.Num(), Data.Vertices.Num(), Drawn, Data.Sections.Num(), Materials.Num(), Data.LodCount);
	return Build(Data, Materials, OutError);
}
