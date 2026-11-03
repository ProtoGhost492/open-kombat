#include "OKLocalization.h"
#include "OKPackage.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

static TAutoConsoleVariable<FString> CVarLanguage(
	TEXT("ok.Language"),
	TEXT("eng"),
	TEXT("The language of MKKE's text: the Coalesced.<lang> file to read (eng, fra, spa, por). Read once, at first use."));

const FOKLocalization& FOKLocalization::Get()
{
	static FOKLocalization Text;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		const FString GameDir = IConsoleManager::Get().FindConsoleVariable(TEXT("ok.GameDir"))->GetString();
		const FString Path = FPaths::Combine(GameDir, TEXT("DiscContentPC/Localization"), TEXT("Coalesced.") + CVarLanguage.GetValueOnGameThread());
		FString Error;
		if (!Text.Load(Path, Error))
		{
			UE_LOG(LogOKAsset, Warning, TEXT("No localised text: %s"), *Error);
		}
	}
	return Text;
}

const FString* FOKLocalization::Find(const FString& Section, const FString& Key) const
{
	return Entries.Find((Section + TEXT("/") + Key).ToLower());
}

bool FOKLocalization::Load(const FString& Path, FString& OutError)
{
	TArray<uint8> File;
	if (!FFileHelper::LoadFileToArray(File, *Path) || File.Num() < 4)
	{
		OutError = FString::Printf(TEXT("can't read %s"), *Path);
		return false;
	}
	const int32 Size = int32(File[0] | (File[1] << 8) | (File[2] << 16) | (uint32(File[3]) << 24));
	TArray<uint8> Data;
	Data.SetNumUninitialized(Size);
	if (Size <= 0 || !FCompression::UncompressMemory(NAME_Zlib, Data.GetData(), Size, File.GetData() + 4, File.Num() - 4))
	{
		OutError = FString::Printf(TEXT("%s isn't a Coalesced bundle"), *Path);
		return false;
	}
	for (uint8& Byte : Data)
	{
		Byte = uint8(~Byte);
	}

	int32 Pos = 0;
	auto ReadInt = [&](int32& Out)
	{
		if (Pos + 4 > Data.Num())
		{
			return false;
		}
		FMemory::Memcpy(&Out, Data.GetData() + Pos, 4);
		Pos += 4;
		return true;
	};
	// A UE3 string: a length with the NUL; negative = that many UTF-16 characters
	auto ReadString = [&](FString& Out)
	{
		int32 Length = 0;
		if (!ReadInt(Length))
		{
			return false;
		}
		const int32 Bytes = Length < 0 ? -Length * 2 : Length;
		if (Bytes < 0 || Pos + Bytes > Data.Num())
		{
			return false;
		}
		Out.Reset();
		if (Length < 0)
		{
			for (int32 i = 0; i + 1 < -Length; ++i)
			{
				Out.AppendChar(TCHAR(Data[Pos + i * 2] | (Data[Pos + i * 2 + 1] << 8)));
			}
		}
		else
		{
			for (int32 i = 0; i + 1 < Length; ++i)
			{
				Out.AppendChar(TCHAR(Data[Pos + i]));
			}
		}
		Pos += Bytes;
		return true;
	};

	int32 Count = 0;
	if (!ReadInt(Count))
	{
		OutError = FString::Printf(TEXT("%s is empty"), *Path);
		return false;
	}
	FString Name, Text;
	for (int32 i = 0; i < Count && ReadString(Name) && ReadString(Text); ++i)
	{
		AddIni(Text);
	}
	UE_LOG(LogOKAsset, Log, TEXT("Localised text: %d entries from %s"), Entries.Num(), *Path);
	return true;
}

void FOKLocalization::AddIni(const FString& Text)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	FString Section;
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line[0] == TEXT(';'))
		{
			continue;
		}
		int32 Equals = INDEX_NONE;
		if (Line[0] == TEXT('[') && Line.EndsWith(TEXT("]")) && !Line.FindChar(TEXT('='), Equals))
		{
			Section = Line.Mid(1, Line.Len() - 2).ToLower();
			continue;
		}
		if (!Line.FindChar(TEXT('='), Equals))
		{
			continue;
		}
		FString Value = Line.Mid(Equals + 1).TrimStartAndEnd();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		Entries.Add(Section + TEXT("/") + Line.Left(Equals).TrimStartAndEnd().ToLower(), MoveTemp(Value));
	}
}
