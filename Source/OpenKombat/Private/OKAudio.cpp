#include "OKAudio.h"
#include "OKByteReader.h"
#include "OKPackage.h"
#include "OKProperties.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundWaveProcedural.h"

#if WITH_DR_LIBS_MP3
#include "dr_libs_mp3decoder.h"
#endif

static TAutoConsoleVariable<float> CVarAudioVolume(TEXT("ok.Audio.Volume"), 1.0f,
	TEXT("The volume MKKE's sounds play at (W6); 0 mutes them."));
static TAutoConsoleVariable<FString> CVarAudioSurface(TEXT("ok.Audio.Surface"), TEXT("wood"),
	TEXT("The floor whose snd_surface_<name> bank footsteps and falls play from. [inf] Each stage names its own (W5); not read yet."));
static TAutoConsoleVariable<int32> CVarAudioTrace(TEXT("ok.Audio.Trace"), 0,
	TEXT("1 logs each sound the scripts play, and the ones that aren't found."));

namespace
{
	uint32 Word(TConstArrayView<uint8> Bytes, int64 At)
	{
		uint32 Value = 0;
		if (At >= 0 && At + 4 <= Bytes.Num())
		{
			FMemory::Memcpy(&Value, Bytes.GetData() + At, 4);
		}
		return Value;
	}

	int64 Find(TConstArrayView<uint8> Bytes, const char* Tag, int64 From = 0)
	{
		for (int64 i = FMath::Max<int64>(From, 0); i + 4 <= Bytes.Num(); ++i)
		{
			if (FMemory::Memcmp(Bytes.GetData() + i, Tag, 4) == 0)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	/** A zero-terminated printable string of Length bytes (the zero included) at At. */
	bool IsName(TConstArrayView<uint8> Bytes, int64 At, uint32 Length)
	{
		if (Length < 2 || At + Length > Bytes.Num() || Bytes[At + Length - 1] != 0)
		{
			return false;
		}
		for (int64 i = At; i < At + Length - 1; ++i)
		{
			if (Bytes[i] < 0x20 || Bytes[i] > 0x7E)
			{
				return false;
			}
		}
		return true;
	}

	/** The length of the MPEG audio frame (layer II or III) whose header is at Header; 0 if there isn't one there. */
	int32 MpegFrameBytes(const uint8* Header)
	{
		if (Header[0] != 0xFF || (Header[1] & 0xE0) != 0xE0)
		{
			return 0;
		}
		const int32 Version = (Header[1] >> 3) & 3;		// 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
		const int32 Layer = (Header[1] >> 1) & 3;			// 1 layer III, 2 layer II
		const int32 BitrateIndex = Header[2] >> 4;
		const int32 RateIndex = (Header[2] >> 2) & 3;
		if (Version == 1 || (Layer != 1 && Layer != 2) || BitrateIndex == 0 || BitrateIndex == 15 || RateIndex == 3)
		{
			return 0;
		}
		static const int32 Mpeg1Layer3[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
		static const int32 Mpeg1Layer2[] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
		static const int32 Mpeg2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
		static const int32 Rates[] = { 44100, 48000, 32000 };
		const bool bMpeg1 = Version == 3;
		const int32 Kbps = bMpeg1 ? (Layer == 1 ? Mpeg1Layer3 : Mpeg1Layer2)[BitrateIndex] : Mpeg2[BitrateIndex];
		const int32 Rate = Rates[RateIndex] >> (bMpeg1 ? 0 : (Version == 2 ? 1 : 2));
		const int32 Factor = (Layer == 1 && !bMpeg1) ? 72 : 144;
		return Factor * Kbps * 1000 / Rate + ((Header[2] >> 1) & 1);
	}

	FString Text(TConstArrayView<uint8> Bytes, int64 At, uint32 Length)
	{
		return FString(int32(Length) - 1, reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + At));
	}
}

bool FOKSoundBank::Load(const FString& InName, TConstArrayView<uint8> Native, FString& OutError)
{
	Name = InName;
	const int64 Fev = Find(Native, "FEV1");
	if (Fev == INDEX_NONE)
	{
		OutError = TEXT("no FEV1");
		return false;
	}
	// A wave bank in the same blob has no name of its own: the waves' bank names all mean it.
	const int64 Fsb = Find(Native, "FSB4", Fev);
	const int64 End = (Fsb != INDEX_NONE ? Fsb : Native.Num()) - Fev;
	if (Fsb != INDEX_NONE && !AddWaveBank(FString(), Native.Slice(int32(Fsb), int32(Native.Num() - Fsb)), OutError))
	{
		return false;
	}
	const TConstArrayView<uint8> Bytes = Native.Slice(int32(Fev), int32(Native.Num() - Fev));

	// Events: u32 kind, u32 length, name, a 16-byte GUID. Simple events (kind 0x10) keep their sound definition 136 bytes
	// after the GUID, behind a count of 1 at +132. Event groups and categories have names too (kind 0); the sound
	// definitions start at the first name beginning with '/'.
	TArray<TPair<FString, uint32>> EventDefs;
	int64 FirstDefinition = INDEX_NONE;
	for (int64 P = 8; P + 8 < End; ++P)
	{
		const uint32 Length = Word(Bytes, P);
		if (Length > 80 || !IsName(Bytes, P + 4, Length))
		{
			continue;
		}
		if (Bytes[P + 4] == '/')
		{
			FirstDefinition = P;
			break;
		}
		const FString EventName = Text(Bytes, P + 4, Length);
		const uint32 Kind = Word(Bytes, P - 4);
		const int64 AfterGuid = P + 4 + Length + 16;
		if ((Kind == 0x10 || Kind == 0x08) && !EventName.Contains(TEXT(".")))
		{
			// Complex events (kind 8) have layers there instead: a count, then each layer's u16 flags, s16 parameter, u16, u16
			// sounds, and each sound {u16, u16 definition}. [inf] The first layer's first sound is taken.
			const uint32 Definition = Kind == 0x10 ? Word(Bytes, AfterGuid + 136) : (Word(Bytes, AfterGuid + 146) & 0xFFFF);
			EventDefs.Emplace(EventName.ToLower(), Definition);
			P += 3 + Length;
		}
	}

	// Sound definitions: u32 length, "/name", u32 play mode, u32 waves, then each wave: u32 kind, u32 weight, a file name, a
	// bank name (both u32 length + text), u32 sample index in that bank, u32 length in milliseconds.
	for (int64 P = FirstDefinition; P != INDEX_NONE && P + 8 < End;)
	{
		const uint32 Length = Word(Bytes, P);
		if (Length > 160 || !IsName(Bytes, P + 4, Length) || Bytes[P + 4] != '/')
		{
			break;
		}
		P += 4 + Length;
		const uint32 Waves = Word(Bytes, P + 4);
		P += 8;
		if (Waves > 64)
		{
			break;
		}
		TArray<TPair<FString, int32>>& Definition = DefinitionWaves.AddDefaulted_GetRef();
		for (uint32 w = 0; w < Waves && P + 8 < End; ++w)
		{
			P += 8;
			P += 4 + Word(Bytes, P);		// the file name
			const uint32 BankLength = Word(Bytes, P);
			const FString Bank = IsName(Bytes, P + 4, BankLength) ? Text(Bytes, P + 4, BankLength).ToLower() : FString();
			P += 4 + BankLength;
			Definition.Emplace(Bank, int32(Word(Bytes, P)));
			P += 8;
		}
	}
	for (const TPair<FString, uint32>& Event : EventDefs)
	{
		if (DefinitionWaves.IsValidIndex(int32(Event.Value)))
		{
			Events.Add(Event.Key, int32(Event.Value));
		}
	}
	Resolve();
	return true;
}

bool FOKSoundBank::AddWaveBank(const FString& BankName, TConstArrayView<uint8> Fsb, FString& OutError)
{
	// FSB4: a 48-byte header (samples, sample header bytes, data bytes, version, mode), 80-byte sample headers, then the
	// samples one after another.
	if (Fsb.Num() < 48 || FMemory::Memcmp(Fsb.GetData(), "FSB4", 4) != 0)
	{
		OutError = TEXT("no FSB4");
		return false;
	}
	if ((Word(Fsb, 20) & 2) != 0)
	{
		OutError = TEXT("FSB4 basic sample headers aren't read");
		return false;
	}
	const uint32 Count = Word(Fsb, 4);
	const int64 DataStart = 48 + int64(Word(Fsb, 8));
	const int64 Size = DataStart + int64(Word(Fsb, 12));
	if (Size > Fsb.Num())
	{
		OutError = TEXT("the FSB4's samples run past its data");
		return false;
	}
	const int32 Blob = Blobs.Emplace(Fsb.GetData(), int32(Size));
	BankStarts.Add(BankName.ToLower(), Samples.Num());
	int64 Header = 48;
	int64 Offset = DataStart;
	for (uint32 i = 0; i < Count && Header + 64 <= DataStart; ++i)
	{
		FSample Sample;
		const ANSICHAR* RawName = reinterpret_cast<const ANSICHAR*>(Fsb.GetData() + Header + 2);
		Sample.Name = FString(FCStringAnsi::Strnlen(RawName, 30), RawName);
		Sample.Blob = Blob;
		Sample.Offset = Offset;
		Sample.Bytes = int32(Word(Fsb, Header + 36));
		Sample.Mode = Word(Fsb, Header + 48);
		Sample.Rate = int32(Word(Fsb, Header + 52));
		Sample.Channels = FMath::Max<int32>(1, Fsb[Header + 62] | (Fsb[Header + 63] << 8));
		Offset += Sample.Bytes;
		Samples.Add(Sample);
		const uint16 HeaderBytes = uint16(Fsb[Header] | (Fsb[Header + 1] << 8));
		Header += HeaderBytes > 0 ? HeaderBytes : 80;
	}
	Resolve();
	return true;
}

void FOKSoundBank::Resolve()
{
	// Each wave names its bank and its sample there; a lone unnamed bank (the one in the event file's blob) stands for all.
	const int32* Lone = BankStarts.Num() == 1 ? BankStarts.Find(FString()) : nullptr;
	Definitions.Reset();
	for (const TArray<TPair<FString, int32>>& Waves : DefinitionWaves)
	{
		TArray<int32>& Definition = Definitions.AddDefaulted_GetRef();
		for (const TPair<FString, int32>& Wave : Waves)
		{
			const int32* Start = Lone ? Lone : BankStarts.Find(Wave.Key);
			if (Start && Samples.IsValidIndex(*Start + Wave.Value))
			{
				Definition.Add(*Start + Wave.Value);
			}
		}
	}
}

const TArray<int32>* FOKSoundBank::FindEvent(const FString& Event) const
{
	const int32* Definition = Events.Find(Event.ToLower());
	return (Definition && Definitions.IsValidIndex(*Definition)) ? &Definitions[*Definition] : nullptr;
}

namespace
{
	/**
	 * FSB's IMA ADPCM (Xbox style): blocks of 36 bytes a channel, 64 samples each: an s16 sample and an s16 step index, then 32
	 * bytes, two samples a byte, low nibble first. Mono and stereo interleave the channels' 4-byte words, headers first; with
	 * more channels (the announcer's 5.1) everything is interleaved in 2-byte words, headers included.
	 */
	void DecodeImaAdpcm(const uint8* In, int32 Bytes, int32 Channels, TArray<int16>& Out)
	{
		static const int32 Steps[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
			80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
			963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
			7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
		static const int32 IndexChange[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
		const int32 BlockBytes = 36 * Channels;
		const int32 Word = Channels > 2 ? 2 : 4;
		uint8 Mono[36];
		for (int32 Block = 0; Block + BlockBytes <= Bytes; Block += BlockBytes)
		{
			const int32 First = Out.Num();
			Out.AddZeroed(64 * Channels);
			for (int32 c = 0; c < Channels; ++c)
			{
				// This channel's 36 bytes, gathered from the block
				for (int32 w = 0; w < 36 / Word; ++w)
				{
					FMemory::Memcpy(Mono + w * Word, In + Block + (w * Channels + c) * Word, Word);
				}
				int32 Predictor = int16(Mono[0] | (Mono[1] << 8));
				int32 Index = FMath::Clamp<int32>(Mono[2], 0, 88);
				for (int32 n = 0; n < 64; ++n)
				{
					const int32 Nibble = (Mono[4 + n / 2] >> ((n & 1) * 4)) & 15;
					const int32 Step = Steps[Index];
					int32 Diff = Step >> 3;
					Diff += (Nibble & 1) ? (Step >> 2) : 0;
					Diff += (Nibble & 2) ? (Step >> 1) : 0;
					Diff += (Nibble & 4) ? Step : 0;
					Predictor = FMath::Clamp(Predictor + ((Nibble & 8) ? -Diff : Diff), -32768, 32767);
					Index = FMath::Clamp(Index + IndexChange[Nibble & 7], 0, 88);
					Out[First + n * Channels + c] = int16(Predictor);
				}
			}
		}
	}

	/** 5.1 (FL, FR, C, LFE, SL, SR) and other layouts down to stereo, for the 2D mix. */
	void DownmixToStereo(TArray<int16>& Pcm, int32 Channels)
	{
		const int32 Frames = Pcm.Num() / Channels;
		TArray<int16> Stereo;
		Stereo.SetNumUninitialized(Frames * 2);
		for (int32 f = 0; f < Frames; ++f)
		{
			const int16* In = Pcm.GetData() + f * Channels;
			float Left = In[0], Right = In[1];
			if (Channels >= 3)
			{
				Left += 0.707f * In[2];
				Right += 0.707f * In[2];
			}
			for (int32 c = 4; c + 1 < Channels; c += 2)
			{
				Left += 0.707f * In[c];
				Right += 0.707f * In[c + 1];
			}
			Stereo[f * 2] = int16(FMath::Clamp(Left, -32768.0f, 32767.0f));
			Stereo[f * 2 + 1] = int16(FMath::Clamp(Right, -32768.0f, 32767.0f));
		}
		Pcm = MoveTemp(Stereo);
	}
}

bool FOKSoundBank::Decode(int32 Index, const TArray<int16>*& OutPcm, int32& OutChannels, int32& OutRate)
{
	if (!Samples.IsValidIndex(Index))
	{
		return false;
	}
	const FSample& Sample = Samples[Index];
	OutChannels = FMath::Min(Sample.Channels, 2);
	OutRate = Sample.Rate;
	if (const TArray<int16>* Found = Decoded.Find(Index))
	{
		OutPcm = Found;
		return Found->Num() > 0;
	}
	TArray<int16>& Pcm = Decoded.Add(Index);
	OutPcm = &Pcm;
	const uint8* In = Blobs[Sample.Blob].GetData() + Sample.Offset;
	if ((Sample.Mode & 0x200) != 0)
	{
#if WITH_DR_LIBS_MP3
		// MPEG: one frame at a time, as the decoder wants; FSB pads between frames, so each is found by its sync word.
		FMP3Decoder Decoder;
		float Frame[1152 * 2];
		for (int32 Done = 0; Done + 4 <= Sample.Bytes;)
		{
			const int32 Length = MpegFrameBytes(In + Done);
			if (Length <= 0)
			{
				++Done;
				continue;
			}
			if (Done + Length > Sample.Bytes)
			{
				break;
			}
			FMP3Decoder::FFrameInfo Info;
			const int32 Got = Decoder.Decode(&Info, Frame, sizeof(Frame), In + Done, Length);
			Done += Length;
			if (Got > 0 && Info.NumChannels > 0 && Info.NumChannels <= 2)
			{
				OutChannels = Info.NumChannels;
				OutRate = Info.SampleRate;
				for (int32 i = 0; i < Got * Info.NumChannels; ++i)
				{
					Pcm.Add(int16(FMath::Clamp(Frame[i], -1.0f, 1.0f) * 32767.0f));
				}
			}
		}
#endif
	}
	else if ((Sample.Mode & 0x400000) != 0)
	{
		DecodeImaAdpcm(In, Sample.Bytes, Sample.Channels, Pcm);
		if (Sample.Channels > 2)
		{
			DownmixToStereo(Pcm, Sample.Channels);
		}
	}
	else if ((Sample.Mode & (0x1000000 | 0x2000000)) == 0)
	{
		// [inf] 16-bit PCM
		Pcm.Append(reinterpret_cast<const int16*>(In), Sample.Bytes / 2);
	}
	if (Pcm.Num() == 0)
	{
		UE_LOG(LogOKAsset, Warning, TEXT("%s: %s (mode %08X) isn't decoded"), *Name, *Sample.Name, Sample.Mode);
		return false;
	}
	return true;
}

FOKAudio& FOKAudio::Get()
{
	static FOKAudio Audio;
	return Audio;
}

void FOKAudio::AddPackage(const FString& PackageName)
{
	Packages.AddUnique(PackageName);
}

void FOKAudio::IndexPackage(const FString& PackageName)
{
	if (PackageBanks.Contains(PackageName))
	{
		return;
	}
	TArray<FString>& Names = PackageBanks.Add(PackageName);
	FString Error;
	TUniquePtr<FOKPackage> Package = FOKPackage::OpenGamePackage(PackageName, Error);
	if (!Package)
	{
		UE_LOG(LogOKAsset, Log, TEXT("%s: no package for sounds (%s)"), *PackageName, *Error);
		return;
	}
	for (const FOKPackageExport* Export : Package->FindExports(TEXT("FmodEventFile")))
	{
		// Some names end in a line feed in the files (the snd_surface_* banks)
		const FString ExportName = Export->Name.TrimEnd();
		if (!ExportName.EndsWith(TEXT("_PS3Data")))
		{
			continue;
		}
		TArray<uint8> Bytes;
		FOKPropertyList Properties;
		if (!Package->ExportData(*Export, Bytes))
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: %s can't be read"), *PackageName, *Export->Name);
			continue;
		}
		FOKByteReader Reader(Bytes);
		if (!Properties.Read(Reader, Package->GetNames()))
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: %s: the property list is damaged"), *PackageName, *Export->Name);
			continue;
		}
		const FString BankName = ExportName.LeftChop(8);
		TSharedPtr<FOKSoundBank> Bank = MakeShared<FOKSoundBank>();
		if (!Bank->Load(BankName, TConstArrayView<uint8>(Bytes.GetData() + Reader.Tell(), int32(Bytes.Num() - Reader.Tell())), Error))
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: sound bank %s: %s"), *PackageName, *BankName, *Error);
			continue;
		}
		if (Bank->NumSamples() == 0)
		{
			AddStreamedBanks(*Package, *Bank, BankName);
		}
		UE_LOG(LogOKAsset, Log, TEXT("%s: sound bank %s, %d events, %d samples"), *PackageName, *BankName, Bank->NumEvents(), Bank->NumSamples());
		Banks.Add(BankName.ToLower(), Bank);
		Names.Add(BankName.ToLower());
	}
}

void FOKAudio::AddStreamedBanks(FOKPackage& Package, FOKSoundBank& Bank, const FString& BankName)
{
	// The PC copy (<bank>_PCData) names each .fsb with a bulk-data header before it: flags 0x40000000 (stored in the package
	// file), 0, 0, and a file offset. The streamed banks lie there one after another, in that order (the announcer, music).
	const FOKPackageExport* Stub = nullptr;
	for (const FOKPackageExport* Export : Package.FindExports(TEXT("FmodEventFile")))
	{
		if (Export->Name.TrimEnd().Equals(BankName + TEXT("_PCData"), ESearchCase::IgnoreCase))
		{
			Stub = Export;
		}
	}
	TArray<uint8> Bytes;
	if (!Stub || !Package.ExportData(*Stub, Bytes))
	{
		return;
	}
	TArray<TPair<FString, uint32>> Files;
	for (int64 P = 4; P + 8 < Bytes.Num(); ++P)
	{
		const uint32 Length = Word(Bytes, P);
		if (Length > 5 && Length < 120 && IsName(Bytes, P + 4, Length))
		{
			const FString File = Text(Bytes, P + 4, Length);
			if (File.EndsWith(TEXT(".fsb"), ESearchCase::IgnoreCase) && !File.Contains(TEXT("/")) && !File.Contains(TEXT("\\")))
			{
				Files.Emplace(File.LeftChop(4), Word(Bytes, P - 4));
			}
			P += 3 + Length;
		}
	}
	const TConstArrayView<uint8> Raw = Package.GetFileBytes();
	int64 At = Files.Num() > 0 ? int64(Files[0].Value) : 0;
	for (const TPair<FString, uint32>& File : Files)
	{
		if (At + 48 > Raw.Num() || FMemory::Memcmp(Raw.GetData() + At, "FSB4", 4) != 0)
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: streamed bank %s isn't at %lld"), *BankName, *File.Key, At);
			break;
		}
		const int64 Size = 48 + int64(Word(Raw, At + 8)) + int64(Word(Raw, At + 12));
		FString Error;
		if (!Bank.AddWaveBank(File.Key, Raw.Slice(int32(At), int32(FMath::Min<int64>(Size, Raw.Num() - At))), Error))
		{
			UE_LOG(LogOKAsset, Warning, TEXT("%s: streamed bank %s: %s"), *BankName, *File.Key, *Error);
			break;
		}
		At += Size;
	}
}

FOKSoundBank* FOKAudio::FindBank(const FString& Group)
{
	const FString Key = Group.ToLower();
	if (TSharedPtr<FOKSoundBank>* Found = Banks.Find(Key))
	{
		return Found->Get();
	}
	// The package named after the group (snd_fightfx1), then the fighters' (SND_SFX_Scorpion is in CHAR_Scorpion).
	IndexPackage(Group);
	for (const FString& Package : Packages)
	{
		if (Banks.Contains(Key))
		{
			break;
		}
		IndexPackage(Package);
	}
	TSharedPtr<FOKSoundBank>* Found = Banks.Find(Key);
	return Found ? Found->Get() : nullptr;
}

void FOKAudio::Play(const FString& Group, const FString& Event, const FString& OwnerPackage)
{
	Pending.Add({ Group, Event, OwnerPackage });
}

void FOKAudio::Flush(UWorld* World)
{
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	Playing.RemoveAll([Now](const TPair<TWeakObjectPtr<UAudioComponent>, double>& Each)
		{
			// A procedural wave plays silence once its audio runs out, so each is stopped when it should end.
			if (Each.Value > Now && Each.Key.IsValid())
			{
				return false;
			}
			if (UAudioComponent* Component = Each.Key.Get())
			{
				Component->Stop();
			}
			return true;
		});
	const TArray<FRequest> Requests = MoveTemp(Pending);
	Pending.Reset();
	for (const FRequest& Request : Requests)
	{
		if (World && CVarAudioVolume.GetValueOnGameThread() > 0.0f)
		{
			Start(World, Request);
		}
	}
}

bool FOKAudio::Dump(const FString& Group, const FString& Event, const FString& OwnerPackage, const FString& Path)
{
	if (!OwnerPackage.IsEmpty())
	{
		AddPackage(OwnerPackage);
	}
	FOKSoundBank* Bank = FindBank(Group);
	const TArray<int32>* Waves = Bank ? Bank->FindEvent(Event) : nullptr;
	const TArray<int16>* Pcm = nullptr;
	int32 Channels = 1, Rate = 48000;
	if (!Waves || Waves->Num() == 0 || !Bank->Decode((*Waves)[0], Pcm, Channels, Rate) || !Pcm)
	{
		return false;
	}
	// A plain 16-bit PCM RIFF file
	TArray<uint8> File;
	auto Put32 = [&File](uint32 V) { File.Append(reinterpret_cast<const uint8*>(&V), 4); };
	auto Put16 = [&File](uint16 V) { File.Append(reinterpret_cast<const uint8*>(&V), 2); };
	const uint32 DataBytes = uint32(Pcm->Num() * 2);
	File.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
	Put32(36 + DataBytes);
	File.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
	Put32(16);
	Put16(1);
	Put16(uint16(Channels));
	Put32(uint32(Rate));
	Put32(uint32(Rate * Channels * 2));
	Put16(uint16(Channels * 2));
	Put16(16);
	File.Append(reinterpret_cast<const uint8*>("data"), 4);
	Put32(DataBytes);
	File.Append(reinterpret_cast<const uint8*>(Pcm->GetData()), int32(DataBytes));
	return FFileHelper::SaveArrayToFile(File, *Path);
}

static FAutoConsoleCommand CmdAudioDump(
	TEXT("ok.Audio.Dump"),
	TEXT("ok.Audio.Dump <group> <event> [package]: writes the event's first wave, decoded, to Saved/OKAudio/<event>.wav"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOKAsset, Warning, TEXT("Usage: ok.Audio.Dump SND_VO_Announcer vo_anno_round01"));
				return;
			}
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OKAudio"), Args[1] + TEXT(".wav"));
			const bool bDone = FOKAudio::Get().Dump(Args[0], Args[1], Args.Num() > 2 ? Args[2] : FString(), Path);
			UE_LOG(LogOKAsset, Log, TEXT("ok.Audio.Dump %s/%s: %s"), *Args[0], *Args[1], bDone ? *Path : TEXT("not found"));
		}));

bool FOKAudio::Start(UWorld* World, const FRequest& Request)
{
	const bool bTrace = CVarAudioTrace.GetValueOnGameThread() != 0;
	FOKSoundBank* Bank = Request.Group.IsEmpty() ? nullptr : FindBank(Request.Group);
	const TArray<int32>* Waves = Bank ? Bank->FindEvent(Request.Event) : nullptr;
	if (!Waves)
	{
		// Voices and foley: the event in one of the owner's banks; footsteps and falls: in the floor's
		const FString Surface = TEXT("snd_surface_") + CVarAudioSurface.GetValueOnGameThread();
		TArray<FString> Look;
		if (!Request.Owner.IsEmpty())
		{
			IndexPackage(Request.Owner);
			Look = PackageBanks.FindRef(Request.Owner);
		}
		IndexPackage(Surface);
		Look.Append(PackageBanks.FindRef(Surface));
		for (const FString& BankName : Look)
		{
			Bank = Banks.FindRef(BankName).Get();
			Waves = Bank ? Bank->FindEvent(Request.Event) : nullptr;
			if (Waves)
			{
				break;
			}
		}
	}
	if (!Waves || Waves->Num() == 0)
	{
		if (bTrace)
		{
			UE_LOG(LogOKAsset, Log, TEXT("sound %s/%s (%s): not found"), *Request.Group, *Request.Event, *Request.Owner);
		}
		return false;
	}
	// A sound definition with several waves plays one of them. [inf] FMOD's play modes (random, no repeat ...) aren't read.
	const int32 Sample = (*Waves)[Random.RandRange(0, Waves->Num() - 1)];
	const TArray<int16>* Pcm = nullptr;
	int32 Channels = 1, Rate = 48000;
	if (!Bank->Decode(Sample, Pcm, Channels, Rate) || !Pcm)
	{
		return false;
	}
	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
	Wave->SetSampleRate(Rate);
	Wave->NumChannels = Channels;
	const float Seconds = float(Pcm->Num()) / float(FMath::Max(1, Channels * Rate));
	Wave->Duration = Seconds;
	Wave->SoundGroup = SOUNDGROUP_Default;
	Wave->bLooping = false;
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Pcm->GetData()), Pcm->Num() * sizeof(int16));
	UAudioComponent* Component = UGameplayStatics::SpawnSound2D(World, Wave, CVarAudioVolume.GetValueOnGameThread());
	if (Component)
	{
		Playing.Emplace(Component, World->GetTimeSeconds() + Seconds + 0.1);
	}
	if (bTrace)
	{
		UE_LOG(LogOKAsset, Log, TEXT("sound %s/%s: %s sample %d, %.2f s"), *Request.Group, *Request.Event, *Bank->GetName(), Sample, Seconds);
	}
	return Component != nullptr;
}
