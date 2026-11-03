#include "OKParticles.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "OKTexture.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#if WITH_EDITOR
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#endif

static TAutoConsoleVariable<int32> CVarTraceParticles(TEXT("ok.Particles.Trace"), 0,
	TEXT("1: log each particle system as it's read and started"));

namespace
{
	const FName TextureParameter(TEXT("Texture"));
	const FName UseTextureColorParameter(TEXT("UseTextureColor"));
	const FName AlphaFromTextureParameter(TEXT("AlphaFromTexture"));
	const FName TintParameter(TEXT("Tint"));

	/**
	 * The two materials every particle material becomes: unlit, translucent or additive. Colour = vertex colour x the
	 * texture (or white, for a texture that only gives a shape) x Tint x UV1.x (the part of a colour above 1);
	 * opacity = vertex alpha x the texture's alpha, or its brightness when it has none. Built in code, like the
	 * character material; outside the editor it falls back to the engine's default material.
	 */
	UMaterialInterface* BaseMaterial(bool bAdditive)
	{
		static TStrongObjectPtr<UMaterialInterface> Cached[2];
		TStrongObjectPtr<UMaterialInterface>& Slot = Cached[bAdditive ? 1 : 0];
		if (Slot.IsValid())
		{
			return Slot.Get();
		}
#if WITH_EDITOR
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), bAdditive ? TEXT("M_OKParticleAdditive") : TEXT("M_OKParticle"), RF_Transient);
		Material->BlendMode = bAdditive ? BLEND_Additive : BLEND_Translucent;
		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		auto Add = [Material](auto* Expression)
		{
			Material->GetExpressionCollection().AddExpression(Expression);
			return Expression;
		};
		auto Multiply = [&](UMaterialExpression* A, int32 OutA, UMaterialExpression* B, int32 OutB)
		{
			UMaterialExpressionMultiply* Product = Add(NewObject<UMaterialExpressionMultiply>(Material));
			Product->A.Connect(OutA, A);
			Product->B.Connect(OutB, B);
			return Product;
		};

		UMaterialExpressionTextureSampleParameter2D* Texture = Add(NewObject<UMaterialExpressionTextureSampleParameter2D>(Material));
		Texture->ParameterName = TextureParameter;
		Texture->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		Texture->SamplerType = SAMPLERTYPE_Color;
		UMaterialExpressionVertexColor* Vertex = Add(NewObject<UMaterialExpressionVertexColor>(Material));

		UMaterialExpressionConstant3Vector* White = Add(NewObject<UMaterialExpressionConstant3Vector>(Material));
		White->Constant = FLinearColor::White;
		UMaterialExpressionScalarParameter* UseColor = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
		UseColor->ParameterName = UseTextureColorParameter;
		UseColor->DefaultValue = 1.0f;
		UMaterialExpressionLinearInterpolate* TextureColor = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
		TextureColor->A.Connect(0, White);
		TextureColor->B.Connect(0, Texture);
		TextureColor->Alpha.Connect(0, UseColor);

		UMaterialExpressionVectorParameter* Tint = Add(NewObject<UMaterialExpressionVectorParameter>(Material));
		Tint->ParameterName = TintParameter;
		Tint->DefaultValue = FLinearColor::White;
		UMaterialExpressionComponentMask* TintRGB = Add(NewObject<UMaterialExpressionComponentMask>(Material));
		TintRGB->Input.Connect(0, Tint);
		TintRGB->R = TintRGB->G = TintRGB->B = 1;
		TintRGB->A = 0;
		UMaterialExpressionTextureCoordinate* Extra = Add(NewObject<UMaterialExpressionTextureCoordinate>(Material));
		Extra->CoordinateIndex = 1;
		UMaterialExpressionComponentMask* Intensity = Add(NewObject<UMaterialExpressionComponentMask>(Material));
		Intensity->Input.Connect(0, Extra);
		Intensity->R = 1;
		Intensity->G = Intensity->B = Intensity->A = 0;
		UMaterialExpression* Color = Multiply(Multiply(Multiply(Vertex, 0, TextureColor, 0), 0, TintRGB, 0), 0, Intensity, 0);

		UMaterialExpressionConstant3Vector* Third = Add(NewObject<UMaterialExpressionConstant3Vector>(Material));
		Third->Constant = FLinearColor(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
		UMaterialExpressionDotProduct* Brightness = Add(NewObject<UMaterialExpressionDotProduct>(Material));
		Brightness->A.Connect(0, Texture);
		Brightness->B.Connect(0, Third);
		UMaterialExpressionScalarParameter* FromAlpha = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
		FromAlpha->ParameterName = AlphaFromTextureParameter;
		FromAlpha->DefaultValue = 1.0f;
		UMaterialExpressionLinearInterpolate* TextureAlpha = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
		TextureAlpha->A.Connect(0, Brightness);
		TextureAlpha->B.Connect(4, Texture);
		TextureAlpha->Alpha.Connect(0, FromAlpha);
		UMaterialExpression* Opacity = Multiply(Vertex, 4, TextureAlpha, 0);

		if (bAdditive)
		{
			Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Multiply(Color, 0, Opacity, 0));
		}
		else
		{
			Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Color);
			Material->GetEditorOnlyData()->Opacity.Connect(0, Opacity);
		}
		Material->PostEditChange();		// compiles the shaders
		Slot.Reset(Material);
#else
		Slot.Reset(UMaterial::GetDefaultMaterial(MD_Surface));
#endif
		return Slot.Get();
	}

	/** A Bool property; Fallback if it's missing. */
	bool Flag(const FOKPropertyList& Properties, const FString& Name, bool Fallback)
	{
		const FOKProperty* Property = Properties.Find(Name);
		return Property && Property->Kind == EOKPropKind::Bool ? Property->Int != 0 : Fallback;
	}

	/** An enum Byte property, stored as a number or as the value's name (matched by its ending, e.g. "_Linear"). */
	int32 Enum(const FOKPropertyList& Properties, const FString& Name, std::initializer_list<const TCHAR*> Values, int32 Fallback)
	{
		const FOKProperty* Property = Properties.Find(Name);
		if (!Property || Property->Kind != EOKPropKind::Byte)
		{
			return Fallback;
		}
		if (Property->Text.IsEmpty())
		{
			return Property->Int;
		}
		int32 Index = 0;
		for (const TCHAR* Value : Values)
		{
			if (Property->Text.EndsWith(Value, ESearchCase::IgnoreCase))
			{
				return Index;
			}
			++Index;
		}
		return Fallback;
	}

	FLinearColor ToColor(const FVector& Rgb, float Alpha)
	{
		return FLinearColor(float(Rgb.X), float(Rgb.Y), float(Rgb.Z), Alpha);
	}

	bool ReadObject(FOKPackage& Package, int32 Ref, FOKPropertyList& Out)
	{
		const TArray<FOKPackageExport>& Exports = Package.GetExports();
		TArray<uint8> Bytes;
		if (!Exports.IsValidIndex(Ref - 1) || !Package.ExportData(Exports[Ref - 1], Bytes))
		{
			return false;
		}
		// Distribution subobjects store a 4-byte index before their tags (DistributionFloatConstant_0 and the like).
		FOKByteReader Reader(Bytes);
		if (Out.Read(Reader, Package.GetNames()))
		{
			return true;
		}
		Out = FOKPropertyList();
		FOKByteReader Skipped(TConstArrayView<uint8>(Bytes).Slice(FMath::Min(4, Bytes.Num()), FMath::Max(0, Bytes.Num() - 4)));
		return Out.Read(Skipped, Package.GetNames());
	}

	/** A Float, or a Vector struct (12 bytes), property as a vector; false if it's missing. */
	bool ReadValue(const FOKPropertyList& Properties, const FString& Name, FVector& Out)
	{
		const FOKProperty* Property = Properties.Find(Name);
		if (Property && Property->Kind == EOKPropKind::Float)
		{
			Out = FVector(Property->Float);
			return true;
		}
		if (Property && Property->Kind == EOKPropKind::Struct && Property->Raw.Num() >= 12)
		{
			float Parts[3];
			FMemory::Memcpy(Parts, Property->Raw.GetData(), sizeof(Parts));
			Out = FVector(Parts[0], Parts[1], Parts[2]);
			return true;
		}
		return false;
	}

	/** A random direction inside a unit sphere (or on it), with only the axes' halves the primitive allows. */
	FVector RandomInSphere(FRandomStream& Random, uint8 Axes, bool bSurface)
	{
		FVector Point;
		do
		{
			Point = FVector(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f));
		} while (Point.SizeSquared() > 1.0 || Point.IsNearlyZero());
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const bool bPositive = (Axes & (1 << Axis)) != 0;
			const bool bNegative = (Axes & (8 << Axis)) != 0;
			if (!bPositive && !bNegative)
			{
				Point[Axis] = 0.0;
			}
			else if (!bNegative)
			{
				Point[Axis] = FMath::Abs(Point[Axis]);
			}
			else if (!bPositive)
			{
				Point[Axis] = -FMath::Abs(Point[Axis]);
			}
		}
		return bSurface ? Point.GetSafeNormal() : Point;
	}

	/** A primitive's positive and negative axis flags (Positive_X ... Negative_Z, all on by default) as 6 bits. */
	uint8 PrimitiveAxes(const FOKPropertyList& Properties)
	{
		const TCHAR* const Names[6] = { TEXT("Positive_X"), TEXT("Positive_Y"), TEXT("Positive_Z"), TEXT("Negative_X"), TEXT("Negative_Y"), TEXT("Negative_Z") };
		uint8 Axes = 0;
		for (int32 i = 0; i < 6; ++i)
		{
			Axes |= Flag(Properties, Names[i], true) ? uint8(1 << i) : 0;
		}
		return Axes;
	}
}

// --- FOKDistribution ---------------------------------------------------------------------------------------------

bool FOKDistribution::Read(const FOKPropertyList& Properties, const FString& Name, int32 InDims, FOKPackage& Package)
{
	Dims = InDims;
	const FOKProperty* Property = Properties.Find(Name);
	if (!Property || Property->Kind != EOKPropKind::Struct)
	{
		return false;
	}
	FOKByteReader Reader(Property->Raw);
	FOKPropertyList Raw;
	if (!Raw.Read(Reader, Package.GetNames()))
	{
		return false;
	}
	StartTime = float(Raw.GetNumber(TEXT("LookupTableStartTime"), 0.0));
	InvTimeScale = float(Raw.GetNumber(TEXT("InvTimeScale"), 0.0));
	int32 Steps = FMath::Max(1, int32(Raw.GetNumber(TEXT("NumEntries"), 1.0)));
	TArray<float> Table;
	if (const FOKProperty* Lookup = Raw.Find(TEXT("LookupTable")))
	{
		FOKByteReader TableReader(Lookup->Raw);
		const int32 Count = TableReader.I32();
		for (int32 i = 0; i < Count && !TableReader.HasError(); ++i)
		{
			Table.Add(TableReader.F32());
		}
	}
	if (Table.Num() > 2)
	{
		Values = TArray<float>(Table.GetData() + 2, Table.Num() - 2);
		const int32 PerStep = Values.Num() / Steps;
		if (PerStep != Dims && PerStep != 2 * Dims)
		{
			Steps = FMath::Max(1, Values.Num() / Dims);
		}
		bUniform = Values.Num() / Steps == 2 * Dims;
		Entries = Steps;
	}

	// The distribution object: a particle parameter's name (the effect may set FX_Blood_Color), and, when the table is
	// empty, the value itself: a constant, a uniform's min and max, or a parameter's default.
	const int32 Source = Raw.GetObject(TEXT("Distribution"));
	FOKPropertyList Object;
	if (Source <= 0 || !Package.GetExports().IsValidIndex(Source - 1) || !ReadObject(Package, Source, Object))
	{
		return true;
	}
	const FString& Class = Package.GetExports()[Source - 1].ClassName;
	if (Class.Contains(TEXT("ParticleParameter")))
	{
		Parameter = FName(*Object.GetText(TEXT("ParameterName")));
	}
	if (Entries > 0 || Class.Contains(TEXT("Curve")))
	{
		return true;
	}
	FVector Low = FVector::ZeroVector;
	FVector High = FVector::ZeroVector;
	if (Class.Contains(TEXT("Uniform")) && (ReadValue(Object, TEXT("Min"), Low) | ReadValue(Object, TEXT("Max"), High)))
	{
		bUniform = true;
	}
	else if (!ReadValue(Object, TEXT("Constant"), Low))
	{
		return true;		// a constant left at its class default: the caller fills that in
	}
	Values.Reset();
	for (int32 d = 0; d < Dims; ++d)
	{
		Values.Add(float(Low[d]));
	}
	for (int32 d = 0; bUniform && d < Dims; ++d)
	{
		Values.Add(float(High[d]));
	}
	Entries = 1;
	return true;
}

FVector FOKDistribution::Get(float Time, FRandomStream& Random) const
{
	if (Entries <= 0)
	{
		return FVector::ZeroVector;
	}
	const float Position = Entries > 1 ? FMath::Clamp((Time - StartTime) * InvTimeScale, 0.0f, float(Entries - 1)) : 0.0f;
	const int32 Step = FMath::Min(int32(Position), Entries - 1);
	const int32 NextStep = FMath::Min(Step + 1, Entries - 1);
	const float Blend = Position - float(Step);
	const int32 Stride = Dims * (bUniform ? 2 : 1);
	FVector Out = FVector::ZeroVector;
	for (int32 d = 0; d < Dims && d < 3; ++d)
	{
		auto At = [this, Stride, d](int32 InStep, int32 Half) { return Values.IsValidIndex(InStep * Stride + Half * Dims + d) ? Values[InStep * Stride + Half * Dims + d] : 0.0f; };
		const float Low = FMath::Lerp(At(Step, 0), At(NextStep, 0), Blend);
		Out[d] = bUniform ? FMath::Lerp(Low, FMath::Lerp(At(Step, 1), At(NextStep, 1), Blend), Random.FRand()) : Low;
	}
	return Dims == 1 ? FVector(Out.X) : Out;
}

// --- AOKParticleActor --------------------------------------------------------------------------------------------

AOKParticleActor::AOKParticleActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;		// after the camera moved, so the quads face it
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Emitter"));
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Particles"));
	Mesh->SetupAttachment(RootComponent);
	// The quads are built in world space, so the mesh stays at the world's origin while the emitter moves.
	Mesh->SetUsingAbsoluteLocation(true);
	Mesh->SetUsingAbsoluteRotation(true);
	Mesh->SetUsingAbsoluteScale(true);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
}

void AOKParticleActor::Init(TSharedPtr<const FOKParticleSystem> InSystem)
{
	System = InSystem;
	Random.Initialize(int32(GetUniqueID()));
	States.SetNum(System ? System->Emitters.Num() : 0);
	for (int32 i = 0; i < States.Num(); ++i)
	{
		const FOKParticleEmitter& Emitter = System->Emitters[i];
		States[i].Duration = Emitter.bDurationRange ? Random.FRandRange(Emitter.DurationLow, Emitter.Duration) : Emitter.Duration;
		States[i].bDone = Emitter.bSkipped;
	}
}

void AOKParticleActor::Finish()
{
	bStopped = true;
	bFinishing = true;
}

bool AOKParticleActor::IsComplete() const
{
	for (const FEmitterState& State : States)
	{
		if (State.Particles.Num() > 0 || (!State.bDone && !bStopped))
		{
			return false;
		}
	}
	return true;
}

FVector AOKParticleActor::Evaluate(const FOKDistribution& Distribution, float Time)
{
	if (!Distribution.Parameter.IsNone())
	{
		if (const FVector* Value = Parameters.Find(Distribution.Parameter))
		{
			return *Value;
		}
	}
	return Distribution.Get(Time, Random);
}

void AOKParticleActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!System)
	{
		return;
	}
	for (int32 i = 0; i < States.Num(); ++i)
	{
		TickEmitter(System->Emitters[i], States[i], DeltaSeconds);
	}
	BuildMesh();
	if (bFinishing && IsComplete())
	{
		Destroy();
	}
}

void AOKParticleActor::TickEmitter(const FOKParticleEmitter& Emitter, FEmitterState& State, float DeltaSeconds)
{
	if (Emitter.bSkipped)
	{
		return;
	}
	Update(Emitter, State, DeltaSeconds);
	if (State.bDone || bStopped)
	{
		return;
	}

	// The emitter's clock (ParticleEmitterInstance::Tick): a delay before the first loop, then loops of Duration.
	State.Time += DeltaSeconds;
	const float Delay = State.Loop == 0 ? Emitter.Delay : 0.0f;
	const float Time = State.Time - Delay;
	if (Time < 0.0f)
	{
		return;
	}
	const float Rate = Emitter.Rate.IsSet() ? FMath::Max(0.0f, float(Evaluate(Emitter.Rate, Time).X) * (Emitter.RateScale.IsSet() ? float(Evaluate(Emitter.RateScale, Time).X) : 1.0f)) : 0.0f;
	State.SpawnFraction += Rate * DeltaSeconds;
	const int32 FromRate = FMath::FloorToInt32(State.SpawnFraction);
	State.SpawnFraction -= float(FromRate);
	int32 Count = FromRate;
	while (State.NextBurst < Emitter.Bursts.Num() && Time >= Emitter.Bursts[State.NextBurst].Time * State.Duration)
	{
		const FOKParticleEmitter::FBurst& Burst = Emitter.Bursts[State.NextBurst++];
		Count += Burst.CountLow >= 0 ? Random.RandRange(Burst.CountLow, Burst.Count) : Burst.Count;
	}
	if (Count > 0)
	{
		Spawn(Emitter, State, FMath::Min(Count, 500), Time);
	}
	if (State.Duration > 0.0f && Time >= State.Duration)
	{
		++State.Loop;
		if (Emitter.Loops > 0 && State.Loop >= Emitter.Loops)
		{
			State.bDone = true;
		}
		else
		{
			State.Time = Time - State.Duration;
			State.NextBurst = 0;
			State.Duration = Emitter.bDurationRange ? Random.FRandRange(Emitter.DurationLow, Emitter.Duration) : Emitter.Duration;
		}
	}
}

void AOKParticleActor::Spawn(const FOKParticleEmitter& Emitter, FEmitterState& State, int32 Count, float SpawnTime)
{
	using EType = FOKParticleModule::EType;
	const FTransform& Transform = GetActorTransform();
	const int32 Images = Emitter.SubImagesH * Emitter.SubImagesV;
	for (int32 n = 0; n < Count; ++n)
	{
		FParticle P;
		FVector Offset = FVector::ZeroVector;		// in the emitter's frame
		FVector Velocity = FVector::ZeroVector;		// in the emitter's frame
		FVector WorldVelocity = FVector::ZeroVector;
		for (const FOKParticleModule& Module : Emitter.Modules)
		{
			switch (Module.Type)
			{
			case EType::Lifetime:
				P.Lifetime = FMath::Max(float(Evaluate(Module.A, SpawnTime).X), 0.001f);
				break;
			case EType::Size:
				P.BaseSize += Evaluate(Module.A, SpawnTime);
				break;
			case EType::Velocity:
			{
				const FVector Start = Evaluate(Module.A, SpawnTime);
				(Module.bFlagA ? WorldVelocity : Velocity) += Start;
				if (Module.B.IsSet())
				{
					Velocity += Offset.GetSafeNormal() * Evaluate(Module.B, SpawnTime).X;
				}
				break;
			}
			case EType::Color:
			{
				P.BaseColor = ToColor(Evaluate(Module.A, SpawnTime), Module.B.IsSet() ? float(Evaluate(Module.B, SpawnTime).X) : 1.0f);
				break;
			}
			case EType::ColorOverLife:
			{
				P.BaseColor = ToColor(Evaluate(Module.A, 0.0f), Module.B.IsSet() ? float(Evaluate(Module.B, 0.0f).X) : 1.0f);
				break;
			}
			case EType::Rotation:
				P.Rotation += UE_TWO_PI * float(Evaluate(Module.A, SpawnTime).X);
				break;
			case EType::RotationRate:
				P.BaseRotationRate += UE_TWO_PI * float(Evaluate(Module.A, SpawnTime).X);
				break;
			case EType::Location:
				Offset += Evaluate(Module.A, SpawnTime);
				break;
			case EType::Sphere:
			{
				const FVector Direction = RandomInSphere(Random, Module.Axes, Module.bFlagA);
				Offset += Evaluate(Module.B, SpawnTime) + Direction * Evaluate(Module.A, SpawnTime).X;
				if (Module.bFlagB)
				{
					Velocity += Direction * Evaluate(Module.C, SpawnTime).X;
				}
				break;
			}
			case EType::Cylinder:
			{
				const int32 Up = FMath::Clamp(Module.HeightAxis, 0, 2);
				FVector Direction = RandomInSphere(Random, Module.Axes, false);
				Direction[Up] = 0.0;
				Direction = Direction.GetSafeNormal() * (Module.bFlagA ? 1.0f : FMath::Sqrt(Random.FRand()));
				FVector Point = Direction * Evaluate(Module.A, SpawnTime).X;
				Point[Up] = Random.FRandRange(-0.5f, 0.5f) * Evaluate(Module.B, SpawnTime).X;
				Offset += Point;
				if (Module.bFlagB)
				{
					FVector Radial = Direction;
					Radial[Up] = 0.0;
					Velocity += Radial.GetSafeNormal() * Evaluate(Module.C, SpawnTime).X;
				}
				break;
			}
			default:
				break;
			}
		}
		if (Emitter.Interpolation == 3 || Emitter.Interpolation == 4)
		{
			P.Image = float(Random.RandRange(0, FMath::Max(Images - 1, 0)));
		}
		P.Color = P.BaseColor;
		P.Size = P.BaseSize;
		P.RotationRate = P.BaseRotationRate;
		if (Emitter.bLocalSpace)
		{
			P.Location = Offset * EffectScale;
			P.BaseVelocity = Velocity * EffectScale + Transform.InverseTransformVectorNoScale(WorldVelocity);
		}
		else
		{
			P.Location = Transform.TransformPositionNoScale(Offset * EffectScale);
			P.BaseVelocity = Transform.TransformVectorNoScale(Velocity * EffectScale) + WorldVelocity;
		}
		P.Velocity = P.BaseVelocity;
		State.Particles.Add(P);
	}
}

void AOKParticleActor::Update(const FOKParticleEmitter& Emitter, FEmitterState& State, float DeltaSeconds)
{
	using EType = FOKParticleModule::EType;
	const int32 Images = Emitter.SubImagesH * Emitter.SubImagesV;
	for (int32 i = State.Particles.Num() - 1; i >= 0; --i)
	{
		FParticle& P = State.Particles[i];
		P.Age += DeltaSeconds;
		P.RelativeTime = P.Age / P.Lifetime;
		if (P.RelativeTime >= 1.0f)
		{
			State.Particles.RemoveAtSwap(i);
			continue;
		}
		// Each frame starts again from the base values (ResetParticleParameters); the modules then change them.
		P.Velocity = P.BaseVelocity;
		P.Size = P.BaseSize;
		P.RotationRate = P.BaseRotationRate;
		P.Color = P.BaseColor;
		const float T = P.RelativeTime;
		bool bKill = false;
		for (const FOKParticleModule& Module : Emitter.Modules)
		{
			switch (Module.Type)
			{
			case EType::SizeMultiplyLife:
			{
				const FVector Scale = Evaluate(Module.A, T);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					P.Size[Axis] *= (Module.Axes & (1 << Axis)) ? Scale[Axis] : 1.0;
				}
				break;
			}
			case EType::SizeScale:
				P.Size = P.BaseSize * Evaluate(Module.A, T);
				break;
			case EType::VelocityOverLife:
				P.Velocity = Module.bFlagA ? Evaluate(Module.A, T) * EffectScale : P.Velocity * Evaluate(Module.A, T);
				break;
			case EType::Acceleration:
			case EType::AccelerationOverLife:
			{
				const FVector Change = Evaluate(Module.A, T) * EffectScale * DeltaSeconds;
				P.Velocity += Change;
				P.BaseVelocity += Change;
				break;
			}
			case EType::ColorOverLife:
			{
				P.Color = ToColor(Evaluate(Module.A, T), Module.B.IsSet() ? float(Evaluate(Module.B, T).X) : P.Color.A);
				break;
			}
			case EType::ColorScaleOverLife:
			{
				const FVector Rgb = Module.A.IsSet() ? Evaluate(Module.A, T) : FVector::OneVector;
				P.Color = ToColor(FVector(P.Color.R, P.Color.G, P.Color.B) * Rgb, P.Color.A * (Module.B.IsSet() ? float(Evaluate(Module.B, T).X) : 1.0f));
				break;
			}
			case EType::RotationRateMultiplyLife:
				P.RotationRate *= float(Evaluate(Module.A, T).X);
				break;
			case EType::SubUV:
				if (Emitter.Interpolation == 1 || Emitter.Interpolation == 2 || Emitter.Interpolation == 0)
				{
					P.Image = float(Evaluate(Module.A, T).X);
				}
				break;
			case EType::KillHeight:
			{
				const double Height = Evaluate(Module.A, T).X;
				const double Z = Emitter.bLocalSpace ? GetActorTransform().TransformPosition(P.Location).Z : P.Location.Z;
				bKill |= Module.bFlagA ? Z < Height : Z > Height;
				break;
			}
			default:
				break;
			}
		}
		if (bKill)
		{
			State.Particles.RemoveAtSwap(i);
			continue;
		}
		if ((Emitter.Interpolation == 3 || Emitter.Interpolation == 4) && Images > 1 && Emitter.RandomImageTime > 0.0f && T - P.ImageChanged >= Emitter.RandomImageTime)
		{
			P.Image = float(Random.RandRange(0, Images - 1));
			P.ImageChanged = T;
		}
		P.Location += P.Velocity * DeltaSeconds;
		P.Rotation += P.RotationRate * DeltaSeconds;
	}
}

void AOKParticleActor::BuildMesh()
{
	const APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* Camera = Player ? Player->PlayerCameraManager : nullptr;
	const FQuat View = Camera ? Camera->GetCameraRotation().Quaternion() : FQuat::Identity;
	const FVector Forward = View.GetForwardVector();
	const FVector Right = View.GetRightVector();
	const FVector Up = View.GetUpVector();
	const FTransform& Transform = GetActorTransform();

	for (int32 e = 0; e < States.Num(); ++e)
	{
		const FOKParticleEmitter& Emitter = System->Emitters[e];
		const TArray<FParticle>& Particles = States[e].Particles;
		if (Particles.Num() == 0)
		{
			if (Mesh->GetNumSections() > e)
			{
				Mesh->ClearMeshSection(e);
			}
			continue;
		}
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<FLinearColor> Colors;
		Vertices.Reserve(Particles.Num() * 4);
		Triangles.Reserve(Particles.Num() * 6);
		const int32 Columns = FMath::Max(1, Emitter.SubImagesH);
		const int32 Rows = FMath::Max(1, Emitter.SubImagesV);
		for (const FParticle& P : Particles)
		{
			const FVector Center = Emitter.bLocalSpace ? Transform.TransformPositionNoScale(P.Location) : P.Location;
			const float Width = float(P.Size.X) * EffectScale;
			const float Height = (Emitter.ScreenAlignment == 0 ? float(P.Size.X) : float(P.Size.Y)) * EffectScale;
			FVector Across = Right;
			FVector Along = Up;
			if (Emitter.ScreenAlignment == 2 && !P.Velocity.IsNearlyZero())
			{
				// Velocity-aligned: the quad's long side follows the motion, turned to face the camera.
				Along = (P.Velocity - Forward * FVector::DotProduct(P.Velocity, Forward)).GetSafeNormal();
				Across = FVector::CrossProduct(Along, Forward).GetSafeNormal();
			}
			else
			{
				const float Sin = FMath::Sin(P.Rotation);
				const float Cos = FMath::Cos(P.Rotation);
				Across = Right * Cos + Up * Sin;
				Along = Up * Cos - Right * Sin;
			}
			const FVector HalfX = Across * (Width * 0.5f);
			const FVector HalfY = Along * (Height * 0.5f);
			const int32 Base = Vertices.Num();
			Vertices.Add(Center - HalfX + HalfY);
			Vertices.Add(Center + HalfX + HalfY);
			Vertices.Add(Center + HalfX - HalfY);
			Vertices.Add(Center - HalfX - HalfY);
			Triangles.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });

			const int32 Image = FMath::Clamp(FMath::FloorToInt32(P.Image), 0, Columns * Rows - 1);
			const float U = float(Image % Columns) / float(Columns);
			const float V = float(Image / Columns) / float(Rows);
			const float Du = 1.0f / float(Columns);
			const float Dv = 1.0f / float(Rows);
			UV0.Append({ FVector2D(U, V), FVector2D(U + Du, V), FVector2D(U + Du, V + Dv), FVector2D(U, V + Dv) });

			// Colours above 1 (Cascade allows them) go in UV1.x so the material can brighten past white.
			const float Peak = FMath::Max3(P.Color.R, P.Color.G, P.Color.B);
			const float Intensity = FMath::Max(1.0f, Peak);
			const FLinearColor Stored(P.Color.R / Intensity, P.Color.G / Intensity, P.Color.B / Intensity, FMath::Clamp(P.Color.A, 0.0f, 1.0f));
			for (int32 k = 0; k < 4; ++k)
			{
				Colors.Add(Stored);
				UV1.Add(FVector2D(Intensity, 0.0f));
			}
		}
		Mesh->CreateMeshSection_LinearColor(e, Vertices, Triangles, {}, UV0, UV1, {}, {}, Colors, {}, false);
		if (UMaterialInterface* Material = Emitter.Material.Material.Get())
		{
			Mesh->SetMaterial(e, Material);
		}
	}
}

// --- FOKParticles ------------------------------------------------------------------------------------------------

FOKParticles::FOKParticles() = default;
FOKParticles::~FOKParticles() = default;

bool FOKParticles::IsTracing()
{
	return CVarTraceParticles.GetValueOnGameThread() != 0;
}

FOKParticles& FOKParticles::Get()
{
	static FOKParticles Instance;
	return Instance;
}

void FOKParticles::AddPackage(const FString& PackageName)
{
	PackageNames.AddUnique(PackageName);
}

FOKPackage* FOKParticles::OpenPackage(const FString& PackageName)
{
	if (TUniquePtr<FOKPackage>* Found = Packages.Find(PackageName))
	{
		return Found->Get();
	}
	FString Error;
	TUniquePtr<FOKPackage> Opened = FOKPackage::OpenGamePackage(PackageName, Error);
	if (!Opened)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("particles: can't open %s: %s"), *PackageName, *Error);
	}
	return Packages.Add(PackageName, MoveTemp(Opened)).Get();
}

AOKParticleActor* FOKParticles::Spawn(const FString& SystemName, const FVector& Location, const FRotator& Rotation)
{
	UWorld* Into = World.Get();
	TSharedPtr<const FOKParticleSystem> System = Into ? Find(SystemName) : nullptr;
	if (!System)
	{
		return nullptr;
	}
	FActorSpawnParameters Parameters;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AOKParticleActor* Actor = Into->SpawnActor<AOKParticleActor>(Location, Rotation, Parameters);
	if (Actor)
	{
		Actor->Init(System);
	}
	if (CVarTraceParticles.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogOKAsset, Log, TEXT("particles: %s at %s"), *SystemName, *Location.ToCompactString());
	}
	return Actor;
}

TSharedPtr<const FOKParticleSystem> FOKParticles::Find(const FString& SystemName)
{
	const FString Key = SystemName.ToLower();
	if (const TSharedPtr<const FOKParticleSystem>* Found = Systems.Find(Key))
	{
		return *Found;
	}
	TSharedPtr<const FOKParticleSystem> System;
	TArray<FString> Search = { TEXT("Startup") };
	Search.Append(PackageNames);
	for (const FString& PackageName : Search)
	{
		FOKPackage* Package = OpenPackage(PackageName);
		if (Package && (System = Read(*Package, SystemName)))
		{
			break;
		}
	}
	if (!System)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("particles: no ParticleSystem %s in Startup or the fighters' packages"), *SystemName);
	}
	Systems.Add(Key, System);
	return System;
}

TSharedPtr<FOKParticleSystem> FOKParticles::Read(FOKPackage& Package, const FString& SystemName)
{
	const FOKPackageExport* Export = nullptr;
	for (const FOKPackageExport* Candidate : Package.FindExports(TEXT("ParticleSystem")))
	{
		if (Candidate->Name.TrimEnd().Equals(SystemName, ESearchCase::IgnoreCase))
		{
			Export = Candidate;
		}
	}
	FOKPropertyList Properties;
	if (!Export || !ReadObject(Package, Export->Index, Properties))
	{
		return nullptr;
	}
	TSharedPtr<FOKParticleSystem> System = MakeShared<FOKParticleSystem>();
	System->Name = SystemName;
	const FOKProperty* Emitters = Properties.Find(TEXT("Emitters"));
	for (const int32 EmitterRef : Emitters ? FOKPropertyList::ReadIntArray(*Emitters) : TArray<int32>())
	{
		FOKParticleEmitter& Emitter = System->Emitters.AddDefaulted_GetRef();
		if (!ReadEmitter(Package, EmitterRef, Emitter))
		{
			Emitter.bSkipped = true;
		}
	}
	if (CVarTraceParticles.GetValueOnGameThread() != 0)
	{
		for (const FOKParticleEmitter& Emitter : System->Emitters)
		{
			UE_LOG(LogOKAsset, Log, TEXT("particles: %s.%s: %s%d modules, %d bursts, duration %g x %d, %dx%d images, %s"), *SystemName, *Emitter.Name,
				Emitter.bSkipped ? TEXT("skipped, ") : TEXT(""), Emitter.Modules.Num(), Emitter.Bursts.Num(), Emitter.Duration, Emitter.Loops,
				Emitter.SubImagesH, Emitter.SubImagesV, Emitter.Material.bAdditive ? TEXT("additive") : TEXT("translucent"));
		}
	}
	return System;
}

bool FOKParticles::ReadEmitter(FOKPackage& Package, int32 EmitterRef, FOKParticleEmitter& Out)
{
	using EType = FOKParticleModule::EType;
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	FOKPropertyList Properties;
	if (!Exports.IsValidIndex(EmitterRef - 1) || !ReadObject(Package, EmitterRef, Properties))
	{
		return false;
	}
	Out.Name = Properties.GetText(TEXT("EmitterName"));
	if (Exports[EmitterRef - 1].ClassName != TEXT("ParticleSpriteEmitter"))
	{
		return false;
	}
	const FOKProperty* Levels = Properties.Find(TEXT("LODLevels"));
	const TArray<int32> LevelRefs = Levels ? FOKPropertyList::ReadIntArray(*Levels) : TArray<int32>();
	FOKPropertyList Level;
	if (LevelRefs.Num() == 0 || !ReadObject(Package, LevelRefs[0], Level) || !Flag(Level, TEXT("bEnabled"), true))
	{
		return false;
	}
	if (Level.GetObject(TEXT("TypeDataModule")) != 0)
	{
		return false;		// mesh, beam and ribbon emitters
	}

	int32 MaterialRef = Properties.GetObject(TEXT("Material"));
	FOKPropertyList Required;
	if (ReadObject(Package, Level.GetObject(TEXT("RequiredModule")), Required))
	{
		MaterialRef = Required.GetObject(TEXT("Material")) != 0 ? Required.GetObject(TEXT("Material")) : MaterialRef;
		Out.SubImagesH = FMath::Max(1, int32(Required.GetNumber(TEXT("SubImages_Horizontal"), 1.0)));
		Out.SubImagesV = FMath::Max(1, int32(Required.GetNumber(TEXT("SubImages_Vertical"), 1.0)));
		Out.Interpolation = Enum(Required, TEXT("InterpolationMethod"), { TEXT("_None"), TEXT("_Linear"), TEXT("_Linear_Blend"), TEXT("_Random"), TEXT("_Random_Blend") }, 0);
		Out.RandomImageTime = float(Required.GetNumber(TEXT("RandomImageTime"), 1.0));
		Out.ScreenAlignment = Enum(Required, TEXT("ScreenAlignment"), { TEXT("_Square"), TEXT("_Rectangle"), TEXT("_Velocity") }, 0);
		Out.Duration = float(Required.GetNumber(TEXT("EmitterDuration"), 1.0));
		Out.DurationLow = float(Required.GetNumber(TEXT("EmitterDurationLow"), 0.0));
		Out.bDurationRange = Flag(Required, TEXT("bEmitterDurationUseRange"), false);
		Out.Loops = int32(Required.GetNumber(TEXT("EmitterLoops"), 0.0));
		Out.Delay = float(Required.GetNumber(TEXT("EmitterDelay"), 0.0));
		Out.bLocalSpace = Flag(Required, TEXT("bUseLocalSpace"), false);
	}
	Out.Material = MakeMaterial(Package, MaterialRef);

	FOKPropertyList Spawn;
	if (ReadObject(Package, Level.GetObject(TEXT("SpawnModule")), Spawn))
	{
		Out.Rate.Read(Spawn, TEXT("Rate"), 1, Package);
		Out.RateScale.Read(Spawn, TEXT("RateScale"), 1, Package);
		TArray<FOKPropertyList> Bursts;
		if (const FOKProperty* List = Spawn.Find(TEXT("BurstList")))
		{
			FOKPropertyList::ReadStructArray(*List, Package.GetNames(), Bursts);
		}
		for (const FOKPropertyList& Burst : Bursts)
		{
			Out.Bursts.Add({ int32(Burst.GetNumber(TEXT("Count"), 0.0)), int32(Burst.GetNumber(TEXT("CountLow"), -1.0)), float(Burst.GetNumber(TEXT("Time"), 0.0)) });
		}
		Out.Bursts.Sort([](const FOKParticleEmitter::FBurst& A, const FOKParticleEmitter::FBurst& B) { return A.Time < B.Time; });
	}

	const FOKProperty* ModuleList = Level.Find(TEXT("Modules"));
	for (const int32 ModuleRef : ModuleList ? FOKPropertyList::ReadIntArray(*ModuleList) : TArray<int32>())
	{
		FOKPropertyList M;
		if (!Exports.IsValidIndex(ModuleRef - 1) || !ReadObject(Package, ModuleRef, M) || !Flag(M, TEXT("bEnabled"), true))
		{
			continue;
		}
		const FString Class = Exports[ModuleRef - 1].ClassName.Replace(TEXT("ParticleModule"), TEXT(""));
		FOKParticleModule Module;
		// Cooked tables are empty for constants left at the class default; those defaults are filled in here.
		auto Dist = [&](FOKDistribution& D, const TCHAR* Name, int32 Dims, float Default)
		{
			if (!D.Read(M, Name, Dims, Package) || !D.IsSet())
			{
				D.Dims = Dims;
				D.Entries = 1;
				D.bUniform = false;
				D.Values.Init(Default, Dims);
			}
		};
		if (Class == TEXT("Lifetime")) { Module.Type = EType::Lifetime; Dist(Module.A, TEXT("Lifetime"), 1, 1.0f); }
		else if (Class == TEXT("Size")) { Module.Type = EType::Size; Dist(Module.A, TEXT("StartSize"), 3, 1.0f); }
		else if (Class == TEXT("SizeMultiplyLife"))
		{
			Module.Type = EType::SizeMultiplyLife;
			Dist(Module.A, TEXT("LifeMultiplier"), 3, 1.0f);
			Module.Axes = (Flag(M, TEXT("MultiplyX"), true) ? 1 : 0) | (Flag(M, TEXT("MultiplyY"), true) ? 2 : 0) | (Flag(M, TEXT("MultiplyZ"), true) ? 4 : 0);
		}
		else if (Class == TEXT("SizeScale")) { Module.Type = EType::SizeScale; Dist(Module.A, TEXT("SizeScale"), 3, 1.0f); }
		else if (Class == TEXT("Velocity"))
		{
			Module.Type = EType::Velocity;
			Dist(Module.A, TEXT("StartVelocity"), 3, 0.0f);
			Module.B.Read(M, TEXT("StartVelocityRadial"), 1, Package);
			Module.bFlagA = Flag(M, TEXT("bInWorldSpace"), false);
		}
		else if (Class == TEXT("VelocityOverLifetime"))
		{
			Module.Type = EType::VelocityOverLife;
			Dist(Module.A, TEXT("VelOverLife"), 3, 1.0f);
			Module.bFlagA = Flag(M, TEXT("Absolute"), false);
		}
		else if (Class == TEXT("Acceleration")) { Module.Type = EType::Acceleration; Dist(Module.A, TEXT("Acceleration"), 3, 0.0f); }
		else if (Class == TEXT("AccelerationOverLifetime")) { Module.Type = EType::AccelerationOverLife; Dist(Module.A, TEXT("AccelOverLife"), 3, 0.0f); }
		else if (Class == TEXT("Color")) { Module.Type = EType::Color; Dist(Module.A, TEXT("StartColor"), 3, 1.0f); Dist(Module.B, TEXT("StartAlpha"), 1, 1.0f); }
		else if (Class == TEXT("ColorOverLife")) { Module.Type = EType::ColorOverLife; Dist(Module.A, TEXT("ColorOverLife"), 3, 1.0f); Dist(Module.B, TEXT("AlphaOverLife"), 1, 1.0f); }
		else if (Class == TEXT("ColorScaleOverLife")) { Module.Type = EType::ColorScaleOverLife; Dist(Module.A, TEXT("ColorScaleOverLife"), 3, 1.0f); Dist(Module.B, TEXT("AlphaScaleOverLife"), 1, 1.0f); }
		else if (Class == TEXT("Rotation")) { Module.Type = EType::Rotation; Dist(Module.A, TEXT("StartRotation"), 1, 0.0f); }
		else if (Class == TEXT("RotationRate")) { Module.Type = EType::RotationRate; Dist(Module.A, TEXT("StartRotationRate"), 1, 0.0f); }
		else if (Class == TEXT("RotationRateMultiplyLife")) { Module.Type = EType::RotationRateMultiplyLife; Dist(Module.A, TEXT("LifeMultiplier"), 1, 1.0f); }
		else if (Class == TEXT("Location")) { Module.Type = EType::Location; Dist(Module.A, TEXT("StartLocation"), 3, 0.0f); }
		else if (Class == TEXT("LocationPrimitiveSphere") || Class == TEXT("LocationPrimitiveCylinder"))
		{
			const bool bSphere = Class == TEXT("LocationPrimitiveSphere");
			Module.Type = bSphere ? EType::Sphere : EType::Cylinder;
			Dist(Module.A, TEXT("StartRadius"), 1, 50.0f);
			Dist(Module.C, TEXT("VelocityScale"), 1, 1.0f);
			if (bSphere)
			{
				Dist(Module.B, TEXT("StartLocation"), 3, 0.0f);
			}
			else
			{
				Dist(Module.B, TEXT("StartHeight"), 1, 50.0f);
				Module.HeightAxis = Enum(M, TEXT("HeightAxis"), { TEXT("_X"), TEXT("_Y"), TEXT("_Z") }, 2);
			}
			Module.bFlagA = Flag(M, TEXT("SurfaceOnly"), false);
			Module.bFlagB = Flag(M, TEXT("Velocity"), false);
			Module.Axes = PrimitiveAxes(M);
		}
		else if (Class == TEXT("SubUV")) { Module.Type = EType::SubUV; Dist(Module.A, TEXT("SubImageIndex"), 1, 0.0f); }
		else if (Class == TEXT("KillHeight")) { Module.Type = EType::KillHeight; Dist(Module.A, TEXT("Height"), 1, 0.0f); Module.bFlagA = Flag(M, TEXT("bFloor"), false); }
		else
		{
			continue;		// collision, decals, orbit, events, ...: not simulated yet
		}
		Out.Modules.Add(MoveTemp(Module));
	}
	return true;
}

UTexture2D* FOKParticles::LoadTexture(FOKPackage& Package, int32 TextureRef)
{
	const FString Name = Package.ObjectName(TextureRef).TrimEnd();
	if (TStrongObjectPtr<UTexture2D>* Found = Textures.Find(Name))
	{
		return Found->Get();
	}
	FOKPackage* Holder = TextureRef > 0 ? &Package : nullptr;
	int32 Ref = TextureRef;
	if (!Holder)
	{
		// An import: the texture is an export of one of the other packages.
		TArray<FString> Search = { TEXT("Startup") };
		Search.Append(PackageNames);
		for (const FString& PackageName : Search)
		{
			FOKPackage* Other = OpenPackage(PackageName);
			for (const FOKPackageExport* Export : Other ? Other->FindExports(TEXT("Texture2D")) : TArray<const FOKPackageExport*>())
			{
				if (Export->Name.TrimEnd() == Name)
				{
					Holder = Other;
					Ref = Export->Index;
				}
			}
			if (Holder)
			{
				break;
			}
		}
	}
	FOKTextureData Data;
	FString Error;
	UTexture2D* Texture = Holder && Data.Read(*Holder, Ref, Error) ? Data.CreateTexture(true, Error) : nullptr;
	if (!Texture)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("particles: texture %s: %s"), *Name, Error.IsEmpty() ? TEXT("not found") : *Error);
	}
	Textures.Add(Name, TStrongObjectPtr<UTexture2D>(Texture));
	return Texture;
}

FOKParticleMaterial FOKParticles::MakeMaterial(FOKPackage& Package, int32 MaterialRef)
{
	FOKParticleMaterial Out;
	const FString Path = Package.ObjectPath(MaterialRef);
	if (TStrongObjectPtr<UMaterialInterface>* Found = Materials.Find(Path))
	{
		Out.Material = Found->Get();
		Out.bAdditive = Found->Get() && Found->Get()->GetBlendMode() == BLEND_Additive;
		return Out;
	}

	// Walk the instance chain to its material: the first texture and tint parameters met win, as a child's override
	// its parent's. Parents may be in another package (MK9_FX_CharMatLibrary is cooked into Startup).
	FOKPackage* At = &Package;
	int32 Ref = MaterialRef;
	int32 BlendMode = 2;
	TArray<TPair<FString, TPair<FOKPackage*, int32>>> TextureParams;
	int32 InstanceParams = MAX_int32;		// TextureParams after this many come from the base material
	FLinearColor Tint = FLinearColor::White;
	bool bTint = false;
	for (int32 Depth = 0; Depth < 6 && Ref != 0; ++Depth)
	{
		if (Ref < 0)
		{
			const FString Name = At->ObjectName(Ref).TrimEnd();
			FOKPackage* Holder = nullptr;
			int32 Found = 0;
			TArray<FString> Search = { TEXT("Startup") };
			Search.Append(PackageNames);
			for (const FString& PackageName : Search)
			{
				FOKPackage* Other = OpenPackage(PackageName);
				if (!Other)
				{
					continue;
				}
				for (const FOKPackageExport& Export : Other->GetExports())
				{
					if (Export.Name.TrimEnd() == Name && Export.ClassName.StartsWith(TEXT("Material")))
					{
						Holder = Other;
						Found = Export.Index;
						break;
					}
				}
				if (Holder)
				{
					break;
				}
			}
			if (!Holder)
			{
				break;
			}
			At = Holder;
			Ref = Found;
		}
		FOKPropertyList M;
		if (!ReadObject(*At, Ref, M))
		{
			break;
		}
		TArray<FOKPropertyList> Values;
		if (const FOKProperty* List = M.Find(TEXT("TextureParameterValues")))
		{
			FOKPropertyList::ReadStructArray(*List, At->GetNames(), Values);
		}
		for (const FOKPropertyList& Value : Values)
		{
			TextureParams.Emplace(Value.GetText(TEXT("ParameterName")), TPair<FOKPackage*, int32>(At, Value.GetObject(TEXT("ParameterValue"))));
		}
		Values.Reset();
		if (const FOKProperty* List = M.Find(TEXT("VectorParameterValues")))
		{
			FOKPropertyList::ReadStructArray(*List, At->GetNames(), Values);
		}
		for (const FOKPropertyList& Value : Values)
		{
			const FOKProperty* Color = Value.Find(TEXT("ParameterValue"));
			if (!bTint && Color && Color->Raw.Num() >= 16 && Value.GetText(TEXT("ParameterName")).Contains(TEXT("Color")))
			{
				FMemory::Memcpy(&Tint, Color->Raw.GetData(), 16);
				bTint = true;
			}
		}
		if (At->GetExports()[Ref - 1].ClassName == TEXT("Material"))
		{
			BlendMode = int32(M.GetNumber(TEXT("BlendMode"), 0.0));
			InstanceParams = TextureParams.Num();
			if (const FOKProperty* Referenced = M.Find(TEXT("ReferencedTextures")))
			{
				for (const int32 TextureRef : FOKPropertyList::ReadIntArray(*Referenced))
				{
					TextureParams.Emplace(At->ObjectName(TextureRef), TPair<FOKPackage*, int32>(At, TextureRef));
				}
			}
			break;
		}
		Ref = M.GetObject(TEXT("Parent"));
	}

	// The texture that gives the sprite its look, from the parameter and texture names: a colour map first, then a
	// normal map whose alpha is the shape (blood's Normal_Alpha). Reflections, noise and distortion maps don't count,
	// and the base material's own textures (DefaultDiffuse placeholders) only when the instances set none.
	auto Score = [](const FString& ParameterName, const FString& TextureName)
	{
		const FString Lower = (ParameterName + TEXT(" ") + TextureName).ToLower();
		if (Lower.Contains(TEXT("cube")) || Lower.Contains(TEXT("reflect")) || Lower.Contains(TEXT("noise")) || Lower.Contains(TEXT("distort"))
			|| Lower.Contains(TEXT("displace")) || Lower.Contains(TEXT("normalmap")) || Lower.Contains(TEXT("_norm ")) || Lower.EndsWith(TEXT("_norm")))
		{
			return 0;
		}
		if (Lower.Contains(TEXT("norma")))
		{
			return 2;
		}
		return Lower.Contains(TEXT("diff")) || Lower.Contains(TEXT("emissive")) || Lower.Contains(TEXT("base")) || Lower.Contains(TEXT("color")) ? 4 : 3;
	};
	int32 Best = -1;
	int32 BestScore = 0;
	int32 BestRank = 0;
	for (int32 i = 0; i < TextureParams.Num(); ++i)
	{
		const TPair<FOKPackage*, int32>& Source = TextureParams[i].Value;
		const int32 S = Source.Value != 0 ? Score(TextureParams[i].Key, Source.Key->ObjectName(Source.Value)) : 0;
		const int32 Rank = S + (i < InstanceParams ? 10 : 0);
		if (S > 0 && Rank > BestRank)
		{
			Best = i;
			BestScore = S;
			BestRank = Rank;
		}
	}

	Out.bAdditive = BlendMode == 3;
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(BaseMaterial(Out.bAdditive), GetTransientPackage());
	if (Best >= 0)
	{
		const TPair<FOKPackage*, int32>& Source = TextureParams[Best].Value;
		if (UTexture2D* Texture = LoadTexture(*Source.Key, Source.Value))
		{
			Instance->SetTextureParameterValue(TextureParameter, Texture);
			const EPixelFormat Format = Texture->GetPixelFormat();
			Instance->SetScalarParameterValue(AlphaFromTextureParameter, Format == PF_DXT1 || Format == PF_G8 ? 0.0f : 1.0f);
		}
		Instance->SetScalarParameterValue(UseTextureColorParameter, BestScore == 2 ? 0.0f : 1.0f);
	}
	if (bTint)
	{
		Instance->SetVectorParameterValue(TintParameter, Tint);
	}
	if (CVarTraceParticles.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogOKAsset, Log, TEXT("particles: material %s: blend %d, texture %s, tint %s"), *Path, BlendMode,
			Best >= 0 ? *TextureParams[Best].Key : TEXT("none"), bTint ? *Tint.ToString() : TEXT("none"));
	}
	Materials.Add(Path, TStrongObjectPtr<UMaterialInterface>(Instance));
	Out.Material = Instance;
	return Out;
}
