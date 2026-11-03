#include "OKFightHUD.h"
#include "OKScriptWorld.h"
#include "OKTexture.h"
#include "OKLocalization.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "Algo/StableSort.h"

static TAutoConsoleVariable<int32> CVarHudShow(
	TEXT("ok.Hud.Show"),
	1,
	TEXT("1 draws the fight HUD, 0 hides it."));

static TAutoConsoleVariable<FString> CVarHudPackage(
	TEXT("ok.Hud.Package"),
	TEXT("Startup"),
	TEXT("The package the HUD's textures come from: MKKE keeps FightingArt.Textures in Startup.xxx."));

namespace
{
	const TCHAR* const ArtPrefix = TEXT("FightingArt.Textures.");

	// The layout, in HUD pixels. A life bar is MKKE's 640-pixel strip (a 64-pixel cap, the 512-pixel middle and a 64-pixel
	// cap) squeezed to BarWidth; player 1's runs from the left edge to the clock, player 2's mirrors it.
	constexpr float BarStrip = 640.0f;
	constexpr float BarCap = 64.0f;
	constexpr float BarWidth = 540.0f;
	constexpr float BarHeight = 32.0f;
	constexpr float BarTop = 28.0f;
	constexpr float BarOuter = 40.0f;		// the gap between a bar and the screen's side
	constexpr float ClockSize = 112.0f;
	const FVector2D ClockCentre(640.0f, 44.0f);
	constexpr float MeterWidth = 400.0f;	// SuperLoad_Bracket (512 x 128) at this width
	constexpr float MeterBottom = 704.0f;

	const FLinearColor TextGold(1.0f, 0.82f, 0.45f);
	const FLinearColor TextWhite(1.0f, 1.0f, 1.0f);

	/** Rises over the first frames of [Start, Until) and falls over the last ones. */
	float FadeInOut(int64 Frame, int64 Start, int64 Until, int64 Frames)
	{
		const float In = float(Frame - Start) / float(Frames);
		const float Out = float(Until - Frame) / float(Frames);
		return FMath::Clamp(FMath::Min(In, Out), 0.0f, 1.0f);
	}
}

void AOKFightHUD::DrawHUD()
{
	Super::DrawHUD();
	const UOKScriptWorld* Scripts = GetWorld() ? GetWorld()->GetSubsystem<UOKScriptWorld>() : nullptr;
	if (!Canvas || !Scripts || CVarHudShow.GetValueOnGameThread() == 0)
	{
		return;
	}
	FOKHudFrame Frame;
	Scripts->GetHudFrame(Frame);
	if (!Frame.bVisible)
	{
		return;
	}
	CanvasScale = Canvas->ClipY / OKHud::Height;
	CanvasOffset = FVector2D((Canvas->ClipX - OKHud::Width * CanvasScale) * 0.5f, 0.0f);

	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (Frame.Sides[Index].bPresent)
		{
			DrawLifeBar(Frame.Sides[Index], Index);
			DrawRoundsWon(Frame.Sides[Index], Index, Frame.RoundsToWin);
			DrawMeter(Frame.Sides[Index], Index);
		}
	}
	DrawClock(Frame.Clock);
	if (Frame.Overlay)
	{
		for (const FOKHudBanner& Banner : Frame.Overlay->GetBanners())
		{
			if (Frame.SimFrame >= Banner.Start && Frame.SimFrame < Banner.Until)
			{
				DrawBanner(Banner, Frame.SimFrame);
			}
		}
		// MKKE keeps its render objects in one list; [inf] higher layers drawn on top
		TArray<const FOKScreenObject*> Objects;
		for (const FOKScreenObject& Object : Frame.Overlay->GetObjects())
		{
			Objects.Add(&Object);
		}
		Algo::StableSortBy(Objects, [](const FOKScreenObject* Object) { return Object->Layer; });
		for (const FOKScreenObject* Object : Objects)
		{
			DrawScreenObject(*Object);
		}
	}
}

UTexture2D* AOKFightHUD::FindTexture(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	if (const TObjectPtr<UTexture2D>* Found = Textures.Find(Path))
	{
		return *Found;
	}
	if (!bTriedArtPackage)
	{
		bTriedArtPackage = true;
		FString Error;
		ArtPackage = FOKPackage::OpenGamePackage(CVarHudPackage.GetValueOnGameThread(), Error);
		if (!ArtPackage)
		{
			UE_LOG(LogOKAsset, Warning, TEXT("No HUD art: %s"), *Error);
		}
	}
	UTexture2D* Texture = nullptr;
	FString Error = TEXT("not in the HUD package");
	if (const FOKPackageExport* Export = ArtPackage ? ArtPackage->FindExportByPath(Path) : nullptr)
	{
		FOKTextureData Data;
		Texture = Data.Read(*ArtPackage, Export->Index, Error) ? Data.CreateTexture(true, Error) : nullptr;
	}
	if (!Texture)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("HUD texture %s: %s"), *Path, *Error);
	}
	Textures.Add(Path, Texture);
	return Texture;
}

void AOKFightHUD::DrawPicture(UTexture2D* Texture, const FVector2D& TopLeft, const FVector2D& Size, float Alpha,
	const FVector2D& UV0, const FVector2D& UV1, float Degrees)
{
	if (!Texture || !Texture->GetResource() || Alpha <= 0.0f)
	{
		return;
	}
	FCanvasTileItem Tile(ToCanvas(TopLeft), Texture->GetResource(), Size * CanvasScale, UV0, UV1, FLinearColor(1.0f, 1.0f, 1.0f, Alpha));
	Tile.BlendMode = SE_BLEND_Translucent;
	if (Degrees != 0.0f)
	{
		Tile.Rotation = FRotator(0.0, Degrees, 0.0);
		Tile.PivotPoint = FVector2D(0.5, 0.5);
	}
	Canvas->DrawItem(Tile);
}

void AOKFightHUD::DrawPictureCentred(UTexture2D* Texture, const FVector2D& Centre, const FVector2D& Scale, float Alpha, float Degrees)
{
	if (Texture)
	{
		const FVector2D Size = FVector2D(Texture->GetSizeX(), Texture->GetSizeY()) * Scale;
		DrawPicture(Texture, Centre - Size * 0.5, Size, Alpha, FVector2D(0.0, 0.0), FVector2D(1.0, 1.0), Degrees);
	}
}

void AOKFightHUD::DrawLabel(const FString& Text, const FVector2D& Point, float Size, const FLinearColor& Color, float Align)
{
	UFont* Font = GEngine ? GEngine->GetLargeFont() : nullptr;
	if (!Font || Text.IsEmpty() || Color.A <= 0.0f)
	{
		return;
	}
	const float Scale = Size * CanvasScale / FMath::Max(1.0f, Font->GetMaxCharHeight());
	float Width = 0.0f, Height = 0.0f;
	Canvas->TextSize(Font, Text, Width, Height, Scale, Scale);
	FCanvasTextItem Label(ToCanvas(Point) - FVector2D(Width * Align, 0.0f), FText::FromString(Text), Font, Color);
	Label.Scale = FVector2D(Scale, Scale);
	Label.bCentreY = true;
	Label.bOutlined = true;
	Label.OutlineColor = FLinearColor(0.0f, 0.0f, 0.0f, Color.A);
	Label.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Label);
}

void AOKFightHUD::DrawLifeBar(const FOKHudSide& Side, int32 Index)
{
	// Each piece covers part of the 640-pixel strip, measured from the bar's outer end; the health pieces show up to
	// Life of it, so the bar drains towards the screen's side.
	struct FPiece { const TCHAR* Name; float From; float To; };
	static const FPiece Backing[] = { { TEXT("TopBarBacking_Mid"), 0.0f, BarStrip - BarCap }, { TEXT("TopBarBacking_End"), BarStrip - BarCap, BarStrip } };
	static const FPiece Health[] = { { TEXT("TopBar_HealthStart"), 0.0f, BarCap }, { TEXT("TopBar_HealthMid"), BarCap, BarStrip - BarCap },
		{ TEXT("TopBar_HealthEnd"), BarStrip - BarCap, BarStrip } };
	static const FPiece Frame[] = { { TEXT("TopBar_Start"), 0.0f, BarCap }, { TEXT("TopBar_Mid"), BarCap, BarStrip - BarCap },
		{ TEXT("TopBar_End"), BarStrip - BarCap, BarStrip } };
	const bool bMirror = Index == 1;
	const float Squeeze = BarWidth / BarStrip;
	auto DrawPieces = [&](const FPiece* Pieces, int32 Count, float Shown)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			const FPiece& Piece = Pieces[i];
			const float To = FMath::Min(Piece.To, Shown);
			if (To <= Piece.From)
			{
				continue;
			}
			const float Part = (To - Piece.From) / (Piece.To - Piece.From);
			const float Width = (To - Piece.From) * Squeeze;
			const float Along = BarOuter + Piece.From * Squeeze;
			const float Left = bMirror ? OKHud::Width - Along - Width : Along;
			// The art is player 2's bar: each piece runs from its inner end (u = 0; the slanted _End pieces) to its outer
			// end (u = 1; the health nub fits the frame's notch there). The part shown is the outer one, and player 1's
			// bar is the same flipped.
			const FVector2D UV0(bMirror ? 1.0f - Part : 1.0f, 0.0f);
			const FVector2D UV1(bMirror ? 1.0f : 1.0f - Part, 1.0f);
			DrawPicture(FindTexture(FString(ArtPrefix) + Piece.Name), FVector2D(Left, BarTop), FVector2D(Width, BarHeight), 1.0f, UV0, UV1);
		}
	};
	DrawPieces(Backing, UE_ARRAY_COUNT(Backing), BarStrip);
	DrawPieces(Health, UE_ARRAY_COUNT(Health), Side.Life * BarStrip);
	DrawPieces(Frame, UE_ARRAY_COUNT(Frame), BarStrip);

	const float NameX = bMirror ? OKHud::Width - BarOuter - 12.0f : BarOuter + 12.0f;
	DrawLabel(Side.Name, FVector2D(NameX, BarTop + BarHeight + 16.0f), 20.0f, TextWhite, bMirror ? 1.0f : 0.0f);
}

void AOKFightHUD::DrawRoundsWon(const FOKHudSide& Side, int32 Index, int32 RoundsToWin)
{
	// A skull per round won, from the clock outwards; the ones still to win dim
	UTexture2D* Skull = FindTexture(FString(ArtPrefix) + TEXT("skull"));
	for (int32 Round = 0; Round < RoundsToWin; ++Round)
	{
		const float Along = BarOuter + BarWidth - 30.0f - Round * 28.0f;
		const float X = Index == 1 ? OKHud::Width - Along : Along;
		DrawPictureCentred(Skull, FVector2D(X, BarTop + BarHeight + 16.0f), FVector2D(0.75, 0.75), Round < Side.RoundsWon ? 1.0f : 0.25f);
	}
}

void AOKFightHUD::DrawClock(int32 Seconds)
{
	// ClockFrame's dial is centred at (63, 45) of its 128 pixels
	UTexture2D* Frame = FindTexture(FString(ArtPrefix) + TEXT("ClockFrame"));
	const FVector2D TopLeft = ClockCentre - FVector2D(63.0f, 45.0f) * (ClockSize / 128.0f);
	DrawPicture(Frame, TopLeft, FVector2D(ClockSize, ClockSize), 1.0f);
	DrawLabel(Seconds >= 0 ? FString::FromInt(Seconds) : FString(TEXT("--")), ClockCentre, 34.0f, TextWhite);
}

void AOKFightHUD::DrawMeter(const FOKHudSide& Side, int32 Index)
{
	// SuperLoad_Bracket: a dial at the outer end and a rail; the fill runs along the rail (u 0.21..0.69, v 0.42..0.58)
	const bool bMirror = Index == 1;
	const float Scale = MeterWidth / 512.0f;
	const FVector2D Size(MeterWidth, 128.0f * Scale);
	const float Left = bMirror ? OKHud::Width - 20.0f - Size.X : 20.0f;
	const float Top = MeterBottom - Size.Y;
	const float RailFrom = 0.21f, RailTo = 0.69f;
	const float Fill = FMath::Clamp(Side.Meter, 0.0f, 1.0f);
	if (Fill > 0.0f)
	{
		const float Width = (RailTo - RailFrom) * Size.X * Fill;
		const float Along = RailFrom * Size.X;
		const float X = bMirror ? Left + Size.X - Along - Width : Left + Along;
		const FVector2D UV0(bMirror ? 1.0f : 0.0f, 0.0f), UV1(bMirror ? 0.0f : 1.0f, 1.0f);
		DrawPicture(FindTexture(FString(ArtPrefix) + TEXT("SuperMeter_Fill")), FVector2D(X, Top + 0.42f * Size.Y), FVector2D(Width, 0.16f * Size.Y), 1.0f, UV0, UV1);
	}
	DrawPicture(FindTexture(FString(ArtPrefix) + TEXT("SuperLoad_Bracket")), FVector2D(Left, Top), Size, 1.0f,
		FVector2D(bMirror ? 1.0f : 0.0f, 0.0f), FVector2D(bMirror ? 0.0f : 1.0f, 1.0f));
	// The bars filled, in the dial
	const int32 Bars = FMath::FloorToInt(Fill * Side.MeterBars + KINDA_SMALL_NUMBER);
	const float DialX = bMirror ? Left + Size.X - 0.17f * Size.X : Left + 0.17f * Size.X;
	DrawLabel(FString::FromInt(Bars), FVector2D(DialX, Top + 0.5f * Size.Y), 28.0f, Bars > 0 ? TextGold : TextWhite);
}

void AOKFightHUD::DrawBanner(const FOKHudBanner& Banner, int64 SimFrame)
{
	const float Alpha = FadeInOut(SimFrame, Banner.Start, Banner.Until, 8);
	const float Age = float(SimFrame - Banner.Start);
	switch (Banner.Kind)
	{
	case FOKHudBanner::EKind::Round:
	{
		const FVector2D Centre(640.0f, 300.0f);
		UTexture2D* Word = FindTexture(FString(ArtPrefix) + TEXT("Round"));
		UTexture2D* Number = Banner.Value >= 1 && Banner.Value <= 5 ? FindTexture(FString::Printf(TEXT("%sNumber_%d"), ArtPrefix, Banner.Value)) : nullptr;
		if (Word && Number)
		{
			DrawPictureCentred(Word, Centre - FVector2D(64.0f, 0.0f), FVector2D(1.0, 1.0), Alpha);
			DrawPictureCentred(Number, Centre + FVector2D(128.0f, 0.0f), FVector2D(1.0, 1.0), Alpha);
		}
		else
		{
			DrawLabel(FString::Printf(TEXT("ROUND %d"), Banner.Value), Centre, 72.0f, FLinearColor(TextGold.R, TextGold.G, TextGold.B, Alpha));
		}
		break;
	}
	case FOKHudBanner::EKind::Fight:
	{
		// It lands: from 1.4 times its size to its own over the first 8 frames
		const float Scale = FMath::Lerp(1.4f, 1.0f, FMath::Clamp(Age / 8.0f, 0.0f, 1.0f));
		if (UTexture2D* Word = FindTexture(FString(ArtPrefix) + TEXT("fight")))
		{
			DrawPictureCentred(Word, FVector2D(640.0f, 300.0f), FVector2D(Scale, Scale), Alpha);
		}
		else
		{
			DrawLabel(TEXT("FIGHT!"), FVector2D(640.0f, 300.0f), 96.0f * Scale, FLinearColor(1.0f, 0.3f, 0.1f, Alpha));
		}
		break;
	}
	case FOKHudBanner::EKind::Combo:
	{
		const FOKLocalization& Text = FOKLocalization::Get();
		const FString* Hits = Text.Find(TEXT("Gameplay.Fighting"), TEXT("Hits"));
		const FString* Damage = Text.Find(TEXT("Gameplay.Fighting"), TEXT("Damage"));
		const float X = Banner.Side == 1 ? 1100.0f : 180.0f;
		DrawLabel(FString::Printf(TEXT("%d %s"), Banner.Value, Hits ? **Hits : TEXT("HITS")), FVector2D(X, 200.0f), 44.0f,
			FLinearColor(TextGold.R, TextGold.G, TextGold.B, Alpha));
		DrawLabel(FString::Printf(TEXT("%.0f%% %s"), Banner.Percent, Damage ? **Damage : TEXT("Damage")), FVector2D(X, 240.0f), 24.0f,
			FLinearColor(1.0f, 1.0f, 1.0f, Alpha));
		break;
	}
	case FOKHudBanner::EKind::Message:
	{
		// It slides in from its side of the screen over the first 8 frames, under where a combo shows
		const float Slide = 1.0f - FMath::Clamp(Age / 8.0f, 0.0f, 1.0f);
		const float X = Banner.Side == 1 ? 1100.0f + 300.0f * Slide : 180.0f - 300.0f * Slide;
		DrawLabel(Banner.Text, FVector2D(X, 290.0f), 40.0f, FLinearColor(TextGold.R, TextGold.G, TextGold.B, Alpha));
		break;
	}
	}
}

void AOKFightHUD::DrawScreenObject(const FOKScreenObject& Object)
{
	if (!Object.Text.IsEmpty())
	{
		// [inf] MKKE's fonts (FightingArt.Textures.MKFont_*) aren't read yet: the engine's, about as large
		DrawLabel(Object.Text, Object.Position, 40.0f * Object.Scale.Y, FLinearColor(TextGold.R, TextGold.G, TextGold.B, Object.Alpha));
		return;
	}
	DrawPictureCentred(FindTexture(Object.Texture), Object.Position, Object.Scale, Object.Alpha, Object.Rotation);
}
