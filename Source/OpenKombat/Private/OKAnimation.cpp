#include "OKAnimation.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "Algo/BinarySearch.h"

namespace
{
	// Key encodings (MK-FORMAT-NOTES, "Animation", key encodings)
	constexpr uint8 KindRotPair = 0x12;		// rotation, two smallest-three keys per 12-byte record
	constexpr uint8 KindRot48 = 0x0A;		// rotation, 4 x 12 bits + u16 frame
	constexpr uint8 KindRotF32 = 0x02;		// rotation, 4 x f32 + u32 frame
	constexpr uint8 KindPosI16 = 0x08;		// translation, u16 frame + 3 x i16 (1/32 cm)
	constexpr uint8 KindPosPair = 0x10;		// translation, two packed keys per 12-byte record
	constexpr uint8 KindPosF32 = 0x00;		// translation, 3 x f32 + u32 frame
	constexpr uint8 KindScaleI16 = 0x04;	// scale, u16 frame + 3 x i16 (1/4096)
	constexpr uint8 KindAttachments = 0x1E; // named attachment targets after all keys (not used)

	/** Bytes of key data for N keys of a kind, or -1 for a kind we can't decode. */
	int64 KeyBytes(uint8 Kind, int32 N)
	{
		switch (Kind)
		{
		case KindRotPair: return int64((N + 1) / 2) * 12;
		case KindRot48: return int64(N) * 8;
		case KindRotF32: return int64(N) * 20;
		case KindPosI16: return int64(N) * 8;
		case KindPosPair: return int64((N + 1) / 2) * 12;
		case KindPosF32: return int64(N) * 16;
		case KindScaleI16: return int64(N) * 8;
		default: return -1;
		}
	}

	/** A signed 12-bit component, scaled so 2047 = 1. */
	float S12(uint32 V)
	{
		return float((V & 0x800) ? int32(V) - 4096 : int32(V)) / 2047.0f;
	}

	/** Smallest-three quaternion: three stored components, the largest rebuilt at Index (x, y, z, w order). */
	FVector4f SmallestThree(int32 Index, uint32 A, uint32 B, uint32 C)
	{
		const float Small[3] = { S12(A), S12(B), S12(C) };
		const float Largest = FMath::Sqrt(FMath::Max(0.0f, 1.0f - Small[0] * Small[0] - Small[1] * Small[1] - Small[2] * Small[2]));
		float Q[4];
		for (int32 i = 0, k = 0; i < 4; ++i)
		{
			Q[i] = (i == Index) ? Largest : Small[k++];
		}
		return FVector4f(Q[0], Q[1], Q[2], Q[3]);
	}

	/** A kind-0x10 key: x (signed 11 bits) | y (signed 10 bits) << 11 | z (signed 11 bits) << 21, in 1/32 cm. */
	FVector4f UnpackPos32(uint32 V)
	{
		const int32 X = V & 0x7FF;
		const int32 Y = (V >> 11) & 0x3FF;
		const int32 Z = V >> 21;
		return FVector4f(((X & 0x400) ? X - 0x800 : X) / 32.0f, ((Y & 0x200) ? Y - 0x400 : Y) / 32.0f, ((Z & 0x400) ? Z - 0x800 : Z) / 32.0f, 0.0f);
	}

	template <typename T>
	T Get(TConstArrayView<uint8> Data, int64 Offset)
	{
		T Value;
		FMemory::Memcpy(&Value, Data.GetData() + Offset, sizeof(T));
		return Value;
	}

	/** An ArrayProperty of FNames. */
	TArray<FString> ReadNameArray(const FOKProperty& Array, const TArray<FString>& Names)
	{
		TArray<FString> Out;
		FOKByteReader R(Array.Raw);
		const int32 Count = R.I32();
		bool bBad = false;
		for (int32 i = 0; i < Count && i < 10000 && !bBad && !R.HasError(); ++i)
		{
			Out.Add(OKReadName(R, Names, bBad));
		}
		return Out;
	}

	/** Reads an AnimSequence's properties; returns its name and where its native data starts */
	bool ReadSequenceHeader(FOKPackage& Package, int32 SequenceRef, TArray<uint8>& OutBytes, FString& OutName, int64& OutNativeStart)
	{
		const TArray<FOKPackageExport>& Exports = Package.GetExports();
		if (!Exports.IsValidIndex(SequenceRef - 1) || !Package.ExportData(Exports[SequenceRef - 1], OutBytes))
		{
			return false;
		}
		FOKByteReader R(OutBytes);
		FOKPropertyList Properties;
		if (!Properties.Read(R, Package.GetNames()))
		{
			return false;
		}
		OutName = Properties.GetText(TEXT("SequenceName"));
		if (OutName.IsEmpty())
		{
			OutName = Exports[SequenceRef - 1].Name;
		}
		OutNativeStart = R.Tell();
		return true;
	}

	/** Calls Visit(trackBones, sequenceRef) for every sequence of every AnimSet; stops when it returns true. */
	void ForEachSequence(FOKPackage& Package, TFunctionRef<bool(const TArray<FString>&, int32)> Visit)
	{
		for (const FOKPackageExport* Set : Package.FindExports(TEXT("AnimSet")))
		{
			TArray<uint8> Bytes;
			if (!Package.ExportData(*Set, Bytes))
			{
				continue;
			}
			FOKByteReader R(Bytes);
			FOKPropertyList Properties;
			if (!Properties.Read(R, Package.GetNames()))
			{
				continue;
			}
			const FOKProperty* Bones = Properties.Find(TEXT("TrackBoneNames"));
			const FOKProperty* Sequences = Properties.Find(TEXT("Sequences"));
			if (!Bones || !Sequences)
			{
				continue;
			}
			const TArray<FString> TrackBones = ReadNameArray(*Bones, Package.GetNames());
			for (const int32 SequenceRef : FOKPropertyList::ReadIntArray(*Sequences))
			{
				if (SequenceRef > 0 && Visit(TrackBones, SequenceRef))
				{
					return;
				}
			}
		}
	}
}

TArray<TPair<FString, int32>> FOKAnimSequence::List(FOKPackage& Package)
{
	TArray<TPair<FString, int32>> Out;
	ForEachSequence(Package, [&](const TArray<FString>&, int32 SequenceRef)
		{
			TArray<uint8> Bytes;
			FString Name;
			int64 NativeStart = 0;
			if (ReadSequenceHeader(Package, SequenceRef, Bytes, Name, NativeStart) && NativeStart + 12 <= Bytes.Num())
			{
				Out.Emplace(Name, Get<int32>(Bytes, NativeStart + 8));
			}
			return false;
		});
	return Out;
}

bool FOKAnimSequence::Find(FOKPackage& Package, const FString& SequenceName, FString& OutError)
{
	bool bFound = false;
	bool bDecoded = false;
	ForEachSequence(Package, [&](const TArray<FString>& InTrackBones, int32 SequenceRef)
		{
			TArray<uint8> Bytes;
			FString ThisName;
			int64 NativeStart = 0;
			if (!ReadSequenceHeader(Package, SequenceRef, Bytes, ThisName, NativeStart) || ThisName != SequenceName)
			{
				return false;
			}
			bFound = true;
			Name = ThisName;
			TrackBones = InTrackBones;
			const TConstArrayView<uint8> Native(Bytes.GetData() + NativeStart, int32(Bytes.Num() - NativeStart));
			// A header variant flags one extra entry in the last group's count (byte 21 with one group; 1 or 2); try the plain
			// layout first.
			const int64 FlagAt = Native.Num() >= 20 ? 21 + 4 * int64(FMath::Clamp(Get<int32>(Native, 16), 1, 4) - 1) : 0;
			bDecoded = Decode(Native, false, OutError)
				|| (FlagAt > 0 && Native.Num() > FlagAt + 3 && (Native[FlagAt] == 1 || Native[FlagAt] == 2) && Decode(Native, true, OutError));
			return true;
		});
	if (!bFound)
	{
		OutError = FString::Printf(TEXT("%s has no sequence called %s"), *Package.GetName(), *SequenceName);
	}
	else if (!bDecoded)
	{
		OutError = FString::Printf(TEXT("%s: %s"), *SequenceName, *OutError);
	}
	return bFound && bDecoded;
}

bool FOKAnimSequence::Decode(TConstArrayView<uint8> Nat, bool bFlaggedExtra, FString& OutError)
{
	Tracks.Reset();
	if (Nat.Num() < 24)
	{
		OutError = TEXT("native data too short");
		return false;
	}
	// Header: f32 (0.29), i32 in-memory size, i32 NumFrames, f32 length, i32 group count.
	NumFrames = Get<int32>(Nat, 8);
	Length = Get<float>(Nat, 12);
	const int32 GroupCount = Get<int32>(Nat, 16);
	if (GroupCount < 1 || GroupCount > 4)
	{
		OutError = TEXT("unrecognised header layout");
		return false;
	}

	// Which group each entry belongs to. One group: u16 entry count + u16 count of extra entries
	// whose u32 field holds their group. Several groups: an i32 entry count per group.
	const int64 Table = 20 + int64(GroupCount) * 4;
	TArray<int32> GroupOf;
	if (GroupCount == 1)
	{
		if (!bFlaggedExtra && Nat[21] != 0)
		{
			OutError = TEXT("unrecognised header layout");
			return false;
		}
		const int32 Count = bFlaggedExtra ? Nat[20] + 1 + Get<uint16>(Nat, 22) : Get<uint16>(Nat, 20) + Get<uint16>(Nat, 22);
		GroupOf.Init(INDEX_NONE, Count);
	}
	else
	{
		if (Table > Nat.Num())
		{
			OutError = TEXT("unrecognised header layout");
			return false;
		}
		for (int32 Group = 0; Group < GroupCount; ++Group)
		{
			// Two-player sequences flag the attachment block on the last group, as the one-group variant does.
			const int64 At = 20 + int64(Group) * 4;
			const int32 Count = (bFlaggedExtra && Group == GroupCount - 1) ? Nat[At] + 1 + Get<uint16>(Nat, At + 2) : Get<int32>(Nat, At);
			if (Count < 0 || Count > 0x10000)
			{
				OutError = TEXT("unrecognised header layout");
				return false;
			}
			for (int32 k = 0; k < Count; ++k)
			{
				GroupOf.Add(Group);
			}
		}
	}

	// 8-byte entries: u8 track, u8 kind, u16 key count (| 0x8000: more entries follow for this track), u32 group.
	struct FEntry
	{
		uint8 Track;
		uint8 Kind;
		int32 Keys;
		uint32 Group;
	};
	int64 P = Table + int64(GroupOf.Num()) * 8;
	if (P > Nat.Num())
	{
		OutError = TEXT("unrecognised header layout");
		return false;
	}
	TArray<FEntry> Entries;
	int64 Need = P;
	for (int32 k = 0; k < GroupOf.Num(); ++k)
	{
		const int64 E = Table + int64(k) * 8;
		const FEntry Entry{ Nat[E], Nat[E + 1], Get<uint16>(Nat, E + 2) & 0x7FFF, GroupOf[k] >= 0 ? uint32(GroupOf[k]) : Get<uint32>(Nat, E + 4) };
		if (Entry.Kind == KindAttachments && bFlaggedExtra && k == GroupOf.Num() - 1)
		{
			continue;	// its block trails the keys
		}
		const int64 Size = KeyBytes(Entry.Kind, Entry.Keys);
		if (Size < 0)
		{
			OutError = FString::Printf(TEXT("track encoding 0x%02x isn't decoded"), Entry.Kind);
			return false;
		}
		Need += Size;
		Entries.Add(Entry);
	}
	const bool bTrailingBlock = Entries.Num() < GroupOf.Num();
	if (Need > Nat.Num() || (!bTrailingBlock && Nat.Num() - Need > 12))		// 0-12 bytes of padding follow the keys
	{
		OutError = TEXT("key data size doesn't match the track table");
		return false;
	}

	for (const FEntry& Entry : Entries)
	{
		FOKAnimTrack Track;
		Track.TrackBone = Entry.Track;
		Track.Group = int32(Entry.Group);
		auto Add = [&Track] (int32 Frame, const FVector4f& Value)
			{
				Track.Frames.Add(Frame);
				Track.Values.Add(Value);
			};
		switch (Entry.Kind)
		{
		case KindRotPair:
			// 96 bits, LSB first: key0 (38) | frame0 (10) | frame1 (10) | key1 (38); 12-bit fields are nibble-arranged.
			for (int32 i = 0; i < (Entry.Keys + 1) / 2; ++i, P += 12)
			{
				const uint8* R = Nat.GetData() + P;
				Add((R[4] >> 6) | (R[5] << 2),
					SmallestThree((R[4] >> 4) & 3, R[0] | ((R[1] & 15) << 8), (R[1] >> 4) | (R[2] << 4), (R[3] << 4) | (R[4] & 15)));
				if (2 * i + 1 < Entry.Keys)
				{
					Add(R[6] | ((R[7] & 3) << 8),
						SmallestThree((R[7] >> 2) & 3, R[8] | ((R[7] >> 4) << 8), R[9] | ((R[10] & 15) << 8), (R[10] >> 4) | (R[11] << 4)));
				}
			}
			break;
		case KindRot48:
			for (int32 i = 0; i < Entry.Keys; ++i, P += 8)
			{
				const uint8* R = Nat.GetData() + P;
				FVector4f Q(S12(R[0] | ((R[1] & 15) << 8)), S12((R[1] >> 4) | (R[2] << 4)), S12((R[3] << 4) | (R[4] & 15)), S12((R[4] >> 4) | (R[5] << 4)));
				const float Size = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);		// four separately quantised components
				Add(R[6] | (R[7] << 8), Size > 0.0f ? Q / Size : FVector4f(0, 0, 0, 1));
			}
			break;
		case KindRotF32:
			for (int32 i = 0; i < Entry.Keys; ++i, P += 20)
			{
				Add(int32(Get<uint32>(Nat, P + 16)), FVector4f(Get<float>(Nat, P), Get<float>(Nat, P + 4), Get<float>(Nat, P + 8), Get<float>(Nat, P + 12)));
			}
			break;
		case KindPosI16:
		case KindScaleI16:
		{
			const float Scale = Entry.Kind == KindPosI16 ? 1.0f / 32.0f : 1.0f / 4096.0f;
			for (int32 i = 0; i < Entry.Keys; ++i, P += 8)
			{
				Add(Get<uint16>(Nat, P), FVector4f(Get<int16>(Nat, P + 2) * Scale, Get<int16>(Nat, P + 4) * Scale, Get<int16>(Nat, P + 6) * Scale, 0.0f));
			}
			break;
		}
		case KindPosPair:
			// u32 key0, u16, u16, u32 key1: key0's frame is the u16 at +6, key1's the one at +4.
			for (int32 i = 0; i < (Entry.Keys + 1) / 2; ++i, P += 12)
			{
				Add(Get<uint16>(Nat, P + 6), UnpackPos32(Get<uint32>(Nat, P)));
				if (2 * i + 1 < Entry.Keys)
				{
					Add(Get<uint16>(Nat, P + 4), UnpackPos32(Get<uint32>(Nat, P + 8)));
				}
			}
			break;
		default:		// KindPosF32
			for (int32 i = 0; i < Entry.Keys; ++i, P += 16)
			{
				Add(int32(Get<uint32>(Nat, P + 12)), FVector4f(Get<float>(Nat, P), Get<float>(Nat, P + 4), Get<float>(Nat, P + 8), 0.0f));
			}
			break;
		}
		Track.Type = (Entry.Kind == KindRotPair || Entry.Kind == KindRot48 || Entry.Kind == KindRotF32) ? FOKAnimTrack::EType::Rotation
			: Entry.Kind == KindScaleI16 ? FOKAnimTrack::EType::Scale : FOKAnimTrack::EType::Translation;
		if (Entry.Group <= 1 && Track.Frames.Num() > 0)
		{
			Tracks.Add(MoveTemp(Track));
		}
	}
	return true;
}

void FOKAnimSequence::Sample(float Frame, const TArray<int32>& TrackToBone, TArray<FTransform>& InOutPose, int32 Group) const
{
	for (const FOKAnimTrack& Track : Tracks)
	{
		if (Track.Group != Group)
		{
			continue;
		}
		const int32 Bone = TrackToBone.IsValidIndex(Track.TrackBone) ? TrackToBone[Track.TrackBone] : INDEX_NONE;
		if (!InOutPose.IsValidIndex(Bone))
		{
			continue;
		}
		// The keys either side of Frame; before the first or after the last key, that key holds.
		const int32 Last = Track.Frames.Num() - 1;
		const int32 Upper = Algo::UpperBound(Track.Frames, Frame);
		const int32 A = FMath::Clamp(Upper - 1, 0, Last);
		const int32 B = FMath::Min(Upper, Last);
		const float Alpha = (A == B) ? 0.0f : (Frame - Track.Frames[A]) / float(Track.Frames[B] - Track.Frames[A]);
		const FVector4f& VA = Track.Values[A];
		const FVector4f& VB = Track.Values[B];

		switch (Track.Type)
		{
		case FOKAnimTrack::EType::Rotation:
		{
			const FQuat4f Q = FQuat4f::Slerp(FQuat4f(VA.X, VA.Y, VA.Z, VA.W), FQuat4f(VB.X, VB.Y, VB.Z, VB.W), Alpha);
			InOutPose[Bone].SetRotation(FQuat(Q.GetNormalized()));
			break;
		}
		case FOKAnimTrack::EType::Translation:
			InOutPose[Bone].SetTranslation(FVector(FMath::Lerp(FVector3f(VA), FVector3f(VB), Alpha)));
			break;
		case FOKAnimTrack::EType::Scale:
			InOutPose[Bone].SetScale3D(FVector(FMath::Lerp(FVector3f(VA), FVector3f(VB), Alpha)));
			break;
		}
	}
}