#include "OkScriptLibrary.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogOKLibrary, Log, All);

int32 FOKScriptLibrary::LoadFolder(const FString& Folder)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.mko")), /*Files*/ true, /*Dirs*/ false);
	Files.Sort();	// the same order on every run and every PC

	for (const FString& FileName : Files)
	{
		TUniquePtr<FOKMkoFile> File = MakeUnique<FOKMkoFile>();
		FString Error;
		if (!File->Load(FPaths::Combine(Folder, FileName), Error))
		{
			UE_LOG(LogOKLibrary, Warning, TEXT("Skipping %s: %s"), *FileName, *Error);
			++Skipped;
			continue;
		}
		Scripts.Add(FileName, MoveTemp(File));
	}
	return Scripts.Num();
}

int32 FOKScriptLibrary::LinkAll()
{
	int32 Unresolved = 0;
	for (TPair<FString, TUniquePtr<FOKMkoFile>>& Pair : Scripts)
	{
		for (FOKMkoImport& Import : Pair.Value->GetImports())
		{
			Import.Target = FindScript(Import.TargetFile);
			Import.TargetFunction = Import.Target
				? Import.Target->FindFunction(Import.NameHash, Import.SignatureHash)
				: INDEX_NONE;

			if (Import.TargetFunction == INDEX_NONE)
			{
				Import.Target = nullptr;
				++Unresolved;
				UE_LOG(LogOKLibrary, Warning, TEXT("%s: can't resolve a call into %s (name hash %08X)"),
					*Pair.Key, *Import.TargetFile, Import.NameHash);
			}
		}
	}
	return Unresolved;
}

const FOKMkoFile* FOKScriptLibrary::FindScript(const FString& FileName) const
{
	const TUniquePtr<FOKMkoFile>* Found = Scripts.Find(FileName);
	return Found ? Found->Get() : nullptr;
}

void FOKScriptLibrary::ForEachScript(TFunctionRef<void(const FOKMkoFile&)> Visitor) const
{
	for (const TPair<FString, TUniquePtr<FOKMkoFile>>& Pair : Scripts)
	{
		Visitor(*Pair.Value);
	}
}

// --- Console tools --------------------------------------------------------

static FAutoConsoleCommand CmdMkoLinkAll(
	TEXT("ok.Mko.LinkAll"),
	TEXT("Load every script in ok.Mko.Dir and link their cross-script calls."),
	FConsoleCommandDelegate::CreateLambda([]()
		{
			// ok.MkoDir is defined in OKMkoFile.cpp; look it up by name.
			IConsoleVariable* DirVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ok.MkoDir"));
			const FString Folder = DirVar ? DirVar->GetString() : FString();

			const double StartSeconds = FPlatformTime::Seconds();
			FOKScriptLibrary Library;
			Library.LoadFolder(Folder);
			const int32 Unresolved = Library.LinkAll();
			const double Milliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

			int32 NumFunctions = 0;
			int32 NumImports = 0;
			Library.ForEachScript([&](const FOKMkoFile& Script)
				{
					NumFunctions += Script.GetFunctions().Num();
					NumImports += Script.GetImports().Num();
				});

			UE_LOG(LogOKLibrary, Log, TEXT("Loaded %d scripts (%d skipped) in %.0f ms: %d functions, %d cross-script calls, %d unresolved"),
				Library.NumScripts(), Library.NumSkipped(), Milliseconds, NumFunctions, NumImports, Unresolved);

			if (const FOKMkoFile* Scorpion = Library.FindScript(TEXT("CHAR_Scorpion.mko")))
			{
				for (int32 i = 0; i < FMath::Min(3, Scorpion->GetImports().Num()); ++i)
				{
					const FOKMkoImport& Import = Scorpion->GetImports()[i];
					if (Import.Target)
					{
						UE_LOG(LogOKLibrary, Log, TEXT(" CHAR_Scorpion import %d -> %s : %s (#%d)"), i + 1,
							*Import.Target->GetName(), *Import.Target->GetFunctions()[Import.TargetFunction].Name,
							Import.TargetFunction + 1);
					}
				}
			}
		}));