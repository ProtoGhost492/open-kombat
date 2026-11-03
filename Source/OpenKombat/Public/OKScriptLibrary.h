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