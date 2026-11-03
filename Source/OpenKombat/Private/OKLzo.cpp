#include "OKLzo.h"

int32 OKLzo::Decompress(const uint8* Src, int32 SrcLen, uint8* Dst, int32 DstLen)
{
	int32 Ip = 0;		// input position
	int32 Op = 0;		// output position
	int32 T = 0;
	bool bBad = false;

	auto Next = [&]() -> int32
		{
			if (Ip >= SrcLen)
			{
				bBad = true;
				return 0;
			}
			return Src[Ip++];
		};
	auto Literal = [&](int32 Count)
		{
			if (Count < 0 || Ip + Count > SrcLen || Op + Count > DstLen)
			{
				bBad = true;
				return;
			}
			FMemory::Memcpy(Dst + Op, Src + Ip, Count);
			Ip += Count;
			Op += Count;
		};
	auto Copy = [&](int32 From, int32 Count)
		{
			if (From < 0 || Op + Count > DstLen)
			{
				bBad = true;
				return;
			}
			for (int32 i = 0; i < Count; ++i)		// byte by byte: a match may overlap the bytes it writes
			{
				Dst[Op++] = Dst[From + i];
			}
		};
	auto Extend = [&](int32 Base) -> int32		// long lengths: each zero byte adds 255
		{
			int32 Value = 0;
			while (Ip < SrcLen && Src[Ip] == 0)
			{
				Value += 255;
				++Ip;
				if (Value > 0x7FFF0000)
				{
					bBad = true;
					return 0;
				}
			}
			return Value + Base + Next();
		};

	// The states mirror the labels in the reference decoder.
	enum class EState { Loop, FirstLiteralRun, Match };
	EState State = EState::Loop;

	if (SrcLen < 1)
	{
		return -1;
	}
	if (Src[0] > 17)
	{
		T = Next() - 17;
		Literal(T);
		if (T < 4)
		{
			T = Next();
			State = EState::Match;
		}
		else
		{
			State = EState::FirstLiteralRun;
		}
	}

	while (!bBad)
	{
		if (State == EState::Loop)
		{
			T = Next();
			if (T >= 16)
			{
				State = EState::Match;
				continue;
			}
			if (T == 0)
			{
				T = Extend(15);
			}
			Literal(T + 3);
			State = EState::FirstLiteralRun;
			continue;
		}

		if (State == EState::FirstLiteralRun)
		{
			T = Next();
			if (T >= 16)
			{
				State = EState::Match;
				continue;
			}
			const int32 From = Op - (1 + 0x0800) - (T >> 2) - (Next() << 2);
			Copy(From, 3);
		}
		else if (T >= 64)
		{
			const int32 From = Op - 1 - ((T >> 2) & 7) - (Next() << 3);
			Copy(From, (T >> 5) - 1 + 2);
		}
		else if (T >= 32)
		{
			T &= 31;
			if (T == 0)
			{
				T = Extend(31);
			}
			const int32 B0 = Next();		// two reads in sequence: C++ doesn't order them inside one expression
			const int32 B1 = Next();
			Copy(Op - 1 - ((B0 >> 2) + (B1 << 6)), T + 2);
		}
		else if (T >= 16)
		{
			int32 From = Op - ((T & 8) << 11);
			T &= 7;
			if (T == 0)
			{
				T = Extend(7);
			}
			const int32 B0 = Next();
			const int32 B1 = Next();
			From -= (B0 >> 2) + (B1 << 6);
			if (From == Op)
			{
				break;		// end of stream
			}
			Copy(From - 0x4000, T + 2);
		}
		else
		{
			Copy(Op - 1 - (T >> 2) - (Next() << 2), 2);
		}

		// After a match: the low 2 bits of the byte before last say how many literals follow.
		if (bBad || Ip < 2)
		{
			bBad = true;
			break;
		}
		T = Src[Ip - 2] & 3;
		if (T == 0)
		{
			State = EState::Loop;
			continue;
		}
		Literal(T);
		T = Next();
		State = EState::Match;
	}
	return bBad ? -1 : Op;
}