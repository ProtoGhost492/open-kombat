#include "OKSimTestActor.h"
#include "OKFightClockSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

AOKSimTestActor::AOKSimTestActor()
{
 	// No Unreal tick: this actor moves only when the fight clock steps it.
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;

	static ConstructorHelpers::FObjectFinder<UStaticMesh>
Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded())
	{
		Mesh->SetStaticMesh(Cube.Object);
	}
}

void AOKSimTestActor::BeginPlay()
{
	Super::BeginPlay();
	StartLocation = GetActorLocation();

	if (UOKFightClockSubsystem* Clock = GetWorld()->GetSubsystem<UOKFightClockSubsystem>())
	{
		Clock->Register(this);
	}
}

void AOKSimTestActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UOKFightClockSubsystem* Clock = GetWorld()->GetSubsystem<UOKFightClockSubsystem>())
	{
		Clock->Unregister(this);
	}
	Super::EndPlay(EndPlayReason);
}

void AOKSimTestActor::SimTick(int64 SimFrame)
{
	// Out for FramesPerLeg frames, back for FramesPerLeg frames, repeat.
	const bool bGoingOut = ((SimFrame - 1) / FramesPerLeg) % 2 == 0;
	AddActorWorldOffset(bGoingOut ? StepPerFrame : -StepPerFrame);

	if (SimFrame % (2 * FramesPerLeg) == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Frame %lld: back at start? %s (offset %s)"),
			SimFrame,
			GetActorLocation().Equals(StartLocation, 0.0) ? TEXT("yes") : TEXT("NO"),
			*(GetActorLocation() - StartLocation).ToString());
	}
}