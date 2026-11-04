#pragma once

#include "CoreMinimal.h"

/** Little-endian reader. Reading past the end sets an error flag and returns zeros: check HasError() once afterwards. */
class FOKByteReader
{
public:
	explicit FOKByteReader(TConstArrayView<uint8> InData) : Data(InData) {}

	bool HasError() const { return bError; }
	int64 Tell() const { return Pos; }

	void Seek(int64 InPos)
	{
		if (InPos < 0 || InPos > Data.Num())
		{
			bError = true;
			return;
		}
		Pos = InPos;
	}
	void Skip(int64 Count) { Seek(Pos + Count); }

	/** The next Count bytes, or null (and the error flag) if there aren't that many. */
	const uint8* Take(int64 Count)
	{
		if (bError || Count < 0 || Pos + Count > Data.Num())
		{
			bError = true;
			return nullptr;
		}
		const uint8* Where = Data.GetData() + Pos;
		Pos += Count;
		return Where;
	}

	uint8 U8() {
		uint8 Value = 0; Read(&Value, 1); return Value;
	}
	int64 Remaining() const {
		return Data.Num() - Pos;
	}
	uint16 U16() { uint16 Value = 0; Read(&Value, 2); return Value; }
	int16 I16() { return int16(U16()); }
	uint32 U32() { uint32 Value = 0; Read(&Value, 4); return Value; }
	int32 I32() { return int32(U32()); }
	float F32() { float Value = 0.0f; Read(&Value, 4); return Value; }

	/** UE3 FString: int32 length, then Latin-1 bytes (length > 0) or UTF-16 characters (length < 0), zero-terminated. */
	FString ReadFString()
	{
		const int32 Length = I32();
		if (Length == 0)
		{
			return FString();
		}
		if (Length > 65536 || Length < -65536)
		{
			bError = true;
			return FString();
		}
		FString Out;
		if (Length > 0)
		{
			const uint8* Chars = Take(Length);
			for (int32 i = 0; Chars && i < Length && Chars[i] != 0; ++i)
			{
				Out.AppendChar(TCHAR(Chars[i]));
			}
		}
		else
		{
			const uint8* Chars = Take(int64(-Length) * 2);
			for (int32 i = 0; Chars && i < -Length; ++i)
			{
				const TCHAR Char = TCHAR(Chars[i * 2] | (Chars[i * 2 + 1] << 8));
				if (Char == 0)
				{
					break;
				}
				Out.AppendChar(Char);
			}
		}
		return Out;
	}

private:
	void Read(void* Out, int64 Count)
	{
		if (const uint8* Where = Take(Count))
		{
			FMemory::Memcpy(Out, Where, Count);
		}
	}

	TConstArrayView<uint8> Data;
	int64 Pos = 0;
	bool bError = false;
};

/** Helpers the mesh readers share. */
namespace OKBytes
{
	/** A packed tangent component: a byte mapped to [-1, 1]. */
	inline float Unpack(uint8 Byte)
	{
		return Byte / 127.5f - 1.0f;
	}

	inline FVector3f UnpackVector(const uint8* Bytes)
	{
		return FVector3f(Unpack(Bytes[0]), Unpack(Bytes[1]), Unpack(Bytes[2]));
	}

	/** Reads an int32 count and checks it against a limit and the bytes left (each element at least MinBytes). */
	inline int32 ReadCount(FOKByteReader& R, int32 Limit, int32 MinBytes, bool& bBad)
	{
		const int32 Count = R.I32();
		if (Count < 0 || Count > Limit || int64(Count) * MinBytes > R.Remaining())
		{
			bBad = true;
			return 0;
		}
		return Count;
	}
}
