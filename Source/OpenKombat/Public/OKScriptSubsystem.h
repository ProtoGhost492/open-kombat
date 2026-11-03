#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "OKScriptLibrary.h"
#include "OKNativeNames.h"
#include "OKScriptSubsystem.generated.h"

/** Owns the loaded MKScript library for as long as the engine runs (editor or game). */
UCLASS()
class OPENKOMBAT_API UOKScriptSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()
	
public: 
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Loads and links every script in ok.MkoDir, replacing what was loaded. */
	void Reload();

	const FOKScriptLibrary* GetLibrary() const {
		return Library.Get();
	}

	const FOKNativeNames& GetNativeNames() const {
		return NativeNames;
	}

private:
	TUniquePtr<FOKScriptLibrary> Library;
	FOKNativeNames NativeNames;
};
