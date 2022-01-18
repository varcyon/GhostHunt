// Copyright Ilgar Lunin. All Rights Reserved.

#include "DecompressSound.h"
#include "Audio.h"
#include "Sound/SoundWave.h"
#include "VoskSoundUtils.h"


UDecompressSound* UDecompressSound::DecompressSound(USoundWave* Sound, bool StopDeviceSounds)
{
	UDecompressSound* BPNode = NewObject<UDecompressSound>();
	BPNode->Sound = Sound;
	BPNode->StopDeviceSounds = StopDeviceSounds;
	return BPNode;
}

void UDecompressSound::Activate() {
	// decompress
	VoskComponentUtils::EnsureSoudWaveDecompressed(Sound, StopDeviceSounds);

	FWaveModInfo WaveInfo;
	TArray<uint8> result;

	// prepare output value
	VoskComponentUtils::EnsureBulkDataLoaded(Sound);
	
	const uint8* RawWaveData = (const uint8*)Sound->RawData.LockReadOnly();
	int32 RawDataSize = Sound->RawData.GetBulkDataSize();

	if (!RawWaveData || RawDataSize == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("LPCM data failed to load for sound %s"), *Sound->GetFullName());
	}
	else if (!WaveInfo.ReadWaveHeader(RawWaveData, RawDataSize, 0))
	{
		// If we failed to parse the wave header, it's either because of an
		// invalid bitdepth or channel configuration.
		UE_LOG(LogTemp, Warning,
			TEXT("Only mono or stereo 16 bit waves allowed: %s (%d bytes)"),
			*Sound->GetFullName(), RawDataSize);
	}
	else
	{
		result.AddUninitialized(WaveInfo.SampleDataSize);
		FMemory::Memcpy(result.GetData(), WaveInfo.SampleDataStart, WaveInfo.SampleDataSize);
	}

	if (!result.Num())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Can't cook %s because there is no source LPCM data"), *Sound->GetFullName());
	}

	Sound->RawData.Unlock();

	// output result
	Finished.Broadcast(result);
}
