#include "OKProperties.h"
#include "OKByteReader.h"
#include "OKPackage.h"

FString OKReadName(FOKByteReader& Reader, const TArray<FString>& Names, bool& bBad)
{
	const int32 Index = Reader.I32();
	const int32 Number = Reader.I32();
	if (!Names.IsValidIndex(Index))
	{
		bBad = true;
		return FString();
	}
	return Number ? FString::Printf(TEXT("%s_%d"), *Names[Index], Number - 1) : Names[Index];
}

static void TakeRaw(FOKByteReader& Reader, int32 Size, TArray<uint8>& Out)
{
	if (const uint8* Bytes = Reader.Take(Size))
	{
		Out.Append(Bytes, Size);
	}
}

bool FOKPropertyList::Read(FOKByteReader& Reader, const TArray<FString>& Names)
{
	bool bBad = false;
	for (int32 Count = 0; Count < 4096; ++Count)
	{
		FOKProperty P;
		P.Name = OKReadName(Reader, Names, bBad);
		if (bBad || Reader.HasError())
		{
			return false;
		}
		if (P.Name == TEXT("None"))
		{
			return true;
		}
		P.Type = OKReadName(Reader, Names, bBad);
		const int32 Size = Reader.I32();
		P.ArrayIndex = Reader.I32();
		if (bBad || Size < 0 || Size > Reader.Remaining())
		{
			return false;
		}

		if (P.Type == TEXT("BoolProperty"))
		{
			// The value sits in the tag iself, as an int32; Size is 0.
			P.Kind = EOKPropKind::Bool;
			P.Int = Reader.I32() != 0 ? 1 : 0;
			Reader.Skip(Size);
		}
		else if (P.Type == TEXT("StructProperty"))
		{
			P.Kind = EOKPropKind::Struct;
			P.Text = OKReadName(Reader, Names, bBad);
			TakeRaw(Reader, Size, P.Raw);
		}
		else
		{
			const int64 Start = Reader.Tell();
			if (P.Type == TEXT("IntProperty"))
			{
				P.Kind = EOKPropKind::Int;
				P.Int = Reader.I32();
			}
			else if (P.Type == TEXT("FloatProperty"))
			{
				P.Kind = EOKPropKind::Float;
				P.Float = Reader.F32();
			}
			else if (P.Type == TEXT("ObjectProperty") || P.Type == TEXT("ComponentProperty") || P.Type == TEXT("ClassProperty"))
			{
				P.Kind = EOKPropKind::Object;
				P.Int = Reader.I32();
			}
			else if (P.Type == TEXT("NameProperty"))
			{
				P.Kind = EOKPropKind::Name;
				P.Text = OKReadName(Reader, Names, bBad);
			}
			else if (P.Type == TEXT("StrProperty"))
			{
				P.Kind = EOKPropKind::String;
				P.Text = Reader.ReadFString();
			}
			else if (P.Type == TEXT("ByteProperty"))
			{
				// Size 8 = an enum value stored as a name; otherwise a plain byte.
				P.Kind = EOKPropKind::Byte;
				if (Size == 8)
				{
					P.Text = OKReadName(Reader, Names, bBad);
				}
				else
				{
					P.Int = Reader.U8();
				}
			}
			else if (P.Type == TEXT("ArrayProperty"))
			{
				// Untyped: the bytes start with the element count/
				P.Kind = EOKPropKind::Array;
				TakeRaw(Reader, Size, P.Raw);
				if (P.Raw.Num() >= 4)
				{
					FMemory::Memcpy(&P.Int, P.Raw.GetData(), 4);
				}
			}
			else
			{
				P.Kind = EOKPropKind::Unknown;
				TakeRaw(Reader, Size, P.Raw);
			}
			Reader.Seek(Start + Size);		// whatever the value was, the next tag starts Size bytes on
		}
		if (bBad || Reader.HasError())
		{
			return false;
		}
		Properties.Add(MoveTemp(P));
	}
	return false;		// no "None" within 4096 tags: not a property list
}

const FOKProperty* FOKPropertyList::Find(const FString& Name, int32 ArrayIndex) const
{
	return Properties.FindByPredicate([&](const FOKProperty& P) { return P.Name == Name && P.ArrayIndex == ArrayIndex; });
}

double FOKPropertyList::GetNumber(const FString& Name, double Fallback) const
{
	const FOKProperty* P = Find(Name);
	if (!P)
	{
		return Fallback;
	}
	switch (P->Kind)
	{
	case EOKPropKind::Int: return P->Int;
	case EOKPropKind::Float: return P->Float;
	case EOKPropKind::Byte: return P->Text.IsEmpty() ? double(P->Int) : Fallback;
	default: return Fallback;
	}
}

int32 FOKPropertyList::GetObject(const FString& Name) const
{
	const FOKProperty* P = Find(Name);
	return (P && P->Kind == EOKPropKind::Object) ? P->Int : 0;
}

FString FOKPropertyList::GetText(const FString& Name) const
{
	const FOKProperty* P = Find(Name);
	return (P && (P->Kind == EOKPropKind::Name || P->Kind == EOKPropKind::String || P->Kind == EOKPropKind::Byte)) ? P->Text : FString();
}

FString FOKPropertyList::DescribeValue(const FOKProperty& P, const FOKPackage& Package)
{
	switch (P.Kind)
	{
	case EOKPropKind::Int: return FString::Printf(TEXT("%d"), P.Int);
	case EOKPropKind::Float: return FString::Printf(TEXT("%g"), P.Float);
	case EOKPropKind::Bool: return P.Int ? TEXT("true") : TEXT("false");
	case EOKPropKind::Byte: return P.Text.IsEmpty() ? FString::Printf(TEXT("%d"), P.Int) : P.Text;
	case EOKPropKind::Name:
	case EOKPropKind::String: return P.Text;
	case EOKPropKind::Object: return P.Int == 0 ? TEXT("None") : Package.ObjectPath(P.Int);
	case EOKPropKind::Struct: return FString::Printf(TEXT("<%s %d bytes>"), *P.Text, P.Raw.Num());
	case EOKPropKind::Array: return FString::Printf(TEXT("<array, %d element(s)>"), P.Int);
	default: return FString::Printf(TEXT("<%s %d bytes>"), *P.Type, P.Raw.Num());
	}
}

bool FOKPropertyList::ReadStructArray(const FOKProperty& Array, const TArray<FString>& Names, TArray<FOKPropertyList>& Out)
{
	FOKByteReader R(Array.Raw);
	const int32 Count = R.I32();
	if (Count < 0 || Count > 100000)
	{
		return false;
	}
	for (int32 i = 0; i < Count; ++i)
	{
		if (!Out.AddDefaulted_GetRef().Read(R, Names))
		{
			return false;
		}
	}
	return true;
}

TArray<int32> FOKPropertyList::ReadIntArray(const FOKProperty& Array)
{
	TArray<int32> Out;
	FOKByteReader R(Array.Raw);
	const int32 Count = R.I32();
	if (Count < 0 || Count >(Array.Raw.Num() - 4) / 4)
	{
		return Out;
	}
	for (int32 i = 0; i < Count; ++i)
	{
		Out.Add(R.I32());
	}
	return Out;
}

bool OKProps::ReadObject(FOKPackage& Package, int32 Ref, FOKPropertyList& Out)
{
	const TArray<FOKPackageExport>& Exports = Package.GetExports();
	TArray<uint8> Bytes;
	if (!Exports.IsValidIndex(Ref - 1) || !Package.ExportData(Exports[Ref - 1], Bytes))
	{
		return false;
	}
	FOKByteReader Reader(Bytes);
	if (Out.Read(Reader, Package.GetNames()))
	{
		return true;
	}
	Out = FOKPropertyList();
	FOKByteReader Skipped(TConstArrayView<uint8>(Bytes).Slice(FMath::Min(4, Bytes.Num()), FMath::Max(0, Bytes.Num() - 4)));
	return Out.Read(Skipped, Package.GetNames());
}
