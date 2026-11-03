#pragma once

#include "CoreMinimal.h"
#include "OKMkoFile.h"
#include "OKScriptThread.h"
#include "OKHud.h"

class FOKMkoFile;
class FOKNativeNames;
class FOKScriptObject;

/** One running copy of a script: its own memory (data + pool), its own code, its own object bindings. */
struct FOKScriptInstance
{
	const FOKMkoFile* File = nullptr;
	int32 Id = 0;					// 1-based; part of every address that points into Memory
	TArray<uint8> Memory;			// data region, then the pool (globals), with fixups applied
	uint32 PoolBase = 0;			// where the pool starts in Memory
	TArray<uint8> Baseline;			// the pool as it was after loading, to report what a run changed
	TArray<uint32> Code;			// setarg patches this
	TArray<uint32> RefHandles;		// object handle for each object reference; 0 = not bound
	TArray<int32> RefBindingIds;	// each object reference's binding id (FOKScriptRuntime::BindingId)
};

/** What a group of script threads share: script instances, the objects they can see, and the dice. */
class OPENKOMBAT_API FOKScriptRuntime
{
public:
	// Tagged values: the top byte says what a value points to.
	static constexpr uint32 StackTag = 0x01;
	static constexpr uint32 HeapTag = 0x02;			// an object with memory
	static constexpr uint32 ObjectTag = 0x03;		// an object without memory
	static constexpr uint32 ClassTag = 0x04;		// a class, for is_proc
	static constexpr uint32 AssetTag = 0x05;		// an asset (animation, texture...) a script names
	static constexpr uint32 InstanceTag = 0x0F;
	static constexpr uint32 FirstMemoryTag = 0x10;
	static constexpr int32 MaxInstances = 0xFF - FirstMemoryTag;

	static uint32 StackAddress(uint32 Offset) { return (StackTag << 24) | Offset; }
	static uint32 MemoryAddress(int32 InstanceId, uint32 Offset) { return (uint32(FirstMemoryTag + InstanceId) << 24) | Offset; }
	static uint32 ClassValue(EOKScriptClass Class) { return (ClassTag << 24) | uint32(Class); }

	explicit FOKScriptRuntime(const FOKNativeNames* InNativeNames = nullptr) : NativeNames(InNativeNames) {}
	FOKScriptRuntime(const FOKScriptRuntime&) = delete;
	FOKScriptRuntime& operator=(const FOKScriptRuntime&) = delete;

	/** The instance of File, created and relocated on first use. Null if there are too many instances. */
	FOKScriptInstance* GetInstance(const FOKMkoFile& File);
	FOKScriptInstance* FindInstanceById(int32 Id) const;
	/** An instance that already exists, by script name ("FGFightEngineScript"). */
	FOKScriptInstance* FindInstanceByName(const FString& ScriptName) const;
	int32 NumInstances() const { return Instances.Num(); }
	void ForEachInstance(TFunctionRef<void(const FOKScriptInstance&)> Visitor) const;

	/** 
	* Lets scripts refer to Object, and returns its handle (never 0). With MemoryBytes > 0 the object
	* also gets that many zeroed bytes in the heap, and its handle is their address. The runtime
	* doesn't own Object.
	*/
	uint32 AddObject(FOKScriptObject& Object, int32 MemoryBytes = 0);

	/** Binds an object-reference name ("my_obj") to a handle. Bind names before creating instances. */
	void BindName(const FString& RefName, uint32 Handle) { Bindings.Add(RefName, Handle); }
	uint32 FindBinding(const FString& RefName) const { const uint32* Found = Bindings.Find(RefName); return Found ? *Found : 0; }

	/**
	 * A small number for an object-reference name. Threads bind names by these (MKKE keeps a name -> object
	 * map on each thread, MKScriptVM +0x9C): a fighter's threads see @my_obj as that fighter.
	 */
	int32 BindingId(const FString& RefName);

	/**
	 * Scripts name assets by number in their own asset table. MKKE binds those operands to the loaded assets
	 * (an AnimSequence pointer), so the value means the same thing in every script it's passed to. We bind
	 * them to a value of our own (AssetTag) when an instance is made; FindAsset turns it back into names.
	 */
	uint32 AssetValue(const FOKMkoAsset& Asset);
	const FOKMkoAsset* FindAsset(uint32 Value) const;

	/** A function reference as scripts store it (1-based in From, or an import), as an instance and index. */
	bool ResolveFunction(const FOKScriptInstance& From, uint32 Raw, FOKScriptInstance*& OutInstance, int32& OutFunction);

	/** The object for a handle, or null. */
	FOKScriptObject* ResolveHandle(uint32 Handle) const;

	const FOKNativeNames* GetNativeNames() const { return NativeNames; }

	/** MKKE's random generator (FUN_005F5080). Returns 31 bits. */
	uint32 NextRandom();
	void SetRandomSeed(uint32 Seed) { RandomSeed = Seed; }

	static constexpr int32 NumGroups = 21;		// MKKE's priority groups (0xE39E00, stride 0x14)

	~FOKScriptRuntime();		// defined in the .cpp, where FOKScriptThread is a complete type

	/** Starts a process (a scheduled thread) at FuncIndex, with those arguments. Returns it, or null with OutError saying why. */
	FOKScriptThread* StartProcess(const FOKMkoFile& Script, int32 FuncIndex, int32 ProcId, int32 Group, bool bCanSleep, FString& OutError,
		TConstArrayView<uint32> Args = TConstArrayView<uint32>());

	/** One frame: every live process, group 0 to 20, newest first. Ended processes are removed afterwards. */
	void TickProcesses(int64 InSimFrame);

	/** Kills every process with this id (kill_proc with no object). Returns how many. */
	int32 KillProcesses(int32 ProcId, uint32 Owner = 0);		// Owner: only processes a fighter adopted

	/** A live process with this id, or null. */
	FOKScriptThread* FindProcess(int32 ProcId) const;

	void ForEachProcess(TFunctionRef<void(const FOKScriptThread&)> Visitor) const;
	int32 NumProcesses() const { return Processes.Num(); }
	int64 GetSimFrame() const { return SimFrame; }
	/** The frame starts: everything this frame (input, the fighters' commands, the scripts) sees its number. */
	void BeginFrame(int64 InSimFrame) { SimFrame = InSimFrame; }

	/** From now on, this handle resolves to null. */
	void RemoveObject(uint32 Handle);

	/** A new process-data object (95.3), owned by the runtime. Returns its handle, or 0 if the heap is full */
	uint32 CreatePdata();

	/** Creates an object the runtime owns, with Bytes of heap memory. Returns its handle, or 0 if the heap is full. */
	uint32 CreateOwned(TUniquePtr<FOKScriptObject> Object, int32 Bytes);

	/** The MK objects (OKObjects) that may exist, oldest first; they tick after the fighters move. */
	TArray<uint32>& GetMkObjects() { return MkObjects; }

	/** Writes a word at a heap address (one with the heap tag). Other addresses are ignored. */
	void SetHeapWord(uint32 Address, uint32 Value)
	{
		uint8* Where = (Address >> 24) == HeapTag ? HeapBytes(Address & 0x00FFFFFF, 4) : nullptr;
		if (Where)
		{
			FMemory::Memcpy(Where, &Value, 4);
		}
	}

	/** A word at any address scripts use: the heap or a script instance's memory. False if it isn't one. */
	bool LoadAnyWord(uint32 Address, uint32& OutValue) const;
	bool StoreAnyWord(uint32 Address, uint32 Value);

	/** Reads a word at a heap address; false for other addresses. */
	bool LoadHeapWord(uint32 Address, uint32& OutValue)
	{
		const uint8* Where = (Address >> 24) == HeapTag ? HeapBytes(Address & 0x00FFFFFF, 4) : nullptr;
		if (Where)
		{
			FMemory::Memcpy(&OutValue, Where, 4);
		}
		return Where != nullptr;
	}

	/** Destroy an object (the *.0 natives): kills a process; deletes objects the runtime owns. */
	void DestroyObject(uint32 Handle);

	/** Size bytes of heap memory at Offset, or null. */
	uint8* HeapBytes(uint32 Offset, int32 Size) { return int64(Offset) + Size <= Heap.Num() ? Heap.GetData() + Offset : nullptr; }

	/** The screen colour scale scripts set with 43.34. (1, 1, 1) = normal. */
	void SetColorScale(const FVector3f& Scale);
	const FVector3f& GetColorScale() const { return ColorScale; }

	/** The screen's brightness for fades to black (43.32 / 43.33, utils.fade_to_black): 1 = no fade, 0 = black. */
	void SetFade(float Brightness) { Fade = Brightness; }
	float GetFade() const { return Fade; }

	/**
	 * The world's time dilation (UE3 WorldInfo +0x280, slow_time 136.33): it scales how far objects and fighters move
	 * and animate each frame, while the frame counter and the scripts keep their pace. A hit pause sets it near 0.
	 */
	void SetTimeDilation(float Dilation) { TimeDilation = Dilation; }
	float GetTimeDilation() const { return TimeDilation; }
	/** char_anim_speed (136.35, game info +0x82B4): [inf] a further scale on the fighters' animation. */
	void SetCharAnimSpeed(float Speed) { CharAnimSpeed = Speed; }
	float GetCharAnimSpeed() const { return CharAnimSpeed; }

	/** The texts and sprites scripts put on the HUD (module 15) and its banners. */
	FOKHudOverlay& GetHud() { return Hud; }
	const FOKHudOverlay& GetHud() const { return Hud; }

	/** The cinema the scripts asked to play (cinema_play, 39.11): a FOKCinemaObject's handle, which the script world plays. */
	void SetActiveCinema(uint32 Handle) { ActiveCinema = Handle; }
	uint32 GetActiveCinema() const { return ActiveCinema; }

	/**
	 * Text a native hands to scripts, such as a localised string (6.3): copied once into runtime memory, 8-bit as MKKE's
	 * strings are. Returns its address, or 0 when the pool is full.
	 */
	uint32 InternString(const FString& Text);

private:
	const FOKNativeNames* NativeNames = nullptr;
	TMap<const FOKMkoFile*, TUniquePtr<FOKScriptInstance>> Instances;
	TArray<FOKScriptInstance*> InstancesById;				// [Id - 1]
	TMap<uint32, FOKScriptObject*> Objects;					// handle -> object
	TMap<uint32, TUniquePtr<FOKScriptObject>> Owned;		// the objects the runtime created itself
	TArray<uint32> MkObjects;
	TArray<uint8> Heap;										// memory for objects with fields
	uint32 NextObjectId = 1;
	FVector3f ColorScale = FVector3f(1.0f, 1.0f, 1.0f);
	float Fade = 1.0f;
	float TimeDilation = 1.0f;
	float CharAnimSpeed = 1.0f;
	FOKHudOverlay Hud;
	uint32 ActiveCinema = 0;
	static constexpr int32 StringPoolBytes = 0x8000;
	FOKScriptObject StringPool;							// owns the memory InternString copies into
	uint32 StringPoolAddress = 0;
	int32 StringPoolUsed = 0;
	TMap<FString, uint32> InternedStrings;
	TMap<FString, uint32> Bindings;
	TMap<FString, int32> BindingIds;
	TArray<FOKMkoAsset> Assets;
	TMap<FString, int32> AssetIndex;					// "set/object" -> index in Assets
	uint32 RandomSeed = 0;

	TArray<TUniquePtr<FOKScriptThread>> Processes;
	int32 NextProcessNumber = 1;
	int64 SimFrame = 0;
};
