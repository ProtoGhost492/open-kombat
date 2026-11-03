#pragma once

#include "CoreMinimal.h"

/** Names for script natives, from the RE notes (registry.json + native-names.tsv). */
class OPENKOMBAT_API FOKNativeNames
{
public:
	/** Loads both tables. On failure, returns false and says why in OutError. */
	bool Load(const FString& RegistryPath, const FString& NamesPath, FString& OutError);

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