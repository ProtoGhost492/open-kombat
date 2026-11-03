#pragma once

#include "CoreMinimal.h"

class FOKPackage;
class UTexture2D;

/** One mip level stored in the package. */
struct FOKTextureMip
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Data;
};

/** A Texture2D as MKKE stores it (MK-FORMAT-NOTES, "Texture2D"). */
struct OPENKOMBAT_API FOKTextureData
{
	FString Name;
	int32 SizeX = 0;
	int32 SizeY = 0;
	int32 Format = -1;				// UE3 EPixelFormat: 2 = A8R8G8B8, 3 = G8, 5 = DXT1, 6 = DXT3, 7 = DXT5, 24 = BC5
	TArray<FOKTextureMip> Mips;		// the stored mips, largest first

	/** Reads export ExportRef (1-based). False, with OutError, if it isn't a readable Texture2D. */
	bool Read(FOKPackage& Package, int32 ExportRef, FString& OutError);

	/** A runtime texture with the stored mips, uploaded as they are (DXT stays compressed). */
	UTexture2D* CreateTexture(bool bSRGB, FString& OutError) const;
};
