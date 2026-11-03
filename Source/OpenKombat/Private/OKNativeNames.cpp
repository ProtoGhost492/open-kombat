#include "OKNativeNames.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FOKNativeNames::Load(const FString& RegistryPath, const FString& NamesPath, FString& OutError)
{
	ModuleSlots.Reset();
	Names.Reset();

	// registry.json: { "1641": 137, "d33e": 6, ... } (keys are hex module ids)
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *RegistryPath))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *RegistryPath);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s isn't valid JSON"), *RegistryPath);
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		ModuleSlots.Add(FParse::HexNumber(*Pair.Key), static_cast<int32>(Pair.Value->AsNumber()));
	}

	// native-names.tsv: native <tab> name <tab> confidence <tab> note
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *NamesPath))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *NamesPath);
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