#pragma once

#include "CoreMinimal.h"

class FOKScriptRuntime;
class FOKScriptThread;
class FOKFighter;
class FOKPlayer;

/** A native reads its arguments from the thread. Return false (after Thread.NativeFall) to stop the script. */
using FOKNativeFn = bool (*)(FOKScriptThread& Thread);

/**
 * Fires the fighter's armed frame events that its animation reached between PreviousFrame and Frame
 * (FUN_008A0400): each starts a script process with an MKAnimEventPdata. Called once per sim frame.
 */
OPENKOMBAT_API void OKFireFrameEvents(FOKScriptRuntime& Runtime, FOKFighter& Fighter, float PreviousFrame, float Frame, float Rate, bool bLooping);

/**
 * Starts a fighter's character script: builds its moveset from moveset_<char> and runs <char>_game_init.
 * Call once the fighter knows its character script.
 */
OPENKOMBAT_API bool OKInitCharacter(FOKScriptRuntime& Runtime, FOKFighter& Fighter, FString& OutError);

/** FightPlayers::Update (FUN_008ACD90): each human player's commands and buttons, once a frame before the scripts tick. */
OPENKOMBAT_API void OKUpdateFightPlayers(FOKScriptRuntime& Runtime, TConstArrayView<FOKFighter*> Fighters);

/**
 * XferToReaction with a script and function as scripts store them (a script value, and a function 1-based in it or an
 * import): the fighter's own thread continues there. Landings use it.
 */
OPENKOMBAT_API bool OKTransferMainThread(FOKScriptRuntime& Runtime, FOKFighter& Fighter, uint32 ScriptValue, uint32 FunctionRaw);

/** The same, by names (FightEngineScript, sleep_forever). The script must have an instance. */
OPENKOMBAT_API bool OKTransferMainThreadTo(FOKScriptRuntime& Runtime, FOKFighter& Fighter, const FString& ScriptName, const FString& FunctionName);

/**
 * A throw's link (F10), once a frame after both fighters have animated: the follower shows its leader's frame, and the
 * link ends when the leader plays something else (MKAnimTrackMatcher, FUN_00490900).
 */
OPENKOMBAT_API void OKUpdateThrow(FOKScriptRuntime& Runtime, FOKFighter& Follower);

/**
 * Ends a throw's link: both get their flags back. bCleanup: the throw was cut short, so the victim takes the cleanup reaction
 * the attack set, if any; set_throw_id lets go without it, sending the victim to a reaction of its own.
 */
OPENKOMBAT_API void OKReleaseThrow(FOKScriptRuntime& Runtime, FOKFighter& Follower, bool bCleanup);

/** FUN_00895A00: the fighter can start a move now: alive, in control, not in a locked state, and past its wait. */
OPENKOMBAT_API bool OKCanAct(FOKScriptRuntime& Runtime, const FOKFighter& Fighter);

/** AddMeter (FUN_0089CE40): the player's meter changes by Delta, kept within 0 and 1. */
OPENKOMBAT_API void OKAddMeter(FOKScriptRuntime& Runtime, FOKPlayer& Player, float Delta);

/**
 * Resolves a contact of the attacker's live attack with the victim (ResolveHit, FUN_008AC0F0): the outcome into the
 * CollisionContext, the damage and the victim's reaction. collision_scan and collision objects use it.
 */
OPENKOMBAT_API void OKResolveContact(FOKScriptRuntime& Runtime, FOKFighter& Attacker, FOKFighter& Victim, uint32 Context);

/** OpenKombat's implementations of MKKE natives, keyed by module number and index (e.g. 137.259). */
class OPENKOMBAT_API FOKNativeTable
{
public:
	/** The built-in table, filled the first time it's asked for. */
	static const FOKNativeTable& Get();

	FOKNativeFn Find(int32 Module, int32 Index) const;
	int32 Num() const { return Natives.Num(); }

	/** Notes that a native ran (for the progress chart). */
	static void NoteRun(uint32 NativeId);
	/**
	 * Writes Saved/OKNatives.tsv: each registered native, whether it's a real one or a stub (NoOp, ReturnZero ...), and
	 * whether it has run in a fight here (kept from earlier runs). The RE repo's progress chart reads it.
	 */
	void WriteReport() const;

private:
	void Add(int32 Module, int32 Index, FOKNativeFn Fn);

	TMap<uint32, FOKNativeFn> Natives;		// (module << 16) | index
};
