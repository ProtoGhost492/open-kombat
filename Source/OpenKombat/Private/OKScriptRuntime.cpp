#include "OKScriptRuntime.h"
#include "OKNatives.h"
#include "OKScriptThread.h"
#include "OKMkoFile.h"

namespace
{
	void PutWord(TArray<uint8>& Memory, int64 Offset, uint32 Value)
	{
		if (Offset >= 0 && Offset + 4 <= Memory.Num())
		{
			FMemory::Memcpy(Memory.GetData() + Offset, &Value, 4);
		}
	}
}

FOKScriptInstance* FOKScriptRuntime::GetInstance(const FOKMkoFile& File)
{
	if (TUniquePtr<FOKScriptInstance>* Existing = Instances.Find(&File))
	{
		return Existing->Get();
	}
	if (Instances.Num() >= MaxInstances)
	{
		return nullptr;			// no address tags left
	}

	TUniquePtr<FOKScriptInstance> Instance = MakeUnique<FOKScriptInstance>();
	FOKScriptInstance& I = *Instance;
	I.File = &File;
	I.Id = Instances.Num() + 1;

	const TConstArrayView<uint8> Stored = File.GetDataAndPool();
	I.Memory = TArray<uint8>(Stored.GetData(), Stored.Num());
	I.PoolBase = File.GetPoolBase();

	I.Code.SetNumZeroed(File.GetCodeBytes() / 4);
	for (int32 i = 0; i < I.Code.Num(); ++i)
	{
		File.ReadCodeWord(i, I.Code[i]);
	}

	// Relocate (FUN_0045A0A0): turn the offsets stored in the file into addresses in this instance.
	for (const FOKMkoFixup& Fix : File.GetFixups())
	{
		switch (Fix.Kind)
		{
		case 0: // pool word -> pointer into the data region
			PutWord(I.Memory, int64(I.PoolBase) + Fix.Where, MemoryAddress(I.Id, Fix.Value));
			break;
		case 1: // code word -> pointer into the data region (e.g. a string argument)
			if (Fix.Where != 0 && Fix.Where % 4 == 0 && int64(Fix.Where / 4) < I.Code.Num())
			{
				I.Code[Fix.Where / 4] = MemoryAddress(I.Id, Fix.Value);
			}
			break;
		case 5: // code word -> an asset (asset-import index): bound to a runtime-wide value (ANIM-RE-NOTES §1)
			if (Fix.Where % 4 == 0 && int64(Fix.Where / 4) < I.Code.Num())
			{
				if (const FOKMkoAsset* Asset = File.FindAsset(Fix.Value + 1))
				{
					I.Code[Fix.Where / 4] = AssetValue(*Asset);
				}
			}
			break;
		case 4: // pool word -> an asset (asset-import index), as FGEffectsEngine's blood tables hold them (W2)
			if (const FOKMkoAsset* Asset = File.FindAsset(Fix.Value + 1))
			{
				PutWord(I.Memory, int64(I.PoolBase) + Fix.Where, AssetValue(*Asset));
			}
			break;
		case 9: // pool word -> a sound (table D index), as FGSoundEngine's impact tables hold them: bound like assets (W6)
			if (const FOKMkoAsset* Sound = File.FindSound(Fix.Value))
			{
				PutWord(I.Memory, int64(I.PoolBase) + Fix.Where, AssetValue(*Sound));
			}
			break;
		case 10: // code word -> a sound (@sound:SND_SFX_Scorpion/spear_launch)
			if (Fix.Where % 4 == 0 && int64(Fix.Where / 4) < I.Code.Num())
			{
				if (const FOKMkoAsset* Sound = File.FindSound(Fix.Value))
				{
					I.Code[Fix.Where / 4] = AssetValue(*Sound);
				}
			}
			break;
		case 2: // pool word -> pointer into the pool
			PutWord(I.Memory, int64(I.PoolBase) + Fix.Where, MemoryAddress(I.Id, I.PoolBase + Fix.Value));
			break;
		case 6: // pool word -> this script instance
			PutWord(I.Memory, int64(I.PoolBase) + Fix.Where, (InstanceTag << 24) | uint32(I.Id));
			break;
		case 7: // pool word -> a native callback, {module hash, 1-based index} (FUN_00459CE0 resolves it to a function)
			PutWord(I.Memory, int64(I.PoolBase) + Fix.Where, Fix.Value);
			break;
		case 8: // code word -> the same: OpenKombat keeps the id, and the code that calls it maps it to C++
			if (Fix.Where != 0 && Fix.Where % 4 == 0 && int64(Fix.Where / 4) < I.Code.Num())
			{
				I.Code[Fix.Where / 4] = Fix.Value;
			}
			break;
		default: // 3: native bindings (we keep native ids)
			break;
		}
	}
	I.Baseline = TArray<uint8>(I.Memory.GetData() + I.PoolBase, I.Memory.Num() - int32(I.PoolBase));

	// Resolve each object-reference name ("my_obj") to a handle once, now.
	const TArray<FString>& RefNames = File.GetObjectRefs();
	I.RefHandles.SetNumZeroed(RefNames.Num());
	for (int32 i = 0; i < RefNames.Num(); ++i)
	{
		if (const uint32* Handle = Bindings.Find(RefNames[i]))
		{
			I.RefHandles[i] = *Handle;
		}
		else if (RefNames[i] == TEXT("active_mkscript"))
		{
			I.RefHandles[i] = (InstanceTag << 24) | uint32(I.Id);		// the script itself (its MKScript object)
		}
		I.RefBindingIds.Add(BindingId(RefNames[i]));
	}

	InstancesById.Add(&I);
	Instances.Add(&File, MoveTemp(Instance));

	// Linking a script (FUN_0045CE90) ends by running its last function, __global__, which constructs its globals:
	// command_init on each command record, and so on.
	const TArray<FOKMkoFunction>& Functions = File.GetFunctions();
	if (Functions.Num() > 0)
	{
		FOKScriptThread Thread(*this);
		uint32 Result = 0;
		FString Error;
		if (!Thread.Run(File, Functions.Num() - 1, {}, Result, Error))
		{
			UE_LOG(LogOKVM, Warning, TEXT("%s.%s: %s"), *File.GetName(), *Functions.Last().Name, *Error);
		}
	}
	return &I;
}

uint32 FOKScriptRuntime::AssetValue(const FOKMkoAsset& Asset)
{
	const FString Key = Asset.Set + TEXT("/") + Asset.Object;
	int32 Index = INDEX_NONE;
	if (const int32* Found = AssetIndex.Find(Key))
	{
		Index = *Found;
	}
	else
	{
		Index = Assets.Add(Asset);
		AssetIndex.Add(Key, Index);
	}
	return (AssetTag << 24) | uint32(Index + 1);
}

const FOKMkoAsset* FOKScriptRuntime::FindAsset(uint32 Value) const
{
	const int32 Index = int32(Value & 0x00FFFFFF) - 1;
	return ((Value >> 24) == AssetTag && Assets.IsValidIndex(Index)) ? &Assets[Index] : nullptr;
}

int32 FOKScriptRuntime::BindingId(const FString& RefName)
{
	if (const int32* Id = BindingIds.Find(RefName))
	{
		return *Id;
	}
	return BindingIds.Add(RefName, BindingIds.Num());
}

bool FOKScriptRuntime::ResolveFunction(const FOKScriptInstance& From, uint32 Raw, FOKScriptInstance*& OutInstance, int32& OutFunction)
{
	if (Raw & 0x80000000)
	{
		const TArray<FOKMkoImport>& Imports = From.File->GetImports();
		const int32 ImportIndex = int32(Raw & 0x3FFFFFFF) - 1;
		if (!Imports.IsValidIndex(ImportIndex) || !Imports[ImportIndex].Target)
		{
			return false;
		}
		OutInstance = GetInstance(*Imports[ImportIndex].Target);
		OutFunction = Imports[ImportIndex].TargetFunction;
		return OutInstance != nullptr;
	}
	OutInstance = FindInstanceById(From.Id);
	OutFunction = int32(Raw) - 1;
	return OutInstance && From.File->GetFunctions().IsValidIndex(OutFunction);
}

bool FOKScriptRuntime::LoadAnyWord(uint32 Address, uint32& OutValue) const
{
	const uint32 Tag = Address >> 24;
	const int64 Offset = Address & 0x00FFFFFF;
	if (Tag == HeapTag)
	{
		return const_cast<FOKScriptRuntime*>(this)->LoadHeapWord(Address, OutValue);
	}
	const FOKScriptInstance* Owner = Tag > FirstMemoryTag ? FindInstanceById(int32(Tag - FirstMemoryTag)) : nullptr;
	if (!Owner || Offset + 4 > Owner->Memory.Num())
	{
		return false;
	}
	FMemory::Memcpy(&OutValue, Owner->Memory.GetData() + Offset, 4);
	return true;
}

bool FOKScriptRuntime::StoreAnyWord(uint32 Address, uint32 Value)
{
	const uint32 Tag = Address >> 24;
	const int64 Offset = Address & 0x00FFFFFF;
	if (Tag == HeapTag)
	{
		SetHeapWord(Address, Value);
		return HeapBytes(uint32(Offset), 4) != nullptr;
	}
	FOKScriptInstance* Owner = Tag > FirstMemoryTag ? FindInstanceById(int32(Tag - FirstMemoryTag)) : nullptr;
	if (!Owner || Offset + 4 > Owner->Memory.Num())
	{
		return false;
	}
	FMemory::Memcpy(Owner->Memory.GetData() + Offset, &Value, 4);
	return true;
}

FOKScriptInstance* FOKScriptRuntime::FindInstanceByName(const FString& ScriptName) const
{
	for (const TPair<const FOKMkoFile*, TUniquePtr<FOKScriptInstance>>& Pair : Instances)
	{
		if (Pair.Key->GetName() == ScriptName || Pair.Key->GetName() == ScriptName + TEXT(".mko"))
		{
			return Pair.Value.Get();
		}
	}
	return nullptr;
}

FOKScriptInstance* FOKScriptRuntime::FindInstanceById(int32 Id) const
{
	return InstancesById.IsValidIndex(Id - 1) ? InstancesById[Id - 1] : nullptr;
}

void FOKScriptRuntime::ForEachInstance(TFunctionRef<void(const FOKScriptInstance&)> Visitor) const
{
	for (const FOKScriptInstance* Instance : InstancesById)
	{
		Visitor(*Instance);
	}
}

uint32 FOKScriptRuntime::AddObject(FOKScriptObject& Object, int32 MemoryBytes)
{
	uint32 Handle = 0;
	if (MemoryBytes > 0)
	{
		const int32 Offset = Align(Heap.Num(), 8);
		if (int64(Offset) + MemoryBytes > 0x00FFFFFF)
		{
			return 0;		// the heap is full (16 MB); nothing is freed yet
		}
		Heap.SetNumZeroed(Offset + MemoryBytes);
		Handle = (HeapTag << 24) | uint32(Offset);
	}
	else
	{
		Handle = (ObjectTag << 24) | NextObjectId++;
	}
	Objects.Add(Handle, &Object);
	return Handle;		
}

FOKScriptObject* FOKScriptRuntime::ResolveHandle(uint32 Handle) const
{
	FOKScriptObject* const* Found = Objects.Find(Handle);
	return Found ? *Found : nullptr;
}

uint32 FOKScriptRuntime::NextRandom()
{
	RandomSeed = RandomSeed * 0x0BB38435u + 0x3619636Bu;		// wraps at 32 bits, exactly like the exe
	return RandomSeed & 0x7FFFFFFF;
}

FOKScriptRuntime::~FOKScriptRuntime()
{
	FOKNativeTable::Get().WriteReport();		// for the progress chart (scripts/re/progress-chart.mjs in the RE repo)
}

FOKScriptThread* FOKScriptRuntime::StartProcess(const FOKMkoFile& Script, int32 FuncIndex, int32 ProcId, int32 Group,
	bool bCanSleep, FString& OutError, TConstArrayView<uint32> Args)
{
	if (Group < 0 || Group >= NumGroups)
	{
		OutError = FString::Printf(TEXT("priority group %d is outside 0-%d"), Group, NumGroups - 1);
		return nullptr;
	}

	TUniquePtr<FOKScriptThread> Thread = MakeUnique<FOKScriptThread>(*this);
	Thread->SetCanSleep(bCanSleep);
	if (!Thread->Start(Script, FuncIndex, Args, OutError))
	{
		return nullptr;
	}
	Thread->SetProcess(NextProcessNumber++, ProcId, Group);
	Thread->SetHandle(AddObject(*Thread));
	Thread->Bind(TEXT("active_vm"), Thread->GetHandle());		// @active_vm: the process itself

	UE_LOG(LogOKVM, Log, TEXT("frame %lld: process %d %s started (id %d, group %d)"), SimFrame, Thread->GetNumber(), *Thread->GetStartName(), ProcId, Group);

	FOKScriptThread* Result = Thread.Get();
	Processes.Add(MoveTemp(Thread));
	return Result;
}

void FOKScriptRuntime::TickProcesses(int64 InSimFrame)
{
	SimFrame = InSimFrame;

	// The processes that exist at the start of the frame. Ones started during the frame first
	// run next frame. [inf: MKKE's list walk might reach a new process the same frame]
	TArray<FOKScriptThread*> ThisFrame;
	ThisFrame.Reserve(Processes.Num());
	for (const TUniquePtr<FOKScriptThread>& Process : Processes)
	{
		ThisFrame.Add(Process.Get());
	}

	for (int32 Group = 0; Group < NumGroups; ++Group)
	{
		// Newest first: MKKE adds new threads at the head of their group's list (FUN_0042DD10).
		for (int32 i = ThisFrame.Num() - 1; i >= 0; --i)
		{
			FOKScriptThread* Thread = ThisFrame[i];
			if (Thread->GetGroup() == Group && Thread->IsAlive())
			{
				Thread->Tick();
			}
		}
	}

	// Remove the processes that ended, saying why. Back to front, so removing keeps the others in order.
	for (int32 i = Processes.Num() - 1; i >= 0; --i)
	{
		const FOKScriptThread& Thread = *Processes[i];
		if (Thread.IsAlive())
		{
			continue;
		}
		const FString& Why = Thread.GetError().IsEmpty() ? Thread.GetEndReason() : Thread.GetError();
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: process %d %s ended after %d instructions: %s"),
			SimFrame, Thread.GetNumber(), *Thread.GetStartName(), Thread.GetInstructionsRun(), *Why);
		// Its process data goes with it. [inf: what MKKE's thread destructor frees isn't traced]
		for (const uint32 AttachedHandle : Thread.GetAttached())
		{
			if (Owned.Contains(AttachedHandle))
			{
				DestroyObject(AttachedHandle);
			}
		}
		RemoveObject(Thread.GetHandle());
		Processes.RemoveAt(i);
	}
}

int32 FOKScriptRuntime::KillProcesses(int32 ProcId, uint32 Owner)
{
	int32 Killed = 0;
	for (const TUniquePtr<FOKScriptThread>& Process : Processes)
	{
		if (Process->IsAlive() && Process->GetProcId() == ProcId && (Owner == 0 || Process->GetOwner() == Owner))
		{
			Process->KillNow(TEXT("killed by kill_proc"));
			++Killed;
		}
	}
	return Killed;
}

FOKScriptThread* FOKScriptRuntime::FindProcess(int32 ProcId) const
{
	for (const TUniquePtr<FOKScriptThread>& Process : Processes)
	{
		if (Process->IsAlive() && Process->GetProcId() == ProcId)
		{
			return Process.Get();
		}
	}
	return nullptr;
}

void FOKScriptRuntime::ForEachProcess(TFunctionRef<void(const FOKScriptThread&)> Visitor)
const
{
	for (const TUniquePtr<FOKScriptThread>& Process : Processes)
	{
		Visitor(*Process);
	}
}

void FOKScriptRuntime::RemoveObject(uint32 Handle)
{
	Objects.Remove(Handle);
}

uint32 FOKScriptRuntime::CreateOwned(TUniquePtr<FOKScriptObject> Object, int32 Bytes)
{
	const uint32 Handle = AddObject(*Object, Bytes);
	if (Handle != 0)
	{
		Owned.Add(Handle, MoveTemp(Object));
	}
	return Handle;
}

uint32 FOKScriptRuntime::CreatePdata()
{
	return CreateOwned(MakeUnique<FOKPdata>(), FOKPdata::Bytes);
}

void FOKScriptRuntime::DestroyObject(uint32 Handle)
{
	FOKScriptObject* Object = ResolveHandle(Handle);
	if (!Object)
	{
		return;
	}
	if (FOKScriptThread* Process = Object->AsThread())
	{
		Process->KillNow(TEXT("destroyed by a script"));
		return;
	}
	if (Owned.Contains(Handle))			// objects we didn't create (the fighters) aren't ours to delete
	{
		RemoveObject(Handle);
		Owned.Remove(Handle);
	}
}

uint32 FOKScriptRuntime::InternString(const FString& Text)
{
	if (const uint32* Found = InternedStrings.Find(Text))
	{
		return *Found;
	}
	if (StringPoolAddress == 0)
	{
		StringPoolAddress = AddObject(StringPool, StringPoolBytes);
	}
	const int32 Size = Text.Len() + 1;
	uint8* Where = StringPoolUsed + Size <= StringPoolBytes ? HeapBytes((StringPoolAddress & 0x00FFFFFF) + StringPoolUsed, Size) : nullptr;
	if (!Where)
	{
		return 0;
	}
	for (int32 i = 0; i < Size; ++i)
	{
		Where[i] = i < Text.Len() ? uint8(Text[i]) : 0;
	}
	const uint32 Address = StringPoolAddress + uint32(StringPoolUsed);
	StringPoolUsed += Size;
	InternedStrings.Add(Text, Address);
	return Address;
}

void FOKScriptRuntime::SetColorScale(const FVector3f& Scale)
{
	if (Scale != ColorScale)
	{
		ColorScale = Scale;
		UE_LOG(LogOKVM, Log, TEXT("frame %lld: colour scale %g, %g, %g"), SimFrame, Scale.X, Scale.Y, Scale.Z);
	}
}