#pragma once

#include "CoreMinimal.h"

/**
 * Script natives' modules and names. The registry (a native id's module id -> its module number, which the native
 * table is keyed by) comes from the game's executable; the names, for logs and disassembly, from the RE notes'
 * native-names.tsv when there is one.
 */
class OPENKOMBAT_API FOKNativeNames
{
public:
	/** Reads the registry from MKKE.exe: its 253-entry module table (0xDF5D88), each entry pointing at the module's id. */
	bool LoadRegistry(const FString& ExePath, FString& OutError);

	/** Reads native-names.tsv (native, name, confidence, note). Returns false if there's no such file. */
	bool LoadNames(const FString& NamesPath);

	/** e.g. "set_attack_block_a", or "n137.249" when the native has no name yet. */
	FString NameFor(uint32 NativeId) const;

	int32 NumModules() const {
		return ModuleSlots.Num();
	}
	int32 NumNames() const {
		return Names.Num();
	}

	/** Module number for a registry module id (0x1641 -> 137), or INDEX_NONE. */
	int32 SlotForModule(uint32 ModuleId) const
	{
		const int32* Slot = ModuleSlots.Find(ModuleId);
		return Slot ? *Slot : INDEX_NONE;
	}

private:
	TMap<uint32, int32> ModuleSlots; // registry module id (0x1641) -> module number (137)
	TMap<FString, FString> Names;	 // "137.249" -> "set_attack_block_a"
};