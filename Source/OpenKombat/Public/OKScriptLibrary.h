#pragma once

#include "CoreMinimal.h"
#include "OKMkoFile.h"

/** Every loaded .mko script, with their cross-script calls linked. */
class OPENKOMBAT_API FOKScriptLibrary
{
public:
	FOKScriptLibrary() = default;

	// Owns its scripts through TUniquePtr, so it can't be copied.
	FOKScriptLibrary(const FOKScriptLibrary&) = delete;
	FOKScriptLibrary& operator=(const FOKScriptLibrary&) = delete;

	/** Loads every .mko in Folder. Files that fail are logged and skipped. Returns the number loaded. */
	int32 LoadFolder(const FString& Folder);

	/**
	 * Loads every script in the game's packages (*.xxx in AssetDir): each MKScriptBinary export is its properties, an
	 * int32 length and the .mko itself, named after the export. A costume's package repeats its character's script;
	 * the first package in name order wins. Returns the number loaded.
	 */
	int32 LoadPackages(const FString& AssetDir);

	/** Points every import at its target function. Returns how many couldn't be resolved. */
	int32 LinkAll();

	/** Finds a script by file name, ignoring case. */
	const FOKMkoFile* FindScript(const FString& FileName) const;

	int32 NumScripts() const {
		return Scripts.Num();
	}
	int32 NumSkipped() const {
		return Skipped;
	}

	void ForEachScript(TFunctionRef<void(const FOKMkoFile&)> Visitor) const;

private:
	// Keyed by file name, e.g. "FightEngineScript.mko". FString map keys ignore case.
	TMap<FString, TUniquePtr<FOKMkoFile>> Scripts;
	int32 Skipped = 0;
};