#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "OKSimTickable.generated.h"

/** Order within one sim frame. Lower phases run first (MKKE frame order, ENGINE §4.1). */
UENUM()
enum class EOKSimPhase : uint8
{
	Input,			// read pads, update input history
	Script,			// MKScript VM threads
	Objects,		// fighters, projectiles, props
	Motion,			// jumps, force-away, pushing
	Collision		// hit scans and hit resolution
};

UINTERFACE(MinimalAPI)
class UOKSimTickable : public UInterface
{
	GENERATED_BODY()
};

/** Anything that advances on the fight clock instead of Unreal's frame tick. */
class OPENKOMBAT_API IOKSimTickable
{
	GENERATED_BODY()

public:
	/** Called once per 1/60 s sim frame. SimFrame counts up from 1. */
	virtual void SimTick(int64 SimFrame) = 0;

	/** Which phase this object runs in. */
	virtual EOKSimPhase GetSimPhase() const { return EOKSimPhase::Objects; }
};