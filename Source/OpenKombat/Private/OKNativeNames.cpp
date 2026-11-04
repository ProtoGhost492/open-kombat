#include "OKNativeNames.h"
#include "Misc/FileHelper.h"

namespace
{
	/** A PE image's bytes, read by virtual address through its section table. */
	struct FOKPeImage
	{
		TArray<uint8> Bytes;
		uint32 ImageBase = 0;
		struct FSection { uint32 Rva = 0, Size = 0, Raw = 0; };
		TArray<FSection> Sections;

		uint32 U32At(uint32 Offset) const
		{
			uint32 Value = 0;
			if (Offset + 4 <= uint32(Bytes.Num()))
			{
				FMemory::Memcpy(&Value, Bytes.GetData() + Offset, 4);
			}
			return Value;
		}

		bool Parse()
		{
			const uint32 Pe = U32At(0x3C);
			if (Bytes.Num() < 0x40 || U32At(Pe) != 0x00004550)		// the "PE" signature
			{
				return false;
			}
			const uint32 SectionCount = U32At(Pe + 4) >> 16;
			const uint32 OptionalSize = U32At(Pe + 20) & 0xFFFF;
			ImageBase = U32At(Pe + 24 + 28);						// PE32: ImageBase at optional header +28
			const uint32 Table = Pe + 24 + OptionalSize;
			for (uint32 i = 0; i < SectionCount; ++i)
			{
				const uint32 At = Table + i * 40;
				Sections.Add({ U32At(At + 12), FMath::Max(U32At(At + 8), U32At(At + 16)), U32At(At + 20) });
			}
			return Sections.Num() > 0;
		}

		/** The file offset of a virtual address, or 0 if no section holds it. */
		uint32 OffsetOf(uint32 Va) const
		{
			const uint32 Rva = Va - ImageBase;
			for (const FSection& S : Sections)
			{
				if (Rva >= S.Rva && Rva < S.Rva + S.Size)
				{
					return S.Raw + (Rva - S.Rva);
				}
			}
			return 0;
		}
	};
}

bool FOKNativeNames::LoadRegistry(const FString& ExePath, FString& OutError)
{
	ModuleSlots.Reset();
	FOKPeImage Image;
	if (!FFileHelper::LoadFileToArray(Image.Bytes, *ExePath) || !Image.Parse())
	{
		OutError = FString::Printf(TEXT("can't read %s as a Windows executable"), *ExePath);
		return false;
	}
	// [obs] MKScript's module table (0xDF5D88): 253 pointers to each module's registration, whose first 16 bits are
	// the module id that native ids carry in their high half. The table's order is the module number.
	constexpr uint32 ModuleTable = 0xDF5D88;
	constexpr int32 ModuleCount = 253;
	const uint32 TableOffset = Image.OffsetOf(ModuleTable);
	for (int32 i = 0; i < ModuleCount && TableOffset != 0; ++i)
	{
		const uint32 Entry = Image.OffsetOf(Image.U32At(TableOffset + i * 4));
		if (Entry == 0)
		{
			break;
		}
		ModuleSlots.Add(Image.U32At(Entry) & 0xFFFF, i);
	}
	if (ModuleSlots.Num() != ModuleCount)
	{
		OutError = FString::Printf(TEXT("%s: the module table isn't where MKKE keeps it (%d of %d modules)"), *ExePath, ModuleSlots.Num(), ModuleCount);
		ModuleSlots.Reset();
		return false;
	}
	return true;
}

bool FOKNativeNames::LoadNames(const FString& NamesPath)
{
	Names.Reset();
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *NamesPath))
	{
		return false;
	}
	for (int32 i = 1; i < Lines.Num(); ++i) // line 0 is the column header
	{
		TArray<FString> Columns;
		Lines[i].ParseIntoArray(Columns, TEXT("\t"), /*InCullEmpty*/ false);
		if (Columns.Num() >= 2 && !Columns[1].IsEmpty())
		{
			Names.Add(Columns[0], Columns[1]);
		}
	}
	return true;
}

FString FOKNativeNames::NameFor(uint32 NativeId) const
{
	const uint32 ModuleId = NativeId >> 16;
	const uint32 Index = (NativeId & 0xFFFF) - 1;

	const int32* Slot = ModuleSlots.Find(ModuleId);
	if (!Slot)
	{
		return FString::Printf(TEXT("n?%04X.%u"), ModuleId, Index);
	}
	const FString Key = FString::Printf(TEXT("%d.%u"), *Slot, Index);
	const FString* Name = Names.Find(Key);
	return Name ? *Name : TEXT("n") + Key;
}