#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * One FMOD Designer event file (system W6): an FmodEventFile export holding an FEV1 (events, sound definitions) and the
 * FSB4 wave bank they play. MKKE's PC packages keep these banks in the PS3 copy (*_PS3Data, MPEG audio); the PC and Xbox
 * copies only name the files, except that streamed banks (the announcer, music) are stored raw in the package file, where
 * the PC copy says.
 */
class OPENKOMBAT_API FOKSoundBank
{
public:
	/** Reads the FEV1 and FSB4 in an FmodEventFile export's data (after its property list). */
	bool Load(const FString& InName, TConstArrayView<uint8> Native, FString& OutError);

	/** Adds a wave bank (an FSB4) the event file's waves name as BankName; unnamed, it stands for every bank they name. */
	bool AddWaveBank(const FString& BankName, TConstArrayView<uint8> Fsb, FString& OutError);

	/** The waves (sample indices) an event chooses from; null if the bank has no such event. */
	const TArray<int32>* FindEvent(const FString& Event) const;

	/** A sample as interleaved 16-bit PCM, decoded once and kept. */
	bool Decode(int32 Sample, const TArray<int16>*& OutPcm, int32& OutChannels, int32& OutRate);

	const FString& GetName() const { return Name; }
	int32 NumEvents() const { return Events.Num(); }
	int32 NumSamples() const { return Samples.Num(); }

private:
	struct FSample
	{
		FString Name;			// sfx_scor_spear_launch01.wav
		int32 Blob = 0;			// which of Blobs holds it
		int64 Offset = 0;		// into that blob
		int32 Bytes = 0;
		int32 Rate = 0;
		int32 Channels = 0;
		uint32 Mode = 0;		// FSB4 sample mode: 0x200 MPEG, 0x400000 IMA ADPCM
	};

	/** Each sound definition's waves to sample indices, once the wave banks they name are in. */
	void Resolve();

	FString Name;
	TArray<TArray<uint8>> Blobs;			// the wave banks' bytes
	TArray<FSample> Samples;				// every wave bank's, one after another
	TMap<FString, int32> BankStarts;		// wave bank name (lower case) -> its first sample
	TArray<TArray<TPair<FString, int32>>> DefinitionWaves;	// sound definitions as stored: {wave bank, sample there}
	TArray<TArray<int32>> Definitions;		// and as sample indices
	TMap<FString, int32> Events;			// event name (lower case) -> sound definition
	TMap<int32, TArray<int16>> Decoded;
};

/**
 * MKKE's sounds, played from the game's own banks (system W6). Scripts name an event as group/event
 * (snd_fightfx1/sfx_hit_face, SND_SFX_Scorpion/spear_launch); voices and foley name only the event, which is found in the
 * banks of the fighter making it (CHAR_Scorpion carries SND_VO_Scorpion and snd_foley_leather). A group's banks come from
 * the package named after it, or from a fighter's package.
 */
class OPENKOMBAT_API FOKAudio
{
public:
	static FOKAudio& Get();

	/** A package to look in for groups that have no package of their own: each fighter's. */
	void AddPackage(const FString& PackageName);

	/** Asks for Group/Event to play (the owner's banks when Group is empty or not found). Plays on the next Flush. */
	void Play(const FString& Group, const FString& Event, const FString& OwnerPackage);

	/** Writes Group/Event's first wave, decoded, to a .wav file (ok.Audio.Dump). False if it isn't found. */
	bool Dump(const FString& Group, const FString& Event, const FString& OwnerPackage, const FString& Path);

	/** Plays what was asked for since the last call, and stops sounds that have finished. Once a sim frame. */
	void Flush(UWorld* World);

private:
	struct FRequest
	{
		FString Group;
		FString Event;
		FString Owner;
	};

	/** The banks a package carries, indexed once. */
	void IndexPackage(const FString& PackageName);
	/** A bank whose wave banks are streamed: stored raw in the package file, where its PC copy says. */
	void AddStreamedBanks(class FOKPackage& Package, FOKSoundBank& Bank, const FString& BankName);
	FOKSoundBank* FindBank(const FString& Group);
	bool Start(UWorld* World, const FRequest& Request);

	TMap<FString, TSharedPtr<FOKSoundBank>> Banks;		// by name, lower case: snd_sfx_scorpion
	TMap<FString, TArray<FString>> PackageBanks;		// package -> the banks in it
	TArray<FString> Packages;							// fighters' packages, searched for groups
	TArray<FRequest> Pending;
	TArray<TPair<TWeakObjectPtr<class UAudioComponent>, double>> Playing;	// and when each ends
	FRandomStream Random{ 0x57AD };
};
