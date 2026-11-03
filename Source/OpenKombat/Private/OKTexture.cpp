#include "OKTexture.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace
{
	constexpr uint32 BulkSeparateFile = 0x01;	// the payload lives in another file (texture cache)
	constexpr uint32 BulkUnused = 0x20;			// not stored at all

	/** UE5's pixel format for a UE3 one, and the bytes per 4x4 block (or per pixel for uncompressed formats). */
	bool PixelFormatFor(int32 Format, EPixelFormat& OutFormat, int32& OutBlockBytes, bool& bOutCompressed)
	{
		bOutCompressed = true;
		switch (Format)
		{
		case 5: OutFormat = PF_DXT1; OutBlockBytes = 8; return true;
		case 6: OutFormat = PF_DXT3; OutBlockBytes = 16; return true;
		case 7: OutFormat = PF_DXT5; OutBlockBytes = 16; return true;
		case 24: OutFormat = PF_BC5; OutBlockBytes = 16; return true;
		case 2: OutFormat = PF_B8G8R8A8; OutBlockBytes = 4; bOutCompressed = false; return true;	// A8R8G8B8 is BGRA in memory
		case 3: OutFormat = PF_G8; OutBlockBytes = 1; bOutCompressed = false; return true;
		default: return false;
		}
	}

	int64 ExpectedMipBytes(int32 Width, int32 Height, int32 BlockBytes, bool bCompressed)
	{
		if (!bCompressed)
		{
			return int64(Width) * Height * BlockBytes;
		}
		return int64(FMath::Max(1, (Width + 3) / 4)) * FMath::Max(1, (Height + 3) / 4) * BlockBytes;
	}
}

bool FOKTextureData::Read(FOKPackage& Package, int32 ExportRef, FString& OutError)
{
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	if (!Exports.IsValidIndex(ExportRef - 1) || Exports[ExportRef - 1].ClassName != TEXT("Texture2D"))
	{
		OutError = FString::Printf(TEXT("%s isn't a Texture2D stored in %s"), *Package.ObjectPath(ExportRef), *Package.GetName());
		return false;
	}
	const FOKPackageExport& Export = Exports[ExportRef - 1];
	TArray<uint8> Bytes;
	if (!Package.ExportData(Export, Bytes))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *Export.Path);
		return false;
	}

	FOKByteReader R(Bytes);
	FOKPropertyList Properties;
	if (!Properties.Read(R, Package.GetNames()))
	{
		OutError = FString::Printf(TEXT("%s: the property list is damaged"), *Export.Path);
		return false;
	}
	Name = Export.Name;
	SizeX = int32(Properties.GetNumber(TEXT("SizeX")));
	SizeY = int32(Properties.GetNumber(TEXT("SizeY")));
	Format = int32(Properties.GetNumber(TEXT("Format"), -1));

	// Two empty bulk-data headers, then the mips: each a bulk header {flags, count, size, offset},
	// the payload when it's stored here, and the mip's size.
	R.Skip(16 * 2);
	const int32 MipCount = R.I32();
	if (MipCount < 0 || MipCount > 32)
	{
		OutError = FString::Printf(TEXT("%s: bad mip count %d"), *Export.Path, MipCount);
		return false;
	}
	Mips.Reset();
	for (int32 i = 0; i < MipCount; ++i)
	{
		const uint32 Flags = R.U32();
		R.Skip(4);
		const int32 Size = R.I32();
		R.Skip(4);
		const bool bStored = !(Flags & (BulkUnused | BulkSeparateFile));
		const uint8* Payload = (bStored && Size > 0) ? R.Take(Size) : nullptr;
		const int32 Width = R.I32();
		const int32 Height = R.I32();
		if (Payload)
		{
			FOKTextureMip& Mip = Mips.AddDefaulted_GetRef();
			Mip.Width = Width;
			Mip.Height = Height;
			Mip.Data.Append(Payload, Size);
		}
	}
	if (R.HasError())
	{
		OutError = FString::Printf(TEXT("%s: the mip data is damaged"), *Export.Path);
		return false;
	}
	if (Mips.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s: no mip is stored in the package (it may be in a texture cache file)"), *Export.Path);
		return false;
	}
	if (Format < 0)
	{
		// No Format property: infer it from the first mip's size, like the viewer does.
		const int64 Pixels = int64(Mips[0].Width) * Mips[0].Height;
		const int64 Size = Mips[0].Data.Num();
		Format = Size * 2 == Pixels ? 5 : Size == Pixels ? 7 : Size == Pixels * 4 ? 2 : -1;
	}
	return true;
}

UTexture2D* FOKTextureData::CreateTexture(bool bSRGB, FString& OutError) const
{
	EPixelFormat PixelFormat = PF_Unknown;
	int32 BlockBytes = 0;
	bool bCompressed = true;
	if (!PixelFormatFor(Format, PixelFormat, BlockBytes, bCompressed))
	{
		OutError = FString::Printf(TEXT("%s: pixel format %d isn't supported"), *Name, Format);
		return nullptr;
	}

	// Use the stored mips as long as each one is half the previous and has all its bytes.
	TArray<const FOKTextureMip*> Chain;
	for (const FOKTextureMip& Mip : Mips)
	{
		if (Chain.Num() > 0)
		{
			const FOKTextureMip& Previous = *Chain.Last();
			if (Mip.Width != FMath::Max(1, Previous.Width / 2) || Mip.Height != FMath::Max(1, Previous.Height / 2))
			{
				break;
			}
		}
		if (Mip.Data.Num() < ExpectedMipBytes(Mip.Width, Mip.Height, BlockBytes, bCompressed))
		{
			break;
		}
		Chain.Add(&Mip);
	}
	if (Chain.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s: the top mip is smaller than its format needs"), *Name);
		return nullptr;
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(Chain[0]->Width, Chain[0]->Height, PixelFormat, MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(*Name)));
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("%s: can't create a %dx%d texture"), *Name, Chain[0]->Width, Chain[0]->Height);
		return nullptr;
	}

	// CreateTransient made one empty mip; replace it with ours.
	FTexturePlatformData* Platform = Texture->GetPlatformData();
	Platform->Mips.Empty();
	for (const FOKTextureMip* Mip : Chain)
	{
		FTexture2DMipMap* Out = new FTexture2DMipMap(Mip->Width, Mip->Height, 1);
		Platform->Mips.Add(Out);
		const int64 Bytes = ExpectedMipBytes(Mip->Width, Mip->Height, BlockBytes, bCompressed);
		Out->BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Out->BulkData.Realloc(Bytes), Mip->Data.GetData(), Bytes);
		Out->BulkData.Unlock();
	}
	Texture->SRGB = bSRGB;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	return Texture;
}
