#pragma once

#include "CoreMinimal.h"

class FOKNativeNames;
class FOKMkoFile;

/** One function record from a .mko script (FIGHT-RE-NOTES §2). */
struct FOKMkoFunction
{
	FString Name;
	uint32 NameHash = 0;			// FNV-1 hash of Name
	uint32 SignatureHash = 0;		// dword 11: tells same-named overloads apart
	uint32 CodeWord = 0;			// where its bytecode starts, in 4-byte words
	uint32 CodeBytes = 0;			// dword 3: size of its bytecode in bytes
	uint32 NumParams = 0;
	uint32 LocalsPoolOffset = 0;	// dword 4: starting values of the locals, in the pool
	uint32 LocalsBytes = 0;			// dword 5: size of the locals
	uint32 MaxArgs = 0;				// dword 6: how many argument words a caller may pass
	uint32 NumTemps = 0;			// dword 10: 8-byte temporaries after the locals
};

/** A load-time relocation (16 bytes): kind, where (a pool or code byte offset), value, extra. */
struct FOKMkoFixup
{
	uint32 Kind = 0;
	uint32 Where = 0;
	uint32 Value = 0;
	uint32 Extra = 0;
};

/** An asset import (table C): scripts name animations and other assets by index + 1 into this table. */
struct FOKMkoAsset
{
	FString Set;		// AnimSet or package, e.g. anims_scorpion
	FString Object;		// e.g. FrontPunch01
};

/** A call into another script (table B, 28-byte records). */
/** A global variable (table A): where it is in the pool and how big it is. Names are only stored as hashes. */
struct FOKMkoGlobal
{
	uint32 NameHash = 0;
	uint32 Size = 0;			// bytes
	uint32 ElementSize = 0;		// bytes per element; Size / ElementSize elements
	uint32 PoolOffset = 0;
};

struct FOKMkoImport
{
	FString TargetFile;		// e.g. "FightEngineScript.mko"
	uint32 NameHash = 0;
	uint32 SignatureHash = 0;

	// Filled in by FOKScriptLibrary::LinkAll.
	const FOKMkoFile* Target = nullptr;
	int32 TargetFunction = INDEX_NONE;
};

/** A loaded MKScript binary: the raw bytes, and where each section starts. */
class OPENKOMBAT_API FOKMkoFile
{
public:
	/** ok.MkoDir: a folder of extracted .mko scripts to load instead of the game's packages. Empty (the default) means the packages. */
	static FString ScriptDir();

	/** Reads and checks a .mko file. On failure, returns false and says why in OutError. */
	bool Load(const FString& Path, FString& OutError);

	/** The same for a script already in memory (an MKScriptBinary's body), named FileName ("CHAR_Scorpion.mko"). */
	bool LoadFromBytes(TArray<uint8>&& InBytes, const FString& FileName, FString& OutError);

	/** File name, e.g. "CHAR_Scorpion.mko". */

	const FString& GetName() const { return Name; }

	const TArray<FOKMkoFunction>& GetFunctions() const { return Functions; }
	const TArray<FOKMkoImport>& GetImports() const {
		return Imports;
	}
	TArray<FOKMkoImport>& GetImports() {
		return Imports;
	}

	uint32 GetNumGlobals() const { return Header[3]; }
	uint32 GetCodeBytes() const { return Header[10]; }
	int32 GetFileBytes() const { return Bytes.Num(); }

	/** Index of the function with this name and signature, or INDEX_NONE. */
	int32 FindFunction(uint32 NameHash, uint32 SignatureHash) const;

	/** The hash MKKE uses for function and global names. */
	static uint32 Fnv1(const FString& Text);

	/** First function with this name (ignoring case), or INDEX_NONE. Overloads share names. */
	int32 FindFunctionByName(const FString& FunctionName) const;

	/** One text line per instruction. NativeNames may be null. Returns false if the code can't be decoded. */
	bool Disassemble(int32 FuncIndex, const FOKNativeNames* NativeNames, TArray<FString>& OutLines) const;

	/** One 4-byte word of bytecode; word 0 is the start of the code section. */
	bool ReadCodeWord(int64 WordIndex, uint32& Out) const { return ReadU32(CodeOffset + WordIndex * 4, Out); }

	/** The data pool as stored in the file: globals' starting values, strings, initial locals. */
	TConstArrayView<uint8> GetPool() const { return TConstArrayView<uint8>(Bytes.GetData() + PoolOffset, Header[12]); }

	/** Name of a module-0 opcode, e.g. "setarg". */
	static const TCHAR* GetOpName(int32 Op);

	/** Names of the object-reference table, in order: "my_obj", "his_obj", ... */
	const TArray<FString>& GetObjectRefs() const { return ObjectRefs; }

	/** An asset a script names by number (index + 1, as in instruction operands), or null. */
	const FOKMkoAsset* FindAsset(uint32 Number) const { return AssetImports.IsValidIndex(int32(Number) - 1) ? &AssetImports[Number - 1] : nullptr; }

	/** A sound a script names (table D, by 0-based index): its group (Set, snd_fightfx1) and event (Object, sfx_hit_face). */
	const FOKMkoAsset* FindSound(uint32 Index) const { return SoundImports.IsValidIndex(int32(Index)) ? &SoundImports[Index] : nullptr; }
	
	/** The data region and the pool, as stored in the file (one block: data first, then the pool). */
	TConstArrayView<uint8> GetDataAndPool() const
	{
		return TConstArrayView<uint8>(Bytes.GetData() + DataOffset, int32(CodeOffset - DataOffset));
	}

	/** Where the pool starts inside GetDataAndPool(). */
	uint32 GetPoolBase() const { return Header[11]; }

	const TArray<FOKMkoFixup>& GetFixups() const { return Fixups; }
	const TArray<FOKMkoGlobal>& GetGlobals() const { return Globals; }

	/** Names a script function passed as a native argument, e.g. "SharedReactions.r_hit_airborn". */
	FString DescribeFunctionRef(uint32 Raw) const { return ImportName(int32(Raw & 0x3FFFFFFF) - 1); }

private:
	bool ReadU32(int64 Offset, uint32& Out) const;
	bool ReadCString(int64 Offset, FString& Out) const;

	FString FormatOperand(uint32 Value, bool bLiteral, bool bIndirect, bool bNativeArg) const;
	FString ImportName(int32 ImportIndex) const;
	static FString FormatLiteral(uint32 Value);

	/** Names of the object-reference table ("my_obj", "his_info", ...). */
	TArray<FString> ObjectRefs;

	TArray<FOKMkoAsset> AssetImports;
	TArray<FOKMkoAsset> SoundImports;
	TArray<FOKMkoGlobal> Globals;

	FString Name;
	TArray<uint8> Bytes;
	uint32 Header[16] = {};

	int64 DataOffset = 0;		// data region; function names are 1-based offsets into it
	int64 PoolOffset = 0;		// string pool and global storage
	int64 CodeOffset = 0;		// bytecode
	int64 FixupOffset = 0;		// relocation records

	TArray<FOKMkoFunction> Functions;
	TArray<FOKMkoImport> Imports;
	TArray<FOKMkoFixup> Fixups;

	/** (name hash << 32 | signature hash) -> function index. */
	TMap<uint64, int32> FunctionIndex;
};
