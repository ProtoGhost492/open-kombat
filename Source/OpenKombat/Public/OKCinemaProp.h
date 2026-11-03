#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OKAnimation.h"
#include "OKCinemaProp.generated.h"

class UPoseableMeshComponent;
class USkeletalMesh;

/**
 * A skeletal actor a cinema spawns for one of its groups (W4): a prop such as Scorpion's sword (SC_Sword_Sk). It
 * shows one of the cinema's sequences at a time, root and all, from where the cinema puts it.
 */
UCLASS()
class OPENKOMBAT_API AOKCinemaProp : public AActor
{
	GENERATED_BODY()

public:
	AOKCinemaProp();

	void SetMesh(USkeletalMesh* InMesh);
	UPoseableMeshComponent* GetMesh() const { return Mesh; }
	/** Shows Sequence at Frame (its tracks mapped onto this mesh's bones the first time it's shown). */
	void Pose(const FOKAnimSequence& Sequence, float Frame);

private:
	UPROPERTY(VisibleAnywhere, Category = "OpenKombat")
	TObjectPtr<UPoseableMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> PropMesh;

	FString MappedSequence;
	TArray<int32> TrackToBone;
};
