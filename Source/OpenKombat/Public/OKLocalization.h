#pragma once

#include "CoreMinimal.h"

/**
 * MKKE's localised text (MENU-RE-NOTES §9.7). Localization/Coalesced.<lang> bundles every .<lang> file: a u32 size, a
 * zlib stream whose bytes are inverted, then u32 count and per file {u32 length, name, u32 length, text}. The texts
 * are ini-style, [Section] then Key=Value, and lookups ignore case, as UE3's do.
 */
class OPENKOMBAT_API FOKLocalization
{
public:
	/** The text for ok.Language, read the first time it's asked for. */
	static const FOKLocalization& Get();

	/** Section's Key ("Gameplay.Fighting", "Wins"), or null. */
	const FString* Find(const FString& Section, const FString& Key) const;

	/** Reads a Coalesced bundle. False, with OutError, if it can't. */
	bool Load(const FString& Path, FString& OutError);

private:
	void AddIni(const FString& Text);

	TMap<FString, FString> Entries;		// "section/key" in lower case -> text
};
