#include "OKPackage.h"
#include "OKByteReader.h"
#include "OKLzo.h"
#include "OKProperties.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogOKAsset);

/**
 * A whole-file compressed package (Startup, Core, Engine, MK9Game...) is a run of compressed chunks, each with
 * the same header as a chunk inside a package (tag, block size, compressed size, uncompressed size, then the
 * blocks' sizes); together they inflate to an ordinary package. The .meta file beside it holds the full size.
 */
static bool InflateWholeFile(const TArray<uint8>& In, TArray<uint8>& Out, FString& OutError)
{
	FOKByteReader R(In);
	while (R.Remaining() >= 16)
	{
		const uint32 ChunkTag = R.U32();
		if (ChunkTag == 0 && !Out.IsEmpty())
		{
			break;		// the file is padded with zeros to a multiple of 32 KB
		}
		const int32 BlockSize = R.I32();
		R.Skip(4);
		const int32 Total = R.I32();
		if (ChunkTag != FOKPackage::Tag || BlockSize <= 0 || Total < 0)
		{
			OutError = FString::Printf(TEXT("bad compressed chunk at %d"), int32(In.Num() - R.Remaining() - 16));
			return false;
		}
		TArray<TPair<int32, int32>> Blocks;
		for (int32 i = 0; i < (Total + BlockSize - 1) / BlockSize; ++i)
		{
			const int32 Compressed = R.I32();
			const int32 Uncompressed = R.I32();
			Blocks.Emplace(Compressed, Uncompressed);
		}
		for (const TPair<int32, int32>& Block : Blocks)
		{
			const uint8* Data = R.Take(Block.Key);
			if (!Data || Block.Value < 0)
			{
				OutError = TEXT("a compressed chunk's block table doesn't fit");
				return false;
			}
			const int32 At = Out.AddUninitialized(Block.Value);
			if (OKLzo::Decompress(Data, Block.Key, Out.GetData() + At, Block.Value) != Block.Value)
			{
				OutError = TEXT("bad LZO data");
				return false;
			}
		}
	}
	return true;
}

TUniquePtr<FOKPackage> FOKPackage::Open(const FString& Path, FString& OutError)
{
	TUniquePtr<FOKPackage> Package = MakeUnique<FOKPackage>();
	Package->Name = FPaths::GetBaseFilename(Path);
	if (!FFileHelper::LoadFileToArray(Package->File, *Path))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *Path);
		return nullptr;
	}
	// Whole-file compressed: the file starts with a chunk header (tag, then the 0x20000 block size).
	if (Package->File.Num() >= 8 && FMemory::Memcmp(Package->File.GetData(), &Tag, 4) == 0
		&& Package->File[4] == 0 && Package->File[5] == 0 && Package->File[6] == 2 && Package->File[7] == 0)
	{
		TArray<uint8> Inflated;
		if (!InflateWholeFile(Package->File, Inflated, OutError))
		{
			OutError = Package->Name + TEXT(": ") + OutError;
			return nullptr;
		}
		Package->File = MoveTemp(Inflated);
	}
	if (!Package->ReadSummary(OutError) || !Package->ReadTables(OutError))
	{
		OutError = Package->Name + TEXT(": ") + OutError;
		return nullptr;
	}
	return Package;
}

bool FOKPackage::ReadSummary(FString& OutError)
{
	FOKByteReader R(File);
	if (R.U32() != Tag)
	{
		OutError = TEXT("not an Unreal package");
		return false;
	}
	FOKPackageSummary& S = Summary;
	S.FileVersion = R.U16();
	S.LicenseeVersion = R.U16();
	if (S.FileVersion == 0 && S.LicenseeVersion == 2)
	{
		// Bytes 4-7 are 0x20000, a chunk's block size: the whole file is one compressed stream.
		OutError = TEXT("whole-file compressed packages (Core, Engine, ...) aren't supported yet");
		return false;
	}
	S.HeaderSize = R.I32();
	const uint8* Marker = R.Take(4);
	if (!Marker || FMemory::Memcmp(Marker, "MK  ", 4) != 0)
	{
		OutError = TEXT("not the MK:KE variant of UE3 (no \"MK  \" marker)");
		return false;
	}
	S.MkValue = R.I32();
	S.FolderName = R.ReadFString();
	S.PackageFlags = R.U32();
	S.NameCount = R.I32();
	S.NameOffset = R.I32();
	S.ExportCount = R.I32();
	S.ExportOffset = R.I32();
	S.ImportCount = R.I32();
	S.ImportOffset = R.I32();
	S.DependsOffset = R.I32();
	R.Skip(4);	// always 0 (GenerationCount in stock UE3)
	R.Skip(16); // GUID
	S.EngineVersion = R.I32();
	S.CookerVersion = R.I32();
	S.CompressionFlags = R.U32();

	const int32 ChunkCount = R.I32();
	if (ChunkCount < 0 || ChunkCount > 100000)
	{
		OutError = TEXT("bad chunk count");
		return false;
	}
	for (int32 i = 0; i < ChunkCount; ++i)
	{
		FOKPackageChunk& C = S.Chunks.AddDefaulted_GetRef();
		C.UncompressedOffset = R.I32();
		C.UncompressedSize = R.I32();
		C.CompressedOffset = R.I32();
		C.CompressedSize = R.I32();
	}
	Decompressed.SetNum(ChunkCount);

	auto Sane = [](int32 N) { return N >= 0 && N < 50000000; };
	if (R.HasError() || !Sane(S.NameCount) || !Sane(S.ExportCount) || !Sane(S.ImportCount)
		|| !Sane(S.NameOffset) || !Sane(S.ExportOffset) || !Sane(S.ImportOffset))
	{
		OutError = TEXT("the summary is damaged or truncated");
		return false;
	}
	return true;
}

int64 FOKPackage::GetUncompressedSize() const
{
	if (Summary.Chunks.Num() == 0)
	{
		return File.Num();
	}
	int64 End = 0;
	for (const FOKPackageChunk& C : Summary.Chunks)
	{
		End = FMath::Max<int64>(End, int64(C.UncompressedOffset) + C.UncompressedSize);
	}
	return End;
}

const TArray<uint8>* FOKPackage::Chunk(int32 Index)
{
	TArray<uint8>& Out = Decompressed[Index];
	const FOKPackageChunk& C = Summary.Chunks[Index];
	if (Out.Num() == C.UncompressedSize && C.UncompressedSize > 0)
	{
		return &Out;		// already done
	}
	if (C.CompressedOffset < 0 || int64(C.CompressedOffset) + C.CompressedSize > File.Num())
	{
		UE_LOG(LogOKAsset, Error, TEXT("%s: chunk %d lies outside the file"), *Name, Index);
			return nullptr;
	}

	// Chunk header: tag, block size, compressed size, uncompressed size; then (compressed, uncompressed) per block.
	FOKByteReader R(TConstArrayView<uint8>(File.GetData() + C.CompressedOffset, C.CompressedSize));
	const uint32 ChunkTag = R.U32();
	const int32 BlockSize = R.I32();
	R.Skip(4);
	const int32 Total = R.I32();
	if (ChunkTag != Tag || BlockSize <= 0 || Total != C.UncompressedSize)
	{
		UE_LOG(LogOKAsset, Error, TEXT("%s: chunk %d has a bad header"), *Name, Index);
		return nullptr;
	}
	TArray<TPair<int32, int32>> Blocks;
	for (int32 i = 0; i < (Total + BlockSize - 1) / BlockSize; ++i)
	{
		const int32 Compressed = R.I32();
		const int32 Uncompressed = R.I32();
		Blocks.Emplace(Compressed, Uncompressed);
	}

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Total);
	int32 Written = 0;
	for (const TPair<int32, int32>& Block : Blocks)
	{
		const uint8* In = R.Take(Block.Key);
		if (!In || Block.Value < 0 || Written + Block.Value > Total)
		{
			UE_LOG(LogOKAsset, Error, TEXT("%s: chunk %d's block table doesn't fit"), *Name, Index);
			return nullptr;
		}
		if (Summary.CompressionFlags & CompressLzo)
		{
			if (OKLzo::Decompress(In, Block.Key, Bytes.GetData() + Written, Block.Value) != Block.Value)
			{
				UE_LOG(LogOKAsset, Error, TEXT("%s: chunk %d: bad LZO data"), *Name, Index);
				return nullptr;
			}
		}
		else if (Summary.CompressionFlags == 0)
		{
			FMemory::Memcpy(Bytes.GetData() + Written, In, Block.Value);
		}
		else
		{
			UE_LOG(LogOKAsset, Error, TEXT("%s: compression %u isn't supported"), *Name, Summary.CompressionFlags);
			return nullptr;
		}
		Written += Block.Value;
	}
	Out = MoveTemp(Bytes);
	return &Out;
}

bool FOKPackage::Read(int64 Offset, int32 Length, TArray<uint8>& Out)
{
	Out.SetNumUninitialized(FMath::Max(Length, 0));
	if (Offset < 0 || Length < 0)
	{
		return false;
	}
	const int64 End = Offset + Length;
	if (Summary.Chunks.Num() == 0)
	{
		if (End > File.Num())
		{
			return false;
		}
		FMemory::Memcpy(Out.GetData(), File.GetData() + Offset, Length);
		return true;
	}

	int64 Filled = 0;
	if (Offset < Summary.NameOffset)
	{
		// Everything before the name table is stored uncompressed, at the same offsets.
		const int64 Count = FMath::Min<int64>(End, Summary.NameOffset) - Offset;
		FMemory::Memcpy(Out.GetData(), File.GetData() + Offset, Count);
		Filled += Count;
	}
	for (int32 i = 0; i < Summary.Chunks.Num() && Filled < Length; ++i)
	{
		const FOKPackageChunk& C = Summary.Chunks[i];
		const int64 Start = C.UncompressedOffset;
		const int64 Stop = Start + C.UncompressedSize;
		if (Stop <= Offset || Start >= End)
		{
			continue;
		}
		const TArray<uint8>* Data = Chunk(i);
		if (!Data)
		{
			return false;
		}
		const int64 From = FMath::Max(Offset, Start);
		const int64 To = FMath::Min(End, Stop);
		FMemory::Memcpy(Out.GetData() + (From - Offset), Data->GetData() + (From - Start), To - From);
		Filled += To - From;
	}
	return Filled == Length;
}

bool FOKPackage::ReadTables(FString& OutError)
{
	const FOKPackageSummary& S = Summary;
	const int64 Start = S.NameOffset;
	const int64 TableEnd = FMath::Max(FMath::Max<int64>(S.HeaderSize, int64(S.ExportOffset) + 1), FMath::Max<int64>(int64(S.ImportOffset) + 1, S.DependsOffset));
	const int64 End = FMath::Min<int64>(GetUncompressedSize(), FMath::Max(TableEnd, Start));
	TArray<uint8> Tables;
	if (!Read(Start, int32(End - Start), Tables))
	{
		OutError = TEXT("can't decompress the tables");
		return false;
	}
	FOKByteReader R(Tables);

	R.Seek(S.NameOffset - Start);
	Names.Reserve(S.NameCount);
	for (int32 i = 0; i < S.NameCount && !R.HasError(); ++i)
	{
		Names.Add(R.ReadFString());
		R.Skip(8);		// object flags
	}

	// An FName on disk: an index into the name table, plus a number (n > 0 means "Name_<n-1>").
	bool bBadName = false;
	auto ReadName = [this, &R, &bBadName]() -> FString
		{
			const int32 Index = R.I32();
			const int32 Number = R.I32();
			if (!Names.IsValidIndex(Index))
			{
				bBadName = true;
				return FString();
			}
			return Number ? FString::Printf(TEXT("%s_%d"), *Names[Index], Number - 1) : Names[Index];
		};

	R.Seek(S.ImportOffset - Start);
	for (int32 i = 0; i < S.ImportCount && !R.HasError(); ++i)
	{
		FOKPackageImport& Import = Imports.AddDefaulted_GetRef();
		Import.ClassPackage = ReadName();
		Import.ClassName = ReadName();
		Import.Outer = R.I32();
		Import.Name = ReadName();
	}

	R.Seek(S.ExportOffset - Start);
	for (int32 i = 0; i < S.ExportCount && !R.HasError(); ++i)
	{
		FOKPackageExport& Export = Exports.AddDefaulted_GetRef();
		Export.Index = i + 1;
		Export.ClassRef = R.I32();
		Export.SuperRef = R.I32();
		Export.Outer = R.I32();
		Export.Name = ReadName();
		Export.Archetype = R.I32();
		Export.ObjectFlags = R.U32();
		Export.ObjectFlags |= uint64(R.U32()) << 32;
		Export.SerialSize = R.I32();
		Export.SerialOffset = R.I32();
		const int32 Components = R.I32();		// component map: (FName, int) pairs, not needed yet
		if (Components < 0 || Components > 10000)
		{
			OutError = FString::Printf(TEXT("export %d has a bad component map"), i + 1);
			return false;
		}
		R.Skip(int64(Components) * 12);
		R.Skip(5 * 4);		// five ints of unknown meaning (ExportFlags, NetObjects, Guid in stock UE3)
	}

	if (R.HasError() || bBadName)
	{
		OutError = TEXT("the name/import/export tables are damaged");
		return false;
	}
	for (FOKPackageExport& Export : Exports)
	{
		Export.ClassName = Export.ClassRef == 0 ? TEXT("Class") : ObjectName(Export.ClassRef);
		Export.Path = ObjectPath(Export.Index);
	}
	return true;
}

FString FOKPackage::ObjectName(int32 Ref) const
{
	if (Ref > 0)
	{
		return Exports.IsValidIndex(Ref - 1) ? Exports[Ref - 1].Name : FString::Printf(TEXT("? export%d"), Ref);
	}
	if (Ref < 0)
	{
		return Imports.IsValidIndex(-Ref - 1) ? Imports[-Ref - 1].Name : FString::Printf(TEXT("? import%d"), Ref);
	}
	return TEXT("None");
}

FString FOKPackage::ObjectPath(int32 Ref) const
{
	TArray<FString> Parts;
	for (int32 Guard = 0; Ref != 0 && Guard < 64; ++Guard)
	{
		if (Ref > 0 && Exports.IsValidIndex(Ref - 1))
		{
			Parts.Insert(Exports[Ref - 1].Name, 0);
			Ref = Exports[Ref - 1].Outer;
		}
		else if (Ref < 0 && Imports.IsValidIndex(-Ref - 1))
		{
			Parts.Insert(Imports[-Ref - 1].Name, 0);
			Ref = Imports[-Ref - 1].Outer;
		}
		else
		{
			break;
		}
	}
	return FString::Join(Parts, TEXT("."));
}

TArray<const FOKPackageExport*> FOKPackage::FindExports(const FString& ClassName) const
{
	TArray<const FOKPackageExport*> Found;
	for (const FOKPackageExport& Export : Exports)
	{
		if (Export.ClassName == ClassName)
		{
			Found.Add(&Export);
		}
	}
	return Found;
}

const FOKPackageExport* FOKPackage::FindExportByPath(const FString& Path) const
{
	return Exports.FindByPredicate([&](const FOKPackageExport& E) { return E.Path == Path; });
}

// --- Console tools ----------------------------------------------

static TAutoConsoleVariable<FString> CVarGameDir(
	TEXT("ok.GameDir"),
	TEXT("C:/Program Files (x86)/Steam/steamapps/common/MortalKombat_KompleteEdition"),
	TEXT("Where MKKE is installed. Packages are read from <GameDir>/DiscContentPC/Asset."));

TUniquePtr<FOKPackage> FOKPackage::OpenGamePackage(const FString& PackageName, FString& OutError)
{
	return Open(FPaths::Combine(CVarGameDir.GetValueOnGameThread(), TEXT("DiscContentPC/Asset"), PackageName + TEXT(".xxx")), OutError);
}

static FAutoConsoleCommand CmdPkgInfo(
	TEXT("ok.Pkg.Info"),
	TEXT("ok.Pkg.Info <Package>: summary and tables of an MKKE package, e.g. ok.Pkg.Info CHAR_Scorpion"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Pkg.Info CHAR_Scorpion"));
				return;
			}
			const double StartTime = FPlatformTime::Seconds();
			FString Error;
			TUniquePtr<FOKPackage> Package = FOKPackage::OpenGamePackage(Args[0], Error);
			if (!Package)
			{
				UE_LOG(LogOKAsset, Error, TEXT("%s"), *Error);
				return;
			}
			const FOKPackageSummary& S = Package->GetSummary();
			UE_LOG(LogOKAsset, Log, TEXT("%s: version %u%u, MK value %d, engine %d, cooker %d, compression %u"),
				*Package->GetName(), S.FileVersion, S.LicenseeVersion, S.MkValue, S.EngineVersion, S.CookerVersion, S.CompressionFlags);
			UE_LOG(LogOKAsset, Log, TEXT("%d names, %d imports, %d exports, %d chunks, %lld bytes uncompressed"),
				S.NameCount, S.ImportCount, S.ExportCount, S.Chunks.Num(), Package->GetUncompressedSize());

			const TArray<FString>& Names = Package->GetNames();
			UE_LOG(LogOKAsset, Log, TEXT("first names: %s"), *FString::Join(TArrayView<const FString>(Names).Left(6), TEXT(", ")));

			// The most common classes, in the order they first appear.
			TArray<TPair<FString, int32>> Classes;
			for (const FOKPackageExport& Export : Package->GetExports())
			{
				TPair<FString, int32>* Entry = Classes.FindByPredicate([&](const TPair<FString, int32>& P) { return P.Key == Export.ClassName; });
				if (Entry)
				{
					++Entry->Value;
				}
				else
				{
					Classes.Emplace(Export.ClassName, 1);
				}
			}
			Classes.StableSort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
			TArray<FString> Top;
			for (int32 i = 0; i < FMath::Min(8, Classes.Num()); ++i)
			{
				Top.Add(FString::Printf(TEXT("%s %d"), *Classes[i].Key, Classes[i].Value));
			}
			UE_LOG(LogOKAsset, Log, TEXT("classes: %s"), *FString::Join(Top, TEXT(", ")));

			// Read the biggest mesh's bytes: that decompresses every chunk it spans.
			const FOKPackageExport* Biggest = nullptr;
			for (const FOKPackageExport* Mesh : Package->FindExports(TEXT("SkeletalMesh")))
			{
				if (!Biggest || Mesh->SerialSize > Biggest->SerialSize)
				{
					Biggest = Mesh;
				}
			}
			TArray<uint8> Bytes;
			if (Biggest && Package->ExportData(*Biggest, Bytes))
			{
				uint32 Sum = 0;
				for (const uint8 Byte : Bytes)
				{
					Sum += Byte;
				}
				UE_LOG(LogOKAsset, Log, TEXT("largest SkeletalMesh: #%d %s, %d bytes, byte sum %u"), Biggest->Index, *Biggest->Path, Biggest->SerialSize, Sum);
			}
			UE_LOG(LogOKAsset, Log, TEXT("(%.0f ms)"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
		}));

static FAutoConsoleCommand CmdPkgProps(
	TEXT("ok.Pkg.Props"),
	TEXT("ok.Pkg.Props <Package> <ObjectPath>: an object's tagged properties, e.g. ok.Pkg.Props CHAR_Scorpion Meshes.Scorpion"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Pkg.Props CHAR_Scorpion Meshes.Scorpion"));
				return;
			}
			FString Error;
			TUniquePtr<FOKPackage> Package = FOKPackage::OpenGamePackage(Args[0], Error);
			const FOKPackageExport* Export = Package ? Package->FindExportByPath(Args[1]) : nullptr;
			TArray<uint8> Data;
			if (!Export || !Package->ExportData(*Export, Data))
			{
				UE_LOG(LogOKAsset, Error, TEXT("Can't read %s in %s %s"), *Args[1], *Args[0], *Error);
				return;
			}

			FOKByteReader Reader(Data);
			FOKPropertyList Properties;
			if (!Properties.Read(Reader, Package->GetNames()))
			{
				UE_LOG(LogOKAsset, Error, TEXT("%s: the property list is damaged"), *Export->Path);
				return;
			}
			UE_LOG(LogOKAsset, Log, TEXT("%s (%s): %d properties, native data at %lld of %d"),
				*Export->Path, *Export->ClassName, Properties.GetAll().Num(), Reader.Tell(), Data.Num());

			for (const FOKProperty& P : Properties.GetAll())
			{
				const FString Key = P.ArrayIndex ? FString::Printf(TEXT("%s[%d]"), *P.Name, P.ArrayIndex) : P.Name;
				UE_LOG(LogOKAsset, Log, TEXT("  %s (%s) = %s"), *Key, *P.Type, *FOKPropertyList::DescribeValue(P, *Package));

				// Material parameters are arrays of tagged structs: show their elements too.
				TArray<FOKPropertyList> Elements;
				if (P.Name.EndsWith(TEXT("ParameterValues")) && FOKPropertyList::ReadStructArray(P, Package->GetNames(), Elements))
				{
					for (int32 i = 0; i < Elements.Num(); ++i)
					{
						TArray<FString> Fields;
						for (const FOKProperty& Field : Elements[i].GetAll())
						{
							Fields.Add(Field.Name + TEXT(" = ") + FOKPropertyList::DescribeValue(Field, *Package));
						}
						UE_LOG(LogOKAsset, Log, TEXT("    [%d] %s"), i, *FString::Join(Fields, TEXT(", ")));
					}
				}
			}
		}));