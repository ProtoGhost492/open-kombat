#pragma once

#include "CoreMinimal.h"
#include "OKScriptObjects.h"

class FOKMkoFile;
class FOKScriptRuntime;
struct FOKScriptInstance;

DECLARE_LOG_CATEGORY_EXTERN(LogOKVM, Log, All);

/**
 * One MKScript thread (FIGHT-RE-NOTES §2.3): either a direct call (Run), or a process that the
 * runtime ticks once per sim frame, sleeping and waking like MKKE's coroutines.
 */
class OPENKOMBAT_API FOKScriptThread : public FOKScriptObject
{
public:
	/** MKKE's thread states (thread +0x2C): 0 dead, 1 running, 2 sleeping, 3 killed. */
	enum class EState : uint8 { Dead, Running, Sleeping, Killed };
	
	explicit FOKScriptThread(FOKScriptRuntime& InRuntime) : Runtime(InRuntime) {}

	/** Runs a function right now, as a direct call. A direct call can't sleep. */
	bool Run(const FOKMkoFile& Script, int32 FuncIndex, TConstArrayView<uint32> Args, uint32& OutResult, FString& OutError);

	/** Sets the thread up at a function, asleep with a zero timer, so it runs on its next Tick. */
	bool Start(const FOKMkoFile& Script, int32 FuncIndex, TConstArrayView<uint32> Args, FString& OutError);

	/** One frame: count the sleep timer down, then run until the thread sleeps, ends or fails. */
	void Tick();

	bool IsAlive() const { return State != EState::Dead; }
	EState GetState() const { return State; }
	float GetTimer() const { return Timer; }
	const FString& GetError() const { return Error; }
	const FString& GetEndReason() const { return EndReason; }
	const FString& GetStartName() const { return StartName; }
	int32 GetInstructionsRun() const { return InstructionsRun; }
	/** How many times the thread was switched to another function (TransferTo): a move or reaction took over. */
	int32 GetTransfers() const { return Transfers; }
	int32 GetMaxDepth() const { return MaxDepth; }

	// --- Process bookkeeping, set by the runtime ---------------------------------
	void SetCanSleep(bool bInCanSleep) { bCanSleep = bInCanSleep; }
	void SetProcess(int32 InNumber, int32 InProcId, int32 InGroup) { Number = InNumber; ProcId = InProcId; Group = InGroup; }
	void SetHandle(uint32 InHandle) { Handle = InHandle; }
	int32 GetNumber() const { return Number; }
	int32 GetProcId() const { return ProcId; }
	/** The fighter that adopted this thread (BindFighter), or 0. */
	uint32 GetOwner() const { return OwnerFighter; }
	int32 GetGroup() const { return Group; }
	uint32 GetHandle() const { return Handle; }

	/** Ends the thread at once (kill_proc). Safe to call on the thread that's running. */
	void KillNow(const FString& Reason) { State = EState::Dead; EndReason = Reason; }

	// --- For natives: valid only while a native is running ----------------------
	int32 NumNativeArgs() const { return int32(NativeOps.Count); }
	/** The native being called, as (module << 16) | index, for natives that share one implementation. */
	uint32 GetNativeId() const { return CurrentNativeId; }
	bool ArgRaw(int32 Index, uint32 & Out) { return ReadRaw(NativeOps, uint32(Index), Out); }
	bool ArgValue(int32 Index, uint32 & Out) { return ReadOperand(NativeOps, uint32(Index), Out); }
	FOKFighter* ArgFighter(int32 Index);
	uint32 ArgFrameAddress(int32 Index);
	FOKFighter* MyFighter();
	const FOKMkoFile* GetCurrentScript() const;
	/** The running script instance as a tagged value, like FUN_00459D70 returns. */
	uint32 CurrentInstanceValue() const;
	bool LoadBytes(uint32 Address, void* Out, int32 Size);
	bool StoreBytes(uint32 Address, const void* In, int32 Size);
	void SetResult(uint32 Value) { Ret = Value; }
	FOKScriptRuntime& GetRuntime() { return Runtime; }
	bool NativeFail(const FString& Why) { return Fail(Why); }

	/** sleep(): wait this many frames. Fails for a thread that can't sleep, as MKKE kills it. */
	bool SleepFor(uint32 Ticks);
	/** die(): end this thread when the current instruction finishes. */
	void Die() { State = EState::Killed; EndReason = TEXT("die()"); }

	virtual FOKScriptThread* AsThread() override { return this; }

	/** Binds an object-reference name for this thread only (MKKE's per-thread map, MKScriptVM +0x9C). */
	void Bind(const FString& RefName, uint32 Handle);
	/** A fighter's threads: @my_obj is the fighter, @his_obj its opponent (the fighter adopting the thread). */
	void BindFighter(const FOKFighter& Fighter);

	/** XferToReaction (FUN_0045C610): drop everything and continue at a function from the next tick. */
	bool TransferTo(FOKScriptInstance& Instance, int32 FuncIndex);

	/** Attach an object to this process (95.5). find_proc (1.22) returns the first one still alive. */
	void AttachObject(uint32 ObjectHandle) { Attached.AddUnique(ObjectHandle); }
	const TArray<uint32>& GetAttached() const { return Attached; }
	uint32 GetFirstAttachedObject() const;

	/**
	 * Calls a function of any script instance as the call instruction does (FUN_0045C090): it runs next, then
	 * returns to the instruction after the one running now. The vm_call natives use it.
	 */
	bool CallFunction(FOKScriptInstance& Instance, int32 FuncIndex, TConstArrayView<uint32> Args);

	/** jump(): drop every call and continue at a function, in the same frame (FUN_0045C590). */
	bool JumpTo(uint32 FunctionRef);

	/** loop_sleep(): sleep, then run the current function again from the top (FUN_0045B260). */
	bool LoopSleep(uint32 Ticks);

private:
	struct FOperands
	{
		uint32 Mode = 0;
		int64 FirstWord = 0;
		uint32 Count = 0;
	};

	struct FCallRecord
	{
		FOKScriptInstance* Instance = nullptr;
		int32 Function = INDEX_NONE;
		int64 ReturnPc = 0;
		uint32 CallLine = 0;
		int32 Frame = 0;
		int32 LocalsEnd = 0;
		int32 Top = 0;
	};

	bool EnterFunction(FOKScriptInstance& Instance, int32 FuncIndex, TConstArrayView<uint32> Args);
	bool ReturnFromFunction();
	bool Call(FOKScriptInstance& Instance, int32 FuncIndex, const FOperands& Ops);

	bool Execute(int32 Op, const FOperands& Ops);
	bool CodeWord(int64 WordIndex, uint32& Out) const;
	bool ReadOperand(const FOperands& Ops, uint32 Index, uint32& Out);
	bool WriteOperand(const FOperands& Ops, uint32 Index, uint32 Value);
	bool ReadRaw(const FOperands& Ops, uint32 Index, uint32& Out);
	uint8* Resolve(uint32 Address, int32 Size);
	uint32 AddressOf(uint32 Raw) const;
	uint32 PoolAddress(uint32 PoolOffset) const;
	bool Load32(uint32 Address, uint32& Out) { return LoadBytes(Address, &Out, 4); }
	bool Store32(uint32 Address, uint32 Value) { return StoreBytes(Address, &Value, 4); }
	bool Fail(const FString& Why);
	static FString Describe(const FOKScriptInstance& Instance, int32 FuncIndex);

	static float AsFloat(uint32 Bits);
	static uint32 FromFloat(float Value);

	FOKScriptRuntime& Runtime;

	// Where we are
	FOKScriptInstance* Current = nullptr;
	int32 CurrentFunction = INDEX_NONE;
	uint32 CurrentNativeId = 0;		// the native being called (GetNativeId)
	int64 CurrentEnd = 0;
	uint32 CurrentLine = 0;
	TArray<FCallRecord> Calls;

	TArray<uint8> Stack;			
	int32 Frame = 0;				
	int32 LocalsEnd = 0;			
	int32 Top = 0;					
	int64 Pc = 0;					
	uint32 Ret = 0;					
	int32 InstructionsRun = 0;
	int32 Transfers = 0;
	int32 MaxDepth = 0;
	FString Error;
	FOperands NativeOps;			

	// NEW: life over time
	EState State = EState::Dead;
	float Timer = 0.0f;				// frames left to sleep
	float Rate = 1.0f;				// how much the timer drops each frame (thread +0x68)
	bool bCanSleep = true;			// false = no stack of its own (MKKE stack mode 0)
	int32 Number = 0;				// OpenKombat's process number, for logs
	int32 ProcId = 0;				// the script's process id (thread +0x50)
	uint32 OwnerFighter = 0;		// the fighter that adopted it
	int32 Group = 14;				// priority group 0-20 (thread +0x44)
	uint32 Handle = 0;
	FString StartName;
	FString EndReason;

	bool ResolveFunctionRef(uint32 Raw, FOKScriptInstance*& OutInstance, int32& OutFunction);
	bool RestartAt(FOKScriptInstance& Instance, int32 FuncIndex);

	TArray<uint32> Bound;			// by binding id; 0 = use the runtime's binding

	/** An object reference's value for this thread: its own binding, or the runtime's. */
	uint32 RefValue(int32 Ref) const;

	TArray<uint32> Attached;		// objects attached to this process (process data)
};
