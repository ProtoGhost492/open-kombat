#include "OKCinema.h"
#include "OKByteReader.h"
#include "OKAnimation.h"
#include "OKCharacterMesh.h"
#include "OKCinemaProp.h"
#include "OKFighterActor.h"
#include "OKParticles.h"
#include "Components/PoseableMeshComponent.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "OKStage.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/PointLight.h"
#include "EngineUtils.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace
{
	/** An export's tags; subobjects that store 4 bytes before them are read past those. */
	bool ReadObject(FOKPackage& Package, int32 Ref, FOKPropertyList& Out)
	{
		const TArray<FOKPackageExport>& Exports = Package.GetExports();
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(Ref - 1) || !Package.ExportData(Exports[Ref - 1], Bytes))
		{
			return false;
		}
		FOKByteReader Reader(Bytes);
		if (Out.Read(Reader, Package.GetNames()))
		{
			return true;
		}
		Out = FOKPropertyList();
		FOKByteReader Skipped(TConstArrayView<uint8>(Bytes).Slice(FMath::Min(4, Bytes.Num()), FMath::Max(0, Bytes.Num() - 4)));
		return Out.Read(Skipped, Package.GetNames());
	}

	/** A struct property's own tags (PositionCurveSet, InterpCurveFloat ...). */
	bool ReadStruct(const FOKPropertyList& Properties, const FString& Name, const TArray<FString>& Names, FOKPropertyList& Out)
	{
		const FOKProperty* Property = Properties.Find(Name);
		if (!Property || Property->Kind != EOKPropKind::Struct)
		{
			return false;
		}
		FOKByteReader Reader(Property->Raw);
		return Out.Read(Reader, Names);
	}

	/** A float property, or one component of a Vector struct property. */
	float Component(const FOKPropertyList& Properties, const FString& Name, int32 Index)
	{
		const FOKProperty* Property = Properties.Find(Name);
		if (!Property)
		{
			return 0.0f;
		}
		if (Property->Kind == EOKPropKind::Float)
		{
			return Property->Float;
		}
		float Parts[3] = {};
		if (Property->Kind == EOKPropKind::Struct && Property->Raw.Num() >= 12)
		{
			FMemory::Memcpy(Parts, Property->Raw.GetData(), sizeof(Parts));
		}
		return Parts[FMath::Clamp(Index, 0, 2)];
	}

	/** An InterpCurveFloat, or one component (0-2) of an InterpCurveVector. */
	FOKInterpCurve ReadCurve(const FOKPropertyList& Properties, const FString& Name, const TArray<FString>& Names, int32 Index = 0)
	{
		FOKInterpCurve Curve;
		FOKPropertyList Struct;
		TArray<FOKPropertyList> Points;
		const FOKProperty* List = ReadStruct(Properties, Name, Names, Struct) ? Struct.Find(TEXT("Points")) : nullptr;
		if (!List || !FOKPropertyList::ReadStructArray(*List, Names, Points))
		{
			return Curve;
		}
		static const TCHAR* const Modes[] = { TEXT("CIM_Linear"), TEXT("CIM_CurveAuto"), TEXT("CIM_Constant"), TEXT("CIM_CurveUser"), TEXT("CIM_CurveBreak"), TEXT("CIM_CurveAutoClamped") };
		for (const FOKPropertyList& Point : Points)
		{
			FOKInterpCurve::FPoint& P = Curve.Points.AddDefaulted_GetRef();
			P.In = float(Point.GetNumber(TEXT("InVal"), 0.0));
			P.Out = Component(Point, TEXT("OutVal"), Index);
			P.Arrive = Component(Point, TEXT("ArriveTangent"), Index);
			P.Leave = Component(Point, TEXT("LeaveTangent"), Index);
			const FOKProperty* Mode = Point.Find(TEXT("InterpMode"));
			if (Mode && Mode->Text.IsEmpty())
			{
				P.Mode = uint8(Mode->Int);
			}
			else if (Mode)
			{
				for (int32 m = 0; m < UE_ARRAY_COUNT(Modes); ++m)
				{
					P.Mode = Mode->Text == Modes[m] ? uint8(m) : P.Mode;
				}
			}
		}
		return Curve;
	}

	/** A focal length (mm) as a horizontal field of view, for 35 mm film's 36 mm gate. [inf] MKKE's own conversion isn't read yet. */
	float FieldOfView(float FocalLength)
	{
		return FocalLength > 0.0f ? FMath::RadiansToDegrees(2.0f * FMath::Atan(18.0f / FocalLength)) : 60.0f;
	}
}

float FOKInterpCurve::Eval(float Time, float Default) const
{
	if (Points.Num() == 0)
	{
		return Default;
	}
	if (Points.Num() == 1 || Time <= Points[0].In)
	{
		return Points[0].Out;
	}
	if (Time >= Points.Last().In)
	{
		return Points.Last().Out;
	}
	int32 i = 0;
	while (i + 1 < Points.Num() - 1 && Time >= Points[i + 1].In)
	{
		++i;
	}
	const FPoint& A = Points[i];
	const FPoint& B = Points[i + 1];
	const float Span = B.In - A.In;
	const float Alpha = Span > 0.0f ? (Time - A.In) / Span : 0.0f;
	switch (A.Mode)
	{
	case 0: return FMath::Lerp(A.Out, B.Out, Alpha);
	case 2: return A.Out;
	default: return FMath::CubicInterp(A.Out, A.Leave * Span, B.Out, B.Arrive * Span, Alpha);
	}
}

FTransform FOKCinemaGroup::MoveAt(float Time) const
{
	const FRotator Rotation(Pitch.Eval(Time), Yaw.Eval(Time), Roll.Eval(Time));
	return FTransform(Rotation, FVector(X.Eval(Time), Y.Eval(Time), Z.Eval(Time)));
}

bool FOKCinemaGroup::AnimAt(float Time, FString& OutSequence, float& OutSeconds) const
{
	const FAnimKey* Current = nullptr;
	for (const FAnimKey& Key : Anims)
	{
		if (Key.Start <= Time && (!Current || Key.Start >= Current->Start))
		{
			Current = &Key;
		}
	}
	if (!Current)
	{
		return false;
	}
	OutSequence = Current->Sequence;
	OutSeconds = Current->StartOffset + (Time - Current->Start) * Current->Rate;
	return true;
}

bool FOKCinemaGroup::IsHiddenAt(float Time) const
{
	bool bHidden = false;
	for (const TPair<float, bool>& Key : Hide)
	{
		bHidden = Key.Key <= Time ? Key.Value : bHidden;
	}
	return bHidden;
}

uint32 FOKCinemaGroup::HiddenRegionsAt(float Time) const
{
	uint32 Hidden = 0;
	for (const TPair<float, uint32>& Key : Dismemberment)
	{
		Hidden = Key.Key <= Time ? Key.Value : Hidden;
	}
	return Hidden;
}

const FOKCinemaGroup::FAttachment* FOKCinemaGroup::AttachmentAt(float Time) const
{
	const FAttachment* Found = nullptr;
	for (const FAttachment& Each : Attachments)
	{
		Found = Each.Time <= Time ? &Each : Found;
	}
	return Found;
}

bool FOKCinemaData::InScene(const FOKPackageExport& Export) const
{
	return ScenePrefix.IsEmpty() || Export.Path.StartsWith(ScenePrefix, ESearchCase::IgnoreCase);
}

bool FOKCinemaData::Read(FOKPackage& Package, FString& OutError, const FString& Scene)
{
	const TArray<FString>& Names = Package.GetNames();
	const bool bSubPackage = !Scene.IsEmpty() && !Scene.Equals(Package.GetName(), ESearchCase::IgnoreCase);
	Name = bSubPackage ? Scene : Package.GetName();
	ScenePrefix = bSubPackage ? Scene + TEXT(".") : FString();
	int32 DataRef = 0;
	float Longest = -1.0f;
	for (const FOKPackageExport* Action : Package.FindExports(TEXT("SeqAct_Cinema")))
	{
		if (!InScene(*Action))
		{
			continue;
		}
		FOKPropertyList Props;
		FOKPropertyList Interp;
		const int32 Ref = ReadObject(Package, Action->Index, Props) ? Props.GetObject(TEXT("InterpData")) : 0;
		if (Ref <= 0 || !ReadObject(Package, Ref, Interp))
		{
			continue;
		}
		const float ActionLength = float(Interp.GetNumber(TEXT("InterpLength"), 0.0));
		// the scene itself is named after the package; the others loop (a held pose)
		const bool bNamed = Props.GetText(TEXT("ObjComment")).Equals(Name, ESearchCase::IgnoreCase);
		if (bNamed || ActionLength > Longest)
		{
			DataRef = Ref;
			Longest = bNamed ? MAX_flt : ActionLength;
		}
	}
	FOKPropertyList Interp;
	if (DataRef <= 0 || !ReadObject(Package, DataRef, Interp))
	{
		OutError = FString::Printf(TEXT("%s has no cinema (SeqAct_Cinema)"), *Name);
		return false;
	}
	Length = float(Interp.GetNumber(TEXT("InterpLength"), 0.0));
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	const FOKProperty* GroupList = Interp.Find(TEXT("InterpGroups"));
	for (const int32 GroupRef : GroupList ? FOKPropertyList::ReadIntArray(*GroupList) : TArray<int32>())
	{
		FOKPropertyList G;
		if (!Exports.IsValidIndex(GroupRef - 1) || !ReadObject(Package, GroupRef, G))
		{
			continue;
		}
		const bool bDirector = Exports[GroupRef - 1].ClassName == TEXT("InterpGroupDirector");
		FOKCinemaGroup Group;
		Group.Name = G.GetText(TEXT("GroupName"));
		Group.ProxyMesh = G.GetText(TEXT("ProxyActorMesh"));
		const int32 Factory = G.GetObject(TEXT("Factory"));
		const FString FactoryClass = Exports.IsValidIndex(Factory - 1) ? Exports[Factory - 1].ClassName : FString();
		FOKPropertyList FactoryProps;
		if (Factory > 0 && ReadObject(Package, Factory, FactoryProps))
		{
			Group.MeshRef = FMath::Max(0, FactoryProps.GetObject(TEXT("SkeletalMesh")));
			Group.StaticMeshRef = FMath::Max(0, FactoryProps.GetObject(TEXT("StaticMesh")));
			const int32 System = FactoryProps.GetObject(TEXT("ParticleSystem"));
			Group.ParticleSystem = System != 0 ? Package.ObjectName(System).TrimEnd() : FString();
		}
		Group.Kind = FactoryClass.Contains(TEXT("Camera")) ? FOKCinemaGroup::EKind::Camera
			: FactoryClass.Contains(TEXT("SkeletalMesh")) ? FOKCinemaGroup::EKind::Actor
			: FactoryClass.Contains(TEXT("Emitter")) ? FOKCinemaGroup::EKind::Emitter
			: (FactoryClass.Contains(TEXT("Mover")) || FactoryClass.Contains(TEXT("StaticMesh"))) ? FOKCinemaGroup::EKind::Static : FOKCinemaGroup::EKind::Other;

		const FOKProperty* TrackList = G.Find(TEXT("InterpTracks"));
		for (const int32 TrackRef : TrackList ? FOKPropertyList::ReadIntArray(*TrackList) : TArray<int32>())
		{
			FOKPropertyList T;
			if (!Exports.IsValidIndex(TrackRef - 1) || !ReadObject(Package, TrackRef, T))
			{
				continue;
			}
			const FString& Class = Exports[TrackRef - 1].ClassName;
			if (Class == TEXT("InterpTrackDirector"))
			{
				TArray<FOKPropertyList> CutList;
				if (const FOKProperty* CutProperty = T.Find(TEXT("CutTrack")))
				{
					FOKPropertyList::ReadStructArray(*CutProperty, Names, CutList);
				}
				for (const FOKPropertyList& Cut : CutList)
				{
					Cuts.Emplace(float(Cut.GetNumber(TEXT("Time"), 0.0)), Cut.GetText(TEXT("TargetCamGroup")));
				}
			}
			else if (Class == TEXT("InterpTrackSlomo"))
			{
				Slomo = ReadCurve(T, TEXT("FloatTrack"), Names);
			}
			else if (Class == TEXT("CinemaTrackMove") && !Group.bHasMove)
			{
				FOKPropertyList Curves;
				if (ReadStruct(T, TEXT("PosCurves"), Names, Curves))
				{
					Group.X = ReadCurve(Curves, TEXT("xCurve"), Names);
					Group.Y = ReadCurve(Curves, TEXT("yCurve"), Names);
					Group.Z = ReadCurve(Curves, TEXT("zCurve"), Names);
				}
				Group.Roll = ReadCurve(T, TEXT("EulerTrack"), Names, 0);
				Group.Pitch = ReadCurve(T, TEXT("EulerTrack"), Names, 1);
				Group.Yaw = ReadCurve(T, TEXT("EulerTrack"), Names, 2);
				Group.bHasMove = true;
			}
			else if (Class == TEXT("CinemaTrackHideActor") || Class == TEXT("InterpTrackToggle") || Class == TEXT("CinemaTrackAttachment"))
			{
				const TCHAR* ListName = Class == TEXT("CinemaTrackHideActor") ? TEXT("Keys") : Class == TEXT("InterpTrackToggle") ? TEXT("ToggleTrack") : TEXT("AttachmentTrack");
				TArray<FOKPropertyList> Keys;
				if (const FOKProperty* List = T.Find(ListName))
				{
					FOKPropertyList::ReadStructArray(*List, Names, Keys);
				}
				for (const FOKPropertyList& Key : Keys)
				{
					const float KeyTime = float(Key.GetNumber(TEXT("Time"), 0.0));
					if (Class == TEXT("CinemaTrackHideActor"))
					{
						const FOKProperty* Flag = Key.Find(TEXT("Flag"));
						Group.Hide.Emplace(KeyTime, Flag && Flag->Int != 0);
					}
					else if (Class == TEXT("InterpTrackToggle"))
					{
						const FOKProperty* Action = Key.Find(TEXT("ToggleAction"));
						Group.Toggles.Emplace(KeyTime, uint8(Action ? Action->Int : 0));
					}
					else
					{
						FOKCinemaGroup::FAttachment& Attach = Group.Attachments.AddDefaulted_GetRef();
						Attach.Time = KeyTime;
						Attach.Group = Key.GetText(TEXT("AttachmentName"));
						Attach.Bone = Key.GetText(TEXT("AttachedBoneName"));
						int32 Units[3] = {};
						if (const FOKProperty* Rotation = Key.Find(TEXT("RelativeRotation")); Rotation && Rotation->Raw.Num() >= 12)
						{
							FMemory::Memcpy(Units, Rotation->Raw.GetData(), sizeof(Units));
						}
						constexpr double ToDegrees = 360.0 / 65536.0;
						Attach.Relative = FTransform(FRotator(Units[0] * ToDegrees, Units[1] * ToDegrees, Units[2] * ToDegrees),
							FVector(Component(Key, TEXT("RelativeLocation"), 0), Component(Key, TEXT("RelativeLocation"), 1), Component(Key, TEXT("RelativeLocation"), 2)));
					}
				}
			}
			else if (Class == TEXT("CinemaTrackCharDismemberment"))
			{
				// each key lists every region hidden from then (0-18), not the ones it adds
				TArray<FOKPropertyList> Keys;
				if (const FOKProperty* List = T.Find(TEXT("CharDismembermentTrack")))
				{
					FOKPropertyList::ReadStructArray(*List, Names, Keys);
				}
				for (const FOKPropertyList& Key : Keys)
				{
					uint32 Hidden = 0;
					const FOKProperty* Regions = Key.Find(TEXT("Regions"));
					for (const int32 Region : Regions ? FOKPropertyList::ReadIntArray(*Regions) : TArray<int32>())
					{
						Hidden |= (Region >= 0 && Region < 32) ? (1u << Region) : 0u;
					}
					Group.Dismemberment.Emplace(float(Key.GetNumber(TEXT("Time"), 0.0)), Hidden);
				}
			}
			else if (Class == TEXT("InterpTrackVectorParticleParam"))
			{
				FOKCinemaGroup::FParameter& Parameter = Group.Parameters.AddDefaulted_GetRef();
				Parameter.Name = FName(*T.GetText(TEXT("ParamNameOut")));
				Parameter.R = ReadCurve(T, TEXT("VectorTrack"), Names, 0);
				Parameter.G = ReadCurve(T, TEXT("VectorTrack"), Names, 1);
				Parameter.B = ReadCurve(T, TEXT("VectorTrack"), Names, 2);
			}
			else if (Class == TEXT("CinemaTrackFocalLength"))
			{
				Group.FocalLength = ReadCurve(T, TEXT("FloatTrack"), Names);
			}
			else if (Class == TEXT("CinemaTrackBlendAnim"))
			{
				TArray<FOKPropertyList> Keys;
				if (const FOKProperty* Seqs = T.Find(TEXT("AnimSeqs")))
				{
					FOKPropertyList::ReadStructArray(*Seqs, Names, Keys);
				}
				for (const FOKPropertyList& Key : Keys)
				{
					if (int32(Key.GetNumber(TEXT("SubTrack"), 0.0)) != 0)
					{
						continue;		// face subtracks
					}
					FOKCinemaGroup::FAnimKey& Anim = Group.Anims.AddDefaulted_GetRef();
					Anim.Start = float(Key.GetNumber(TEXT("StartTime"), 0.0));
					Anim.Sequence = Key.GetText(TEXT("AnimSeqName"));
					Anim.StartOffset = float(Key.GetNumber(TEXT("AnimStartOffset"), 0.0));
					Anim.EndOffset = float(Key.GetNumber(TEXT("AnimEndOffset"), 0.0));
					Anim.Rate = float(Key.GetNumber(TEXT("AnimPlayRate"), 1.0));
					const FOKProperty* Looping = Key.Find(TEXT("bLooping"));
					Anim.bLooping = Looping && Looping->Int != 0;
				}
			}
		}
		if (!bDirector)
		{
			Groups.Add(MoveTemp(Group));
		}
	}
	ReadLights(Package);
	return true;
}

void FOKCinemaData::ReadLights(FOKPackage& Package)
{
	// The level's PointLights (actor tags after a 28-byte native prefix, components after 4), where the level puts them.
	// A cinema's moves are in the same space as its level: Scorpion_Victory's are level positions (Attacker1 at
	// 2102, -17432, -4000), the point light 1.2 m above and in front of the winner.
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	const auto ReadAt = [&Package, &Exports](int32 Ref, int32 Prefix, FOKPropertyList& Out)
	{
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(Ref - 1) || !Package.ExportData(Exports[Ref - 1], Bytes) || Bytes.Num() < Prefix)
		{
			return false;
		}
		FOKByteReader Reader(TConstArrayView<uint8>(Bytes).Slice(Prefix, Bytes.Num() - Prefix));
		return Out.Read(Reader, Package.GetNames());
	};
	const auto LocationOf = [](const FOKPropertyList& Actor)
	{
		const FOKProperty* Location = Actor.Find(TEXT("Location"));
		FVector3f Value = FVector3f::ZeroVector;
		if (Location && Location->Raw.Num() >= 12)
		{
			FMemory::Memcpy(&Value, Location->Raw.GetData(), 12);
		}
		return FVector(Value);
	};
	for (const FOKPackageExport* Export : Package.FindExports(TEXT("PointLight")))
	{
		if (!InScene(*Export))
		{
			continue;
		}
		FOKPropertyList Actor, Component;
		if (!ReadAt(Export->Index, 28, Actor) || !ReadAt(Actor.GetObject(TEXT("LightComponent")), 4, Component))
		{
			continue;
		}
		FLight& Light = Lights.AddDefaulted_GetRef();
		Light.Offset = LocationOf(Actor);
		Light.Brightness = float(Component.GetNumber(TEXT("Brightness"), 1.0));
		Light.Radius = float(Component.GetNumber(TEXT("Radius"), 1024.0));
		if (const FOKProperty* Color = Component.Find(TEXT("LightColor")); Color && Color->Raw.Num() >= 4)
		{
			Light.Color = FColor(Color->Raw[2], Color->Raw[1], Color->Raw[0]);		// stored B, G, R, A
		}
	}
}

const FOKCinemaGroup* FOKCinemaData::FindGroup(const FString& GroupName) const
{
	return Groups.FindByPredicate([&GroupName](const FOKCinemaGroup& G) { return G.Name.Equals(GroupName, ESearchCase::IgnoreCase); });
}

bool FOKCinemaPlayer::Start(const FString& PackageName, AOKFighterActor* InAttacker, AOKFighterActor* InVictim, FString& OutError, bool bInHoldAtEnd)
{
	Stop();
	bHoldAtEnd = bInHoldAtEnd;
	// A cinema is a package of its own (Scorpion_Fatality1), or cooked into the attacker's (an X-ray, Scorpion_XRAY).
	TUniquePtr<FOKPackage> Opened = FOKPackage::OpenGamePackage(PackageName, OutError);
	if (!Opened && InAttacker && !InAttacker->GetPackageName().IsEmpty())
	{
		FString Unused;
		Opened = FOKPackage::OpenGamePackage(InAttacker->GetPackageName(), Unused);
	}
	if (!Opened)
	{
		return false;
	}
	Package = TSharedPtr<FOKPackage>(Opened.Release());
	Data = FOKCinemaData();
	if (!Data.Read(*Package, OutError, PackageName) || !InAttacker)
	{
		Package.Reset();
		return false;
	}
	// The scene's origin: the attacker on its floor, facing its way.
	const FVector Where = InAttacker->GetActorLocation();
	Scene = FTransform(FRotator(0.0, InAttacker->GetActorForwardVector().Rotation().Yaw, 0.0), FVector(Where.X, Where.Y, InAttacker->GetFloorZ()));
	Attacker = InAttacker;
	Victim = InVictim;
	FOKParticles::Get().AddPackage(Package->GetName());		// its effects (FX_Blood's) are cooked into it
	InAttacker->BeginCinema(Package);
	if (InVictim)
	{
		InVictim->BeginCinema(Package);
	}
	// The level's lights, placed from the scene. [inf] UE3's Brightness 1 taken as 5000 unitless.
	for (const FOKCinemaData::FLight& Light : Data.Lights)
	{
		APointLight* Spawned = InAttacker->GetWorld()->SpawnActor<APointLight>(Scene.TransformPosition(Light.Offset), FRotator::ZeroRotator);
		if (UPointLightComponent* Component = Spawned ? Cast<UPointLightComponent>(Spawned->GetLightComponent()) : nullptr)
		{
			Spawned->SetMobility(EComponentMobility::Movable);
			Component->SetIntensityUnits(ELightUnits::Unitless);
			Component->SetIntensity(Light.Brightness * 5000.0f);
			Component->SetLightColor(FLinearColor(Light.Color));
			Component->SetAttenuationRadius(Light.Radius);
			LightActors.Add(Spawned);
		}
	}
	// [design] A cinema with a set of its own (static groups: the victory's Netherrealm) is somewhere else in MKKE, a
	// streamed level far from the stage; here it plays on the stage, so the stage is hidden while it shows.
	const bool bOwnSet = Data.Groups.ContainsByPredicate([](const FOKCinemaGroup& Group) { return Group.Kind == FOKCinemaGroup::EKind::Static && Group.StaticMeshRef > 0; });
	for (TActorIterator<AActor> It(InAttacker->GetWorld()); It && bOwnSet; ++It)
	{
		if (It->ActorHasTag(TEXT("OKStage")) && !It->IsA<ACameraActor>() && !It->IsHidden())
		{
			It->SetActorHiddenInGame(true);
			HiddenStage.Add(*It);
		}
	}
	Time = 0.0f;
	bPlaying = true;
	UE_LOG(LogOKAsset, Log, TEXT("cinema %s: %.2f s, %d groups, %d camera cuts, %d lights%s"), *Data.Name, Data.Length, Data.Groups.Num(), Data.Cuts.Num(),
		Data.Lights.Num(), bOwnSet ? TEXT(", its own set (the stage is hidden)") : TEXT(""));
	return true;
}

void FOKCinemaPlayer::Stop()
{
	for (const TWeakObjectPtr<AOKFighterActor>& Bound : { Attacker, Victim })
	{
		if (AOKFighterActor* Actor = Bound.Get())
		{
			OKCharacterMesh::ShowRegions(Actor->GetMesh(), 0);
			Actor->EndCinema();
		}
	}
	for (const TPair<int32, TWeakObjectPtr<AOKCinemaProp>>& Prop : Props)
	{
		if (AOKCinemaProp* Actor = Prop.Value.Get())
		{
			Actor->Destroy();
		}
	}
	for (const TPair<int32, TWeakObjectPtr<AOKParticleActor>>& Emitter : Emitters)
	{
		if (AOKParticleActor* Actor = Emitter.Value.Get())
		{
			Actor->Finish();
		}
	}
	for (const TPair<int32, TWeakObjectPtr<AActor>>& Piece : SetPieces)
	{
		if (AActor* Actor = Piece.Value.Get())
		{
			Actor->Destroy();
		}
	}
	for (const TWeakObjectPtr<AActor>& Light : LightActors)
	{
		if (AActor* Actor = Light.Get())
		{
			Actor->Destroy();
		}
	}
	for (const TWeakObjectPtr<AActor>& Part : HiddenStage)
	{
		if (AActor* Actor = Part.Get())
		{
			Actor->SetActorHiddenInGame(false);
		}
	}
	HiddenStage.Reset();
	Props.Reset();
	Emitters.Reset();
	SetPieces.Reset();
	LightActors.Reset();
	PropSequences.Reset();
	Attacker.Reset();
	Victim.Reset();
	Package.Reset();
	bPlaying = false;
	bHolding = false;
}

void FOKCinemaPlayer::Step(UWorld* World, float Seconds)
{
	if (!bPlaying)
	{
		return;
	}
	const float Before = Time;
	Time += Seconds * FMath::Max(0.0f, Data.Slomo.Eval(Time, 1.0f));
	if (Time >= Data.Length)
	{
		UE_LOG(LogOKAsset, Log, TEXT("cinema %s ended%s"), *Data.Name, bHoldAtEnd ? TEXT(", holding its last frame") : TEXT(""));
		if (bHoldAtEnd)
		{
			bPlaying = false;
			bHolding = true;
		}
		else
		{
			Stop();
		}
		return;
	}
	Pose(Attacker.Get(), Data.FindGroup(TEXT("Attacker")));
	Pose(Victim.Get(), Data.FindGroup(TEXT("Victim")));
	StepProps(World);
	StepEmitters(Before);
	View(World);
}

const FOKAnimSequence* FOKCinemaPlayer::PropSequence(const FString& SequenceName)
{
	if (const TSharedPtr<FOKAnimSequence>* Found = PropSequences.Find(SequenceName))
	{
		return Found->Get();
	}
	TSharedPtr<FOKAnimSequence> Loaded = MakeShared<FOKAnimSequence>();
	FString Error;
	if (!Package || !Loaded->Find(*Package, SequenceName, Error))
	{
		UE_LOG(LogOKAsset, Warning, TEXT("cinema %s: prop sequence %s: %s"), *Data.Name, *SequenceName, *Error);
		Loaded.Reset();
	}
	PropSequences.Add(SequenceName, Loaded);
	return Loaded.Get();
}

void FOKCinemaPlayer::StepProps(UWorld* World)
{
	for (int32 i = 0; i < Data.Groups.Num(); ++i)
	{
		// Attacker and Victim are the fighters; Attacker1, Victim1-3 are more copies of their meshes (the cut pieces),
		// each showing some regions. Any other skeletal group with a mesh of its own (its factory's) is a prop, such
		// as a sword, whatever proxy mesh it was made with.
		const FOKCinemaGroup& Group = Data.Groups[i];
		const auto IsPieceOf = [&Group](const TCHAR* Fighter)
		{
			return Group.Name.StartsWith(Fighter, ESearchCase::IgnoreCase) && Group.Name.Len() > FCString::Strlen(Fighter)
				&& FChar::IsDigit(Group.Name[FCString::Strlen(Fighter)]);
		};
		const AOKFighterActor* Whole = IsPieceOf(TEXT("Victim")) ? Victim.Get() : IsPieceOf(TEXT("Attacker")) ? Attacker.Get() : nullptr;
		const bool bPiece = IsPieceOf(TEXT("Victim")) || IsPieceOf(TEXT("Attacker"));
		const bool bFighter = Group.Name.Equals(TEXT("Attacker"), ESearchCase::IgnoreCase) || Group.Name.Equals(TEXT("Victim"), ESearchCase::IgnoreCase);
		if (Group.Kind != FOKCinemaGroup::EKind::Actor || !World || bFighter || (bPiece ? !Whole : Group.MeshRef <= 0))
		{
			continue;
		}
		TWeakObjectPtr<AOKCinemaProp>* Slot = Props.Find(i);
		if (!Slot)
		{
			FString Error = TEXT("no mesh");
			USkeletalMesh* Mesh = Whole ? Cast<USkeletalMesh>(Whole->GetMesh()->GetSkinnedAsset()) : OKCharacterMesh::LoadExport(*Package, Group.MeshRef, Error);
			AOKCinemaProp* Spawned = Mesh ? World->SpawnActor<AOKCinemaProp>() : nullptr;
			if (Spawned)
			{
				Spawned->SetMesh(Mesh);
			}
			else
			{
				UE_LOG(LogOKAsset, Warning, TEXT("cinema %s: prop %s: %s"), *Data.Name, *Group.Name, *Error);
			}
			Slot = &Props.Add(i, Spawned);
		}
		AOKCinemaProp* Prop = Slot->Get();
		if (!Prop)
		{
			continue;
		}
		Prop->SetActorTransform(Group.MoveAt(Time) * Scene);
		Prop->SetActorHiddenInGame(Group.IsHiddenAt(Time));
		FString SequenceName;
		float Seconds = 0.0f;
		const FOKAnimSequence* Sequence = Group.AnimAt(Time, SequenceName, Seconds) ? PropSequence(SequenceName) : nullptr;
		if (Sequence && Sequence->NumFrames > 0)
		{
			Prop->Pose(*Sequence, FMath::Clamp(Seconds * Sequence->FramesPerSecond(), 0.0f, float(Sequence->NumFrames - 1)));
		}
		OKCharacterMesh::ShowRegions(Prop->GetMesh(), Group.HiddenRegionsAt(Time));
	}

	// Set pieces: static groups (ActorFactoryMover), moved and hidden by their tracks.
	for (int32 i = 0; i < Data.Groups.Num() && World; ++i)
	{
		const FOKCinemaGroup& Group = Data.Groups[i];
		if (Group.Kind != FOKCinemaGroup::EKind::Static || Group.StaticMeshRef <= 0)
		{
			continue;
		}
		TWeakObjectPtr<AActor>* Slot = SetPieces.Find(i);
		if (!Slot)
		{
			FString Error;
			UStaticMesh* Mesh = OKStage::LoadMesh(*Package, Group.StaticMeshRef, Error);
			AStaticMeshActor* Spawned = Mesh ? World->SpawnActor<AStaticMeshActor>() : nullptr;
			if (Spawned)
			{
				Spawned->SetMobility(EComponentMobility::Movable);
				Spawned->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Spawned->GetStaticMeshComponent()->SetCastShadow(false);		// [design] MKKE lights its scenes with their own lights; without them, the fight stage's sun lights the fighters
				Spawned->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			}
			else
			{
				UE_LOG(LogOKAsset, Warning, TEXT("cinema %s: set piece %s: %s"), *Data.Name, *Group.Name, *Error);
			}
			Slot = &SetPieces.Add(i, Spawned);
		}
		if (AActor* Piece = Slot->Get())
		{
			Piece->SetActorTransform(Group.MoveAt(Time) * Scene);
			Piece->SetActorHiddenInGame(Group.IsHiddenAt(Time));
		}
	}
}

bool FOKCinemaPlayer::GroupBone(const FString& GroupName, const FString& Bone, FTransform& Out) const
{
	const AOKFighterActor* Fighter = GroupName.Equals(TEXT("Attacker"), ESearchCase::IgnoreCase) ? Attacker.Get()
		: GroupName.Equals(TEXT("Victim"), ESearchCase::IgnoreCase) ? Victim.Get() : nullptr;
	if (Fighter)
	{
		const FName BoneName(*Bone);
		const bool bHasBone = !Bone.IsEmpty() && Fighter->GetMesh()->GetBoneIndex(BoneName) != INDEX_NONE;
		Out = bHasBone ? Fighter->GetMesh()->GetBoneTransformByName(BoneName, EBoneSpaces::WorldSpace) : Fighter->GetActorTransform();
		Out.SetScale3D(FVector::OneVector);
		return true;
	}
	return false;
}

void FOKCinemaPlayer::StepEmitters(float Before)
{
	for (int32 i = 0; i < Data.Groups.Num(); ++i)
	{
		const FOKCinemaGroup& Group = Data.Groups[i];
		if (Group.Kind != FOKCinemaGroup::EKind::Emitter || Group.ParticleSystem.IsEmpty())
		{
			continue;
		}
		// Toggles passed this frame: on (and trigger) start the system again, off stops it spawning.
		TWeakObjectPtr<AOKParticleActor>& Slot = Emitters.FindOrAdd(i);
		for (const TPair<float, uint8>& Toggle : Group.Toggles)
		{
			if (Toggle.Key < Before || Toggle.Key >= Time || (Toggle.Key == 0.0f && Before > 0.0f))
			{
				continue;
			}
			const bool bOn = Toggle.Value == 1 || Toggle.Value == 3 || (Toggle.Value == 2 && !Slot.IsValid());
			if (Slot.IsValid())
			{
				Slot->Finish();
				Slot.Reset();
			}
			if (bOn)
			{
				Slot = FOKParticles::Get().Spawn(Group.ParticleSystem, Scene.GetLocation(), Scene.Rotator());
			}
		}
		AOKParticleActor* Emitter = Slot.Get();
		if (!Emitter)
		{
			continue;
		}
		FTransform Where = Group.MoveAt(Time) * Scene;
		FTransform Parent;
		const FOKCinemaGroup::FAttachment* Attach = Group.AttachmentAt(Time);
		if (Attach && GroupBone(Attach->Group, Attach->Bone, Parent))
		{
			Where = Attach->Relative * Parent;
		}
		Emitter->SetActorLocationAndRotation(Where.GetLocation(), Where.GetRotation());
		for (const FOKCinemaGroup::FParameter& Parameter : Group.Parameters)
		{
			Emitter->SetVectorParameter(Parameter.Name, FVector(Parameter.R.Eval(Time), Parameter.G.Eval(Time), Parameter.B.Eval(Time)));
		}
	}
}

void FOKCinemaPlayer::Pose(AOKFighterActor* Actor, const FOKCinemaGroup* Group)
{
	FString Sequence;
	float Seconds = 0.0f;
	if (Actor && Group && Group->AnimAt(Time, Sequence, Seconds))
	{
		Actor->CinemaPose(Sequence, Seconds, Group->MoveAt(Time) * Scene);
	}
	if (Actor && Group)
	{
		OKCharacterMesh::ShowRegions(Actor->GetMesh(), Group->HiddenRegionsAt(Time));
	}
}

void FOKCinemaPlayer::View(UWorld* World)
{
	const TPair<float, FString>* Cut = nullptr;
	for (const TPair<float, FString>& Each : Data.Cuts)
	{
		Cut = Each.Key <= Time ? &Each : Cut;
	}
	const FOKCinemaGroup* Camera = Cut ? Data.FindGroup(Cut->Value) : nullptr;
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	ACameraActor* ViewActor = Controller ? Cast<ACameraActor>(Controller->GetViewTarget()) : nullptr;
	if (!Camera || !ViewActor)
	{
		return;
	}
	const FTransform Eye = Camera->MoveAt(Time) * Scene;
	ViewActor->SetActorLocationAndRotation(Eye.GetLocation(), Eye.GetRotation());
	ViewActor->GetCameraComponent()->SetFieldOfView(FieldOfView(Camera->FocalLength.Eval(Time, 35.0f)));
}
