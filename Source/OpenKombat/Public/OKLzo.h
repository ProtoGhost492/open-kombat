#pragma once

#include "CoreMinimal.h"

namespace OKLzo
{
	/** LZO1X decompression, bounds-checked like lzo1x_decompress_safe. Returns the bytes written, or -1 for a bad stream. */
	int32 Decompress(const uint8* Src, int32 SrcLen, uint8* Dst, int32 DstLen);
}
