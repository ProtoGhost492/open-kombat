#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "OKScriptObjects.h"
#include "OKScriptRuntime.h"
#include "OKSimTickable.h"
#include "OKFightCamera.h"
#include "OKFightAI.h"
#include "OKCinema.h"
#include "OKScriptWorld.generated.h"

/** The script runtime of a running game world: its processes tick once per sim frame, in the Script phase. */
UCLASS()
class OPENKOMBAT_API UOKScriptWorld : public UWorldSubsystem, public IOKSimTickable
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	// IOKSimTickable
	virtual void SimTick(int64 SimFrame) override;
	virtual EOKSimPhase GetSimPhase() const override { return EOKSimPhase::Script; }

	FOKScriptRuntime* GetRuntime() const { return Runtime.Get(); }

	/** The script fighters: 0 = @my_obj, 1 = @his_obj. */
	FOKFighter& GetFighter(int32 Index) { return Index == 0 ? Me : Him; }

	/**
	 * Starts the fight once both fighters are loaded: their main threads at FGFightEngineScript.p_player_entry
	 * and utils.enable_controllers. ScreenRight is the world direction of the screen's right, for facing.
	 */
	void StartFight(const FVector& InScreenRight);

	/** What the fight HUD shows this frame (AOKFightHUD): life, meter, rounds and the clock, from MKKE's records. */
	void GetHudFrame(FOKHudFrame& Out) const;

	/** Holds input masks for a side from frame From for Frames frames, on top of the keyboard (ok.Input.Hold). */
	void HoldInput(int32 Side, uint32 Mask, int64 From, int64 Frames) { Holds.Add({ Side, Mask, From, From + Frames }); }
	int64 GetSimFrame() const { return Runtime ? Runtime->GetSimFrame() : 0; }

	/** Plays a cinema package with Side's fighter as its Attacker and the other as its Victim, at sim frame At (W4). */
	void PlayCinema(const FString& PackageName, int32 Side, int64 At) { PendingCinema = { PackageName, Side, At }; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	FOKFighter Me;			// @my_obj, until real fighters exist
	FOKFighter Him;			// @his_obj
	FOKPlayer Players[2];	// @my_info / @his_info: player 1 and player 2
	FOKGameInfo GameInfo;	// @GFGGameInfo / @GGameInfo
	FVector ScreenRight = FVector::YAxisVector;
	bool bFightStarted = false;
	struct FHold { int32 Side; uint32 Mask; int64 From; int64 To; };
	TArray<FHold> Holds;

	/** Moves the view to the fight camera's framing (W3), unless ok.Camera.Follow is 0. */
	void UpdateCamera();
	FOKFightCamera FightCamera;
	FOKFightAI ComputerPlayer;		// plays the side ok.AI.Side names (F9)
	int32 ComputerSide = -1;
	bool bCameraPlaced = false;

	struct FPendingCinema { FString Package; int32 Side = 0; int64 At = 0; };
	FPendingCinema PendingCinema;
	FOKCinemaPlayer Cinema;			// while it plays, it has the view and both fighters (W4)

	/** The masks player 1's keyboard holds: WASD, U I J K for FP BP FK BK, L block, O throw, P flip stance, Y tag. */
	uint32 KeyboardMask() const;
	TUniquePtr<FOKScriptRuntime> Runtime;
};
