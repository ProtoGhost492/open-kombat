#pragma once

#include "CoreMinimal.h"

class FOKByteReader;
class FOKPackage;

/** What a tagged property holds. Structs and arrays are untyped on disk, so they keep their bytes. */
enum class EOKPropKind : uint8 { Int, Float, Bool, Byte, Name, String, Object, Struct, Array, Unknown };

/** One UE3 tagged property (MK:KE, package version 472). */
struct FOKProperty
{
	FString Name;
	FString Type;		// "IntProperty", "StructProperty", ...
	int32 ArrayIndex = 0;		// fixed-size arrays store one tag per element: Name[ArrayIndex]
	EOKPropKind Kind = EOKPropKind::Unknown;

	int32 Int = 0;		// Int, raw Byte, Bool (0/1), Object (a reference), Array (element count)
	float Float = 0.0f;
	FString Text;		// Name, String, enum Byte, Struct (the struct's name)
	TArray<uint8> Raw;	// Struct, Array, Unknown: the bytes as stored
};

/** The property list every object's data starts with. */
class OPENKOMBAT_API FOKPropertyList
{
public:
	/** Reads tags up to "None". False if the list is damaged. */
	bool Read(FOKByteReader& Reader, const TArray<FString>& Names);

	const TArray<FOKProperty>& GetAll() const {
		return Properties;
	}
	const FOKProperty* Find(const FString& Name, int32 ArrayIndex = 0) const;

	/** An Int, Float or raw Byte property as a number; Fallback if it's missing or another kind. */
	double GetNumber(const FString& Name, double Fallback = 0.0) const;
	/** An Object property's reference; 0 if it's missing. */
	int32 GetObject(const FString& Name) const;
	/** A Name, String or enum Byte property's text; empty if it's missing. */
	FString GetText(const FString& Name) const;

	/** A property's value as text, for logs. Object references are shown as paths. */
	static FString DescribeValue(const FOKProperty& Property, const FOKPackage& Package);

	/** The elements of an ArrayProperty whose elements are tagged structs (e.g. TextureParameterValues). */
	static bool ReadStructArray(const FOKProperty& Array, const TArray<FString>& Names, TArray<FOKPropertyList>& Out);
	/** The elements of an ArrayProperty of int32s (e.g. object references). */
	static TArray<int32> ReadIntArray(const FOKProperty& Array);

private:
	TArray<FOKProperty> Properties;
};

/** An FName on disk: an index into the name table, plus a number (n > 0 means "Name_<n-1>"). Sets bBad for a bad index. */
OPENKOMBAT_API FString OKReadName(FOKByteReader& Reader, const TArray<FString>& Names, bool& bBad);