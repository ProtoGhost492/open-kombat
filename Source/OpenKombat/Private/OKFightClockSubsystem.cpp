#include "OKFightClockSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogOKClock, Log, All);

void UOKFightClockSubsystem::Tick(float DeltaTime)
{
	Accumulator += DeltaTime;
	
	// MKKE never catches up: if a frame is late it runs one step and keeps only
	// the leftover fraction, so a slow machine plays in slow motion instead of
	// skipping fight frames.
	if (Accumulator >= StepSeconds)
	{
		StepSimulation();
		Accumulator = FMath::Fmod(Accumulator, StepSeconds);
	}
}

void UOKFightClockSubsystem::Register(UObject* Object)
{
	if (!Object || !Object->Implements<UOKSimTickable>() || Tickables.Contains(Object))
	{
		return;
	}

	// Instert after everything in the same or an earlier phase, so the list stays
	// in phase order and objects in one phase keep the order they registered in.
	const EOKSimPhase Phase = CastChecked<IOKSimTickable>(Object)->GetSimPhase();
	int32 Index = Tickables.Num();
	for (int32 i = 0; i < Tickables.Num(); ++i)
	{
		const IOKSimTickable* Other = Cast<IOKSimTickable>(Tickables[i].Get());
		if (Other && Other->GetSimPhase() > Phase)
		{
			Index = i;
			break;
		}
	}
	Tickables.Insert(Object, Index);
}

void UOKFightClockSubsystem::Unregister(UObject* Object)
{
	Tickables.RemoveAll([Object](const TWeakObjectPtr<UObject>& Ptr)
		{
			return !Ptr.IsValid() || Ptr.Get() == Object;
		});
}

void UOKFightClockSubsystem::StepSimulation()
{
	++SimFrame;
	
	// Step a copy of the list: an object may spawn or destroy others mid-frame.
	const TArray<TWeakObjectPtr<UObject>> Snapshot = Tickables;
	for (const TWeakObjectPtr<UObject>& Ptr : Snapshot)
	{
		if (IOKSimTickable* Tickable = Cast<IOKSimTickable>(Ptr.Get()))
		{
			Tickable->SimTick(SimFrame);
		}
	}

	if (SimFrame % 60 == 0)
	{
		UE_LOG(LogOKClock, Log, TEXT("Sim frame %lld (%.2f s of fight time)"),
			SimFrame, SimFrame * StepSeconds);
	}
}

TStatId UOKFightClockSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOKFightClockSubsystem, STATGROUP_Tickables);
}

bool UOKFightClockSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}	
	