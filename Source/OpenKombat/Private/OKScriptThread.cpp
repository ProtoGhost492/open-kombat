#include "OKScriptThread.h"
#include "OKMkoFile.h"
#include "OKScriptRuntime.h"
#include "OKScriptSubsystem.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "OKNativeNames.h"
#include "OKNatives.h"
#include "OKScriptObjects.h"

static TAutoConsoleVariable<int32> CVarTraceNatives(TEXT("ok.VM.TraceNatives"), 0,
	TEXT("1: log every native call with the script function and line it came from"));

DEFINE_LOG_CATEGORY(LogOKVM);

namespace
{
	constexpr int32 StackBytes = 0x2000;
	constexpr int32 MaxInstructions = 1000000;
}

float FOKScriptThread::AsFloat(uint32 Bits)
{
	float Value;
	FMemory::Memcpy(&Value, &Bits, 4);
	return Value;
}

uint32 FOKScriptThread::FromFloat(float Value)
{
	uint32 Bits;
	FMemory::Memcpy(&Bits, &Value, 4);
	return Bits;
}

FString FOKScriptThread::Describe(const FOKScriptInstance& Instance, int32 FuncIndex)
{
	return FPaths::GetBaseFilename(Instance.File->GetName()) + TEXT(".") + Instance.File->GetFunctions()[FuncIndex].Name;
}

uint32 FOKScriptThread::GetFirstAttachedObject() const
{
	for (const uint32 ObjectHandle : Attached)
	{
		if (Runtime.ResolveHandle(ObjectHandle))
		{
			return ObjectHandle;
		}
	}
	return 0;
}

bool FOKScriptThread::ResolveFunctionRef(uint32 Raw, FOKScriptInstance*& OutInstance, int32& OutFunction)
{
	if (Raw & 0x80000000)
	{
		// An imported function: another script.
		const TArray<FOKMkoImport>& Imports = Current->File->GetImports();
		const int32 ImportIndex = int32(Raw & 0x3FFFFFFF) - 1;
		if (!Imports.IsValidIndex(ImportIndex) || !Imports[ImportIndex].Target)
		{
			return Fail(TEXT("jump to an import that isn't linked"));
		}
		OutInstance = Runtime.GetInstance(*Imports[ImportIndex].Target);
		OutFunction = Imports[ImportIndex].TargetFunction;
		return OutInstance ? true : Fail(TEXT("too many script instances"));
	}

	// A function in this script, 1-based.
	OutInstance = Current;
	OutFunction = int32(Raw) - 1;
	return Current->File->GetFunctions().IsValidIndex(OutFunction) ? true : Fail(TEXT("jump to a function that doesn't exist"));
}

uint32 FOKScriptThread::RefValue(int32 Ref) const
{
	const int32 Id = Current->RefBindingIds.IsValidIndex(Ref) ? Current->RefBindingIds[Ref] : INDEX_NONE;
	if (Bound.IsValidIndex(Id) && Bound[Id] != 0)
	{
		return Bound[Id];
	}
	return Current->RefHandles[Ref];
}

void FOKScriptThread::Bind(const FString& RefName, uint32 InHandle)
{
	const int32 Id = Runtime.BindingId(RefName);
	if (Bound.Num() <= Id)
	{
		Bound.SetNumZeroed(Id + 1);
	}
	Bound[Id] = InHandle;
}

void FOKScriptThread::BindFighter(const FOKFighter& Fighter)
{
	OwnerFighter = Fighter.Handle;
	Bind(TEXT("my_obj"), Fighter.Handle);
	Bind(TEXT("his_obj"), Fighter.Opponent ? Fighter.Opponent->Handle : 0);
	Bind(TEXT("my_info"), Fighter.Player ? Fighter.Player->Handle : 0);
	Bind(TEXT("his_info"), (Fighter.Opponent && Fighter.Opponent->Player) ? Fighter.Opponent->Player->Handle : 0);
}

bool FOKScriptThread::TransferTo(FOKScriptInstance& Instance, int32 FuncIndex)
{
	if (!Instance.File->GetFunctions().IsValidIndex(FuncIndex) || !RestartAt(Instance, FuncIndex))
	{
		return false;
	}
	StartName = Describe(Instance, FuncIndex);
	State = EState::Sleeping;
	Timer = 0.0f;
	++Transfers;
	return true;
}

bool FOKScriptThread::RestartAt(FOKScriptInstance& Instance, int32 FuncIndex)
{
	// FUN_0045C4C0 then FUN_0045A570: unwind every call, clear the stack, enter the function fresh.
	Calls.Reset();
	Top = 0;
	FMemory::Memzero(Stack.GetData(), Stack.Num());
	return EnterFunction(Instance, FuncIndex, TConstArrayView<uint32>());
}

bool FOKScriptThread::JumpTo(uint32 FunctionRef)
{
	FOKScriptInstance* Instance = nullptr;
	int32 Function = INDEX_NONE;
	if (!ResolveFunctionRef(FunctionRef, Instance, Function) || !RestartAt(*Instance, Function))
	{
		return false;
	}
	Timer = 0.0f;		// MKKE yields with a zero timer, which means: carry on running now
	return true;
}

bool FOKScriptThread::LoopSleep(uint32 Ticks)
{
	if (Calls.Num() > 0)
	{
		// MKKE kills a thread that calls loop_sleep from inside a call.
		State = EState::Killed;
		EndReason = TEXT("loop_sleep inside a call: MKKE kills the thread");
		return true;
	}
	// Unlike sleep, loop_sleep doesn't care whether the process has a stack of its own (FUN_0045B260): a process
	// started with stack class 0 (p_anim_event_scanner) loops with it.
	if (!RestartAt(*Current, CurrentFunction))
	{
		return false;
	}
	Timer = float(Ticks);
	if (State == EState::Running)
	{
		State = EState::Sleeping;
	}
	return true;
}

bool FOKScriptThread::Fail(const FString& Why)
{
	if (Error.IsEmpty())
	{
		Error = Why;
	}
	return false;
}

bool FOKScriptThread::SleepFor(uint32 Ticks)
{
	if (!bCanSleep)
	{
		// FUN_00458320: a thread with no stack of its own is killed if it tries to sleep.
		return Fail(TEXT("sleep in a thread that can't sleep (a direct call): MKKE kills it"));
	}
	Timer = float(Ticks);		// unsigned, like MKKE
	if (State == EState::Running)
	{
		State = EState::Sleeping;
	}
	return true;
}

bool FOKScriptThread::CodeWord(int64 WordIndex, uint32& Out) const
{
	// NEW: code comes from the running instance, which setarg may have patched.
	if (!Current || WordIndex < 0 || WordIndex >= Current->Code.Num())
	{
		return false;
	}
	Out = Current->Code[WordIndex];
	return true;
}

uint8* FOKScriptThread::Resolve(uint32 Address, int32 Size)
{
	const uint32 Tag = Address >> 24;
	const int64 Offset = Address & 0x00FFFFFF;

	if (Tag == FOKScriptRuntime::StackTag)
	{
		return Offset + Size <= Stack.Num() ? Stack.GetData() + Offset : nullptr;
	}
	if (Tag == FOKScriptRuntime::HeapTag)
	{
		return Runtime.HeapBytes(uint32(Offset), Size);
	}
	if (Tag > FOKScriptRuntime::FirstMemoryTag)
	{
		FOKScriptInstance* Owner = Runtime.FindInstanceById(int32(Tag - FOKScriptRuntime::FirstMemoryTag));
		return (Owner && Offset + Size <= Owner->Memory.Num()) ? Owner->Memory.GetData() + Offset : nullptr;
	}
	return nullptr;			// not a valid address: object handles, small numbers, garbage
}

bool FOKScriptThread::LoadBytes(uint32 Address, void* Out, int32 Size)
{
	const uint8* Where = Resolve(Address, Size);
	if (!Where)
	{
		return Fail(FString::Printf(TEXT("%08X isn't a valid address"), Address));
	}
	FMemory::Memcpy(Out, Where, Size);
	return true;
}

bool FOKScriptThread::StoreBytes(uint32 Address, const void* In, int32 Size)
{
	uint8* Where = Resolve(Address, Size);
	if (!Where)
	{
		return Fail(FString::Printf(TEXT("can't write to %08X: not a valid address"), Address));
	}
	FMemory::Memcpy(Where, In, Size);
	return true;
}

uint32 FOKScriptThread::PoolAddress(uint32 PoolOffset) const
{
	return FOKScriptRuntime::MemoryAddress(Current->Id, Current->PoolBase + PoolOffset);
}

uint32 FOKScriptThread::AddressOf(uint32 Raw) const
{
	// The address an operand names: a pool loation (bit 30) or a frame location. Object refs have none.
	if (Raw & 0x80000000)
	{
		return 0;
	}
	return (Raw & 0x40000000) ? PoolAddress(Raw & 0x3FFFFFFF) : FOKScriptRuntime::StackAddress(uint32(Frame) + Raw);
}

uint32 FOKScriptThread::ArgFrameAddress(int32 Index)
{
	uint32 Raw = 0;
	return ArgRaw(Index, Raw) ? FOKScriptRuntime::StackAddress(uint32(Frame) + Raw) : 0;
}

FOKFighter* FOKScriptThread::MyFighter()
{
	// Like FUN_00492E20: look up "my_obj" in the running script.
	const int32 Ref = Current->File->GetObjectRefs().IndexOfByKey(FString(TEXT("my_obj")));
	if (!Current->RefHandles.IsValidIndex(Ref))
	{
		return nullptr;
	}
	FOKScriptObject* Object = Runtime.ResolveHandle(RefValue(Ref));
	return Object ? Object->AsFighter() : nullptr;
}

FOKFighter* FOKScriptThread::ArgFighter(int32 Index)
{
	uint32 ObjectHandle = 0;
	if (!ArgValue(Index, ObjectHandle))
	{
		return nullptr;
	}
	FOKScriptObject* Object = Runtime.ResolveHandle(ObjectHandle);
	return Object ? Object->AsFighter() : nullptr;
}

bool FOKScriptThread::ReadRaw(const FOperands& Ops, uint32 Index, uint32& Out)
{
	if (Index >= Ops.Count || !CodeWord(Ops.FirstWord + Index, Out))
	{
		return Fail(FString::Printf(TEXT("operand %u is missing"), Index));
	}
	return true;
}

bool FOKScriptThread::ReadOperand(const FOperands& Ops, uint32 Index, uint32& Out)
{
	uint32 Raw = 0;
	if (!ReadRaw(Ops, Index, Raw))
	{
		return false;
	}

	// The engine only honours mode bits for operands 0-2; later operands are always plain values.
	const bool bLiteral = Index > 2 || ((Ops.Mode >> (2 * Index)) & 1);
	const bool bIndirect = Index <= 2 && ((Ops.Mode >> (2 * Index + 1)) & 1);
	if (bLiteral)
	{
		Out = Raw;
		return true;
	}

	if (Raw & 0x80000000)
	{
		// Object reference: entry n of this script's table (my_obj, his_obj ...), bound to a handle.
		// Indirect reads the object's first word.
		const int32 Ref = int32(Raw & 0x3FFFFFFF) - 1;
		if (!Current->RefHandles.IsValidIndex(Ref))
		{
			return Fail(FString::Printf(TEXT("object reference %d doesn't exist"), Ref + 1));
		}
		Out = RefValue(Ref);
		return bIndirect ? Load32(Out, Out) : true;
	}

	// A frame or pool location; if indirect, it holds a pointer to the value.
	if (!Load32(AddressOf(Raw), Out))
	{
		return false;
	}
	return bIndirect ? Load32(Out, Out) : true;
}

bool FOKScriptThread::WriteOperand(const FOperands& Ops, uint32 Index, uint32 Value)
{
	uint32 Raw = 0;
	if (!ReadRaw(Ops, Index, Raw))
	{
		return false;
	}
	const bool bIndirect = Index <= 2 && ((Ops.Mode >> (2 * Index + 1)) & 1);
	if (Raw & 0x80000000)
	{
		// Writing through it stores the object's first word. A plain write sets the reference itself: an engine global
		// the scripts keep as a value (mov @GXRay_E3_Hack, 1). [inf] MKKE's reference points at the variable; here each
		// script instance holds its own copy, which serves while one script both writes and reads it.
		const int32 Ref = int32(Raw & 0x3FFFFFFF) - 1;
		if (!Current->RefHandles.IsValidIndex(Ref))
		{
			return Fail(FString::Printf(TEXT("object reference %d doesn't exist"), Ref + 1));
		}
		if (!bIndirect)
		{
			Current->RefHandles[Ref] = Value;
			return true;
		}
		return Store32(RefValue(Ref), Value);
	}
	
	uint32 Address = AddressOf(Raw);
	if (bIndirect && !Load32(Address, Address))		// the location holds the real destination
	{
		return false;
	}
	return Store32(Address, Value);
}

// NEW: frames and calls -------------------------------------------------

const FOKMkoFile* FOKScriptThread::GetCurrentScript() const
{
	return Current ? Current->File : nullptr;
}

uint32 FOKScriptThread::CurrentInstanceValue() const
{
	return Current ? (FOKScriptRuntime::InstanceTag << 24) | uint32(Current->Id) : 0;
}

bool FOKScriptThread::EnterFunction(FOKScriptInstance& Instance, int32 FuncIndex, TConstArrayView<uint32> Args)
{
	const FOKMkoFunction& Fn = Instance.File->GetFunctions()[FuncIndex];

	// The new frame starts where the caller's ends (FUN_0045C090): locals, 8-byte temporaries,
	// then 16 bytes MKKE uses for the return record. We keep the record in Calls but reserve
	// the space, so frames are the same size and stack overflows happen at the same depth.
	const int32 NewFrame = Top;
	const int32 NewLocalsEnd = NewFrame + int32(Fn.LocalsBytes);
	const int64 NewTop = int64(NewLocalsEnd) + int64(Fn.NumTemps) * 8 + 16;
	if (NewTop > Stack.Num())
	{
		return Fail(TEXT("script stack overflow: calls nested too deeply"));
	}

	FMemory::Memzero(Stack.GetData() + NewFrame, int32(NewTop) - NewFrame);

	const int64 LocalsStart = int64(Instance.PoolBase) + Fn.LocalsPoolOffset;
	const int32 StoredLocals = int32(FMath::Clamp<int64>(Instance.Memory.Num() - LocalsStart, 0, Fn.LocalsBytes));
	FMemory::Memcpy(Stack.GetData() + NewFrame, Instance.Memory.GetData() + LocalsStart, StoredLocals);

	if (Args.Num() <= int32(Fn.MaxArgs))			// MKKE silently skips the arguments if there are too many
	{
		FMemory::Memcpy(Stack.GetData() + NewFrame, Args.GetData(), Args.Num() * sizeof(uint32));
	}

	Current = &Instance;
	CurrentFunction = FuncIndex;
	CurrentEnd = int64(Fn.CodeWord) + Fn.CodeBytes / 4;
	Frame = NewFrame;
	LocalsEnd = NewLocalsEnd;
	Top = int32(NewTop);
	Pc = Fn.CodeWord;
	return true;
}

bool FOKScriptThread::Call(FOKScriptInstance& Instance, int32 FuncIndex, const FOperands& Ops)
{
	if (!Instance.File->GetFunctions().IsValidIndex(FuncIndex))
	{
		return Fail(TEXT("call to a function that doesn't exist"));
	}

	// Arguments are the raw operand words after the target, which setarg filled in just before.
	// Read them now, while the caller's code is still current.
	TArray<uint32, TInlineAllocator<16>> Args;
	for (uint32 k = 1; k < Ops.Count; ++k)
	{
		uint32 Word = 0;
		if (!ReadRaw(Ops, k, Word))
		{
			return false;
		}
		Args.Add(Word);
	}
	return CallFunction(Instance, FuncIndex, Args);
}

bool FOKScriptThread::CallFunction(FOKScriptInstance& Instance, int32 FuncIndex, TConstArrayView<uint32> Args)
{
	if (!Instance.File->GetFunctions().IsValidIndex(FuncIndex))
	{
		return Fail(TEXT("call to a function that doesn't exist"));
	}
	FCallRecord& Saved = Calls.AddDefaulted_GetRef();
	Saved.Instance = Current;
	Saved.Function = CurrentFunction;
	Saved.ReturnPc = Pc;			// already past the call instruction
	Saved.CallLine = CurrentLine;
	Saved.Frame = Frame;
	Saved.LocalsEnd = LocalsEnd;
	Saved.Top = Top;
	MaxDepth = FMath::Max(MaxDepth, Calls.Num());

	if (!EnterFunction(Instance, FuncIndex, Args))
	{
		Calls.Pop();
		return false;
	}
	return true;
}

bool FOKScriptThread::ReturnFromFunction()
{
	if (Calls.Num() == 0)
	{
		return false;			// the outermost function returned
	}
	const FCallRecord Saved = Calls.Pop();
	Current = Saved.Instance;
	CurrentFunction = Saved.Function;
	CurrentLine = Saved.CallLine;
	Pc = Saved.ReturnPc;
	Frame = Saved.Frame;
	LocalsEnd = Saved.LocalsEnd;
	Top = Saved.Top;

	const FOKMkoFunction& Fn = Current->File->GetFunctions()[CurrentFunction];
	CurrentEnd = int64(Fn.CodeWord) + Fn.CodeBytes / 4;
	return true;
}

// --------------------------------------------------------------------------

bool FOKScriptThread::Start(const FOKMkoFile& Script, int32 FuncIndex, TConstArrayView<uint32> Args, FString& OutError)
{
	if (!Script.GetFunctions().IsValidIndex(FuncIndex))
	{
		OutError = TEXT("no such function");
		return false;
	}
	const FOKMkoFunction& Fn = Script.GetFunctions()[FuncIndex];
	if (Args.Num() > int32(Fn.MaxArgs))
	{
		OutError = FString::Printf(TEXT("% s takes at most %u arguments"), *Fn.Name, Fn.MaxArgs);
		return false;
	}

	Stack.Reset();
	// [inf] A direct call gets at least its function's frame: __global__ declares about 120 KB of locals (most of
	// them the globals it constructs). MKKE runs it on a process with no stack of its own; how it fits isn't read.
	Stack.SetNumZeroed(FMath::Max<int64>(StackBytes, int64(Fn.LocalsBytes) + int64(Fn.NumTemps) * 8 + 16));
	Calls.Reset();
	Attached.Reset();
	Ret = 0;
	InstructionsRun = 0;
	MaxDepth = 0;
	Top = 0;
	Error.Reset();
	EndReason.Reset();

	FOKScriptInstance* First = Runtime.GetInstance(Script);
	if (!First || !EnterFunction(*First, FuncIndex, Args))
	{
		OutError = First ? Error : TEXT("too many script instances");
		State = EState::Dead;
		return false;
	}
	StartName = Describe(*First, FuncIndex);

	// Like a new MKKE process (FUN_0045CB20): asleep with a zero timer, so it runs
	// the next time its group is ticked.
	State = EState::Sleeping;
	Timer = 0.0f;
	return true;
}

void FOKScriptThread::Tick()
{
	if (State == EState::Sleeping)
	{
		// FUN_0045C700: count the timer down by the thread's rate; wake (this same frame) at zero.
		if (Timer > 0.0f)
		{
			Timer = FMath::Max(0.0f, Timer - Rate);
		}
		if (Timer <= 0.0f)
		{
			State = EState::Running;
		}
	}

	int32 ThisTick = 0;
	while (State == EState::Running)
	{
		uint32 Handler = 0;
		if (Pc < CurrentEnd && !CodeWord(Pc, Handler))
		{
			Fail(TEXT("code runs past the end of the file"));
			State = EState::Dead;
			break;
		}
		if (Pc >= CurrentEnd || Handler == 0)
		{
			// A zero word (or the end of the function) returns to the caller.
			if (!ReturnFromFunction())
			{
				State = EState::Dead; // the outermost function returned: the thread is done
				EndReason = TEXT("returned");
			}
			continue;
		}
		if (++ThisTick > MaxInstructions)
		{
			Fail(TEXT("ran 1,000,000 instructions in one frame without sleeping (infinite loop?)"));
			State = EState::Dead;
			break;
		}
		++InstructionsRun;

		FOperands Ops;
		uint32 LineAndCount = 0;
		if (!CodeWord(Pc + 1, Ops.Mode) || !CodeWord(Pc + 2, LineAndCount))
		{
			Fail(TEXT("instruction header runs past the end of the file"));
			State = EState::Dead;
			break;
		}
		Ops.Count = LineAndCount & 0xFFFF;
		Ops.FirstWord = Pc + 3;
		CurrentLine = LineAndCount >> 16;

		const int64 InstructionPc = Pc;
		Pc += 3 + Ops.Count;

		const int32 Op = int32(Handler & 0xFFFF) - 1;
		if ((Handler >> 16) != 0 || !Execute(Op, Ops))
		{
			Fail(TEXT("unknown handler"));
			FString Trace = FString::Printf(TEXT("at %05llx (%s) in %s, line %u: %s"), InstructionPc,
				FOKMkoFile::GetOpName(Op), *Describe(*Current, CurrentFunction), CurrentLine, *Error);
			for (int32 i = Calls.Num() - 1; i >= 0; --i)
			{
				Trace += FString::Printf(TEXT("\n   called from %s, line %u"), *Describe(*Calls[i].Instance, Calls[i].Function), Calls[i].CallLine);
			}
			Error = Trace;
			State = EState::Dead;
			break;
		}
		// After each instruction, the loop condition checks whether it slept, died or was killed.
	}

	if (State == EState::Killed)
	{
		State = EState::Dead;	// die (): the thread ends straight away
	}
}

bool FOKScriptThread::Run(const FOKMkoFile& Script, int32 FuncIndex, TConstArrayView<uint32> Args, uint32& OutResult, FString& OutError)
{
	bCanSleep = false;		// a direct call has no stack of its own, like an MKKE stack-mode-0 thread
	if (!Start(Script, FuncIndex, Args, OutError))
	{
		return false;
	}
	State = EState::Running;
	Tick();
	if (!Error.IsEmpty())
	{
		OutError = Error;
		return false;
	}
	OutResult = Ret;
	return true;
}

bool FOKScriptThread::Execute(int32 Op, const FOperands& Ops)
{
	uint32 A = 0;
	uint32 B = 0;

	// Comparisons and logic also set the return register, which jz tests.
	auto Compare = [&](bool bResult)
		{
			Ret = bResult ? 1u : 0u;
			return WriteOperand(Ops, 0, Ret);
		};

	// Binary ops (8-55): dst = A op B, with A = operand 1 and B = operand 2.
	if (Op >= 8 && Op <= 55)
	{
		if (!ReadOperand(Ops, 1, A) || !ReadOperand(Ops, 2, B))
		{
			return false;
		}
	}
	const int32 SA = int32(A), SB = int32(B);
	const float FA = AsFloat(A), FB = AsFloat(B);

	switch (Op)
	{
	case 0: // NEW: setarg value, slot: write value into the code, Slot words past this instruction
	{
		uint32 Slot = 0;
		if (!ReadOperand(Ops, 0, A) || !ReadRaw(Ops, 1, Slot))
		{
			return false;
		}
		const int64 Target = Pc + int32(Slot);		// Pc already points past this instruction
		if (Target < 0 || Target >= Current->Code.Num())
		{
			return Fail(TEXT("setarg slot is outside the code"));
		}
		Current->Code[Target] = A;
		return true;
	}
	case 1: // mov dst, src. Mode 0x40 = byte move, 0x80 = 16-bit move; other bits are kept.
	{
		uint32 Old = 0;
		if (!ReadOperand(Ops, 0, Old) || !ReadOperand(Ops, 1, A))
		{
			return false;
		}
		const uint32 Mask = (Ops.Mode & 0x40) ? 0xFFu : (Ops.Mode & 0x80) ? 0xFFFFu : 0xFFFFFFFFu;
		return WriteOperand(Ops, 0, (Old & ~Mask) | (A & Mask));
	}
	case 2: // sttmp offset, value: store into the frame; MKKE ignores stores past the locals
	case 3: // sttmpd offset, float: the same, as an 8-byte double
	{
		uint32 Offset = 0;
		if (!ReadRaw(Ops, 0, Offset) || !ReadOperand(Ops, 1, A))
		{
			return false;
		}
		const int32 Size = (Op == 3) ? 8 : 4;
		if (int64(Frame) + Offset + Size > LocalsEnd)
		{
			return true;
		}
		if (Op == 3)
		{
			const double AsDouble = AsFloat(A);
			return StoreBytes(FOKScriptRuntime::StackAddress(uint32(Frame) + Offset), &AsDouble, 8);
		}
		return Store32(FOKScriptRuntime::StackAddress(uint32(Frame) + Offset), A);
	}
	case 4: return WriteOperand(Ops, 0, Ret);			// getret dst
	case 5: return ReadOperand(Ops, 0, Ret);			// setret value

	case 6: // NEW: ldidx dst, base, index, size: the address of base[index], or (indirect) the value there
	{
		uint32 BaseRaw = 0, Index = 0, ElementSize = 0;
		if (!ReadRaw(Ops, 1, BaseRaw) || !ReadOperand(Ops, 2, Index) || !ReadRaw(Ops, 3, ElementSize))
		{
			return false;
		}
		const bool bUseLocationItself = (Ops.Mode & 0x04) != 0;		// operand 1's literal bit
		const bool bReadValue = (Ops.Mode & 0x08) != 0;				// operand 1's indirect bit

		uint32 Base = 0;
		if (BaseRaw & 0x80000000)
		{
			// An object reference: the base is the object itself (FUN_00458B00). For an object with
			// memory, its handle is the address of its fields.
			const int32 Ref = int32(BaseRaw & 0x3FFFFFFF) - 1;
			if (bUseLocationItself || !Current->RefHandles.IsValidIndex(Ref))
			{
				return Fail(TEXT("ldidx into an object reference's own slot isn't supported"));
			}
			Base = RefValue(Ref);
		}
		else if (BaseRaw & 0x40000000)
		{
			Base = PoolAddress(BaseRaw & 0x3FFFFFFF);
			if (!bUseLocationItself && !Load32(Base, Base))
			{
				return false;
			}
		}
		else if (bUseLocationItself)
		{
			Base = BaseRaw;		// MKKE uses the raw word itself as the address
		}
		else if (!Load32(FOKScriptRuntime::StackAddress(uint32(Frame) + BaseRaw), Base))
		{
			return false;
		}

		const uint32 Element = Base + ElementSize * Index;
		if (!bReadValue)
		{
			return WriteOperand(Ops, 0, Element);
		}
		if (Base == 0)
		{
			return true;	// MKKE skips the read through a null pointer
		}
		if (Ops.Mode & 0x40)
		{
			int8 Byte = 0;
			return LoadBytes(Element, &Byte, 1) && WriteOperand(Ops, 0, uint32(int32(Byte)));
		}
		if (Ops.Mode & 0x80)
		{
			int16 Half = 0;
			return LoadBytes(Element, &Half, 2) && WriteOperand(Ops, 0, uint32(int32(Half)));
		}
		uint32 Word = 0;
		return Load32(Element, Word) && WriteOperand(Ops, 0, Word);
	}

	case 74: case 82: case 90: // NEW: deref dst, Lx: the word the pointer in Lx points at
	{
		uint32 Raw = 0, Pointer = 0;
		if (!ReadRaw(Ops, 1, Raw) || !Load32(FOKScriptRuntime::StackAddress(uint32(Frame) + Raw), Pointer) || !Load32(Pointer, Ret))
		{
			return false;
		}
		return WriteOperand(Ops, 0, Ret);
	}

	case 75: // NEW: lea dst, x: the address of x (for an object reference: the object, i.e. its handle)
	case 83: // NEW: leapool dst, x: the same, for frame and pool locations only
	{
		uint32 Raw = 0;
		if (!ReadRaw(Ops, 1, Raw))
		{
			return false;
		}
		if (Raw & 0x80000000)
		{
			const int32 Ref = int32(Raw & 0x3FFFFFFF) - 1;
			if (Op == 83 || !Current->RefHandles.IsValidIndex(Ref))
			{
				return Fail(TEXT("lea of an object reference that doesn't exist"));
			}
			Ret = RefValue(Ref);
		}
		else
		{
			Ret = AddressOf(Raw);
		}
		return WriteOperand(Ops, 0, Ret);
	}

	// NEW: increment/decrement operand 1 in place; dst and the return register get the old (post) or new (pre) value.
	case 67: case 76:	// postinc
	case 68: case 77:	// preinc
	case 69: case 78:	// postdec
	case 70: case 79:	// predec
	{
		if (!ReadOperand(Ops, 1, A))
		{
			return false;
		}
		const bool bIncrement = (Op == 67 || Op == 76 || Op == 68 || Op == 77);
		const bool bPost = (Op == 67 || Op == 76 || Op == 69 || Op == 78);
		const uint32 New = bIncrement ? A + 1 : A - 1;
		if (!WriteOperand(Ops, 1, New))
		{
			return false;
		}
		Ret = bPost ? A : New;
		return WriteOperand(Ops, 0, Ret);
	}
	case 84:			// fpostinc
	case 85: case 87:	// fpredec
	case 86:			// fpostdec
	{
		if (!ReadOperand(Ops, 1, A))
		{
			return false;
		}
		const float Old = AsFloat(A);
		const float New = Old + (Op == 84 ? 1.0f : -1.0f);
		if (!WriteOperand(Ops, 1, FromFloat(New)))
		{
			return false;
		}
		Ret = FromFloat((Op == 85 || Op == 87) ? New : Old);
		return WriteOperand(Ops, 0, Ret);
	}

		// Integer arithmetic. Ops 18-27 are the unsigned forms.
	case 8: case 18: return WriteOperand(Ops, 0, A + B);
	case 9: case 19: return WriteOperand(Ops, 0, A - B);
	case 10: case 20: return WriteOperand(Ops, 0, A * B);
	case 11: return SB == 0 ? Fail(TEXT("division by zero")) : WriteOperand(Ops, 0, uint32(SA / SB));
	case 12: return SB == 0 ? Fail(TEXT("division by zero")) : WriteOperand(Ops, 0, uint32(SA % SB));
	case 21: return B == 0 ? Fail(TEXT("division by zero")) : WriteOperand(Ops, 0, A / B);
	case 22: return B == 0 ? Fail(TEXT("division by zero")) : WriteOperand(Ops, 0, A % B);
	case 13: case 23: return WriteOperand(Ops, 0, A & B);
	case 14: case 24: return WriteOperand(Ops, 0, A | B);
	case 15: case 25: return WriteOperand(Ops, 0, A ^ B);
	case 16: case 26: return WriteOperand(Ops, 0, A << (B & 31));
	case 17: return WriteOperand(Ops, 0, uint32(SA >> (B & 31)));			// arithmetic shift
	case 27: return WriteOperand(Ops, 0, A >> (B & 31));					// logical shift

		// Float arithmetic.
	case 28: return WriteOperand(Ops, 0, FromFloat(FA + FB));
	case 29: return WriteOperand(Ops, 0, FromFloat(FA - FB));
	case 30: return WriteOperand(Ops, 0, FromFloat(FA * FB));
	case 31: return WriteOperand(Ops, 0, FromFloat(FA / FB));

		// Comparisons: signed (32-39), unsigned (40-47), float (48-55).
	case 32: return Compare(SA < SB);
	case 33: case 41: return Compare(A == B);
	case 34: return Compare(SA > SB);
	case 35: case 43: return Compare(A != B);
	case 36: return Compare(SA <= SB);
	case 37: return Compare(SA >= SB);
	case 38: case 46: return Compare(A != 0 || B != 0);
	case 39: case 47: return Compare(A != 0 && B != 0);
	case 40: return Compare(A < B);
	case 42: return Compare(A > B);
	case 44: return Compare(A <= B);
	case 45: return Compare(A >= B);
	case 48: return Compare(FA < FB);
	case 49: return Compare(FA == FB);
	case 50: return Compare(FA > FB);
	case 51: return Compare(FA != FB);
	case 52: return Compare(FA <= FB);
	case 53: return Compare(FA >= FB);
	case 54: return Compare(FA != 0.0f || FB != 0.0f);
	case 55: return Compare(FA != 0.0f && FB != 0.0f);

		// Conversions: dst = convert(operand 1).
	case 56: return ReadOperand(Ops, 1, A) && WriteOperand(Ops, 0, FromFloat(float(int32(A))));
	case 57: return ReadOperand(Ops, 1, A) && WriteOperand(Ops, 0, FromFloat(float(A)));
	case 58: // f2i: x87 rounding, to nearest with ties to even (not C truncation)
		return ReadOperand(Ops, 1, A) && WriteOperand(Ops, 0, uint32(int32(FMath::RoundHalfToEven(AsFloat(A)))));

		// Jumps: relative to the next instruction, in words. jz tests the return register.
	case 59: 
	case 60:
	{
		uint32  Offset = 0;
		if (!ReadRaw(Ops, 0, Offset))
		{
			return false;
		}
		if (Op == 60 || Ret == 0)
		{
			Pc += int32(Offset);
		}
		return true;
	}
	
	case 61: // NEW: call function(args...): same script; operand 0 is a 1-based function index
	{
		uint32 Target = 0;
		if (!ReadRaw(Ops, 0, Target))
		{
			return false;
		}
		return Call(*Current, int32(Target) - 1, Ops);
	}
	case 62: // NEW: callx import(args...): another script, in that script's own instance
	{
		uint32 Raw = 0;
		if (!ReadRaw(Ops, 0, Raw))
		{
			return false;
		}
		const TArray<FOKMkoImport>& Imports = Current->File->GetImports();
		const int32 ImportIndex = int32(Raw & 0x3FFFFFFF) - 1;
		if (!Imports.IsValidIndex(ImportIndex) || !Imports[ImportIndex].Target)
		{
			return Fail(TEXT("callx to an import that isn't linked"));
		}
		const FOKMkoImport& Import = Imports[ImportIndex];
		FOKScriptInstance* Target = Runtime.GetInstance(*Import.Target);
		return Target ? Call(*Target, Import.TargetFunction, Ops) : Fail(TEXT("too many script instances"));
	}

	case 63: // NEW: callref entry, table, args...: table = {script instance, function numbers...}; calls table[entry]
	{
		uint32 Entry = 0, Table = 0;
		if (!ReadRaw(Ops, 0, Entry) || !ReadOperand(Ops, 1, Table))
		{
			return false;
		}
		if (Table == 0)
		{
			Ret = 0;		// no callback table (the game's sound dispatcher isn't built yet)
			return true;
		}
		uint32 ScriptValue = 0, Function = 0;
		if (!Load32(Table, ScriptValue) || !Load32(Table + Entry * 4, Function))
		{
			return false;
		}
		if (Function == 0)
		{
			Ret = 0;		// MKKE skips an empty entry (FUN_0045D740)
			return true;
		}
		FOKScriptInstance* Target = (ScriptValue >> 24) == FOKScriptRuntime::InstanceTag ? Runtime.FindInstanceById(int32(ScriptValue & 0x00FFFFFF)) : nullptr;
		if (!Target)
		{
			return Fail(FString::Printf(TEXT("callref: %08X isn't a script instance"), ScriptValue));
		}
		// The arguments start one operand later than for call, after the table.
		FOperands Args = Ops;
		Args.FirstWord += 1;
		Args.Count = Ops.Count > 0 ? Ops.Count - 1 : 0;
		Args.Mode = Ops.Mode >> 2;
		return Call(*Target, int32(Function) - 1, Args);
	}

	case 64: // NEW: native id, args...: call into OpenKombat's C++
	{
		uint32 NativeId = 0;
		if (!ReadRaw(Ops, 0, NativeId))
		{
			return false;
		}
		const FOKNativeNames* Names = Runtime.GetNativeNames();
		const int32 Module = Names ? Names->SlotForModule(NativeId >> 16) : INDEX_NONE;
		const int32 NativeIndex = int32(NativeId & 0xFFFF) - 1;
		const FOKNativeFn Fn = (Module != INDEX_NONE) ? FOKNativeTable::Get().Find(Module, NativeIndex) : nullptr;
		if (!Fn)
		{
			return Fail(FString::Printf(TEXT("native %s (%d.%d) isn't implemented yet"),
				Names ? *Names ->NameFor(NativeId) : TEXT("?"), Module, NativeIndex));
		}

		// Like CallNative (0x4596D0): skip the native id, so argument 0 is the next operand.
		// The mode bits stay where they are, so they now describe arguments 0-2.
		NativeOps.Mode = Ops.Mode;
		NativeOps.FirstWord = Ops.FirstWord + 1;
		NativeOps.Count = Ops.Count > 0 ? Ops.Count - 1 : 0;

		if (CVarTraceNatives.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogOKVM, Log, TEXT("frame %lld: %s, line %u: %s"), Runtime.GetSimFrame(), *Describe(*Current, CurrentFunction), CurrentLine,
				Names ? *Names->NameFor(NativeId) : TEXT("?"));
		}

		// A native may hit an error while reading an argument but carry on; treat that as failure too.
		CurrentNativeId = (uint32(Module) << 16) | uint32(NativeIndex);
		FOKNativeTable::NoteRun(CurrentNativeId);
		return Fn(*this) && Error.IsEmpty();
	}

	// Unary ops: dst = op(operand 1); these also set the return register.
	case 71: case 72: case 73: case 80: case 81: case 88:
	{
		if (!ReadOperand(Ops, 1, A))
		{
			return false;
		}
		Ret = (Op == 71) ? uint32(-int32(A))
			: (Op == 72 || Op == 80) ? ~A
			: (Op == 73 || Op == 81) ? uint32(A == 0)
			: FromFloat(-AsFloat(A));
		return WriteOperand(Ops, 0, Ret);
	}

	case 91: // NEW: switch count, default, value, (key, jump) x count: binary search over the sorted keys
	{
		uint32 Count = 0, Jump = 0, Value = 0;
		if (!ReadRaw(Ops, 0, Count) || !ReadRaw(Ops, 1, Jump) || !ReadOperand(Ops, 2, Value))
		{
			return false;
		}
		int32 Low = 0;
		int32 High = int32(Count) - 1;
		while (Low <= High)
		{
			const int32 Mid = (Low + High) / 2;
			uint32 Key = 0;
			if (!ReadRaw(Ops, 3 + Mid * 2, Key))
			{
				return false;
			}
			if (int32(Value) < int32(Key))
			{
				High = Mid - 1;
			}
			else if (int32(Value) > int32(Key))
			{
				Low = Mid + 1;
			}
			else
			{
				if (!ReadRaw(Ops, 4 + Mid * 2, Jump))
				{
					return false;
				}
				break;
			}
		}
		Pc += int32(Jump);		// relative to the next instruction, like jmp
		return true;
	}

	case 92: // NEW: trackobj var, slot: MKKE's weak-reference bookkeeping. Our handles are already weak.
		return true;

	default:
		return Fail(FString::Printf(TEXT("opcode %s isn't supported yet"), FOKMkoFile::GetOpName(Op)));
	}

}

// --- Console tools --------------------------------------------------

/** "7" is an integer; "7.0" or "7f" is a float. */
static uint32 ParseScriptArg(const FString& Text)
{
	if (Text.Contains(TEXT(".")) || Text.EndsWith(TEXT("f")))
	{
		const float Value = FCString::Atof(*Text.LeftChop(Text.EndsWith(TEXT("f")) ? 1 : 0));
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, 4);
		return Bits;
	}
	return uint32(FCString::Atoi(*Text));
}

static TAutoConsoleVariable<int32> CVarVMSeed(
	TEXT("ok.VM.Seed"),
	0,
	TEXT("Random seed for ok.VM.Call (MKKE's generator)."));

static FAutoConsoleCommand CmdVMCall(
	TEXT("ok.VM.Call"),
	TEXT("ok.VM.Call <Script> <Function> [args...]: run a function, e.g. ok.VM.Call PlayerSelect GetTeammateNumber 2"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const UOKScriptSubsystem* Scripts = GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
			if (!Scripts || !Scripts->GetLibrary() || Args.Num() < 2)
			{
				UE_LOG(LogOKVM, Warning, TEXT("Usage: ok.VM.Call PlayerSelect GetTeammateNumber 2"));
				return;
			}
			const FOKMkoFile* Script = Scripts->GetLibrary()->FindScript(Args[0] + TEXT(".mko"));
			const int32 Index = Script ? Script->FindFunctionByName(Args[1]) : INDEX_NONE;
			if (Index == INDEX_NONE)
			{
				UE_LOG(LogOKVM, Error, TEXT("Can't find %s.%s"), *Args[0], *Args[1]);
				return;
			}

			TArray<uint32> CallArgs;
			for (int32 i = 2; i < Args.Num(); ++i)
			{
				CallArgs.Add(ParseScriptArg(Args[i]));
			}

			// Two test fighters for @my_obj and @his_obj. Declared before the runtime, so they
			// outlive it: the runtime holds pointers to them.
			FOKGameInfo GameInfo;
			FOKFighter Me;
			FOKFighter Him;
			Me.Name = TEXT("my_obj");
			Him.Name = TEXT("his_obj");

			FOKScriptRuntime Runtime(&Scripts->GetNativeNames());
			Runtime.SetRandomSeed(uint32(CVarVMSeed.GetValueOnGameThread()));
			Me.Handle = Runtime.AddObject(Me, FOKFighter::Bytes);
			Him.Handle = Runtime.AddObject(Him, FOKFighter::Bytes);
			GameInfo.Handle = Runtime.AddObject(GameInfo, FOKGameInfo::Bytes);
			Runtime.BindName(TEXT("GFGGameInfo"), GameInfo.Handle);
			Runtime.BindName(TEXT("GGameInfo"), GameInfo.Handle);

			FOKScriptThread Thread(Runtime);
			Thread.Bind(TEXT("my_obj"), Me.Handle);
			Thread.Bind(TEXT("his_obj"), Him.Handle);

			uint32 Result = 0;
			FString Error;
			const FString Call = FString::Printf(TEXT("%s.%s(%s)"), *Args[0], *Args[1],
				*FString::Join(TArrayView<const FString>(Args).RightChop(2), TEXT(", ")));

			if (Thread.Run(*Script, Index, CallArgs, Result, Error))
			{
				float AsFloat;
				FMemory::Memcpy(&AsFloat, &Result, 4);
				UE_LOG(LogOKVM, Log, TEXT("%s returned %d (0x%08X, as float %g) after %d instructions, call depth %d, %d script instances"),
					*Call, int32(Result), Result, AsFloat, Thread.GetInstructionsRun(), Thread.GetMaxDepth(), Runtime.NumInstances());
			}
			else
			{
				UE_LOG(LogOKVM, Error, TEXT("%s stopped after %d instructions, %d script instances: %s"),
					*Call, Thread.GetInstructionsRun(), Runtime.NumInstances(), *Error);
			}

			const FString Pending = Me.DescribeAttack(Runtime, FOKFighter::PendingAttack);
			const FString Live = Me.DescribeAttack(Runtime, FOKFighter::LiveAttack);
			if (!Pending.IsEmpty())
			{
				UE_LOG(LogOKVM, Log, TEXT("my_obj's pending attack:\n%s"), *Pending);
			}
			if (!Live.IsEmpty())
			{
				UE_LOG(LogOKVM, Log, TEXT("my_obj's live attacK:\n%s"), *Live);
			}
			if (const uint8* Counter = Runtime.HeapBytes((Me.Handle & 0x00FFFFFF) + 0x2F70, 4))
			{
				uint32 Launched = 0;
				FMemory::Memcpy(&Launched, Counter, 4);
				UE_LOG(LogOKVM, Log, TEXT("my_obj +0x2F70 (attacks launched): %u"), Launched);
			}

			// What did the run write? Compare each instance's pool with how it was after loading.
			Runtime.ForEachInstance([](const FOKScriptInstance& Instance)
				{
					TArray<FString> Changes;
					for (int32 Offset = 0; Offset + 4 <= Instance.Baseline.Num(); Offset += 4)
					{
						int32 Was = 0, Now = 0;
						FMemory::Memcpy(&Was, Instance.Baseline.GetData() + Offset, 4);
						FMemory::Memcpy(&Now, Instance.Memory.GetData() + Instance.PoolBase + Offset, 4);
						if (Was != Now && Changes.Num() < 8)
						{
							Changes.Add(FString::Printf(TEXT("g%x: %d -> %d"), Offset, Was, Now));
						}
					}
					if (Changes.Num() > 0)
					{
						UE_LOG(LogOKVM, Log, TEXT("%s changed globals: %s"), *Instance.File->GetName(), *FString::Join(Changes, TEXT(", ")));
					}
				});
		}));