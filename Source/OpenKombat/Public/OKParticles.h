#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OKParticles.generated.h"

class FOKPackage;
class FOKPropertyList;
class UMaterialInterface;
class UProceduralMeshComponent;

/**
 * A Cascade distribution as MKKE cooks it (FRawDistribution, system W2): a lookup table of [min, max], then Entries
 * time steps of Dims values each, or twice that for a uniform (each step a min and a max). Time steps are
 * InvTimeScale apart from StartTime, and values between them are blended. A table may be empty: then the
 * distribution object holds the value, which is 0 in every one met so far.
 */
struct OPENKOMBAT_API FOKDistribution
{
	TArray<float> Values;		// the steps, without the [min, max] at the front
	int32 Dims = 1;
	int32 Entries = 0;
	bool bUniform = false;
	float StartTime = 0.0f;
	float InvTimeScale = 0.0f;
	FName Parameter;			// a DistributionVectorParticleParameter's name (FX_Blood_Color): the effect can set it

	/** Reads a RawDistributionFloat or RawDistributionVector property (Dims 1 or 3). False if it's missing. */
	bool Read(const FOKPropertyList& Properties, const FString& Name, int32 InDims, FOKPackage& Package);
	bool IsSet() const { return Entries > 0; }
	/** The value at Time (seconds, or 0-1 of a particle's life); a uniform picks between its min and max. */
	FVector Get(float Time, FRandomStream& Random) const;
	float GetFloat(float Time, FRandomStream& Random) const { return float(Get(Time, Random).X); }
};

/** One ParticleModule of an emitter's first LOD level, with what OpenKombat uses of it. */
struct FOKParticleModule
{
	enum class EType : uint8
	{
		Lifetime, Size, SizeMultiplyLife, SizeScale, Velocity, VelocityOverLife, Acceleration, AccelerationOverLife,
		Color, ColorOverLife, ColorScaleOverLife, Rotation, RotationRate, RotationRateMultiplyLife, Location,
		Sphere, Cylinder, SubUV, KillHeight,
	};
	EType Type = EType::Lifetime;
	FOKDistribution A;			// the module's main distribution (Lifetime, StartSize, StartVelocity, ...)
	FOKDistribution B;			// a second one (StartVelocityRadial, AlphaOverLife, StartHeight, ...)
	FOKDistribution C;			// a third (VelocityScale, StartLocation)
	bool bFlagA = false;		// per type: bInWorldSpace, bAbsolute, bSurfaceOnly, bFloor
	bool bFlagB = false;		// bVelocity
	uint8 Axes = 0x7;			// SizeMultiplyLife's MultiplyX/Y/Z; a primitive's positive/negative axes (6 bits)
	int32 HeightAxis = 2;		// cylinder: 0 X, 1 Y, 2 Z
};

/** What an emitter's material comes down to here: a texture, how it blends and which channels count. */
struct FOKParticleMaterial
{
	TWeakObjectPtr<UMaterialInterface> Material;
	bool bAdditive = false;
};

/** A ParticleSpriteEmitter: the required and spawn modules' settings and the rest of its modules. */
struct OPENKOMBAT_API FOKParticleEmitter
{
	struct FBurst
	{
		int32 Count = 0;
		int32 CountLow = -1;	// -1: exactly Count
		float Time = 0.0f;		// 0-1 of the emitter's duration
	};

	FString Name;
	FOKParticleMaterial Material;
	int32 SubImagesH = 1;
	int32 SubImagesV = 1;
	int32 Interpolation = 0;		// 0 none, 1 linear, 2 linear blend, 3 random, 4 random blend
	float RandomImageTime = 1.0f;
	int32 ScreenAlignment = 0;		// 0 square, 1 rectangle, 2 velocity
	float Duration = 1.0f;
	float DurationLow = 0.0f;
	bool bDurationRange = false;
	int32 Loops = 0;				// 0: forever
	float Delay = 0.0f;
	bool bLocalSpace = false;
	FOKDistribution Rate;
	FOKDistribution RateScale;
	TArray<FBurst> Bursts;
	TArray<FOKParticleModule> Modules;
	bool bSkipped = false;			// a mesh, beam or ribbon emitter, not drawn yet
};

/** A ParticleSystem read from a package. */
struct OPENKOMBAT_API FOKParticleSystem
{
	FString Name;
	TArray<FOKParticleEmitter> Emitters;
};

/**
 * One playing ParticleSystem (MKKE's MKParticle and the ParticleSystemComponent at its +0xF8). The particles are
 * simulated on the CPU each frame, like Cascade's sprite emitters, and drawn as camera-facing quads.
 */
UCLASS()
class OPENKOMBAT_API AOKParticleActor : public AActor
{
	GENERATED_BODY()

public:
	AOKParticleActor();

	void Init(TSharedPtr<const FOKParticleSystem> InSystem);
	/** Stops every emitter spawning (fx_stop_emitter); the live particles live out their lives. */
	void StopEmitters() { bStopped = true; }
	/** Stops spawning and goes once its particles have died. */
	void Finish();
	/** True when no emitter will spawn again and no particle is alive. */
	bool IsComplete() const;
	/** A colour parameter (82.17 fx_blood_tint: FX_Blood_Color) that DistributionVectorParticleParameters read. */
	void SetVectorParameter(FName ParameterName, const FVector& Value) { Parameters.Add(ParameterName, Value); }
	void SetEffectScale(float InScale) { EffectScale = InScale; }

	virtual void Tick(float DeltaSeconds) override;

private:
	struct FParticle
	{
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FVector BaseVelocity = FVector::ZeroVector;
		FVector Size = FVector::OneVector;
		FVector BaseSize = FVector::OneVector;
		FLinearColor Color = FLinearColor::White;
		FLinearColor BaseColor = FLinearColor::White;
		float Rotation = 0.0f;			// radians
		float RotationRate = 0.0f;
		float BaseRotationRate = 0.0f;
		float Age = 0.0f;
		float Lifetime = 1.0f;
		float RelativeTime = 0.0f;		// Age / Lifetime
		float Image = 0.0f;
		float ImageChanged = 0.0f;		// the relative time a random SubUV image was picked
	};

	struct FEmitterState
	{
		TArray<FParticle> Particles;
		float Time = 0.0f;				// in this loop, after the delay
		float Duration = 1.0f;
		float SpawnFraction = 0.0f;
		int32 Loop = 0;
		int32 NextBurst = 0;
		bool bDone = false;
	};

	void TickEmitter(const FOKParticleEmitter& Emitter, FEmitterState& State, float DeltaSeconds);
	void Spawn(const FOKParticleEmitter& Emitter, FEmitterState& State, int32 Count, float SpawnTime);
	void Update(const FOKParticleEmitter& Emitter, FEmitterState& State, float DeltaSeconds);
	FVector Evaluate(const FOKDistribution& Distribution, float Time);
	void BuildMesh();

	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Mesh;

	TSharedPtr<const FOKParticleSystem> System;
	TArray<FEmitterState> States;
	TMap<FName, FVector> Parameters;
	FRandomStream Random;
	float EffectScale = 1.0f;
	bool bStopped = false;
	bool bFinishing = false;
};

/**
 * MKKE's particle systems, found by name in the packages that hold them: Startup (fx_blood and the other shared
 * effects were cooked into it) and the fighters' packages (fx_subzero is in CHAR_SubZero).
 */
class OPENKOMBAT_API FOKParticles
{
public:
	static FOKParticles& Get();

	FOKParticles();
	~FOKParticles();
	FOKParticles(const FOKParticles&) = delete;
	FOKParticles& operator=(const FOKParticles&) = delete;

	/** The world effects spawn into, for this fight. */
	void SetWorld(UWorld* InWorld) { World = InWorld; }
	/** A package to search for systems, after Startup: each fighter's. */
	void AddPackage(const FString& PackageName);
	/** ok.Particles.Trace: log effects as they're read, started and moved. */
	static bool IsTracing();

	/** Starts the named system at Location (fx_create); null if there's no such system or no world. */
	AOKParticleActor* Spawn(const FString& SystemName, const FVector& Location, const FRotator& Rotation);

private:
	TSharedPtr<const FOKParticleSystem> Find(const FString& SystemName);
	TSharedPtr<FOKParticleSystem> Read(FOKPackage& Package, const FString& SystemName);
	bool ReadEmitter(FOKPackage& Package, int32 EmitterRef, FOKParticleEmitter& Out);
	FOKParticleMaterial MakeMaterial(FOKPackage& Package, int32 MaterialRef);
	FOKPackage* OpenPackage(const FString& PackageName);
	/** A Texture2D by reference: an export of Package, or an import found by name in the other packages. */
	class UTexture2D* LoadTexture(FOKPackage& Package, int32 TextureRef);

	TWeakObjectPtr<UWorld> World;
	TArray<FString> PackageNames;
	TMap<FString, TUniquePtr<FOKPackage>> Packages;
	TMap<FString, TSharedPtr<const FOKParticleSystem>> Systems;		// by name, lower case; null when not found
	TMap<FString, TStrongObjectPtr<UMaterialInterface>> Materials;	// by material path
	TMap<FString, TStrongObjectPtr<class UTexture2D>> Textures;		// by texture name
};
