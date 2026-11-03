#include "OKStage.h"
#include "OKByteReader.h"
#include "OKCharacterMesh.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "OKStaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * An object's property list after its native prefix: Prefix bytes when the class's prefix is known (28 on a
	 * StaticMeshActor, 4 on a StaticMeshComponent, 0 on materials), otherwise -1, and the first aligned offset up to
	 * MaxOffset that parses is taken. The known size comes first because the search can be fooled: on some
	 * StaticMeshComponents the 4 prefix bytes themselves read as six plausible tags.
	 */
	bool ReadPropertiesAfterPrefix(FOKPackage& Package, int32 Ref, int32 Prefix, int32 MaxOffset, FOKPropertyList& Out)
	{
		const TArray<FOKPackageExport>& Exports = Package.GetExports();
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(Ref - 1) || !Package.ExportData(Exports[Ref - 1], Bytes))
		{
			return false;
		}
		auto TryAt = [&](int32 Offset)
			{
				FOKByteReader R(Bytes);
				R.Seek(Offset);
				FOKPropertyList List;
				if (Offset > Bytes.Num() - 8 || !List.Read(R, Package.GetNames()) || List.GetAll().Num() == 0)
				{
					return false;
				}
				Out = MoveTemp(List);
				return true;
			};
		if (Prefix >= 0 && TryAt(Prefix))
		{
			return true;
		}
		for (int32 Offset = 0; Offset <= MaxOffset; Offset += 4)
		{
			if (TryAt(Offset))
			{
				return true;
			}
		}
		return false;
	}

	FVector VectorOf(const FOKPropertyList& List, const TCHAR* Name, const FVector& Default)
	{
		const FOKProperty* P = List.Find(Name);
		if (!P || P->Kind != EOKPropKind::Struct || P->Raw.Num() < 12)
		{
			return Default;
		}
		float V[3];
		FMemory::Memcpy(V, P->Raw.GetData(), 12);
		return FVector(V[0], V[1], V[2]);
	}

	/** A UE3 Rotator: int32 pitch, yaw, roll with 65536 to a turn. */
	FRotator RotatorOf(const FOKPropertyList& List, const TCHAR* Name)
	{
		const FOKProperty* P = List.Find(Name);
		if (!P || P->Kind != EOKPropKind::Struct || P->Raw.Num() < 12)
		{
			return FRotator::ZeroRotator;
		}
		int32 V[3];
		FMemory::Memcpy(V, P->Raw.GetData(), 12);
		constexpr double ToDegrees = 360.0 / 65536.0;
		return FRotator(V[0] * ToDegrees, V[1] * ToDegrees, V[2] * ToDegrees);
	}

	bool BoolOf(const FOKPropertyList& List, const TCHAR* Name)
	{
		const FOKProperty* P = List.Find(Name);
		return P && P->Kind == EOKPropKind::Bool && P->Int != 0;
	}

	bool IsBaseColorName(const FString& Name)
	{
		const FString N = Name.ToLower();
		return !N.Contains(TEXT("norm")) && (N.Contains(TEXT("diff")) || N.Contains(TEXT("albedo")) || N.Contains(TEXT("basecolor"))
			|| N.Contains(TEXT("basecolour")) || N.Contains(TEXT("colormap")) || N.Contains(TEXT("colourmap")));
	}

	bool IsTexture2D(const FOKPackage& Package, int32 Ref)
	{
		return Ref > 0 && Package.GetExports().IsValidIndex(Ref - 1) && Package.GetExports()[Ref - 1].ClassName == TEXT("Texture2D");
	}

	/**
	 * The base-colour texture of a stage material, as the viewer picks it (material.ts): a texture parameter with a
	 * diffuse-like name on the instance or its parents in this package, else one of a base Material's
	 * ReferencedTextures (by name, then the first that isn't a normal, specular or mask map). 0 if none.
	 * [inf] The parent material's shader network isn't evaluated, so this is a naming heuristic.
	 */
	int32 BaseColorTexture(FOKPackage& Package, int32 MaterialRef, int32 Depth = 0)
	{
		FOKPropertyList Props;
		if (MaterialRef <= 0 || Depth > 8 || !ReadPropertiesAfterPrefix(Package, MaterialRef, 0, 0, Props))
		{
			return 0;
		}
		TArray<FOKPropertyList> Values;
		if (const FOKProperty* Textures = Props.Find(TEXT("TextureParameterValues")))
		{
			FOKPropertyList::ReadStructArray(*Textures, Package.GetNames(), Values);
		}
		for (const FOKPropertyList& Value : Values)
		{
			const int32 Texture = Value.GetObject(TEXT("ParameterValue"));
			if (IsBaseColorName(Value.GetText(TEXT("ParameterName"))) && IsTexture2D(Package, Texture))
			{
				return Texture;
			}
		}
		if (const int32 FromParent = BaseColorTexture(Package, Props.GetObject(TEXT("Parent")), Depth + 1))
		{
			return FromParent;
		}
		const FOKProperty* Referenced = Props.Find(TEXT("ReferencedTextures"));
		const TArray<int32> Refs = Referenced ? FOKPropertyList::ReadIntArray(*Referenced) : TArray<int32>();
		// A base Material lists every texture its network samples, overlays included (CY_TrapWood_Blood_Decal_Mat
		// lists CY_Ground_Blood_Diff before CY_TrapWood_Diff). Of the diffuse-named ones, take the one sharing the
		// longest prefix with the material's own name.
		const FString MaterialName = Package.ObjectName(MaterialRef).ToLower();
		int32 Best = 0, BestShared = -1;
		for (const int32 Texture : Refs)
		{
			const FString Name = IsTexture2D(Package, Texture) ? Package.ObjectName(Texture).ToLower() : FString();
			if (Name.Contains(TEXT("diff")) || Name.EndsWith(TEXT("_d")) || Name.Contains(TEXT("albedo")) || Name.Contains(TEXT("_col")))
			{
				int32 Shared = 0;
				while (Shared < Name.Len() && Shared < MaterialName.Len() && Name[Shared] == MaterialName[Shared])
				{
					++Shared;
				}
				if (Shared > BestShared)
				{
					Best = Texture;
					BestShared = Shared;
				}
			}
		}
		if (Best)
		{
			return Best;
		}
		for (const int32 Texture : Refs)
		{
			const FString Name = IsTexture2D(Package, Texture) ? Package.ObjectName(Texture).ToLower() : FString();
			if (!Name.IsEmpty() && !Name.Contains(TEXT("norm")) && !Name.Contains(TEXT("spec")) && !Name.Contains(TEXT("mask"))
				&& !Name.Contains(TEXT("pmsk")) && !Name.EndsWith(TEXT("_n")) && !Name.EndsWith(TEXT("_s")))
			{
				return Texture;
			}
		}
		return 0;
	}

	/** UE3 EBlendMode 2-4 (translucent, additive, modulate), on the material or the parents it inherits it from. */
	bool IsTranslucent(FOKPackage& Package, int32 MaterialRef, int32 Depth = 0)
	{
		FOKPropertyList Props;
		if (MaterialRef <= 0 || Depth > 8 || !ReadPropertiesAfterPrefix(Package, MaterialRef, 0, 0, Props))
		{
			return false;
		}
		if (const FOKProperty* Blend = Props.Find(TEXT("BlendMode")))
		{
			return Blend->Text.Contains(TEXT("Translucent")) || Blend->Text.Contains(TEXT("Additive")) || Blend->Text.Contains(TEXT("Modulate"))
				|| (Blend->Text.IsEmpty() && Blend->Int >= 2 && Blend->Int <= 4);
		}
		return IsTranslucent(Package, Props.GetObject(TEXT("Parent")), Depth + 1);
	}

	TAutoConsoleVariable<FString> CVarStageSkip(TEXT("ok.Stage.Skip"), TEXT(""),
		TEXT("Parts of static mesh names that stages leave out, joined by +, e.g. CY_Sky+CY_Water (for looking into a stage)."));

	bool IsCinematicLevel(const FString& Path)
	{
		return Path.Contains(TEXT("_Cine_")) || Path.Contains(TEXT("Cinema")) || Path.Contains(TEXT("_Fatality"));
	}
}

bool OKStage::Spawn(UWorld* World, const FString& PackageName, FOKStage& OutStage, FString& OutError)
{
	TUniquePtr<FOKPackage> Package = World ? FOKPackage::OpenGamePackage(PackageName, OutError) : nullptr;
	if (!Package)
	{
		return false;
	}
	const TArray<FOKPackageExport>& Exports = Package->GetExports();
	auto LevelOf = [&](const FOKPackageExport& Export) -> const FOKPackageExport*
		{
			return Exports.IsValidIndex(Export.Outer - 1) && Exports[Export.Outer - 1].ClassName == TEXT("Level") ? &Exports[Export.Outer - 1] : nullptr;
		};

	// The fight line.
	for (const FOKPackageExport& Export : Exports)
	{
		FOKPropertyList Props;
		if (Export.ClassName == TEXT("MKArenaConstrainPoint") && ReadPropertiesAfterPrefix(*Package, Export.Index, -1, 512, Props))
		{
			const FString Tag = Props.GetText(TEXT("Tag"));
			FVector& Point = Tag.EndsWith(TEXT("_Right")) ? OutStage.Right : OutStage.Left;
			Point = VectorOf(Props, TEXT("Location"), FVector::ZeroVector);
		}
	}
	OutStage.bHasArena = !OutStage.Left.Equals(OutStage.Right);

	AActor* Stage = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Stage, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Stage->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Stage->SetActorLabel(PackageName);
#endif

	TMap<int32, UStaticMesh*> Meshes;				// by StaticMesh reference; null when it couldn't be built
	TMap<int32, UMaterialInterface*> Materials;		// by material reference
	// Translucent materials with no base colour in the package (CY_waves_Mat: an opacity mask) have nothing an
	// opaque stand-in could show; meshes drawn only with them are left out. [inf] Their networks are system W8.
	TSet<int32> NothingToDraw;
	TMap<int32, TArray<int32>> ElementRefs;			// by StaticMesh reference
	auto MaterialFor = [&](int32 Ref) -> UMaterialInterface*
		{
			if (UMaterialInterface** Found = Materials.Find(Ref))
			{
				return *Found;
			}
			const int32 Texture = BaseColorTexture(*Package, Ref);
			if (Texture == 0)
			{
				UE_LOG(LogOKAsset, Log, TEXT("%s: no base colour texture in the package for %s"), *PackageName, *Package->ObjectPath(Ref));
				if (IsTranslucent(*Package, Ref))
				{
					NothingToDraw.Add(Ref);
				}
			}
			UMaterialInterface* Material = OKCharacterMesh::MakeMaterialFromTexture(*Package, Texture);
			Materials.Add(Ref, Material);
			return Material;
		};

	for (const FOKPackageExport& Export : Exports)
	{
		const FOKPackageExport* Level = Export.ClassName == TEXT("StaticMeshActor") ? LevelOf(Export) : nullptr;
		if (!Level || IsCinematicLevel(Level->Path))
		{
			continue;
		}
		FOKPropertyList Actor, Component;
		const int32 ComponentRef = ReadPropertiesAfterPrefix(*Package, Export.Index, 28, 64, Actor) ? Actor.GetObject(TEXT("StaticMeshComponent")) : 0;
		if (BoolOf(Actor, TEXT("bHidden")) || ComponentRef <= 0 || !ReadPropertiesAfterPrefix(*Package, ComponentRef, 4, 64, Component)
			|| BoolOf(Component, TEXT("HiddenGame")))		// hidden: collision floors (*_FloorCol_St) and editor-only geometry
		{
			++OutStage.Skipped;
			continue;
		}
		// A property left at its default comes from the object's archetype (actors placed from a template).
		int32 MeshRef = Component.GetObject(TEXT("StaticMesh"));
		for (int32 Archetype = Exports[ComponentRef - 1].Archetype, Depth = 0; MeshRef == 0 && Archetype != 0 && Depth < 4; ++Depth)
		{
			FOKPropertyList Template;
			if (Archetype < 0 || !ReadPropertiesAfterPrefix(*Package, Archetype, 4, 64, Template))
			{
				UE_LOG(LogOKAsset, Log, TEXT("%s: %s takes its mesh from %s, which isn't readable here"), *PackageName, *Export.Path, *Package->ObjectPath(Archetype));
				break;
			}
			MeshRef = Template.GetObject(TEXT("StaticMesh"));
			Archetype = Exports[Archetype - 1].Archetype;
		}
		TArray<FString> SkipNames;
		CVarStageSkip.GetValueOnGameThread().ParseIntoArray(SkipNames, TEXT("+"));
		if (MeshRef > 0 && SkipNames.ContainsByPredicate([&](const FString& Part) { return Package->ObjectName(MeshRef).Contains(Part); }))
		{
			++OutStage.Skipped;
			continue;
		}
		if (MeshRef <= 0)
		{
			if (MeshRef < 0)
			{
				UE_LOG(LogOKAsset, Log, TEXT("%s: %s uses %s, stored in another package"), *PackageName, *Export.Path, *Package->ObjectPath(MeshRef));
			}
			++OutStage.Skipped;		// none, or stored in another package
			continue;
		}

		UStaticMesh* Mesh = nullptr;
		if (UStaticMesh** Found = Meshes.Find(MeshRef))
		{
			Mesh = *Found;
		}
		else
		{
			FOKStaticMeshData Data;
			FString Error;
			if (Data.Read(*Package, MeshRef, Error))
			{
				TArray<UMaterialInterface*> ElementMaterials;
				TArray<int32>& Refs = ElementRefs.Add(MeshRef);
				for (const FOKStaticMeshElement& Element : Data.Elements)
				{
					ElementMaterials.Add(MaterialFor(Element.MaterialRef));
					Refs.Add(Element.MaterialRef);
				}
				Mesh = OKStaticMesh::Build(Data, ElementMaterials, Error);
				UE_LOG(LogOKAsset, Verbose, TEXT("%s: %s, %d vertices, %d indices, %d elements, %d UV sets, bounds %s"), *PackageName, *Data.Name, Data.Positions.Num(), Data.Indices.Num(), Data.Elements.Num(), Data.UVs.Num(), Mesh ? *Mesh->GetBounds().BoxExtent.ToString() : TEXT("-"));
			}
			if (!Mesh)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("%s: %s"), *PackageName, *Error);
			}
			Meshes.Add(MeshRef, Mesh);
		}
		if (!Mesh)
		{
			++OutStage.Skipped;
			continue;
		}

		// Left out if every element's material (after the component's overrides) has nothing to draw.
		TArray<int32> Effective = ElementRefs.FindRef(MeshRef);
		const FOKProperty* Overrides = Component.Find(TEXT("Materials"));
		const TArray<int32> OverrideRefs = Overrides ? FOKPropertyList::ReadIntArray(*Overrides) : TArray<int32>();
		for (int32 i = 0; i < OverrideRefs.Num() && i < Effective.Num(); ++i)
		{
			if (OverrideRefs[i] != 0)
			{
				MaterialFor(OverrideRefs[i]);
				Effective[i] = OverrideRefs[i];
			}
		}
		if (Effective.Num() > 0 && !Effective.ContainsByPredicate([&](int32 Ref) { return !NothingToDraw.Contains(Ref); }))
		{
			++OutStage.Skipped;
			continue;
		}

		// The component sits on the actor: world = component-relative × actor.
		const double DrawScale = Actor.GetNumber(TEXT("DrawScale"), 1.0);
		const FTransform ActorTransform(RotatorOf(Actor, TEXT("Rotation")), VectorOf(Actor, TEXT("Location"), FVector::ZeroVector),
			VectorOf(Actor, TEXT("DrawScale3D"), FVector::OneVector) * DrawScale);
		const double Scale = Component.GetNumber(TEXT("Scale"), 1.0);
		const FTransform ComponentTransform(RotatorOf(Component, TEXT("Rotation")), VectorOf(Component, TEXT("Translation"), FVector::ZeroVector),
			VectorOf(Component, TEXT("Scale3D"), FVector::OneVector) * Scale);

		// Levels reuse actor names (StaticMeshActor_12 in each), and a second object of the same name would replace the first.
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Stage, MakeUniqueObjectName(Stage, UStaticMeshComponent::StaticClass(), FName(*Export.Name)));
		Part->SetMobility(EComponentMobility::Movable);
		Part->SetupAttachment(Root);
		Part->SetRelativeTransform(ComponentTransform * ActorTransform);
		Part->SetStaticMesh(Mesh);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// The component's Materials override the mesh's, element by element.
		for (int32 i = 0; i < OverrideRefs.Num(); ++i)
		{
			if (OverrideRefs[i] != 0)
			{
				Part->SetMaterial(i, MaterialFor(OverrideRefs[i]));
			}
		}
		Part->RegisterComponent();
		Stage->AddInstanceComponent(Part);
		++OutStage.Placements;
	}

	for (const TPair<int32, UStaticMesh*>& Pair : Meshes)
	{
		OutStage.Meshes += Pair.Value ? 1 : 0;
	}
	OutStage.Actor = Stage;
	UE_LOG(LogOKAsset, Log, TEXT("%s: %d placements of %d meshes, %d materials, %d actors skipped; arena %s"), *PackageName, OutStage.Placements,
		OutStage.Meshes, Materials.Num(), OutStage.Skipped, OutStage.bHasArena ? *FString::Printf(TEXT("%s to %s"), *OutStage.Left.ToString(), *OutStage.Right.ToString()) : TEXT("not found"));
	if (OutStage.Placements == 0)
	{
		OutError = FString::Printf(TEXT("%s: no static mesh actors could be built"), *PackageName);
		return false;
	}
	return true;
}

UStaticMesh* OKStage::LoadMesh(FOKPackage& Package, int32 MeshRef, FString& OutError)
{
	FOKStaticMeshData Data;
	if (!Data.Read(Package, MeshRef, OutError))
	{
		return nullptr;
	}
	TArray<UMaterialInterface*> ElementMaterials;
	for (const FOKStaticMeshElement& Element : Data.Elements)
	{
		ElementMaterials.Add(OKCharacterMesh::MakeMaterialFromTexture(Package, BaseColorTexture(Package, Element.MaterialRef)));
	}
	return OKStaticMesh::Build(Data, ElementMaterials, OutError);
}
