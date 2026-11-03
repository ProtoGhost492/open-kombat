#pragma once

#include "CoreMinimal.h"

/**
 * The fight HUD's data. A game fills an FOKHudFrame from its own state each frame and AOKFightHUD draws it, so the
 * drawing doesn't depend on where a game keeps its life bars. Scripts add their own texts and sprites through an
 * FOKHudOverlay (MKKE's render objects, module 15).
 */
namespace OKHud
{
	// The HUD's screen: MKKE places HUD objects in 1280 x 720 pixels (7.154 / 7.155 return its size, and 7.157
	// scales a text size by height / 720), whatever the real resolution.
	constexpr float Width = 1280.0f;
	constexpr float Height = 720.0f;
}

/** A text or sprite the scripts put on screen: MKKE's render object (0xEC bytes, FUN_00B90420). */
struct FOKScreenObject
{
	int32 Id = 0;					// +0x04: what the setters and 15.0 find it by
	int32 Layer = 0;				// +0x08
	FString Text;					// a text's text; empty for a sprite
	FString Texture;				// a sprite's texture, by path ("FightingArt.Textures.FinishHim"); a text's font
	FVector2D Position = FVector2D::ZeroVector;	// +0x34: its centre in HUD pixels; 15.20 also sets the copy at +0x44
	FVector2D Scale = FVector2D(1.0, 1.0);		// +0x20 / +0x24
	float Alpha = 1.0f;				// +0x1C; 15.13 sets it from 0..255
	float Rotation = 0.0f;			// +0x28, degrees
};

/** A message the HUD shows for a while. */
struct FOKHudBanner
{
	enum class EKind : uint8 { Round, Fight, Combo, Message };

	EKind Kind = EKind::Round;
	int32 Value = 0;				// Round: its number; Combo: the hits
	float Percent = 0.0f;			// Combo: the damage in percent of the life bar
	FString Text;					// Message: what it says ("ESCAPE")
	int32 Side = -1;				// -1: the centre; 0 / 1: that player's half
	int64 Start = 0;				// the sim frames it shows in
	int64 Until = 0;
};

/** What the scripts have put on screen. The script runtime keeps one; the HUD draws it. */
class OPENKOMBAT_API FOKHudOverlay
{
public:
	/** A new object. MKKE adds to its list without checking the id, and the setters change the first match. */
	FOKScreenObject& Add(int32 Id, int32 Layer);
	FOKScreenObject* Find(int32 Id);
	/** Removes every object with this id (15.0, FUN_00B92130). */
	void Free(int32 Id);

	/** Shows a banner, replacing any of the same kind on the same side. */
	void ShowBanner(const FOKHudBanner& Banner);

	const TArray<FOKScreenObject>& GetObjects() const { return Objects; }
	const TArray<FOKHudBanner>& GetBanners() const { return Banners; }

private:
	TArray<FOKScreenObject> Objects;
	TArray<FOKHudBanner> Banners;
};

/** One side of the fight as the HUD shows it. */
struct FOKHudSide
{
	FString Name;
	float Life = 1.0f;				// 0 to 1
	float Meter = 0.0f;				// 0 to 1
	int32 MeterBars = 3;			// the meter's segments
	int32 RoundsWon = 0;
	bool bPresent = false;
};

/** Everything the HUD draws in a frame. */
struct FOKHudFrame
{
	FOKHudSide Sides[2];
	int32 Clock = -1;				// seconds left; -1 = no clock
	int32 RoundsToWin = 2;
	bool bVisible = false;			// false before the fight starts
	int64 SimFrame = 0;
	const FOKHudOverlay* Overlay = nullptr;
};
