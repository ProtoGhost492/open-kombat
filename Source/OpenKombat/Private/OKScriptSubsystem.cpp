#include "OKScriptSubsystem.h"
#include "OKMkoFile.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogOKScripts, Log, All);

void UOKScriptSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Reload();
}

void UOKScriptSubsystem::Deinitialize()
{
	Library.Reset();
	Super::Deinitialize();
}

void UOKScriptSubsystem::Reload()
{
	const FString Folder = FOKMkoFile::ScriptDir();

	// The name tables sit one folder up from the scripts: OpenKombatData/registry.json, OpenKombatData/native-names.tsv

	const FString DataDir = FPaths::GetPath(Folder);
	FString NamesError;
	if (NativeNames.Load(FPaths::Combine(DataDir, TEXT("registry.json")), FPaths::Combine(DataDir, TEXT("native-names.tsv")), NamesError))
	{
		UE_LOG(LogOKScripts, Log, TEXT("Native names: %d modules, %d named natives"), NativeNames.NumModules(), NativeNames.NumNames());
	}
	else
	{
		UE_LOG(LogOKScripts, Warning, TEXT("Native names not loaded (%s); natives will show as numbers"), *NamesError);
	}

	const double StartSeconds = FPlatformTime::Seconds();
	TUniquePtr<FOKScriptLibrary> NewLibrary = MakeUnique<FOKScriptLibrary>();
	NewLibrary->LoadFolder(Folder);
	const int32 Unresolved = NewLibrary->LinkAll();
	Library = MoveTemp(NewLibrary); // swap in only once it's fully built

	UE_LOG(LogOKScripts, Log, TEXT("Script library: %d scripts from %s in %.0f ms, %d unresolved calls"),
		Library->NumScripts(), *Folder, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, Unresolved);
}

// --- Console tools ----------------------------------------------

static UOKScriptSubsystem* GetScriptSubsystem()
{
	return GEngine ? GEngine->GetEngineSubsystem<UOKScriptSubsystem>() : nullptr;
}

static FAutoConsoleCommand CmdMkoReload(
	TEXT("ok.Mko.Reload"),
	TEXT("Reload every script from ok.MkoDir."),
	FConsoleCommandDelegate::CreateLambda([]()
		{
			if (UOKScriptSubsystem* Scripts = GetScriptSubsystem())
			{
				Scripts->Reload();
			}
		}));

static FAutoConsoleCommand CmdMkoDis(
	TEXT("ok.Mko.Dis"),
	TEXT("ok.Mko.Dis <Script> <Function>: disassemble one function, e.g. ok.Mko.Dis CHAR_Scorpion attack1"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOKScriptSubsystem* Scripts = GetScriptSubsystem();
			if (!Scripts || !Scripts->GetLibrary() || Args.Num() < 2)
			{
				UE_LOG(LogOKScripts, Warning, TEXT("Usage: ok.Mko.Dis CHAR_Scorpion attack1"));
				return;
			}

			const FOKMkoFile* Script = Scripts->GetLibrary()->FindScript(Args[0] + TEXT(".mko"));
			if (!Script)
			{
				UE_LOG(LogOKScripts, Error, TEXT("No script named %s"), *Args[0]);
				return;
			}

			const int32 Index = Script->FindFunctionByName(Args[1]);
			if (Index == INDEX_NONE)
			{
				UE_LOG(LogOKScripts, Error, TEXT("%s has no function named %s"), *Script->GetName(), *Args[1]);
				return;
			}

			TArray<FString> Lines;
			Script->Disassemble(Index, &Scripts->GetNativeNames(), Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogOKScripts, Log, TEXT("%s"), *Line);
			}
		}));
