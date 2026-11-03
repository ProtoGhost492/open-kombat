#pragma once

#include "CoreMinimal.h"

class FOKScriptRuntime;
class FOKFighter;
class FOKPlayer;

/**
 * MKKE's input buffer (FIGHT-RE-NOTES §6.4): one per controller, 0x60C bytes, in the runtime's heap with MKKE's
 * layout. A player record points at its buffer from +0xB40. Each frame the poll (FUN_00424A50) rebuilds the held
 * mask and records each newly pressed mask, with the frame counter, in one of two 30-entry rings: directions in
 * one, buttons in the other.
 *
 * Masks: 1 up, 2 down, 4 left, 8 right; 0x40 FP, 0x10 BP, 0x20 FK, 0x80 BK, 0x8000 block, 0x2000 throw,
 * 0x4000 flip stance, 0x1000 tag (FUN_00425F00 maps input codes to these).
 */
namespace OKInput
{
	constexpr int32 Bytes = 0x60C;
	constexpr uint32 Owner = 0x00;			// the player record
	constexpr uint32 State = 0x08;			// byte: & 0xC9 = connected
	constexpr uint32 Held = 0x0C;			// masks held this frame
	constexpr uint32 PreviousHeld = 0x18;	// and last frame
	constexpr uint32 PressHead = 0x54;		// byte: newest entry of the press ring
	constexpr uint32 DirHead = 0x55;		// byte: newest entry of the direction ring
	constexpr uint32 Presses = 0x58;		// 30 x {mask, frame, direction code, flag}
	constexpr uint32 Directions = 0x238;	// 30 x {mask, frame, ?, flag}
	constexpr uint32 ReleaseHead = 0x56;	// byte: newest entry of the release ring
	constexpr uint32 Releases = 0x418;		// 30 x {mask, frame, direction code, flag}: buttons let go (FUN_004256A0)
	constexpr uint32 Cursor = 0x5F8;		// byte: the last press read (FUN_00425E40 / FUN_00425A00)
	constexpr int32 RingSize = 30;
	constexpr int32 EntryBytes = 0x10;

	/** An input code (0x8000000F FP ...) as a buffer mask (FUN_00425F00); 0 if it has none. */
	uint32 CodeToMask(uint32 Code);
	/** A buffer mask as an input code (FUN_00426010). */
	uint32 MaskToCode(uint32 Mask);

	/** Makes a buffer for a player and points player +0xB40 at it. */
	uint32 CreateBuffer(FOKScriptRuntime& Runtime, FOKPlayer& Player);

	/** One frame of the poll (FUN_00424A50): the held mask becomes Held, and each new press is recorded (FUN_00425770). */
	void Poll(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 HeldMask, uint32 Frame, TFunctionRef<uint32()> DirectionNow);

	/** Held now (FUN_00425480), pressed this frame (FUN_00425510). */
	bool IsHeld(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask);
	bool JustPressed(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask);

	/** The next unread press, if it's Mask (and Direction, when bUseDirection) (FUN_00425E40). */
	bool NextPress(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask, uint32 Direction, bool bUseDirection, uint32* OutFrame);
	/** Two presses in a row within 7 frames, either order: a chord typed one button at a time (FUN_00425D80). */
	bool NextChord(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 MaskA, uint32 MaskB, uint32 Direction, bool bUseDirection, uint32* OutFrame);
	/** Drops every waiting press: the cursor catches up with the newest (FUN_004259B0). */
	void Flush(FOKScriptRuntime& Runtime, uint32 Buffer);
	/** The frame a button mask was last let go, or 0 (FUN_00425B50). */
	uint32 LastRelease(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Mask);
	/** Forgets the releases: each entry's frame becomes 0 (FUN_00425980). */
	void ClearReleases(FOKScriptRuntime& Runtime, uint32 Buffer);
	/** Marks the next press read (FUN_00425A00). */
	void Consume(FOKScriptRuntime& Runtime, uint32 Buffer);
	/** The newest press as {code, direction} (FUN_00426EC0). */
	bool LastPress(FOKScriptRuntime& Runtime, uint32 Buffer, uint32& OutCode, uint32& OutDirection);
	/** Both masks were pressed within Window frames of now (FUN_00425CA0). */
	bool PressedWithin(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 MaskA, uint32 MaskB, uint32 Window, uint32 Now);
	/**
	 * A motion (Towards, Away, Up, Down codes, -1 terminated, up to 10) against the direction ring, newest first
	 * (MatchInputHistory, FUN_00426240): every step within Window frames of now and MinGap apart.
	 */
	bool MatchMotion(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Window, const TArray<uint32>& Steps, uint32 Now, uint32 MinGap, uint32 TowardsMask, uint32 AwayMask);
	/**
	 * The same measured from a press at frame Now (FUN_00426690): a step may also come up to Late frames after it,
	 * and OutOldest gets the frame of the motion's first step.
	 */
	bool MatchMotionAt(FOKScriptRuntime& Runtime, uint32 Buffer, uint32 Window, const TArray<uint32>& Steps, uint32 Now, uint32 Late, uint32 MinGap,
		uint32 TowardsMask, uint32 AwayMask, uint32* OutOldest);
}

/**
 * A player record's input tests (FOKPlayer virtuals and helpers), which the command matchers, the button
 * dispatch and the input natives use. They read the player's buffer and its fighter's facing.
 */
namespace OKPlayerInput
{
	/** Towards and away as buffer masks: 8 and 4 when the fighter faces right (FUN_004518D0). */
	void FacingMasks(const FOKFighter& Fighter, uint32& OutTowards, uint32& OutAway);
	/** Held (FUN_00446910) and pressed this frame (FUN_004468D0); both are off while the player can't act (+0x944). */
	bool Held(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code);
	bool JustPressed(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code);
	/** Player virtual +0x14 (FUN_00452550): a command button or chord pressed this frame. */
	bool Pressed(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code);
	/** Player virtual +0x1C (FUN_00453030): held, with walking, crouching and jumping excluding each other. */
	bool HeldCommand(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code);
	/** Player virtual +0x34 (FUN_00893200): the direction held as a code (Towards/Away/Up/Down, 0x80000000 none). */
	uint32 Direction(FOKScriptRuntime& Runtime, const FOKPlayer& Player);
	/** A press waiting in the buffer (FUN_00451FB0), consumed when bConsume; native 78.67. */
	bool TryPress(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code, uint32* OutFrame, bool bConsume);
	/** A motion for a command record: its sequence within Window (FUN_00451940 -> FUN_00427010). */
	bool Motion(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Window, uint32 Sequence);
	/** The same, measured from a press at Frame (FUN_004519D0 -> FUN_00426690). */
	bool MotionAt(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Window, uint32 Sequence, uint32 Frame, uint32 Late, uint32* OutOldest);
	/**
	 * A 2-in-1 cancel's input (FUN_00455AF0, native 78.86): the button waiting in the buffer, or with bAllowRelease
	 * its last release, with the motion entered around it; MaxAge, when not 0, limits how old the motion may be.
	 */
	bool TwoInOne(FOKScriptRuntime& Runtime, const FOKPlayer& Player, uint32 Code, bool bAllowRelease, uint32 Sequence, uint32 Window, uint32 Late, uint32 MaxAge);
}
