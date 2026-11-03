#include "OKCinemaProp.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"

AOKCinemaProp::AOKCinemaProp()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AOKCinemaProp::SetMesh(USkeletalMesh* InMesh)
{
	PropMesh = InMesh;
	Mesh->SetSkinnedAssetAndUpdate(InMesh);
	MappedSequence.Reset();
}

void AOKCinemaProp::Pose(const FOKAnimSequence& Sequence, float Frame)
{
	if (!PropMesh || Mesh->BoneSpaceTransforms.Num() != PropMesh->GetRefSkeleton().GetNum())
	{
		return;
	}
	if (MappedSequence != Sequence.Name)
	{
		TrackToBone.Reset();
		for (const FString& Bone : Sequence.TrackBones)
		{
			TrackToBone.Add(PropMesh->GetRefSkeleton().FindBoneIndex(FName(*Bone)));
		}
		MappedSequence = Sequence.Name;
	}
	TArray<FTransform> Pose = PropMesh->GetRefSkeleton().GetRefBonePose();
	Sequence.Sample(Frame, TrackToBone, Pose);
	Mesh->BoneSpaceTransforms = Pose;
	Mesh->MarkRefreshTransformDirty();
}
