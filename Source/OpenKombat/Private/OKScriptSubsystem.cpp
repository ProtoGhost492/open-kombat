#include "OKScriptSubsystem.h"
#include "OKMkoFile.h"
#include "OKPackage.h"
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
	// The scripts come from the game's packages, or from ok.MkoDir when it names a folder of extracted ones.
	const FString Folder = FOKMkoFile::ScriptDir();
	const FString GameDir = FOKPackage::GameDir();

	// The registry, which native dispatch needs, comes from the game's executable. The names are only for logs: the
	// RE notes' native-names.tsv, one folder up from ok.MkoDir, or in <Documents>/OpenKombatData.
	FString RegistryError;
	if (!NativeNames.LoadRegistry(FPaths::Combine(GameDir, TEXT("DiscContentPC/MKKE.exe")), RegistryError))
	{
		UE_LOG(LogOKScripts, Error, TEXT("No native registry (%s): scripts can't call natives. Set ok.GameDir to your MKKE install."), *RegistryError);
	}
	const FString DataDir = Folder.IsEmpty() ? FPaths::Combine(FPlatformProcess::UserDir(), TEXT("OpenKombatData")) : FPaths::GetPath(Folder);
	if (!NativeNames.LoadNames(FPaths::Combine(DataDir, TEXT("native-names.tsv"))))
	{
		UE_LOG(LogOKScripts, Log, TEXT("No native-names.tsv in %s; natives show as numbers"), *DataDir);
	}
	UE_LOG(LogOKScripts, Log, TEXT("Native registry: %d modules, %d named natives"), NativeNames.NumModules(), NativeNames.NumNames());

	const double StartSeconds = FPlatformTime::Seconds();
	TUniquePtr<FOKScriptLibrary> NewLibrary = MakeUnique<FOKScriptLibrary>();
	const FString Source = Folder.IsEmpty() ? FPaths::Combine(GameDir, TEXT("DiscContentPC/Asset")) : Folder;
	if (Folder.IsEmpty())
	{
		NewLibrary->LoadPackages(Source);
	}
	else
	{
		NewLibrary->LoadFolder(Folder);
	}
	const int32 Unresolved = NewLibrary->LinkAll();
	Library = MoveTemp(NewLibrary); // swap in only once it's fully built

	UE_LOG(LogOKScripts, Log, TEXT("Script library: %d scripts from %s in %.0f ms, %d unresolved calls"),
		Library->NumScripts(), *Source, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, Unresolved);
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
