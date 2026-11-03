#include "OKFightCamera.h"
#include "OKMkoFile.h"
#include "OKScriptObjects.h"
#include "OKScriptRuntime.h"

namespace
{
	// [design] The framing. MKKE's distances (variables 1 and 2, 400 and 600) belong to its own lens; ours keeps the
	// near framing OpenKombat had (650 units back, 45 degrees across) and MKKE's 600 / 400 ratio for the far one.
	constexpr double NearDistance = 650.0;
	constexpr double FarDistance = NearDistance * 600.0 / 400.0;
	constexpr double FramingMargin = 150.0;		// past each fighter, at the sides of the picture
	constexpr double EdgeInset = 100.0;			// the focus stays this far inside the arena's ends
	constexpr double EyeHeight = 150.0;
	constexpr double LookHeight = 110.0;
	constexpr float Degrees = 45.0f;

	// MKKE's rates (FUN_00876F40 for the eye, FUN_0086BB70 for the view): a fraction of the way to the goal each frame,
	// more the further the view is off its goal, up to variable 60's angle. The eye travels slowly (variable 52, plus 55
	// at full angle; down variable 54, up 53, so a jump lifts it slowly) while the view turns faster (56, plus 59).
	constexpr int32 TravelRate = 52;
	constexpr int32 TravelRateOff = 55;
	constexpr int32 RiseRate = 53;
	constexpr int32 FallRate = 54;
	constexpr int32 TurnRate = 56;
	constexpr int32 TurnRateOff = 59;
	constexpr int32 OffAngle = 60;

	float BitsToFloat(uint32 Bits)
	{
		float Value;
		FMemory::Memcpy(&Value, &Bits, 4);
		return Value;
	}
}

uint32 FOKFightCamera::Create(FOKScriptRuntime& Runtime, const FOKMkoFile* CameraScript)
{
	Handle = Runtime.CreateOwned(MakeUnique<FOKCameraObject>(), FOKCameraObject::Bytes);
	const FOKMkoGlobal* Defaults = nullptr;
	if (CameraScript)
	{
		const uint32 Hash = FOKMkoFile::Fnv1(TEXT("CameraVariables"));
		Defaults = CameraScript->GetGlobals().FindByPredicate([Hash](const FOKMkoGlobal& Global) { return Global.NameHash == Hash; });
	}
	const TConstArrayView<uint8> Pool = CameraScript ? CameraScript->GetPool() : TConstArrayView<uint8>();
	if (!Defaults || int64(Defaults->PoolOffset) + FOKCameraObject::NumVariables * 4 > Pool.Num())
	{
		UE_LOG(LogOKVM, Warning, TEXT("No CameraVariables in FGCameraEngine: the camera variables start at 0"));
		return Handle;
	}
	for (int32 i = 0; i < FOKCameraObject::NumVariables; ++i)
	{
		uint32 Word = 0;
		FMemory::Memcpy(&Word, Pool.GetData() + Defaults->PoolOffset + i * 4, 4);
		Runtime.SetHeapWord(Handle + FOKCameraObject::Variables + i * 4, Word);
	}
	return Handle;
}

float FOKFightCamera::Variable(const FOKScriptRuntime& Runtime, int32 Index) const
{
	uint32 Bits = 0;
	Runtime.LoadAnyWord(Handle + FOKCameraObject::Variables + Index * 4, Bits);
	return BitsToFloat(Bits);
}

void FOKFightCamera::Update(const FOKScriptRuntime& Runtime, const FInput& Input, bool bSnap)
{
	const FVector Right = FVector(Input.ScreenRight.X, Input.ScreenRight.Y, 0.0).GetSafeNormal();
	const FVector Forward = FVector::CrossProduct(Right, FVector::UpVector);		// from the camera towards the line
	const FVector Middle = (Input.Fighters[0] + Input.Fighters[1]) * 0.5;
	const double Separation = FMath::Abs(FVector::DotProduct(Input.Fighters[1] - Input.Fighters[0], Right));

	// The goal: centred between the fighters, inside the arena, far enough back to show both with a margin
	double Along = FVector::DotProduct(Middle - Input.LineStart, Right);
	if (Input.LineLength > 2.0 * EdgeInset)
	{
		Along = FMath::Clamp(Along, EdgeInset, Input.LineLength - EdgeInset);
	}
	const FVector GoalFocus = Middle + Right * (Along - FVector::DotProduct(Middle - Input.LineStart, Right));
	const double HalfWidth = FMath::Tan(FMath::DegreesToRadians(Degrees * 0.5));
	const double GoalDistance = FMath::Clamp((Separation * 0.5 + FramingMargin) / HalfWidth, NearDistance, FarDistance);

	const FVector GoalTarget = Middle + FVector(0.0, 0.0, LookHeight);
	if (bSnap || Distance <= 0.0)
	{
		Focus = GoalFocus;
		Distance = GoalDistance;
		Target = GoalTarget;
	}
	else
	{
		// How far the view is off its goal, as a share of variable 60's angle
		const FVector Looking = (Target - Eye).GetSafeNormal();
		const FVector Wanted = (GoalTarget - Eye).GetSafeNormal();
		const double Limit = FMath::Max(1.0f, Variable(Runtime, OffAngle));
		const double Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Looking, Wanted), -1.0, 1.0)));
		const double Off = FMath::Min(Angle, Limit) / Limit;
		const double Travel = FMath::Clamp(Variable(Runtime, TravelRate) + Off * Variable(Runtime, TravelRateOff), 0.0, 1.0);
		const double Turn = FMath::Clamp(Variable(Runtime, TurnRate) + Off * Variable(Runtime, TurnRateOff), 0.0, 1.0);
		const FVector Step = GoalFocus - Focus;
		const double Vertical = FMath::Clamp(Variable(Runtime, Step.Z > 0.0 ? RiseRate : FallRate), 0.0f, 1.0f);
		Focus += FVector(Step.X * Travel, Step.Y * Travel, Step.Z * Vertical);
		Distance += (GoalDistance - Distance) * Travel;
		Target += (GoalTarget - Target) * Turn;
	}

	Eye = Focus - Forward * Distance + FVector(0.0, 0.0, EyeHeight);
	Rotation = (Target - Eye).Rotation();
	FieldOfView = Degrees;

	// The shake scripts put on the camera object (CameraEngine.p_screen_shaker -> 149.23): a move of the eye, and a turn
	// in UE3 rotator units (65536 a turn) [inf], as MKKE's tick adds them (FUN_008789D0)
	uint32 Words[6] = {};
	for (int32 i = 0; i < 3; ++i)
	{
		Runtime.LoadAnyWord(Handle + FOKCameraObject::ShakePosition + i * 4, Words[i]);
		Runtime.LoadAnyWord(Handle + FOKCameraObject::ShakeRotation + i * 4, Words[3 + i]);
	}
	Eye += FVector(BitsToFloat(Words[0]), BitsToFloat(Words[1]), BitsToFloat(Words[2]));
	const double Unit = 360.0 / 65536.0;
	Rotation += FRotator(double(int32(Words[3])) * Unit, double(int32(Words[4])) * Unit, double(int32(Words[5])) * Unit);
}
