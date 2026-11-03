#include "OKInput.h"
#include "OKScriptObjects.h"
#include "OKScriptRuntime.h"

namespace
{
	/** Reads and writes in the runtime's heap. Addresses outside it read as 0 and ignore writes. */
	struct FHeap
	{
		FOKScriptRuntime& Runtime;

		uint32 Word(uint32 Address) const
		{
			uint32 Value = 0;
			Runtime.LoadHeapWord(Address, Value);
			return Value;
		}
		void SetWord(uint32 Address, uint32 Value) const { Runtime.SetHeapWord(Address, Value); }
		uint8 Byte(uint32 Address) const
		{
			const uint8* Where = (Address >> 24) == FOKScriptRuntime::HeapTag ? Runtime.HeapBytes(Address & 0x00FFFFFF, 1) : nullptr;
			return Where ? *Where : 0;
		}
		void SetByte(uint32 Address, uint8 Value) const
		{
			if (uint8* Where = (Address >> 24) == FOKScriptRuntime::HeapTag ? Runtime.HeapBytes(Address & 0x00FFFFFF, 1) : nullptr)
			{
				*Where = Value;
			}
		}
	};

	/** The next ring index after I, wrapping at 30. */
	uint32 NextIndex(uint32 I) { return I + 1 > 0x1D ? 0 : I + 1; }

	uint32 PressEntry(uint32 Buffer, uint32 Index) { return Buffer + OKInput::Presses + Index * OKInput::EntryBytes; }
	uint32 DirEntry(uint32 Buffer, uint32 Index) { return Buffer + OKInput::Directions + Index * OKInput::EntryBytes; }

	bool Connected(const FHeap& H, uint32 Buffer) { return Buffer != 0 && (H.Byte(Buffer + OKInput::State) & 0xC9) != 0; }

	/**
	 * The frame a mask was last recorded in a ring, or 0: newest to the start, then from the end back to the
	 * newest (FUN_00425BC0 presses, FUN_00425C30 directions, FUN_00425B50 releases).
	 */
	uint32 LastFrameIn(const FHeap& H, uint32 Buffer, uint32 HeadAt, uint32 Ring, uint32 Mask)
	{
		const uint32 Head = H.Byte(Buffer + HeadAt);
		auto Entry = [&](uint32 I) { return Buffer + Ring + I * OKInput::EntryBytes; };
		for (uint32 I = Head; I != 0; --I)
		{
			if (H.Word(Entry(I)) == Mask)
			{
				return H.Word(Entry(I) + 4);
			}
		}
		if (H.Word(Entry(0)) == Mask)
		{
			return H.Word(Entry(0) + 4);
		}
		for (uint32 I = 0x1D; Head < I; --I)
		{
			if (H.Word(Entry(I)) == Mask)
			{
				return H.Word(Entry(I) + 4);
			}
		}
		return 0;
	}

	uint32 LastFrameOf(const FHeap& H, uint32 Buffer, uint32 Mask, bool bDirections)
	{
		return bDirections ? LastFrameIn(H, Buffer, OKInput::DirHead, OKInput::Directions, Mask)
			: LastFrameIn(H, Buffer, OKInput::PressHead, OKInput::Presses, Mask);
	}
}

uint32 OKInput::CodeToMask(uint32 Code)
{
	switch (Code)
	{
	case 0x80000003: return 1;
	case 0x80000004: return 2;
	case 0x80000005: case 0x80000006: case 0x80000007: case 0x80000008: case 0x8000000F:
	case 0x80000014: case 0x80000015: case 0x80000016: case 0x80000017: case 0x80000018: case 0x80000019:
	case 0x80000034: case 0x80000035: case 0x80000036: case 0x80000037: case 0x80000038:
		return 0x40;
	case 0x8000000D: return 8;
	case 0x8000000E: return 4;
	case 0x80000010: return 0x10;
	case 0x80000011: return 0x20;
	case 0x80000012: return 0x80;
	case 0x8000001B: case 0x80000033: return 0x4000;
	case 0x8000001C: return 0x8000;
	case 0x8000001D: return 0x2000;
	case 0x8000001E: case 0x80000032: return 0x1000;
	case 0x8000002B: return 0x10002;
	case 0x8000002C: return 0x10001;
	case 0x8000003D: return 0x200;
	default: return 0;
	}
}

uint32 OKInput::MaskToCode(uint32 Mask)
{
	switch (Mask)
	{
	case 1: return 0x80000003;
	case 2: return 0x80000004;
	case 4: return 0x8000000E;
	case 8: return 0x8000000D;
	case 0x10: return 0x80000010;
	case 0x20: return 0x80000011;
	case 0x40: return 0x8000000F;
	case 0x80: return 0x80000012;
	case 0x1000: return 0x8000001E;
	case 0x2000: return 0x8000001D;
	case 0x4000: return 0x8000001B;
	case 0x8000: return 0x8000001C;
	default: return 0x80000000;
	}
}

uint32 OKInput::CreateBuffer(FOKScriptRuntime& Runtime, FOKPlayer& Player)
{
	const uint32 Buffer = Runtime.CreateOwned(MakeUnique<FOKScriptObject>(), Bytes);
	const FHeap H{ Runtime };
	H.SetWord(Buffer + Owner, Player.Handle);
	H.SetByte(Buffer + State, 0x01);		// connected
	H.SetWord(Player.Handle + FOKPlayer::InputBuffer, Buffer);
	return Buffer;
}

void OKInput::Poll(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 HeldMask, uint32 Frame, TFunctionRef<uint32()> DirectionNow)
{
	const FHeap H{ Runtime };
	if (!Connected(H, Buffer))
	{
		return;
	}
	const uint32 Previous = H.Word(Buffer + Held);
	H.SetWord(Buffer + PreviousHeld, Previous);
	H.SetWord(Buffer + Held, HeldMask);
	const uint32 DirectionCode = DirectionNow();		// the owner's direction (player virtual +0x34), from the new held mask

	// [inf] MKKE walks its controller's binding table; this is its mask order as far as it matters: directions,
	// the four attack buttons, then block, throw, flip stance and tag.
	static const uint32 Masks[] = { 1, 2, 4, 8, 0x40, 0x10, 0x20, 0x80, 0x8000, 0x2000, 0x4000, 0x1000 };
	for (const uint32 Mask : Masks)
	{
		const bool bDirection = Mask == 1 || Mask == 2 || Mask == 4 || Mask == 8;
		if ((HeldMask & Mask) == 0 && (Previous & Mask) != 0 && !bDirection)
		{
			// FUN_004256A0: a button let go goes to the release ring, with the direction held.
			// [inf] MKKE records it only while FUN_00454810(0) allows; that test isn't read.
			const uint32 Head = NextIndex(H.Byte(Buffer + ReleaseHead));
			H.SetByte(Buffer + ReleaseHead, uint8(Head));
			const uint32 Entry = Buffer + Releases + Head * EntryBytes;
			H.SetWord(Entry, Mask);
			H.SetWord(Entry + 4, Frame);
			H.SetWord(Entry + 8, DirectionCode);
			H.SetWord(Entry + 12, 0);
			continue;
		}
		if ((HeldMask & Mask) == 0 || (Previous & Mask) != 0)
		{
			continue;		// not a new press
		}
		// FUN_00425770: directions to the direction ring; buttons to the press ring with the direction held.
		const uint32 HeadAt = Buffer + (bDirection ? DirHead : PressHead);
		const uint32 Head = NextIndex(H.Byte(HeadAt));
		H.SetByte(HeadAt, uint8(Head));
		const uint32 Entry = bDirection ? DirEntry(Buffer, Head) : PressEntry(Buffer, Head);
		H.SetWord(Entry, Mask);
		H.SetWord(Entry + 4, Frame);
		H.SetWord(Entry + 8, bDirection ? 0 : DirectionCode);
		H.SetWord(Entry + 12, 0);
	}
}

bool OKInput::IsHeld(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask)
{
	const FHeap H{ Runtime };
	return Connected(H, Buffer) && (H.Word(Buffer + Held) & Mask) != 0;
}

bool OKInput::JustPressed(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask)
{
	const FHeap H{ Runtime };
	return Connected(H, Buffer) && (H.Word(Buffer + Held) & Mask) != 0 && (H.Word(Buffer + PreviousHeld) & Mask) == 0;
}

bool OKInput::NextPress(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask, uint32 Direction, bool bUseDirection, uint32* OutFrame)
{
	const FHeap H{ Runtime };
	if (Buffer == 0)
	{
		return false;
	}
	uint32 Ahead = H.Byte(Buffer + Cursor);
	uint32 At = Ahead;
	for (;;)
	{
		if (H.Byte(Buffer + Cursor) != H.Byte(Buffer + PressHead))
		{
			Ahead = NextIndex(At);
		}
		if (At == H.Byte(Buffer + PressHead))
		{
			return false;
		}
		const uint32 Entry = PressEntry(Buffer, Ahead);
		if (H.Word(Entry) != 0x100 && H.Word(Entry + 12) == 0)
		{
			// The next real press decides: it's the one asked for, or nothing is.
			if (H.Word(Entry) == Mask && (!bUseDirection || H.Word(Entry + 8) == Direction))
			{
				if (OutFrame)
				{
					*OutFrame = H.Word(Entry + 4);
				}
				return true;
			}
			return false;
		}
		H.SetByte(Buffer + Cursor, uint8(Ahead));
		At = Ahead;
	}
}

bool OKInput::NextChord(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 MaskA, uint32 MaskB, uint32 Direction, bool bUseDirection, uint32* OutFrame)
{
	const FHeap H{ Runtime };
	if (Buffer == 0)
	{
		return false;
	}
	const uint32 Head = H.Byte(Buffer + PressHead);
	const uint32 Read = H.Byte(Buffer + Cursor);
	const uint32 First = Read != Head ? NextIndex(Read) : Read;
	if (Read == Head || First == Head)
	{
		return false;
	}
	const uint32 Second = NextIndex(First);
	const uint32 A = PressEntry(Buffer, First), B = PressEntry(Buffer, Second);
	if (bUseDirection && H.Word(A + 8) != Direction)
	{
		return false;
	}
	const uint32 Mask = H.Word(A);
	const uint32 Other = Mask == MaskA ? MaskB : (Mask == MaskB ? MaskA : 0);
	if (Other != 0 && H.Word(B) == Other && H.Word(B + 4) - H.Word(A + 4) < 7)
	{
		if (OutFrame)
		{
			*OutFrame = H.Word(B + 4);
		}
		return true;
	}
	return false;
}

void OKInput::Flush(FOKScriptRuntime& Runtime, uint32 Buffer)
{
	const FHeap H{ Runtime };
	if (Buffer == 0)
	{
		return;
	}
	const uint32 Head = H.Byte(Buffer + PressHead);
	H.SetByte(Buffer + Cursor, uint8(Head));
	H.SetWord(Buffer + Cursor + 4, H.Word(Buffer + Presses + Head * EntryBytes));		// the last press read: mask
	H.SetWord(Buffer + Cursor + 8, H.Word(Buffer + Presses + Head * EntryBytes + 4));	// and frame
	ClearReleases(Runtime, Buffer);
}

uint32 OKInput::LastRelease(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask)
{
	const FHeap H{ Runtime };
	return Buffer ? LastFrameIn(H, Buffer, ReleaseHead, Releases, Mask) : 0;
}

void OKInput::ClearReleases(FOKScriptRuntime& Runtime, uint32 Buffer)
{
	const FHeap H{ Runtime };
	for (int32 i = 0; Buffer && i < RingSize; ++i)
	{
		H.SetWord(Buffer + Releases + i * EntryBytes + 4, 0);
	}
}

void OKInput::Consume(FOKScriptRuntime& Runtime, uint32 Buffer)
{
	const FHeap H{ Runtime };
	if (Buffer == 0)
	{
		return;
	}
	const uint32 OldHead = H.Byte(Buffer + PressHead);
	if (H.Byte(Buffer + Cursor) != OldHead)
	{
		const uint32 Read = NextIndex(H.Byte(Buffer + Cursor));
		H.SetByte(Buffer + Cursor, uint8(Read));
		// The reader keeps at most four presses waiting: when it falls further behind, the newest press moves to
		// four past the cursor and the ones in between are dropped.
		const uint32 Head = H.Byte(Buffer + PressHead);
		const uint32 Waiting = Head < Read ? Head + 30 - Read : Head - Read;
		if (Waiting > 4)
		{
			const uint32 NewHead = (Read + 4) % 30;
			const uint32 From = PressEntry(Buffer, OldHead), To = PressEntry(Buffer, NewHead);
			H.SetByte(Buffer + PressHead, uint8(NewHead));
			H.SetWord(To, H.Word(From));
			H.SetWord(To + 4, H.Word(From + 4));
			H.SetWord(To + 8, H.Word(From + 8));
		}
	}
	const uint32 Entry = PressEntry(Buffer, H.Byte(Buffer + Cursor));
	H.SetWord(Buffer + 0x5FC, H.Word(Entry));
	H.SetWord(Buffer + 0x600, H.Word(Entry + 4));
}

bool OKInput::LastPress(FOKScriptRuntime& Runtime, uint32 Buffer, uint32& OutCode, uint32& OutDirection)
{
	const FHeap H{ Runtime };
	if (Buffer == 0 || H.Byte(Buffer + PressHead) == H.Byte(Buffer + Cursor))
	{
		return false;
	}
	const uint32 Entry = PressEntry(Buffer, H.Byte(Buffer + PressHead));
	OutCode = MaskToCode(H.Word(Entry));
	OutDirection = H.Word(Entry + 8);
	return true;
}

bool OKInput::PressedWithin(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 MaskA, uint32 MaskB, uint32 Window, uint32 Now)
{
	const FHeap H{ Runtime };
	for (const uint32 Mask : { MaskA, MaskB })
	{
		uint32 Frame = LastFrameOf(H, Buffer, Mask, false);
		if (Frame == 0)
		{
			Frame = LastFrameOf(H, Buffer, Mask, true);
		}
		if (Frame == 0 || Now - Frame > Window)
		{
			return false;
		}
	}
	return true;
}

bool OKInput::MatchMotion(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Window, const TArray<uint32>& Steps, uint32 Now, uint32 MinGap,
	uint32 TowardsMask, uint32 AwayMask)
{
	return MatchMotionAt(Runtime, Buffer, Window, Steps, Now, MAX_uint32, MinGap, TowardsMask, AwayMask, nullptr);
}

bool OKInput::MatchMotionAt(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Window, const TArray<uint32>& Steps, uint32 Now, uint32 Late,
	uint32 MinGap, uint32 TowardsMask, uint32 AwayMask, uint32* OutOldest)
{
	const FHeap H{ Runtime };
	if (Buffer == 0)
	{
		return false;
	}
	int32 Left = Steps.Num();		// steps still to find, matched newest (last) first
	int32 Index = H.Byte(Buffer + DirHead);
	uint32 Since = Now;				// the frame of the step matched last
	uint32 Skipped = 0;
	uint32 LenientFrom = 0;
	bool bAnyMatched = false;
	bool bLenient = false;			// one unrelated entry was passed: the next step must follow within 3 frames

	auto IsTowards = [](uint32 Code) { return Code == 0x80000001 || Code == 0x8000000D; };
	auto IsAway = [](uint32 Code) { return Code == 0x80000002 || Code == 0x8000000E; };
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		if (Left < 1)
		{
			if (OutOldest)
			{
				*OutOldest = Since;
			}
			return true;
		}
		const uint32 Entry = DirEntry(Buffer, uint32(Index));
		Index = Index - 1 < 0 ? 0x1D : Index - 1;
		const uint32 Mask = H.Word(Entry), Frame = H.Word(Entry + 4);
		const uint32 Step = Steps[Left - 1];

		bool bMatched = false;
		if (bLenient)
		{
			if (IsTowards(Step)) { bMatched = Mask == TowardsMask; }
			else if (IsAway(Step)) { bMatched = Mask == AwayMask; }
			else if (Step == 0x80000003) { bMatched = Mask == 1; }
			else if (Step == 0x80000004) { bMatched = Mask == 2; }
			else { return false; }
			if (!bMatched || LenientFrom - Frame > 3)
			{
				return false;
			}
			LenientFrom = 0;
		}
		else
		{
			bool bOpposite = false;
			if (IsTowards(Step)) { bMatched = Mask == TowardsMask; bOpposite = Mask == AwayMask; }
			else if (IsAway(Step)) { bMatched = Mask == AwayMask; bOpposite = Mask == TowardsMask; }
			else if (Step == 0x80000003) { bMatched = Mask == 1; bOpposite = Mask == 2; }
			else if (Step == 0x80000004) { bMatched = Mask == 2; bOpposite = Mask == 1; }
			else { return false; }
			if (!bMatched)
			{
				if (bOpposite)
				{
					return false;		// the opposite direction breaks the motion
				}
				++Skipped;
				if (Since - Frame > 2 || Skipped > 1)
				{
					bLenient = true;
					Skipped = 0;
					LenientFrom = Frame;
				}
				continue;
			}
		}
		bLenient = false;
		if (Late != MAX_uint32 && uint64(Frame) > uint64(Now) + Late)
		{
			return false;		// too long after the press (FUN_00426690 only)
		}
		const int32 Age = int32(Now - Frame);
		if (uint32(FMath::Abs(Age)) > Window)
		{
			return false;
		}
		if (Since - Frame < MinGap && bAnyMatched)
		{
			return false;
		}
		bAnyMatched = true;
		--Left;
		Since = Frame;
	}
	return false;
}

// --- The player's tests ----------------------------------------------------------------------------------

namespace
{
	uint32 BufferOf(FOKScriptRuntime& Runtime, const FOKPlayer& Player)
	{
		uint32 Buffer = 0;
		Runtime.LoadHeapWord(Player.Handle + FOKPlayer::InputBuffer, Buffer);
		return Buffer;
	}

	bool InputOff(FOKScriptRuntime& Runtime, const FOKPlayer& Player)
	{
		uint32 Off = 0;
		Runtime.LoadHeapWord(Player.Handle + FOKPlayer::InputOff, Off);
		return Off == 1;
	}

	bool FacingRight(const FOKPlayer& Player) { return Player.Fighter && Player.Fighter->bFacingRight; }
}

void OKPlayerInput::FacingMasks(const FOKFighter& Fighter, uint32& OutTowards, uint32& OutAway)
{
	OutTowards = Fighter.bFacingRight ? 8 : 4;
	OutAway = Fighter.bFacingRight ? 4 : 8;
}

bool OKPlayerInput::Held(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code)
{
	return !InputOff(Runtime, Player) && OKInput::IsHeld(Runtime, BufferOf(Runtime, Player), OKInput::CodeToMask(Code));
}

bool OKPlayerInput::JustPressed(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code)
{
	return !InputOff(Runtime, Player) && OKInput::JustPressed(Runtime, BufferOf(Runtime, Player), OKInput::CodeToMask(Code));
}

namespace
{
	/** Both buttons within 90 frames (FUN_00446500 -> FUN_00425CA0). */
	bool Within(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 A, uint32 B)
	{
		return OKInput::PressedWithin(Runtime, BufferOf(Runtime, Player), OKInput::CodeToMask(A), OKInput::CodeToMask(B), 0x5A, uint32(Runtime.GetSimFrame()));
	}

	/** +0x14's chords: one button pressed this frame while the other is held, both within 90 frames. */
	bool PressedChord(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 A, uint32 B, uint32 WithinA, uint32 WithinB)
	{
		uint32 Other = 0;
		if (OKPlayerInput::JustPressed(Runtime, Player, A)) { Other = B; }
		else if (OKPlayerInput::JustPressed(Runtime, Player, B)) { Other = A; }
		else { return false; }
		return OKPlayerInput::Held(Runtime, Player, Other) && Within(Runtime, Player, WithinA, WithinB);
	}

	/** +0x1C's chords: both held, both within 90 frames. */
	bool HeldChord(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 A, uint32 B)
	{
		return OKPlayerInput::Held(Runtime, Player, A) && OKPlayerInput::Held(Runtime, Player, B) && Within(Runtime, Player, A, B);
	}
}

bool OKPlayerInput::Pressed(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code)
{
	const uint32 Right = 0x8000000D, Left = 0x8000000E, Up = 0x80000003, Down = 0x80000004, Block = 0x8000001C, Tag = 0x8000001E;
	const bool bRight = FacingRight(Player);
	const uint32 Towards = bRight ? Right : Left, Away = bRight ? Left : Right;
	switch (Code)
	{
	case 0x80000001: return JustPressed(Runtime, Player, Towards);
	case 0x80000002: return JustPressed(Runtime, Player, Away);
	case 0x80000003: return !Held(Runtime, Player, Tag) && JustPressed(Runtime, Player, Up);
	case 0x80000005: return PressedChord(Runtime, Player, Towards, Block, Towards, Block);
	case 0x80000006: return PressedChord(Runtime, Player, Away, Block, Away, Block);
	case 0x80000007: return PressedChord(Runtime, Player, Up, Block, Up, Block);
	case 0x80000008: return PressedChord(Runtime, Player, Down, Block, Down, Block);
	case 0x80000014: return PressedChord(Runtime, Player, 0x8000000F, 0x80000010, 0x8000000F, 0x80000010);
	case 0x80000015: return PressedChord(Runtime, Player, 0x8000000F, 0x80000011, 0x8000000F, 0x80000011);
	case 0x80000016: return PressedChord(Runtime, Player, 0x8000000F, 0x80000012, 0x8000000F, 0x80000012);
	case 0x80000017: return PressedChord(Runtime, Player, 0x80000010, 0x80000011, 0x80000011, 0x80000010);
	case 0x80000018: return PressedChord(Runtime, Player, 0x80000010, 0x80000012, 0x80000012, 0x80000010);
	case 0x80000019: return PressedChord(Runtime, Player, 0x80000011, 0x80000012, 0x80000011, 0x80000012);
	case 0x8000001F: return JustPressed(Runtime, Player, Down) && JustPressed(Runtime, Player, Away);
	case 0x80000020: return JustPressed(Runtime, Player, Down) && JustPressed(Runtime, Player, Towards);
	case 0x80000021: return JustPressed(Runtime, Player, Up) && JustPressed(Runtime, Player, Away);
	case 0x80000022: return JustPressed(Runtime, Player, Up) && JustPressed(Runtime, Player, Towards);
	case 0x8000002C: return !Held(Runtime, Player, Tag) && JustPressed(Runtime, Player, 0x8000002C);
	case 0x8000002D: return !Held(Runtime, Player, Tag) && JustPressed(Runtime, Player, Up) && !JustPressed(Runtime, Player, 0x8000002C);
	case 0x8000002E: return JustPressed(Runtime, Player, Up) && JustPressed(Runtime, Player, Away) && !JustPressed(Runtime, Player, 0x8000002C);
	case 0x8000002F: return JustPressed(Runtime, Player, Up) && JustPressed(Runtime, Player, Towards) && !JustPressed(Runtime, Player, 0x8000002C);
	case 0x80000030: return Held(Runtime, Player, Down) && JustPressed(Runtime, Player, 0x8000001D);
	case 0x80000031: return Held(Runtime, Player, Down) && PressedChord(Runtime, Player, 0x80000010, 0x80000012, 0x80000012, 0x80000010);
	case 0x80000032: return JustPressed(Runtime, Player, Tag) || PressedChord(Runtime, Player, 0x80000010, 0x80000012, 0x80000010, 0x80000012);
	case 0x80000033: return JustPressed(Runtime, Player, 0x8000001B) || PressedChord(Runtime, Player, 0x80000011, 0x80000012, 0x80000011, 0x80000012);
	case 0x80000034: return PressedChord(Runtime, Player, 0x8000000F, Block, 0x8000000F, Block);
	case 0x80000035: return PressedChord(Runtime, Player, 0x80000010, Block, 0x80000010, Block);
	case 0x80000036: return PressedChord(Runtime, Player, 0x80000011, Block, 0x80000011, Block);
	case 0x80000037: return PressedChord(Runtime, Player, 0x80000012, Block, 0x80000012, Block);
	case 0x80000038: return PressedChord(Runtime, Player, 0x8000001B, Block, 0x8000001B, Block);
	case 0x80000039: return JustPressed(Runtime, Player, 0x8000001D) || PressedChord(Runtime, Player, 0x8000000F, 0x80000011, 0x8000000F, 0x80000011);
	default: return JustPressed(Runtime, Player, Code);
	}
}

bool OKPlayerInput::HeldCommand(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code)
{
	const uint32 Right = 0x8000000D, Left = 0x8000000E, Up = 0x80000003, Down = 0x80000004, Block = 0x8000001C, Tag = 0x8000001E, Flip = 0x8000001B;
	const bool bRight = FacingRight(Player);
	const uint32 Towards = bRight ? Right : Left, Away = bRight ? Left : Right;
	auto H = [&](uint32 C) { return Held(Runtime, Player, C); };
	switch (Code)
	{
	case 0x80000001: return H(Towards) && !H(Down) && !H(Up);
	case 0x80000002: return H(Away) && !H(Down) && !H(Up);
	case 0x80000003: return H(Up) && !H(Right) && !H(Left) && !H(Tag) && !H(Flip);
	case 0x80000004: return H(Down) && !H(Right) && !H(Left) && !H(Tag) && !H(Flip);
	case 0x80000005: return H(Towards) && H(Block) && Within(Runtime, Player, Towards, Block);
	case 0x80000006: return H(Away) && H(Block) && Within(Runtime, Player, Away, Block);
	case 0x80000007: return HeldChord(Runtime, Player, Up, Block);
	case 0x80000008: return HeldChord(Runtime, Player, Down, Block);
	case 0x80000014: return HeldChord(Runtime, Player, 0x8000000F, 0x80000010);
	case 0x80000015: return HeldChord(Runtime, Player, 0x8000000F, 0x80000011);
	case 0x80000016: return HeldChord(Runtime, Player, 0x8000000F, 0x80000012);
	case 0x80000017: return HeldChord(Runtime, Player, 0x80000010, 0x80000011);
	case 0x80000018: return HeldChord(Runtime, Player, 0x80000010, 0x80000012);
	case 0x80000019: return HeldChord(Runtime, Player, 0x80000011, 0x80000012);
	case 0x8000001F: return H(Down) && H(Away);
	case 0x80000020: return H(Down) && H(Towards);
	case 0x80000021: return H(Up) && H(Away);
	case 0x80000022: return H(Up) && H(Towards);
	case 0x8000002B: case 0x8000002C: return H(Code) && !H(Tag) && !H(Flip);
	case 0x8000002D: return H(Up) && !H(Right) && !H(Left) && !H(Tag) && !H(Flip) && !H(0x8000002C);
	case 0x8000002E: return H(Up) && H(Away) && !H(0x8000002C);
	case 0x8000002F: return H(Up) && H(Towards) && !H(0x8000002C);
	case 0x80000030: return H(Down) && H(0x8000001D);
	case 0x80000031: return H(Down) && HeldChord(Runtime, Player, 0x80000010, 0x80000012);
	case 0x80000032: return H(Tag) || HeldChord(Runtime, Player, 0x80000010, 0x80000012);
	case 0x80000033: return H(Flip) || HeldChord(Runtime, Player, 0x80000011, 0x80000012);
	case 0x80000034: return HeldChord(Runtime, Player, 0x8000000F, Block);
	case 0x80000035: return HeldChord(Runtime, Player, 0x80000010, Block);
	case 0x80000036: return HeldChord(Runtime, Player, 0x80000011, Block);
	case 0x80000037: return HeldChord(Runtime, Player, 0x80000012, Block);
	case 0x80000038: return HeldChord(Runtime, Player, Flip, Block) || (H(Block) && H(0x80000011) && H(0x80000012));
	case 0x80000039: return H(0x8000001D) || HeldChord(Runtime, Player, 0x8000000F, 0x80000011);
	default: return H(Code);
	}
}

uint32 OKPlayerInput::Direction(FOKScriptRuntime& Runtime, const FOKPlayer& Player)
{
	if (Held(Runtime, Player, 0x80000003))
	{
		return 0x80000003;
	}
	if (Held(Runtime, Player, 0x80000004))
	{
		uint32 Previous = 0;
		if (Player.Fighter)
		{
			Runtime.LoadHeapWord(Player.Fighter->Handle + FOKFighter::PreviousState, Previous);
		}
		return Previous != 0x100 ? 0x80000004 : 0x80000000;
	}
	const bool bRight = FacingRight(Player);
	if (Held(Runtime, Player, 0x8000000D))
	{
		return bRight ? 0x80000001 : 0x80000002;
	}
	if (Held(Runtime, Player, 0x8000000E))
	{
		return bRight ? 0x80000002 : 0x80000001;
	}
	return 0x80000000;
}

bool OKPlayerInput::TryPress(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code, uint32* OutFrame, bool bConsume)
{
	const uint32 Buffer = BufferOf(Runtime, Player);
	auto Chord = [&](uint32 A, uint32 B) { return OKInput::NextChord(Runtime, Buffer, A, B, 0xFFFFFFFFu, false, OutFrame); };
	auto Next = [&](uint32 C) { return OKInput::NextPress(Runtime, Buffer, OKInput::CodeToMask(C), 0xFFFFFFFFu, false, OutFrame); };
	auto WithDir = [&](uint32 Dir) { return OKInput::NextPress(Runtime, Buffer, 0x8000, Dir, true, OutFrame); };
	bool bFound = false;
	bool bChord = false;
	switch (Code)
	{
	case 0x80000032: bFound = Chord(0x80, 0x10) || Next(0x8000001E); break;
	case 0x80000033: bFound = Chord(0x20, 0x80) || Next(0x8000001B); break;
	case 0x80000039: bFound = Chord(0x40, 0x20) || Next(0x8000001D); break;
	case 0x80000014: bFound = Chord(0x40, 0x10); bChord = true; break;
	case 0x80000015: bFound = Chord(0x40, 0x20); bChord = true; break;
	case 0x80000016: bFound = Chord(0x40, 0x80); bChord = true; break;
	case 0x80000017: bFound = Chord(0x10, 0x20); bChord = true; break;
	case 0x80000018: bFound = Chord(0x10, 0x80); bChord = true; break;
	case 0x80000019: bFound = Chord(0x20, 0x80); bChord = true; break;
	case 0x80000034: bFound = Chord(0x40, 0x8000); bChord = true; break;
	case 0x80000035: bFound = Chord(0x10, 0x8000); bChord = true; break;
	case 0x80000036: bFound = Chord(0x20, 0x8000); bChord = true; break;
	case 0x80000037: bFound = Chord(0x80, 0x8000); bChord = true; break;
	case 0x80000038: bFound = Chord(0x4000, 0x8000); bChord = true; break;
	case 0x80000005: bFound = WithDir(0x80000001); bChord = true; break;
	case 0x80000006: bFound = WithDir(0x80000002); bChord = true; break;
	case 0x80000007: bFound = WithDir(0x80000003); bChord = true; break;
	case 0x80000008: bFound = WithDir(0x80000004); bChord = true; break;
	case 0x80000001: case 0x80000002: case 0x80000003: case 0x80000004:
		// [unk] A direction waiting in the direction ring (FUN_00451BA0 -> FUN_00426160) isn't decoded yet.
		bFound = false;
		break;
	default: bFound = Next(Code); break;
	}
	if (bFound && bConsume)
	{
		// FUN_00492500: a chord took two presses, so it's consumed twice.
		OKInput::Consume(Runtime, Buffer);
		if (bChord)
		{
			OKInput::Consume(Runtime, Buffer);
		}
	}
	return bFound;
}

namespace
{
	/** A command's direction sequence: up to 10 codes, ended by -1 or 0. */
	TArray<uint32> ReadSteps(FOKScriptRuntime& Runtime, uint32 Sequence)
	{
		TArray<uint32> Steps;
		for (int32 i = 0; i < 10; ++i)
		{
			uint32 Step = 0;
			if (!Runtime.LoadAnyWord(Sequence + i * 4, Step) || Step == 0xFFFFFFFFu || Step == 0)
			{
				break;
			}
			Steps.Add(Step);
		}
		return Steps;
	}
}

bool OKPlayerInput::MotionAt(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Window, uint32 Sequence, uint32 Frame, uint32 Late, uint32* OutOldest)
{
	if (!Player.Fighter || Sequence == 0)
	{
		return false;
	}
	uint32 Towards = 0, Away = 0, MinGap = 0;
	FacingMasks(*Player.Fighter, Towards, Away);
	Runtime.LoadHeapWord(Player.Fighter->Handle + 0x218, MinGap);
	return OKInput::MatchMotionAt(Runtime, BufferOf(Runtime, Player), Window, ReadSteps(Runtime, Sequence), Frame, Late, MinGap, Towards, Away, OutOldest);
}

bool OKPlayerInput::TwoInOne(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code, bool bAllowRelease, uint32 Sequence, uint32 Window,
	uint32 Late, uint32 MaxAge)
{
	uint32 Scanning = 0;
	Runtime.LoadHeapWord(Player.Handle + FOKPlayer::Scanning, Scanning);
	bAllowRelease &= Scanning != 1;
	const uint32 Now = uint32(Runtime.GetSimFrame());
	auto Recent = [&](uint32 Oldest) { return MaxAge == 0 || float(Now - Oldest) < float(MaxAge); };

	// The press itself, left in the buffer for the move to take.
	uint32 Frame = 0, Oldest = 0;
	if (Scanning != 2 && TryPress(Runtime, Player, Code, &Frame, false) && MotionAt(Runtime, Player, Window, Sequence, Frame, Late, &Oldest))
	{
		if (Recent(Oldest))
		{
			return true;
		}
	}
	// Or the button let go (FUN_00451C10), with a 6-frame window. [inf] Chords and the codes that test a direction
	// with block have their own release tests (FUN_00425D20), not ported; those find nothing here.
	const bool bDirection = Code == 0x80000001 || Code == 0x80000002 || Code == 0x80000003 || Code == 0x80000004;
	const uint32 Mask = OKInput::CodeToMask(Code);
	const bool bSingle = !bDirection && (Mask == 0x40 || Mask == 0x10 || Mask == 0x20 || Mask == 0x80 || Mask == 0x8000 || Mask == 0x2000 || Mask == 0x4000 || Mask == 0x1000)
		&& OKInput::MaskToCode(Mask) == Code;
	const uint32 Buffer = BufferOf(Runtime, Player);
	Frame = bSingle ? OKInput::LastRelease(Runtime, Buffer, Mask) : 0;
	if (!bAllowRelease || Frame == 0 || !MotionAt(Runtime, Player, 6, Sequence, Frame, Late, &Oldest) || !Recent(Oldest))
	{
		return false;
	}
	OKInput::ClearReleases(Runtime, Buffer);
	return true;
}

bool OKPlayerInput::Motion(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Window, uint32 Sequence)
{
	if (!Player.Fighter || Sequence == 0)
	{
		return false;
	}
	TArray<uint32> Steps;
	for (int32 i = 0; i < 10; ++i)
	{
		uint32 Step = 0;
		if (!Runtime.LoadAnyWord(Sequence + i * 4, Step) || Step == 0xFFFFFFFFu || Step == 0)
		{
			break;
		}
		Steps.Add(Step);
	}
	uint32 Towards = 0, Away = 0, MinGap = 0;
	FacingMasks(*Player.Fighter, Towards, Away);
	Runtime.LoadHeapWord(Player.Fighter->Handle + 0x218, MinGap);
	// FUN_00892150 scales the window in tower speed modes (fighter +0x21C); 0 is normal speed.
	return OKInput::MatchMotion(Runtime, BufferOf(Runtime, Player), Window, Steps, uint32(Runtime.GetSimFrame()), MinGap, Towards, Away);
}
