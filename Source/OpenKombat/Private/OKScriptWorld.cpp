#include "OKScriptWorld.h"
#include "OKFightClockSubsystem.h"
#include "OKMkoFile.h"
#include "OKScriptSubsystem.h"
#include "OKScriptThread.h"
#include "OKInput.h"
#include "OKNatives.h"
#include "OKFightHUD.h"
#include "OKLocalization.h"
#include "OKAudio.h"
#include "OKParticles.h"
#include "OKFighterActor.h"
#include "InputCoreTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"

namespace
{
	/** [inf] What a fighter's +0x2FD0 points at: stats floats the blood helpers add to. Nothing reads them yet. */
	class FOKTallies : public FOKScriptObject
	{
	public:
		static constexpr int32 Bytes = 0x100;
	};
}

// The Options screen's Gameplay settings, which MKKE keeps in the saved game-settings record (MENU-RE-NOTES §9.2).
static TAutoConsoleVariable<int32> CVarRoundTime(TEXT("ok.Fight.RoundTime"), 90, TEXT("Round Time in seconds (0: no clock)"));
static TAutoConsoleVariable<int32> CVarRoundsToWin(TEXT("ok.Fight.RoundsToWin"), 2, TEXT("Rounds to Win"));
static TAutoConsoleVariable<float> CVarStartMeter(TEXT("ok.Fight.Meter"), 0.333328f,
	TEXT("The super meter each side starts with, 0 to 1 (a bar is 0.3333; MKKE's practice mode starts it full). Read when a fight starts."));
static TAutoConsoleVariable<int32> CVarAISide(TEXT("ok.AI.Side"), 1,
	TEXT("The side the computer plays (F9): 0 player 1, 1 player 2, -1 none. Read when a fight starts."));
static TAutoConsoleVariable<int32> CVarAIDifficulty(TEXT("ok.AI.Difficulty"), 4,
	TEXT("The computer's difficulty, 0 to 9: the column of MKAI's tables it uses. Read when a fight starts."));
static TAutoConsoleVariable<int32> CVarCameraFollow(TEXT("ok.Camera.Follow"), 1,
	TEXT("1: the fight camera follows the fighters (W3). 0: it stays where ok.Camera puts it."));
static TAutoConsoleVariable<FString> CVarShotFrames(TEXT("ok.Debug.ShotFrames"), TEXT(""),
	TEXT("Sim frames to take screenshots at, e.g. 120+360 (Saved/Screenshots/.../OKSim_<frame>.png). For automated checks."));

static uint32 FloatToBits(float Value)
{
	uint32 Bits;
	FMemory::Memcpy(&Bits, &Value, 4);
	return Bits;
}

void UOKScriptWorld::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
	Runtime = MakeUnique<FOKScriptRuntime>(Scripts ? &Scripts->GetNativeNames() : nullptr);

	if (IConsoleVariable* Seed = IConsoleManager::Get().FindConsoleVariable(TEXT("ok.VM.Seed")))
	{
		Runtime->SetRandomSeed(uint32(Seed->GetInt()));
	}

	Me.Name = TEXT("my_obj");
	Him.Name = TEXT("his_obj");
	Me.Handle = Runtime->AddObject(Me, FOKFighter::Bytes);
	Him.Handle = Runtime->AddObject(Him, FOKFighter::Bytes);
	// @my_obj and @his_obj are bound per thread: a fighter's threads see that fighter (FOKScriptThread::BindFighter).

	// Each fighter's player record, linked both ways as MKKE does when it spawns a fighter (around FUN_008AE9A0).
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FOKFighter& Fighter = GetFighter(Side);
		FOKPlayer& Player = Players[Side];
		Player.Handle = Runtime->AddObject(Player, FOKPlayer::Bytes);
		Player.Fighter = &Fighter;
		Fighter.Player = &Player;
		Runtime->SetHeapWord(Fighter.Handle + FOKFighter::PlayerInfo, Player.Handle);
		Runtime->SetHeapWord(Fighter.Handle + FOKFighter::Tallies, Runtime->CreateOwned(MakeUnique<FOKTallies>(), FOKTallies::Bytes));
		Runtime->SetHeapWord(Fighter.Handle + FOKFighter::Life, FloatToBits(Fighter.Health));
		Runtime->SetHeapWord(Player.Handle + FOKPlayer::ActiveCharacter + 4, Fighter.Handle);
		Runtime->SetHeapWord(Player.Handle + FOKPlayer::ActiveCharacter + 8, Fighter.Handle);	// our objects' serial is their handle
		Runtime->SetHeapWord(Player.Handle + FOKPlayer::Side, uint32(Side));
		Runtime->SetHeapWord(Player.Handle + FOKPlayer::DamageMultiplier, FloatToBits(1.0f));	// [inf] full damage to start
		Runtime->SetHeapWord(Player.Handle + FOKPlayer::Meter, FloatToBits(0.333328f));		// a bar to start (FUN_00898E50)
		OKInput::CreateBuffer(*Runtime, Player);
		GameInfo.Players[Side] = &Player;
	}

	for (int32 Side = 0; Side < 2; ++Side)
	{
		Runtime->SetHeapWord(Players[Side].Handle + FOKPlayer::Opponent, Players[1 - Side].Handle);
	}

	GameInfo.Handle = Runtime->AddObject(GameInfo, FOKGameInfo::Bytes);
	// GameInfo +0x1C is the stage's object (the floor height comes from it, and FGEffectsEngine reads the landing
	// dust's tint at +0x88 -> +0x24). [inf] Until stages are script objects (W1), a stand-in with a white tint.
	const uint32 Stage = Runtime->CreateOwned(MakeUnique<FOKScriptObject>(), 0x1000);	// gamelogic reads +0x854 (a secret fight)
	const uint32 StageLooks = Runtime->CreateOwned(MakeUnique<FOKScriptObject>(), 0x40);
	for (uint32 Offset = 0x24; Offset < 0x34; Offset += 4)
	{
		Runtime->SetHeapWord(StageLooks + Offset, FloatToBits(1.0f));
	}
	Runtime->SetHeapWord(Stage + 0x88, StageLooks);
	Runtime->SetHeapWord(GameInfo.Handle + 0x1C, Stage);
	// The match settings (system F11): the game mode (1 = versus, MENU-RE-NOTES §3), and +0x184 the saved game-settings
	// record (inline), where Round Time is +4 and Rounds to Win +8 (natives 36.3 / 36.5): StartFight fills them in.
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::GameMode, 1);
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::State, 8);
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::Round, 1);		// game_init (136.17, from gamelogic.SetupFight)
	Runtime->BindName(TEXT("GFGGameInfo"), GameInfo.Handle);
	Runtime->BindName(TEXT("GGameInfo"), GameInfo.Handle);

	if (UOKFightClockSubsystem* Clock = InWorld.GetSubsystem<UOKFightClockSubsystem>())
	{
		Clock->Register(this);
	}
}

void UOKScriptWorld::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UOKFightClockSubsystem* Clock = World->GetSubsystem<UOKFightClockSubsystem>())
		{
			Clock->Unregister(this);
		}
	}
	Runtime.Reset();		// destroys the processes before the fighters they may point at
	Super::Deinitialize();
}

void UOKScriptWorld::SimTick(int64 SimFrame)
{
	if (!Runtime)
	{
		return;
	}
	Runtime->BeginFrame(SimFrame);
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::FrameCounter, uint32(SimFrame));
	TArray<FString> ShotFrames;
	CVarShotFrames.GetValueOnGameThread().ParseIntoArray(ShotFrames, TEXT("+"));
	if (ShotFrames.Contains(FString::Printf(TEXT("%lld"), SimFrame)))
	{
		FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("OKSim_%lld"), SimFrame), false, false);
	}

	// Input, as in MK9Game::Tick: the controllers are polled, then FightPlayers::Update runs the commands and
	// buttons, then the scripts tick (FIGHT-RE-NOTES §3, §6.4).
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FOKFighter& Fighter = GetFighter(Side);
		if (Fighter.Body && Fighter.Opponent && Fighter.Opponent->Body)
		{
			Fighter.bFacingRight = FVector::DotProduct(Fighter.Opponent->Body->GetWorldLocation() - Fighter.Body->GetWorldLocation(), ScreenRight) > 0.0;
		}
		uint32 Held = Side == ComputerSide ? 0 : (Side == 0 ? KeyboardMask() : 0);
		if (Side == ComputerSide && Fighter.Opponent)
		{
			Held |= ComputerPlayer.Think(*Runtime, Fighter, *Fighter.Opponent, uint32(SimFrame));
		}
		for (const FHold& Hold : Holds)
		{
			if (Hold.Side == Side && SimFrame >= Hold.From && SimFrame < Hold.To)
			{
				Held |= Hold.Mask;
			}
		}
		uint32 Buffer = 0;
		Runtime->LoadHeapWord(Players[Side].Handle + FOKPlayer::InputBuffer, Buffer);
		FOKPlayer& Player = Players[Side];
		OKInput::Poll(*Runtime, Buffer, Held, uint32(SimFrame), [this, &Player]() { return OKPlayerInput::Direction(*Runtime, Player); });
	}
	Holds.RemoveAll([SimFrame](const FHold& Hold) { return SimFrame >= Hold.To; });
	FOKFighter* const Fighters[2] = { &Me, &Him };
	OKUpdateFightPlayers(*Runtime, Fighters);

	Runtime->TickProcesses(SimFrame);
	if (!PendingCinema.Package.IsEmpty() && SimFrame >= PendingCinema.At && Me.Body && Him.Body)
	{
		// The fighter actor is the only body there is.
		AOKFighterActor* Bodies[2] = { static_cast<AOKFighterActor*>(Me.Body), static_cast<AOKFighterActor*>(Him.Body) };
		const int32 Side = PendingCinema.Side == 1 ? 1 : 0;
		FString Error;
		if (!Cinema.Start(PendingCinema.Package, Bodies[Side], Bodies[1 - Side], Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("cinema %s: %s"), *PendingCinema.Package, *Error);
		}
		PendingCinema = FPendingCinema();
	}
	// A cinema the scripts play (cinema_play): the fighters bound to its Attacker and Victim groups play them.
	FOKScriptObject* const SceneObject = Runtime->GetActiveCinema() != 0 ? Runtime->ResolveHandle(Runtime->GetActiveCinema()) : nullptr;
	FOKCinemaObject* const Scene = SceneObject && SceneObject->IsA(EOKScriptClass::Cinema) ? static_cast<FOKCinemaObject*>(SceneObject) : nullptr;
	if (Scene && Scene->State == FOKCinemaObject::EState::Requested)
	{
		const auto Bound = [Scene](const TCHAR* Group) -> AOKFighterActor*
		{
			FOKFighter* const* Fighter = Scene->Actors.Find(Group);
			return (Fighter && *Fighter && (*Fighter)->Body) ? static_cast<AOKFighterActor*>((*Fighter)->Body) : nullptr;
		};
		FString Error;
		const bool bStarted = Cinema.Start(Scene->Package, Bound(TEXT("Attacker")), Bound(TEXT("Victim")), Error, true);
		if (!bStarted)
		{
			UE_LOG(LogOKVM, Warning, TEXT("cinema %s: %s"), *Scene->Package, *Error);
		}
		Scene->State = bStarted ? FOKCinemaObject::EState::Playing : FOKCinemaObject::EState::Done;
		Scene->Length = Cinema.GetLength();
	}
	if (Scene && Scene->bSkip && Cinema.IsPlaying())
	{
		Cinema.Stop();
	}
	Cinema.Step(GetWorld(), Runtime->GetTimeDilation() / 60.0f);		// a Matinee runs on the world's dilated time
	if (Scene && Scene->State == FOKCinemaObject::EState::Playing)
	{
		Scene->Position = Cinema.GetTime();
		Scene->State = Cinema.IsPlaying() ? FOKCinemaObject::EState::Playing : FOKCinemaObject::EState::Done;
	}
	if (!Scene || Scene->State == FOKCinemaObject::EState::Done)
	{
		Runtime->SetActiveCinema(0);
	}
	// [design] A scripted cinema holds its last frame. In a live round (game flag 1: an X-ray) the fight takes the
	// fighters back as soon as it ends; after the round (a fatality, a victory) the hold lasts until the next cinema,
	// so a finisher's result stays on screen.
	uint32 GameFlags = 0;
	Runtime->LoadHeapWord(GameInfo.Handle + FOKGameInfo::Flags, GameFlags);
	if (!Scene && Cinema.IsShowing() && !Cinema.IsPlaying() && (GameFlags & 1) != 0)
	{
		Cinema.Stop();
	}
	FOKAudio::Get().Flush(GetWorld());		// the sounds the scripts asked for this frame (W6)

	// Show the scripts' screen colour scale through the player's camera.
	if (APlayerController* Player = GetWorld()->GetFirstPlayerController())
	{
		if (APlayerCameraManager* Camera = Player->PlayerCameraManager)
		{
			const FVector3f Scale = Runtime->GetColorScale() * Runtime->GetFade();
			Camera->bEnableColorScaling = true;
			Camera->bEnableColorScaleInterp = false;
			Camera->ColorScale = FVector(Scale.X, Scale.Y, Scale.Z);
		}
	}
	UpdateCamera();
}

void UOKScriptWorld::UpdateCamera()
{
	if (!bFightStarted || FightCamera.GetHandle() == 0 || !Me.Body || !Him.Body || CVarCameraFollow.GetValueOnGameThread() == 0 || Cinema.IsShowing())
	{
		return;
	}
	FOKFightCamera::FInput Input;
	Input.Fighters[0] = Me.Body->GetWorldLocation();
	Input.Fighters[1] = Him.Body->GetWorldLocation();
	Input.ScreenRight = ScreenRight;
	Input.LineStart = Me.LineStart;
	Input.LineLength = Me.LineLength;
	FightCamera.Update(*Runtime, Input, !bCameraPlaced);
	bCameraPlaced = true;
	const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (ACameraActor* View = Controller ? Cast<ACameraActor>(Controller->GetViewTarget()) : nullptr)
	{
		View->SetActorLocationAndRotation(FightCamera.GetEye(), FightCamera.GetRotation());
		View->GetCameraComponent()->SetFieldOfView(FightCamera.GetFieldOfView());
		// MKKE has no motion blur, and UE's smears the fighters once the camera moves
		View->GetCameraComponent()->PostProcessSettings.bOverride_MotionBlurAmount = true;
		View->GetCameraComponent()->PostProcessSettings.MotionBlurAmount = 0.0f;
	}
}

uint32 UOKScriptWorld::KeyboardMask() const
{
	const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!Controller)
	{
		return 0;
	}
	static const TPair<FKey, uint32> Keys[] = {
		{ EKeys::W, 1 }, { EKeys::S, 2 }, { EKeys::A, 4 }, { EKeys::D, 8 },
		{ EKeys::U, 0x40 }, { EKeys::I, 0x10 }, { EKeys::J, 0x20 }, { EKeys::K, 0x80 },
		{ EKeys::L, 0x8000 }, { EKeys::O, 0x2000 }, { EKeys::P, 0x4000 }, { EKeys::Y, 0x1000 } };
	uint32 Mask = 0;
	for (const TPair<FKey, uint32>& Key : Keys)
	{
		if (Controller->IsInputKeyDown(Key.Key))
		{
			Mask |= Key.Value;
		}
	}
	return Mask;
}

void UOKScriptWorld::StartFight(const FVector& InScreenRight)
{
	ScreenRight = InScreenRight.GetSafeNormal();
	for (int32 Side = 0; Side < 2; ++Side)
	{
		GetFighter(Side).LineDirection = ScreenRight;
	}
	const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
	const FOKScriptLibrary* Library = Scripts ? Scripts->GetLibrary() : nullptr;
	const FOKMkoFile* Fight = Library ? Library->FindScript(TEXT("FGFightEngineScript.mko")) : nullptr;
	const FOKMkoFile* Utils = Library ? Library->FindScript(TEXT("utils.mko")) : nullptr;
	if (!Runtime || !Fight)
	{
		return;
	}
	bFightStarted = true;
	// The computer player (F9), with MKAI's tables
	// Voices, foley and character sounds are in the fighters' own packages (W6)
	FOKAudio::Get().AddPackage(Me.Name);
	FOKAudio::Get().AddPackage(Him.Name);
	FOKParticles::Get().SetWorld(GetWorld());
	FOKParticles::Get().AddPackage(Me.Name);
	FOKParticles::Get().AddPackage(Him.Name);
	ComputerSide = CVarAISide.GetValueOnGameThread();
	if (ComputerSide == 0 || ComputerSide == 1)
	{
		uint32 Seed = 0;
		if (IConsoleVariable* SeedVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ok.VM.Seed")))
		{
			Seed = uint32(SeedVar->GetInt());
		}
		ComputerPlayer.Setup(Library->FindScript(TEXT("MKAI.mko")), CVarAIDifficulty.GetValueOnGameThread(), Seed);
	}
	// The camera object scripts reach through game info +0x180, with FGCameraEngine's variables (W3)
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::Camera, FightCamera.Create(*Runtime, Library->FindScript(TEXT("FGCameraEngine.mko"))));
	if (APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		Controller->ClientSetHUD(AOKFightHUD::StaticClass());
	}
	// In MKKE the round intros (gamelogic, system F11) leave each fighter in stance before p_player_entry runs; with no
	// intros, FGFightEngineScript.blend_to_stance(0, 0, 0) does that here. p_player_entry then finds a stance to idle on.
	const int32 BlendToStance = Fight->FindFunctionByName(TEXT("blend_to_stance"));
	for (int32 Side = 0; Side < 2 && BlendToStance != INDEX_NONE; ++Side)
	{
		FOKScriptThread Thread(*Runtime);
		Thread.BindFighter(GetFighter(Side));
		uint32 Result = 0;
		FString Error;
		if (!Thread.Run(*Fight, BlendToStance, { 0u, 0u, 0u }, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: blend_to_stance: %s"), *GetFighter(Side).Name, *Error);
		}
	}
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FOKFighter& Fighter = GetFighter(Side);
		FString Error;
		FOKScriptThread* Main = Runtime->StartProcess(*Fight, Fight->FindFunctionByName(TEXT("p_player_entry")), 0, 14, true, Error);
		if (!Main)
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s: no main thread: %s"), *Fighter.Name, *Error);
			continue;
		}
		Main->BindFighter(Fighter);
		Fighter.MainThread = Main->GetHandle();
	}
	// The match: gamelogic.ExecuteFight runs the rounds (FIGHT-RE-NOTES §8b): the round clock, the winner, the poses and
	// the next round. It enables the controllers itself after "Fight!".
	// [obs] The rest of the record as a saved MK9GSET holds it: version 0x97D5, then +0xC..+0x1C 1, 1, 1, 2, 2. +0x10 must be
	// set for a fatality to be offered (gamelogic.ck_fatality_available); +0x1C is the ladder difficulty.
	static const uint32 SavedSettings[] = { 0x97D5, 90, 2, 1, 1, 1, 2, 2 };
	for (int32 i = 0; i < UE_ARRAY_COUNT(SavedSettings); ++i)
	{
		Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::Settings + i * 4, SavedSettings[i]);
	}
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::Settings + 4, uint32(FMath::Max(0, CVarRoundTime.GetValueOnGameThread())));
	Runtime->SetHeapWord(GameInfo.Handle + FOKGameInfo::Settings + 8, uint32(FMath::Max(1, CVarRoundsToWin.GetValueOnGameThread())));
	for (const FOKPlayer& Player : Players)
	{
		Runtime->SetHeapWord(Player.Handle + FOKPlayer::Meter, FloatToBits(FMath::Clamp(CVarStartMeter.GetValueOnGameThread(), 0.0f, 1.0f)));
	}
	const FOKMkoFile* GameLogic = Library->FindScript(TEXT("gamelogic.mko"));
	const int32 Execute = GameLogic ? GameLogic->FindFunctionByName(TEXT("ExecuteFight")) : INDEX_NONE;
	if (Execute != INDEX_NONE)
	{
		FString Error;
		if (Runtime->StartProcess(*GameLogic, Execute, 0, 14, true, Error))
		{
			return;
		}
		UE_LOG(LogOKVM, Warning, TEXT("gamelogic.ExecuteFight: %s"), *Error);
	}
	// Without gamelogic, utils.enable_controllers: each player's control stack gets a 1 (78.93), so its fighter can act.
	const int32 Enable = Utils ? Utils->FindFunctionByName(TEXT("enable_controllers")) : INDEX_NONE;
	if (Enable != INDEX_NONE)
	{
		FOKScriptThread Thread(*Runtime);
		uint32 Result = 0;
		FString Error;
		if (!Thread.Run(*Utils, Enable, {}, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("utils.enable_controllers: %s"), *Error);
		}
	}
}

void UOKScriptWorld::GetHudFrame(FOKHudFrame& Out) const
{
	Out = FOKHudFrame();
	if (!Runtime || !bFightStarted)
	{
		return;
	}
	auto Word = [this](uint32 Address)
	{
		uint32 Value = 0;
		Runtime->LoadAnyWord(Address, Value);
		return Value;
	};
	auto Real = [&Word](uint32 Address)
	{
		const uint32 Bits = Word(Address);
		float Value;
		FMemory::Memcpy(&Value, &Bits, 4);
		return Value;
	};
	Out.bVisible = true;
	Out.SimFrame = Runtime->GetSimFrame();
	Out.Overlay = &Runtime->GetHud();
	Out.Clock = Word(GameInfo.Handle + FOKGameInfo::Settings + 4) != 0 ? int32(Word(GameInfo.Handle + FOKGameInfo::Clock)) : -1;
	Out.RoundsToWin = FMath::Max(1, int32(Word(GameInfo.Handle + FOKGameInfo::Settings + 8)));
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const FOKFighter& Fighter = Side == 0 ? Me : Him;
		const FOKPlayer& Player = Players[Side];
		FOKHudSide& Shown = Out.Sides[Side];
		// The names player select shows, keyed by the package name without CHAR_: [PlayerSelect] subzero=SUB-ZERO
		const FString Key = Fighter.Name.StartsWith(TEXT("CHAR_")) ? Fighter.Name.Mid(5) : Fighter.Name;
		const FString* Name = FOKLocalization::Get().Find(TEXT("PlayerSelect"), Key);
		Shown.Name = Name ? *Name : Key.ToUpper();
		Shown.Life = FMath::Clamp(Fighter.Health, 0.0f, 1.0f);
		Shown.Meter = FMath::Clamp(Real(Player.Handle + FOKPlayer::Meter), 0.0f, 1.0f);
		Shown.RoundsWon = int32(Word(Player.Handle + FOKPlayer::RoundsWon));
		Shown.bPresent = Fighter.Handle != 0;
	}
}

bool UOKScriptWorld::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

// --- Console tools -----------------------------------------

/** The script world of the game that's playing (Play in Editor or a standalone game). */
static UOKScriptWorld* FindPlayingScriptWorld();

static FAutoConsoleCommand CmdInputHold(
	TEXT("ok.Input.Hold"),
	TEXT("ok.Input.Hold <side> <maskHex> <startInFrames> <frames>: hold input masks for a side, e.g. ok.Input.Hold 0 40 30 2 presses FP ")
	TEXT("from 30 frames from now for 2 frames. Masks: 1 up 2 down 4 left 8 right, 40 FP 10 BP 20 FK 80 BK, 8000 block."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOKScriptWorld* ScriptWorld = FindPlayingScriptWorld();
			if (!ScriptWorld || Args.Num() < 4)
			{
				UE_LOG(LogOKVM, Warning, TEXT("Press Play first. Usage: ok.Input.Hold 0 40 30 2"));
				return;
			}
			const int64 Now = ScriptWorld->GetSimFrame();
			ScriptWorld->HoldInput(FCString::Atoi(*Args[0]), FParse::HexNumber(*Args[1]), Now + FCString::Atoi64(*Args[2]), FCString::Atoi64(*Args[3]));
		}));

static UOKScriptWorld* FindPlayingScriptWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
		{
			if (UOKScriptWorld* ScriptWorld = World->GetSubsystem<UOKScriptWorld>())
			{
				return ScriptWorld;
			}
		}
	}
	return nullptr;
}

static FAutoConsoleCommand CmdCinemaPlay(
	TEXT("ok.Cinema.Play"),
	TEXT("ok.Cinema.Play <package> [attackerSide] [inFrames]: plays a cinema package (Scorpion_Fatality1) with that side's fighter as ")
	TEXT("its Attacker and the other as its Victim, from that many frames from now."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOKScriptWorld* ScriptWorld = FindPlayingScriptWorld();
			if (!ScriptWorld || Args.Num() < 1)
			{
				UE_LOG(LogOKVM, Warning, TEXT("Press Play first. Usage: ok.Cinema.Play Scorpion_Fatality1 0 60"));
				return;
			}
			ScriptWorld->PlayCinema(Args[0], Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 0, ScriptWorld->GetSimFrame() + (Args.Num() > 2 ? FCString::Atoi64(*Args[2]) : 0));
		}));

static FAutoConsoleCommand CmdVMSpawn(
	TEXT("ok.VM.Spawn"),
	TEXT("ok.VM.Spawn <Script> <Function> [group]: start a script process in the playing world"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOKScriptWorld* ScriptWorld = FindPlayingScriptWorld();
			const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
			if (!ScriptWorld || !ScriptWorld->GetRuntime() || !Scripts || !Scripts->GetLibrary() || Args.Num() < 2)
			{
				UE_LOG(LogOKVM, Warning, TEXT("Press Play first. Usage: ok.VM.Spawn EffectsEngine play_secret_fight_flashes"));
				return;
			}
			const FOKMkoFile* Script = Scripts->GetLibrary()->FindScript(Args[0] + TEXT(".mko"));
			const int32 Index = Script ? Script->FindFunctionByName(Args[1]) : INDEX_NONE;
			if (Index == INDEX_NONE)
			{
				UE_LOG(LogOKVM, Error, TEXT("Can't find %s.%s"), *Args[0], *Args[1]);
				return;
			}
			const int32 Group = Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 14;
			FString Error;
			if (!ScriptWorld->GetRuntime()->StartProcess(*Script, Index, 0, Group, true, Error))
			{
				UE_LOG(LogOKVM, Error, TEXT("Can't start %s.%s: %s"), *Args[0], *Args[1], *Error);
			}
		}));

static FAutoConsoleCommand CmdVMProcs(
	TEXT("ok.VM.Procs"),
	TEXT("List the script processes in the playing world."),
	FConsoleCommandDelegate::CreateLambda([]()
		{
			UOKScriptWorld* ScriptWorld = FindPlayingScriptWorld();
			if (!ScriptWorld || !ScriptWorld->GetRuntime())
			{
				UE_LOG(LogOKVM, Warning, TEXT("Press Play first."));
				return;
			}
			const FOKScriptRuntime& Runtime = *ScriptWorld->GetRuntime();
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: %d processes"), Runtime.GetSimFrame(), Runtime.NumProcesses());
			Runtime.ForEachProcess([](const FOKScriptThread& Thread)
				{
					const TCHAR* State = Thread.GetState() == FOKScriptThread::EState::Sleeping ? TEXT("sleeping")
						: Thread.GetState() == FOKScriptThread::EState::Running ? TEXT("running") : TEXT("ending");
					UE_LOG(LogOKVM, Log, TEXT(" process %d %s: %s, %g frames left, id %d, group %d, %d instructions so far"),
						Thread.GetNumber(), *Thread.GetStartName(), State, Thread.GetTimer(),
						Thread.GetProcId(), Thread.GetGroup(), Thread.GetInstructionsRun());
				});
		}));