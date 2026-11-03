#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OKSimTickable.h"
#include "OKSimTestActor.generated.h"

/** Moves back and forth on the fight clock. Same path at any frame rate. */
UCLASS()
class OPENKOMBAT_API AOKSimTestActor : public AActor, public IOKSimTickable
{
	GENERATED_BODY()
	
public:	
	AOKSimTestActor();

	virtual void SimTick(int64 SimFrame) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** Distance moved per sim frame. */
	UPROPERTY(EditAnywhere, Category = "OpenKombat")
	FVector StepPerFrame = FVector(0.0, 2.0, 0.0);

	/** Frames before turning around. */
	UPROPERTY(EditAnywhere, Category = "OpenKombat")
	int32 FramesPerLeg = 120;

private:
	FVector StartLocation;
};
