#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "OKHud.h"
#include "OKPackage.h"
#include "OKFightHUD.generated.h"

class UFont;
class UTexture2D;

/**
 * The fight HUD (system W7). MKKE draws its HUD with Scaleform; ours goes straight onto the canvas, with the textures
 * of MKKE's own HUD art (FightingArt.Textures in Startup.xxx) in a layout of our own on a 1280 x 720 screen: life bars
 * and names at the top, the clock between them, the rounds won under the bars, the super meter at the bottom, then
 * the banners and the objects scripts add. It draws whatever FOKHudFrame the game fills, so other games can use it.
 */
UCLASS()
class OPENKOMBAT_API AOKFightHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	/** A texture of the HUD art package, by path ("FightingArt.Textures.ClockFrame"); null if it has none. */
	UTexture2D* FindTexture(const FString& Path);

	/** HUD pixels to the canvas: the 1280 x 720 screen is scaled to the canvas height and centred. */
	FVector2D ToCanvas(const FVector2D& Point) const { return CanvasOffset + Point * CanvasScale; }

	/** Part of a texture (UV0..UV1) over a rectangle in HUD pixels, turned Degrees about its centre. */
	void DrawPicture(UTexture2D* Texture, const FVector2D& TopLeft, const FVector2D& Size, float Alpha,
		const FVector2D& UV0 = FVector2D(0.0, 0.0), const FVector2D& UV1 = FVector2D(1.0, 1.0), float Degrees = 0.0f);
	/** A whole texture centred on a point, at Scale times its own size. */
	void DrawPictureCentred(UTexture2D* Texture, const FVector2D& Centre, const FVector2D& Scale, float Alpha, float Degrees = 0.0f);
	/** Text Size HUD pixels high, centred on Point's height; Align 0 starts it at Point, 0.5 centres it, 1 ends it there. */
	void DrawLabel(const FString& Text, const FVector2D& Point, float Size, const FLinearColor& Color, float Align = 0.5f);

	void DrawLifeBar(const FOKHudSide& Side, int32 Index);
	void DrawRoundsWon(const FOKHudSide& Side, int32 Index, int32 RoundsToWin);
	void DrawClock(int32 Seconds);
	void DrawMeter(const FOKHudSide& Side, int32 Index);
	void DrawBanner(const FOKHudBanner& Banner, int64 SimFrame);
	void DrawScreenObject(const FOKScreenObject& Object);

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> Textures;		// by path; null for one the package doesn't have

	TUniquePtr<FOKPackage> ArtPackage;
	bool bTriedArtPackage = false;
	float CanvasScale = 1.0f;
	FVector2D CanvasOffset = FVector2D::ZeroVector;
};
