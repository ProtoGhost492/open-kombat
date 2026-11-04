#include "OKMkoFile.h"
#include "OKNativeNames.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogOKScript, Log, All);

namespace
{
	constexpr uint32 MkoMagic = 0x638A9464;
	constexpr int64 HeaderBytes = 0x40;		// 16 dwords
	constexpr int32 OpNative = 64;
	constexpr int32 OpSetArg = 0;
	constexpr int32 OpCallImport = 62;		// callx

	// Module-0 handlers, indexed by (handler id & 0xFFFF) - 1. Same names as scripts/re/disasm.mjs.
	const TCHAR* const OpNames[] =
	{
		TEXT("setarg"), TEXT("mov"), TEXT("sttmp"), TEXT("sttmpd"), TEXT("getret"), TEXT("setret"), TEXT("ldidx"), TEXT("ptrdiff"),
		TEXT("add"), TEXT("sub"), TEXT("mul"), TEXT("div"), TEXT("mod"), TEXT("and"), TEXT("or"), TEXT("xor"), TEXT("shl"), TEXT("sar"),
		TEXT("add"), TEXT("sub"), TEXT("mul"), TEXT("udiv"), TEXT("umod"), TEXT("and"), TEXT("or"), TEXT("xor"), TEXT("shl"), TEXT("shr"),
		TEXT("fadd"), TEXT("fsub"), TEXT("fmul"), TEXT("fdiv"),
		TEXT("lt"), TEXT("eq"), TEXT("gt"), TEXT("ne"), TEXT("le"), TEXT("ge"), TEXT("lor"), TEXT("land"),
		TEXT("ult"), TEXT("eq"), TEXT("ugt"), TEXT("ne"), TEXT("ule"), TEXT("uge"), TEXT("lor"), TEXT("land"),
		TEXT("flt"), TEXT("feq"), TEXT("fgt"), TEXT("fne"), TEXT("fle"), TEXT("fge"), TEXT("flor"), TEXT("fland"),
		TEXT("i2f"), TEXT("u2f"), TEXT("f2i"), TEXT("jz"), TEXT("jmp"), TEXT("call"), TEXT("callx"), TEXT("callref"), TEXT("native"),
		TEXT("bfext"), TEXT("bfins"), TEXT("postinc"), TEXT("preinc"), TEXT("postdec"), TEXT("predec"), TEXT("neg"), TEXT("not"),
		TEXT("lnot"), TEXT("deref"), TEXT("lea"), TEXT("postinc"), TEXT("preinc"), TEXT("postdec"), TEXT("predec"), TEXT("not"),
		TEXT("lnot"), TEXT("deref"), TEXT("leapool"), TEXT("fpostinc"), TEXT("fpredec"), TEXT("fpostdec"), TEXT("fpredec"),
		TEXT("fneg"), TEXT("flnot"), TEXT("deref"), TEXT("switch"), TEXT("trackobj"),
	};
	static_assert(UE_ARRAY_COUNT(OpNames) == 93, "MKScript has 93 handlers");
}

const TCHAR* FOKMkoFile::GetOpName(int32 Op)
{
	return (Op >= 0 && Op < UE_ARRAY_COUNT(OpNames)) ? OpNames[Op] : TEXT("?");
}

uint32 FOKMkoFile::Fnv1(const FString& Text)
{
	uint32 Hash = 0x811C9DC5;
	for (const TCHAR Ch : Text)
	{
		Hash = (Hash * 0x01000193) ^ static_cast<uint8>(Ch);
	}
	return Hash;
}

bool FOKMkoFile::ReadU32(int64 Offset, uint32& Out) const
{
	if (Offset < 0 || Offset + 4 > Bytes.Num())
	{
		return false;
	}
	FMemory::Memcpy(&Out, Bytes.GetData() + Offset, 4);		// PC files are little-endian, like x64
	return true;
}

bool FOKMkoFile::ReadCString(int64 Offset, FString& Out) const
{
	if (Offset < 0 || Offset >= Bytes.Num())
	{
		return false;
	}
	int64 End = Offset;
	while (End < Bytes.Num() && Bytes[End] != 0)
	{
		++End;
	}
	if (End == Bytes.Num())
	{
		return false;	// ran off the end without finding the terminating zero
	}
	Out = FString(static_cast<int32>(End - Offset), reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset));
	return true;
}

bool FOKMkoFile::Load(const FString& Path, FString& OutError)
{
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
	{
		*this = FOKMkoFile();
		OutError = TEXT("can't read the file");
		return false;
	}
	return LoadFromBytes(MoveTemp(FileBytes), FPaths::GetCleanFilename(Path), OutError);
}

bool FOKMkoFile::LoadFromBytes(TArray<uint8>&& InBytes, const FString& FileName, FString& OutError)
{
	*this = FOKMkoFile();	// start clean if this object is reused
	Name = FileName;
	Bytes = MoveTemp(InBytes);

	// Header: 16 little-endian dwords.
	for (int32 i = 0; i < 16; ++i)
	{
		if (!ReadU32(i * 4, Header[i]))
		{
			OutError = TEXT("shorter than the 64-byte header");
			return false;
		}
	}
	if (Header[0] != 1)
	{
		OutError = TEXT("not a little-endian (PC) script");
		return false;
	}
	if (Header[1] != MkoMagic && Header[1] != 0)
	{
		OutError = FString::Printf(TEXT("bad magic 0x%08X"), Header[1]);
		return false;
	}

	// Function records: 48 bytes plus 16 per parameter, after one 4-byte slot per function.
	const uint32 NumFunctions = Header[2];
	TArray<TStaticArray<uint32, 12>> Records;
	Records.SetNum(NumFunctions);

	int64 Offset = HeaderBytes + int64(NumFunctions) * 4;
	for (uint32 f = 0; f < NumFunctions; ++f)
	{
		for (int32 k = 0; k < 12; ++k)
		{
			if (!ReadU32(Offset + k * 4, Records[f][k]))
			{
				OutError = FString::Printf(TEXT("function record %u runs past the end of the file"), f);
				return false;
			}
		}
		Offset += (int64(Records[f][9]) + 3) * 16;
	}

	// Table A: globals (4-byte slots, then 16-byte records {name hash, size, element size, pool offset}).
	Offset += int64(Header[3]) * 4;
	Globals.Reserve(Header[3]);
	for (uint32 i = 0; i < Header[3]; ++i)
	{
		FOKMkoGlobal& Global = Globals.AddDefaulted_GetRef();
		if (!ReadU32(Offset, Global.NameHash) || !ReadU32(Offset + 4, Global.Size) || !ReadU32(Offset + 8, Global.ElementSize) || !ReadU32(Offset + 12, Global.PoolOffset))
		{
			OutError = FString::Printf(TEXT("global %u runs past the end of the file"), i);
			return false;
		}
		Offset += 16;
	}

	// NEW: table B, imported functions (4-byte slots, then 28-byte records).
	// The file names are read below, once we know where the data region starts.
	const uint32 NumImports = Header[4];
	TArray<TStaticArray<uint32, 3>> ImportRecords;
	ImportRecords.SetNum(NumImports);

	Offset += int64(NumImports) * 4;
	for (uint32 i = 0; i < NumImports; ++i)
	{
		for (int32 k = 0; k < 3; ++k)
		{
			if (!ReadU32(Offset + k * 4, ImportRecords[i][k]))
			{
				OutError = FString::Printf(TEXT("import record %u runs past the end of the file"), i);
				return false;
			}
		}
		Offset += 28;
	}

	// The remaining tables.
	// Object references: 16-byte records, no slots. Dword 0 is the name's data offset (1-based).
	TArray<uint32> ObjectRefNameOffsets;
	for (uint32 i = 0; i < Header[5]; ++i)
	{
		uint32 NameOffset = 0;
		if (!ReadU32(Offset, NameOffset))
		{
			OutError = FString::Printf(TEXT("object reference %u runs past the end of the file"), i);
			return false;
		}
		ObjectRefNameOffsets.Add(NameOffset);
		Offset += 16;
	}

	Offset += int64(Header[9]) * 8;			// 8-byte entries
	
	// Table C: asset imports (animations, textures...): an index dword each, then 16-byte records
	// {set or package name, object name, 0, 0}, names as 1-based data offsets (ANIM-RE-NOTES §1).
	Offset += int64(Header[6]) * 4;
	TArray<TPair<uint32, uint32>> AssetNameOffsets;
	for (uint32 i = 0; i < Header[6]; ++i)
	{
		uint32 SetName = 0, ObjectName = 0;
		if (!ReadU32(Offset, SetName) || !ReadU32(Offset + 4, ObjectName))
		{
			OutError = FString::Printf(TEXT("asset import %u runs past the end of the file"), i);
			return false;
		}
		AssetNameOffsets.Emplace(SetName, ObjectName);
		Offset += 16;
	}

	// Table D: sound imports: an index dword each, then 20-byte records {group name, event name, 0, 0, -1}, names as 1-based
	// data offsets (snd_fightfx1 / sfx_hit_face).
	Offset += int64(Header[7]) * 4;
	TArray<TPair<uint32, uint32>> SoundNameOffsets;
	for (uint32 i = 0; i < Header[7]; ++i)
	{
		uint32 GroupName = 0, EventName = 0;
		if (!ReadU32(Offset, GroupName) || !ReadU32(Offset + 4, EventName))
		{
			OutError = FString::Printf(TEXT("sound import %u runs past the end of the file"), i);
			return false;
		}
		SoundNameOffsets.Emplace(GroupName, EventName);
		Offset += 20;
	}

	DataOffset = Offset;
	PoolOffset = DataOffset + Header[11];
	CodeOffset = PoolOffset + Header[12];
	FixupOffset = CodeOffset + Header[10];
	const int64 End = FixupOffset + int64(Header[13]) * 16;

	if (End > Bytes.Num())
	{
		OutError = FString::Printf(TEXT("sections end at byte %lld but the file has only %d"), End, Bytes.Num());
		return false;
	}
	
	Fixups.Reserve(Header[13]);
	for (uint32 i = 0; i < Header[13]; ++i)
	{
		FOKMkoFixup& Fix = Fixups.AddDefaulted_GetRef();
		const int64 At = FixupOffset + int64(i) * 16;
		if (!ReadU32(At, Fix.Kind) || !ReadU32(At + 4, Fix.Where) || !ReadU32(At + 8, Fix.Value) || !ReadU32(At + 12, Fix.Extra))
		{
			OutError = FString::Printf(TEXT("fixup %u runs past the end of the file"), i);
			return false;
		}
	}

	// Every MKKE script is zero-padded up to the next multiple of 32 bytes, always adding
	// 1-32 bytes (a full 32 when the sections already end on a boundary). Any other size
	// means we added up the sections wrong.
	const int64 ExpectedSize = (End / 32 + 1) * 32;
	if (Bytes.Num() != ExpectedSize)
	{
		UE_LOG(LogOKScript, Warning, TEXT("%s: sections end at %lld, so the file should be %lld bytes, but it is %d"),
				*Name, End, ExpectedSize, Bytes.Num());
	}

	Functions.Reserve(NumFunctions);
	for (uint32 f = 0; f < NumFunctions; ++f)
	{
		FOKMkoFunction& Fn = Functions.AddDefaulted_GetRef();
		Fn.NameHash = Records[f][1];
		Fn.SignatureHash = Records[f][11];	// NEW
		Fn.CodeWord = Records[f][2];
		Fn.NumParams = Records[f][9];
		Fn.CodeBytes = Records[f][3];
		Fn.LocalsPoolOffset = Records[f][4];
		Fn.LocalsBytes = Records[f][5];
		Fn.MaxArgs = Records[f][6];
		Fn.NumTemps = Records[f][10];
		if (int64(Fn.CodeWord) * 4 + Fn.CodeBytes > Header[10])
		{
			OutError = FString::Printf(TEXT("function %u's code runs past the code section"), f);
			return false;
		}

		if (Records[f][0] == 0 || !ReadCString(DataOffset + Records[f][0] - 1, Fn.Name))
		{
			OutError = FString::Printf(TEXT("function %u has no readable name"), f);
			return false;
		}

		// Every block starts inside the pool, but a block's declared size can run past the end
		// of the pool; the VM zero-fills whatever isn't stored.
		if (Fn.LocalsPoolOffset > Header[12])
		{
			OutError = FString::Printf(TEXT("function %u's initial locals start past the pool"), f);
			return false;
		}

		// NEW: index by (name, signature). Keep the first if a pair ever repeats.
		const uint64 Key = (uint64(Fn.NameHash) << 32) | Fn.SignatureHash;
		if (!FunctionIndex.Contains(Key))
		{
			FunctionIndex.Add(Key, static_cast<int32>(f));
		}
	}

	// NEW: imports, now that the data region is known.
	Imports.Reserve(NumImports);
	for (uint32 i = 0; i < NumImports; ++i)
	{
		FOKMkoImport& Import = Imports.AddDefaulted_GetRef();
		Import.NameHash = ImportRecords[i][0];
		Import.SignatureHash = ImportRecords[i][2];

		if (ImportRecords[i][1] == 0 || !ReadCString(DataOffset + ImportRecords[i][1] - 1, Import.TargetFile))
		{
			OutError = FString::Printf(TEXT("import %u has no readable file name"), i);
			return false;
		}
	}

	ObjectRefs.Reserve(ObjectRefNameOffsets.Num());
	for (int32 i = 0; i < ObjectRefNameOffsets.Num(); ++i)
	{
		FString& RefName = ObjectRefs.AddDefaulted_GetRef();
		if (ObjectRefNameOffsets[i] == 0 || !ReadCString(DataOffset + ObjectRefNameOffsets[i] - 1, RefName))
		{
			OutError = FString::Printf(TEXT("object reference %d has no readable name"), i);
			return false;
		}
	}

	for (const TPair<uint32, uint32>& Names : SoundNameOffsets)
	{
		FOKMkoAsset& Sound = SoundImports.AddDefaulted_GetRef();
		if (Names.Key != 0)
		{
			ReadCString(DataOffset + Names.Key - 1, Sound.Set);
		}
		if (Names.Value != 0)
		{
			ReadCString(DataOffset + Names.Value - 1, Sound.Object);
		}
	}

	AssetImports.Reserve(AssetNameOffsets.Num());
	for (const TPair<uint32, uint32>& Names : AssetNameOffsets)
	{
		FOKMkoAsset& Asset = AssetImports.AddDefaulted_GetRef();
		if (Names.Key != 0)
		{
			ReadCString(DataOffset + Names.Key - 1, Asset.Set);
		}
		if (Names.Value != 0)
		{
			ReadCString(DataOffset + Names.Value - 1, Asset.Object);
		}
	}

	return true;
}

int32 FOKMkoFile::FindFunction(uint32 NameHash, uint32 SignatureHash) const
{
	const int32* Found = FunctionIndex.Find((uint64(NameHash) << 32) | SignatureHash);
	return Found ? *Found : INDEX_NONE;
}

int32 FOKMkoFile::FindFunctionByName(const FString& FunctionName) const
{
	for (int32 i = 0; i < Functions.Num(); ++i)
	{
		if (Functions[i].Name == FunctionName)	// FString == ignores case
		{
			return i;
		}
	}
	return INDEX_NONE;
}

FString FOKMkoFile::FormatLiteral(uint32 Value)
{
	float AsFloat = 0.0f;
	FMemory::Memcpy(&AsFloat, &Value, 4);

	// Same test as the RE tools: plausible floats print as floats, everything else as integers.
	if (Value > 0xFFFF && Value < 0x7F800000 && FMath::Abs(AsFloat) > 1e-6f && FMath::Abs(AsFloat) < 1e7f)
	{
		return FString::Printf(TEXT("%gf"), AsFloat);
	}
	return FString::Printf(TEXT("%d"), static_cast<int32>(Value));
}

FString FOKMkoFile::ImportName(int32 ImportIndex) const
{
	if (!Imports.IsValidIndex(ImportIndex))
	{
		return FString::Printf(TEXT("import#%d"), ImportIndex + 1);
	}
	const FOKMkoImport& Import = Imports[ImportIndex];
	if (!Import.Target)
	{
		return Import.TargetFile + TEXT(".?");
	}
	return FPaths::GetBaseFilename(Import.Target->GetName()) + TEXT(".")
		+ Import.Target->GetFunctions()[Import.TargetFunction].Name;
}

FString FOKMkoFile::FormatOperand(uint32 Value, bool bLiteral, bool bIndirect, bool bNativeArg) const
{
	const uint32 Index = Value & 0x3FFFFFFF;
	const bool bTopBit = (Value & 0x80000000) != 0;
	const bool bPoolBit = (Value & 0x40000000) != 0;

	FString Text;
	if (bNativeArg && bTopBit && !bIndirect && Index > uint32(ObjectRefs.Num()) && Imports.IsValidIndex(int32(Index) - 1))
	{
		// A script function handed to a native, e.g. a reaction to run on hit.
		Text = TEXT("&") + ImportName(int32(Index) - 1);
	}
	else
	{
		// Natives read some arguments as raw values without the literal bit.
		if (bNativeArg && !bLiteral && !bIndirect && !bTopBit && !bPoolBit && (Value > 0x2000 || (Value & 3)))
		{
			bLiteral = true;
		}

		if (bLiteral)
		{
			Text = FormatLiteral(Value);
		}
		else if (bTopBit)
		{
			Text = ObjectRefs.IsValidIndex(int32(Index) - 1)
				? TEXT("@") + ObjectRefs[Index - 1]
				: FString::Printf(TEXT("ref%u"), Index);
		}
		else if (bPoolBit)
		{
			uint32 Stored = 0;
			Text = ReadU32(PoolOffset + Index, Stored)
				? FString::Printf(TEXT("g%x{%s}"), Index, *FormatLiteral(Stored))
				: FormatLiteral(Value); // not a real pool offset: a raw value with bit 30 set
		}
		else
		{
			Text = FString::Printf(TEXT("L%x"), Value);
		}
	}
	return bIndirect ? TEXT("[") + Text + TEXT("]") : Text;
}

bool FOKMkoFile::Disassemble(int32 FuncIndex, const FOKNativeNames* NativeNames, TArray<FString>& OutLines) const
{
	if (!Functions.IsValidIndex(FuncIndex))
	{
		return false;
	}
	const FOKMkoFunction& Fn = Functions[FuncIndex];
	const int64 FirstWord = Fn.CodeWord;
	const int64 EndWord = FirstWord + Fn.CodeBytes / 4;

	OutLines.Add(FString::Printf(TEXT("%s : %s (#%d), %lld words"), *Name, *Fn.Name, FuncIndex + 1, EndWord - FirstWord));

	auto ReadWord = [this](int64 WordIndex, uint32& Out) { return ReadU32(CodeOffset + WordIndex * 4, Out); };

	// setarg writes a value into an argument slot of a later instruction. Remember what
	// went where, so calls show their real arguments instead of placeholders.
	TMap<int64, FString> Patched;

	int64 Pc = FirstWord;
	while (Pc < EndWord)
	{
		uint32 Handler = 0, Mode = 0, LineAndCount = 0;
		if (!ReadWord(Pc, Handler))
		{
			OutLines.Add(TEXT(" (code runs past the end of the file)"));
			return false;
		}
		if (Handler == 0)
		{
			OutLines.Add(FString::Printf(TEXT(" %05llx (padding)"), Pc));
			++Pc;
			continue;
		}
		if (!ReadWord(Pc + 1, Mode) || !ReadWord(Pc + 2, LineAndCount))
		{
			OutLines.Add(TEXT(" (instruction header runs past the end of the file)"));
			return false;
		}

		const int32 Op = int32(Handler & 0xFFFF) - 1;
		const uint32 Line = LineAndCount >> 16;
		const uint32 Count = LineAndCount & 0xFFFF;
		if ((Handler >> 16) != 0 || Op < 0 || Op >= UE_ARRAY_COUNT(OpNames))
		{
			OutLines.Add(FString::Printf(TEXT(" %05llx unknown handler %08X"), Pc, Handler));
			return false;
		}

		const bool bNative = (Op == OpNative);
		FString Head = OpNames[Op];
		TArray<FString> Args;

		for (uint32 k = 0; k < Count; ++k)
		{
			const int64 Word = Pc + 3 + k;
			uint32 Value = 0;
			if (!ReadWord(Word, Value))
			{
				OutLines.Add(TEXT(" (operands run past the end of the file)"));
				return false;
			}

			if (k == 0 && bNative)
			{
				Head = NativeNames ? NativeNames->NameFor(Value) : FString::Printf(TEXT("%04X:%u"), Value >> 16, (Value & 0xFFFF) - 1);
				continue;
			}
			if (k == 0 && Op == OpCallImport)
			{
				Head = TEXT("callx ") + ImportName(int32(Value & 0x3FFFFFFF) - 1);
				continue;
			}
			if (const FString* Filled = Patched.Find(Word))
			{
				Args.Add(TEXT("<") + *Filled + TEXT(">"));
				continue;
			}

			// For natives the mode bits start at operand 1, because operand 0 is the native id.
			const uint32 Bit = 2 * (bNative ? k - 1 : k);
			const bool bLiteral = Bit < 32 && ((Mode >> Bit) & 1);
			const bool bIndirect = Bit + 1 < 32 && ((Mode >> (Bit + 1)) & 1);
			Args.Add(FormatOperand(Value, bLiteral, bIndirect, bNative));
		}

		FString Text;
		uint32 Slot = 0;
		if (Op == OpSetArg && Count == 2 && ReadWord(Pc + 4, Slot))
			{
				// The slot counts words from the end of this setarg.
			Patched.Add(Pc + 3 + Count + Slot, Args[0]);
			Text = FString::Printf(TEXT("setarg +%u = %s"), Slot, *Args[0]);
			}
		else
		{
			Text = Args.Num() > 0 ? Head + TEXT(" ") + FString::Join(Args, TEXT(", ")) : Head;
		}

		OutLines.Add(FString::Printf(TEXT(" %05llx L%-4u %s"), Pc, Line, *Text));
		Pc += 3 + Count;
	}
	return true;
}

// --- Console tools ---------------------------------------------------------------

static TAutoConsoleVariable<FString> CVarMkoDir(
	TEXT("ok.MkoDir"),
	TEXT(""),
	TEXT("A folder of extracted .mko scripts to load instead of the game's packages; empty reads the packages."));

FString FOKMkoFile::ScriptDir()
{
	return CVarMkoDir.GetValueOnGameThread();
}

static FAutoConsoleCommand CmdMkoList(
	TEXT("ok.Mko.List"),
	TEXT("ok.Mko.List <Name>: Load <Name>.mko from ok.MkoDir and list its functions."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOKScript, Warning, TEXT("Usage: ok.Mko.List CHAR_Scorpion"));
				return;
			}

			const FString Path = FPaths::Combine(FOKMkoFile::ScriptDir(), Args[0] + TEXT(".mko"));
			FOKMkoFile File;
			FString Error;
			if (!File.Load(Path, Error))
			{
				UE_LOG(LogOKScript, Error, TEXT("%s: %s"), *Path, *Error);
				return;
			}

			// If we found the names in the right place, each one hashes to the value stored next to it.
			int32 HashesMatch = 0;
			for (const FOKMkoFunction& Fn : File.GetFunctions())
			{
				HashesMatch += (FOKMkoFile::Fnv1(Fn.Name) == Fn.NameHash) ? 1 : 0;
			}

			UE_LOG(LogOKScript, Log, TEXT("%s: %d bytes, %d functions (%d name hashes match), %u globals, %u bytes of code"),
				*Args[0], File.GetFileBytes(), File.GetFunctions().Num(), HashesMatch,
				File.GetNumGlobals(), File.GetCodeBytes());

			const int32 Shown = FMath::Min(File.GetFunctions().Num(), 10);
			for (int32 i = 0; i < Shown; ++i)
			{
				const FOKMkoFunction& Fn = File.GetFunctions()[i];
				UE_LOG(LogOKScript, Log, TEXT(" #%d %s (%u params, code word %u)"), i + 1, *Fn.Name, Fn.NumParams, Fn.CodeWord);
			}
		}
	));