// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "OKSimTickable.h"
#include "OKFightClockSubsystem.generated.h"

/**
 * Fixed 60Hz fight clock. Rendering runs at any rate; the fight simulation
 * advances in whole 1/60 s steps, like MKKE's frame loop.
 */
UCLASS()
class OPENKOMBAT_API UOKFightClockSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
	
public:
	static constexpr double StepSeconds = 1.0 / 60.0;
	
	// UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	
	/** Number of simulation frames since the world started. */
	int64 GetSimFrame() const { return SimFrame; }

	/** Start stepping an object that implements IOKSimTickable. */
	void Register(UObject* Object);

	/** Stop stepping it. Safe to call during a SimTick. */
	void Unregister(UObject* Object);
	
protected:
	// Only run in real games and Play-In-Editor, not in the editor's own preview worlds.
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	
private:
	void StepSimulation();
	
	double Accumulator = 0.0;
	int64 SimFrame = 0;

	/** Sorted by phase; within a phase, by registration order. */
	TArray<TWeakObjectPtr<UObject>> Tickables;
};
