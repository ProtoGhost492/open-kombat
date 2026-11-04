#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOKAsset, Log, All);

class IFileHandle;

/** Where one compressed chunk's bytes go in the uncompressed package, and where they are in the file. */
struct FOKPackageChunk
{
	int32 UncompressedOffset = 0;
	int32 UncompressedSize = 0;
	int32 CompressedOffset = 0;
	int32 CompressedSize = 0;
};

/** The package summary (MK-FORMAT-NOTES, "Package container"). */
struct FOKPackageSummary
{
	uint16 FileVersion = 0;			// 472
	uint16 LicenseeVersion = 0;		// 46
	int32 HeaderSize = 0;
	int32 MkValue = 0;				// after the "MK  " marker; always 62 so far
	FString FolderName;
	uint32 PackageFlags = 0;
	int32 NameCount = 0;
	int32 NameOffset = 0;
	int32 ExportCount = 0;
	int32 ExportOffset = 0;
	int32 ImportCount = 0;
	int32 ImportOffset = 0;
	int32 DependsOffset = 0;
	int32 EngineVersion = 0;		// 2605
	int32 CookerVersion = 0;		// 139
	uint32 CompressionFlags = 0;	// 2 = LZO
	TArray<FOKPackageChunk> Chunks;
};

struct FOKPackageImport
{
	FString ClassPackage;
	FString ClassName;
	int32 Outer = 0;
	FString Name;
};

struct FOKPackageExport
{
	int32 Index = 0;			// 1-based, as object references count
	int32 ClassRef = 0;
	int32 SuperRef = 0;
	int32 Outer = 0;
	FString Name;
	int32 Archetype = 0;
	uint64 ObjectFlags = 0;
	int32 SerialSize = 0;		// the object's bytes in the uncompressed package
	int32 SerialOffset = 0;
	FString ClassName;			// resolved after loading
	FString Path;				// Outer.Outer.Name
};

/** 
* A MK:KE package (.xxx): UE3 with MK's changes. Object references are ints: > 0 = export n,
* < 0 = import -n, 0 = none. Chunks are decompressed the first time something reads them.
*/
class OPENKOMBAT_API FOKPackage
{
public:
	static constexpr uint32 Tag = 0x9E2A83C1;
	static constexpr uint32 CompressLzo = 2;

	FOKPackage();
	~FOKPackage();

	/** Reads the file, the summary and the tables. Null, with OutError, on failure */
	static TUniquePtr<FOKPackage> Open(const FString& Path, FString& OutError);

	/**
	 * The same, reading only the head of the file now and each chunk when something first needs it, for scans over
	 * many packages (the scripts' library). GetFileBytes is then only the head. Whole-file compressed packages
	 * (Startup, Core...) are still read whole.
	 */
	static TUniquePtr<FOKPackage> OpenLazy(const FString& Path, FString& OutError);

	const FString& GetName() const {
		return Name;
	}
	const FOKPackageSummary& GetSummary() const {
		return Summary;
	}
	/** The file as stored (streamed sound banks are read from it raw). */
	TConstArrayView<uint8> GetFileBytes() const { return File; }
	const TArray<FString>& GetNames() const {
		return Names;
	}
	const TArray<FOKPackageImport>& GetImports() const {
		return Imports;
	}
	const TArray<FOKPackageExport>& GetExports() const {
		return Exports;
	}
	int64 GetUncompressedSize() const;

	/** Length bytes at Offset of the uncompressed package. False if they can't all be read. */
	bool Read(int64 Offset, int32 Length, TArray<uint8>& Out);
	bool ExportData(const FOKPackageExport& Export, TArray<uint8>& Out) {
		return Read(Export.SerialOffset, Export.SerialSize, Out);
	}

	FString ObjectName(int32 Ref) const;
	FString ObjectPath(int32 Ref) const;
	TArray<const FOKPackageExport*> FindExports(const FString& ClassName) const;
	const FOKPackageExport* FindExportByPath(const FString& Path) const;

	/** ok.GameDir: where MKKE is installed. */
	static FString GameDir();

	/** Opens <ok.GameDir>/DiscContentPC/Asset/<PackageName>.xxx. */
	static TUniquePtr<FOKPackage> OpenGamePackage(const FString& PackageName, FString& OutError);

private:
	bool ReadSummary(FString& OutError);
	bool ReadTables(FString& OutError);
	const TArray<uint8>* Chunk(int32 Index);
	static TUniquePtr<FOKPackage> OpenFile(const FString& Path, bool bLazy, FString& OutError);
	/** Count stored bytes at Offset: in File, or read into Scratch from the handle. Null if they aren't there. */
	const uint8* StoredBytes(int64 Offset, int64 Count, TArray<uint8>& Scratch) const;

	FString Name;
	TArray<uint8> File;			// the whole file as stored, or its head when opened lazily
	TUniquePtr<IFileHandle> Handle;	// the open file, when lazy
	int64 FileSize = 0;
	FOKPackageSummary Summary;
	TArray<FString> Names;
	TArray<FOKPackageImport> Imports;
	TArray<FOKPackageExport> Exports;
	TArray<TArray<uint8>> Decompressed;			// one per chunk; empty until first read
};