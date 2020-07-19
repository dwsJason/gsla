//
// LZB Encode / Decode
// 
#include "lzb.h"

#include <stdio.h>
#include <string.h>

#include "bctypes.h"

#include "assert.h"

//
// This is written specifically for the GSLA, so opcodes emitted are designed
// to work with our version of a run/skip/dump
//

//
//Command Word, encoded low-high, what the bits mean:
//
// xxx_xxxx_xxxx_xxx is the number of bytes 1-16384 to follow (0 == 1 byte)
//
//%0xxx_xxxx_xxxx_xxx1 - Copy Bytes - straight copy bytes
//%1xxx_xxxx_xxxx_xxx1 - Skip Bytes - skip bytes / move the cursor
//%1xxx_xxxx_xxxx_xxx0 - Dictionary Copy Bytes from  frame buffer to frame buffer
//
//%0000_0000_0000_0000- Source Skip -> Source pointer skips to next bank of data
//%0000_0000_0000_0010- End of Frame - end of frame
//%0000_0000_0000_0110- End of Animation / End of File / no more frames
//

#define MAX_DICTIONARY_SIZE (32 * 1024)
#define MAX_STRING_SIZE     (16384)
//
// Yes This is a 32K Buffer, of bytes, with no structure to it
//
static unsigned char *pDictionary = nullptr;

struct DataString {
	// Information about the data we're trying to match
	int size;
	unsigned char *pData;
};

static int AddDictionary(const DataString& data, int dictionarySize);
static int EmitLiteral(unsigned char *pDest, DataString& data);
static int ConcatLiteral(unsigned char *pDest, DataString& data);
static int EmitReference(unsigned char *pDest, int dictionaryOffset, DataString& data);
static int DictionaryMatch(const DataString& data, int dictionarySize);

// Stuff I need for a faster version
static DataString LongestMatch(const DataString& data, const DataString& dictionary);
static DataString LongestMatch(const DataString& data, const DataString& dictionary, int cursorPosition);

//
//  New Version, still Brute Force, but not as many times
//
int LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize)
{
	//printf("LZB Compress %d bytes\n", sourceSize);

	unsigned char *pOriginalDest = pDest;

	DataString sourceData;
	DataString dictionaryData;
	DataString candidateData;

	// Source Data Stream - will compress until the size is zero
	sourceData.pData = pSource;
	sourceData.size  = sourceSize;

	// Remember, this eventually will point at the frame buffer
	dictionaryData.pData = pSource;
	dictionaryData.size = 0;

	// dumb last emit is a literal stuff
	bool bLastEmitIsLiteral = false;
	unsigned char* pLastLiteralDest = nullptr;

	while (sourceData.size > 0)
	{
		candidateData = LongestMatch(sourceData, dictionaryData);

		// If no match, or the match is too small, then take the next byte
		// and emit as literal
		if ((0 == candidateData.size)) // || (candidateData.size < 4))
		{
			candidateData.size = 1;
			candidateData.pData = sourceData.pData;
		}

		// Adjust source stream
		sourceData.pData += candidateData.size;
		sourceData.size  -= candidateData.size;

		dictionaryData.size = AddDictionary(candidateData, dictionaryData.size);

		if (candidateData.size > 3)
		{
			// Emit a dictionary reference
			pDest += (int)EmitReference(pDest, (int)(candidateData.pData - dictionaryData.pData), candidateData);
			bLastEmitIsLiteral = false;
		}
		else if (bLastEmitIsLiteral)
		{
			// Concatenate this literal onto the previous literal
			pDest += ConcatLiteral(pLastLiteralDest, candidateData);
		}
		else
		{
			// Emit a new literal
			pLastLiteralDest = pDest;
			bLastEmitIsLiteral = true;
			pDest += EmitLiteral(pDest, candidateData);
		}
	}

	return (int)(pDest - pOriginalDest);
}


//
// This works, but it's stupidly slow, because it uses brute force, and
// because the brute force starts over everytime I grow the data string
//
int Old_LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize)
{
	//printf("LZB_Compress %d bytes\n", sourceSize);

	// Initialize Dictionary
	int bytesInDictionary = 0;		// eventually add the ability to start with the dictionary filled
	pDictionary = pSource;

	int processedBytes = 0;
	int bytesEmitted = 0;

	// dumb last emit is a literal stuff
	bool bLastEmitIsLiteral = false;
	int  lastEmittedLiteralOffset = 0;

	DataString candidate_data;
	candidate_data.pData = pSource;
	candidate_data.size = 0;

	int MatchOffset = -1;
	int PreviousMatchOffset = -1;

	while (processedBytes < sourceSize)
	{
		// Add a byte to the candidate_data, also tally number of processed
		processedBytes++;
		candidate_data.size++;  

		// Basic Flow Idea Here
		// If there's a match, then add to the candidate data, and see if
		// there's a bigger match (use previous result to speed up search)
		// else
		// if there's a previous match, and it's large enough, emit that
		// else emit what we have as a literal


		// (KMP is probably the last planned optmization here)
		PreviousMatchOffset = MatchOffset; 

		MatchOffset = DictionaryMatch(candidate_data, bytesInDictionary);

		// The dictionary only contains bytes that have been emitted, so we
		// can't add this byte until we've emitted it?
		if (MatchOffset < 0)
		{
			// Was there a dictionary match

			// Previous Data, we can't get here with candidate_data.size == 0
			// this is an opportunity to use an assert
			candidate_data.size--;

			MatchOffset = PreviousMatchOffset; //DictionaryMatch(candidate_data, bytesInDictionary);

			if ((MatchOffset >= 0) && candidate_data.size > 3)
			{
				processedBytes--;
				bytesInDictionary = AddDictionary(candidate_data, bytesInDictionary);
				bytesEmitted += EmitReference(pDest + bytesEmitted, MatchOffset, candidate_data);
				bLastEmitIsLiteral = false;
			}
			else
			{
				if (0 == candidate_data.size)
				{
					candidate_data.size++;
				}
				else
				{
					processedBytes--;

					//if (candidate_data.size > 1)
					//{
					//	processedBytes -= (candidate_data.size - 1);
					//	candidate_data.size = 1;
					//}
				}


				// Add Dictionary
				bytesInDictionary = AddDictionary(candidate_data, bytesInDictionary);

				if (bLastEmitIsLiteral)
				{
					// If the last emit was a literal, I want to concatenate
					// this literal into the previous opcode, to save space
					bytesEmitted += ConcatLiteral(pDest + lastEmittedLiteralOffset, candidate_data);
				}
				else
				{
					lastEmittedLiteralOffset = bytesEmitted;
					bytesEmitted += EmitLiteral(pDest + bytesEmitted, candidate_data);
				}
				bLastEmitIsLiteral = true;
				//MatchOffset = -1;
			}
		}
	}

	if (candidate_data.size > 0)
	{

		int MatchOffset = DictionaryMatch(candidate_data, bytesInDictionary);

		if ((MatchOffset >=0) && candidate_data.size > 2)
		{
			bytesInDictionary = AddDictionary(candidate_data, bytesInDictionary);
			bytesEmitted += EmitReference(pDest + bytesEmitted, MatchOffset, candidate_data);
		}
		else
		{
			// Add Dictionary
			bytesInDictionary = AddDictionary(candidate_data, bytesInDictionary);

			if (bLastEmitIsLiteral)
			{
				// If the last emit was a literal, I want to concatenate
				// this literal into the previous opcode, to save space
				bytesEmitted += ConcatLiteral(pDest + lastEmittedLiteralOffset, candidate_data);
			}
			else
			{
				bytesEmitted += EmitLiteral(pDest + bytesEmitted, candidate_data);
			}
		}
	}

	return bytesEmitted;
}

//------------------------------------------------------------------------------
// Return new dictionarySize
static int AddDictionary(const DataString& data, int dictionarySize)
{
	int dataIndex = 0;
	while (dataIndex < data.size)
	{
		pDictionary[ dictionarySize++ ] = data.pData[ dataIndex++ ];
	}

	dictionarySize += data.size;

	return dictionarySize;
}

//------------------------------------------------------------------------------
//
// Return longest match of data, in dictionary
//

DataString LongestMatch(const DataString& data, const DataString& dictionary)
{
	DataString result;
	result.pData = nullptr;
	result.size = 0;

	// Find the longest matching data in the dictionary
	if ((dictionary.size > 0) && (data.size > 0))
	{
		DataString candidate;
		candidate.pData = data.pData;
		candidate.size = 0;

		// First Check for a pattern / run-length style match
		// Check the end of the dictionary, to see if this data could be a
		// pattern "run" (where we can repeat a pattern for X many times for free
		// using the memcpy with overlapping source/dest buffers)
		// (This is a dictionary based pattern run/length)
		{
			// Check for pattern sizes, start small
			int max_pattern_size = 4096;
			if (dictionary.size < max_pattern_size)  max_pattern_size = dictionary.size;
			if (data.size < max_pattern_size) max_pattern_size = data.size;

			for (int pattern_size = 1; pattern_size <= max_pattern_size; ++pattern_size)
			{
				int pattern_start = dictionary.size - pattern_size;

				for (int dataIndex = 0; dataIndex < data.size; ++dataIndex)
				{
					if (data.pData[ dataIndex ] == dictionary.pData[ pattern_start + (dataIndex % pattern_size) ])
					{
						candidate.pData = dictionary.pData + pattern_start;
						candidate.size = dataIndex+1;
						continue;
					}

					break;
				}

				//if (candidate.size < pattern_size)
				//	break;

				if (candidate.size > result.size)
				{
					result = candidate;
				}
			}
		}

		// As an optimization
		int dictionarySize = dictionary.size; // - 1;	// This last string has already been checked by, the
												    // run-length matcher above

		// As the size grows, we're missing potential matches in here
		// I think the best way to counter this is to attempt somthing
		// like KMP

		if (dictionarySize > candidate.size)
		{
			// Check the dictionary for a match, brute force
			for (int dictionaryIndex = 0; dictionaryIndex <= (dictionarySize-candidate.size); ++dictionaryIndex)
			{
				int sizeAvailable = dictionarySize - dictionaryIndex;

				if (sizeAvailable > data.size) sizeAvailable = data.size;

				// this could index off the end of the dictionary!!! FIX ME
				for (int dataIndex = 0; dataIndex < sizeAvailable; ++dataIndex)
				{
					if (data.pData[ dataIndex ] == dictionary.pData[ dictionaryIndex + dataIndex ])
					{
						if (dataIndex >= candidate.size)
						{
							candidate.pData = dictionary.pData + dictionaryIndex;
							candidate.size = dataIndex + 1;
						}
						continue;
					}

					break;
				}

				if (candidate.size > result.size)
				{
					result = candidate;
					//dictionaryIndex = -1;
					break;
				}
			}
		}
	}

	return result;
}
//------------------------------------------------------------------------------
DataString LongestMatch(const DataString& data, const DataString& dictionary, int cursorPosition)
{
	DataString result;
	result.pData = nullptr;
	result.size = 0;

	// Find the longest matching data in the dictionary
	if ((dictionary.size > 0) && (data.size > 0))
	{
		DataString candidate;
		candidate.pData = data.pData;
		candidate.size = 0;

		// First Check for a pattern / run-length style match
		// Check the end of the dictionary, to see if this data could be a
		// pattern "run" (where we can repeat a pattern for X many times for free
		// using the memcpy with overlapping source/dest buffers)
		// (This is a dictionary based pattern run/length)
		{
			// Check for pattern sizes, start small
			int max_pattern_size = 4096;
			if (cursorPosition < max_pattern_size)  max_pattern_size = cursorPosition;
			if (data.size < max_pattern_size) max_pattern_size = data.size;

			for (int pattern_size = 1; pattern_size <= max_pattern_size; ++pattern_size)
			{
				int pattern_start = cursorPosition - pattern_size;

				for (int dataIndex = 0; dataIndex < data.size; ++dataIndex)
				{
					if (data.pData[ dataIndex ] == dictionary.pData[ pattern_start + (dataIndex % pattern_size) ])
					{
						candidate.pData = dictionary.pData + pattern_start;
						candidate.size = dataIndex+1;
						continue;
					}

					break;
				}

				if (candidate.size > result.size)
				{
					result = candidate;
				}
			}
		}

		// This will keep us from finding matches that we can't use

		int dictionarySize = cursorPosition;

		// As the size grows, we're missing potential matches in here
		// I think the best way to counter this is to attempt somthing
		// like KMP

		if (dictionarySize > candidate.size)
		{
			// Check the dictionary for a match, brute force
			for (int dictionaryIndex = 0; dictionaryIndex <= (dictionarySize-candidate.size); ++dictionaryIndex)
			{
				int sizeAvailable = dictionarySize - dictionaryIndex;

				if (sizeAvailable > data.size) sizeAvailable = data.size;

				// this could index off the end of the dictionary!!! FIX ME
				for (int dataIndex = 0; dataIndex < sizeAvailable; ++dataIndex)
				{
					if (data.pData[ dataIndex ] == dictionary.pData[ dictionaryIndex + dataIndex ])
					{
						if (dataIndex >= candidate.size)
						{
							candidate.pData = dictionary.pData + dictionaryIndex;
							candidate.size = dataIndex + 1;
						}
						continue;
					}

					break;
				}

				if (candidate.size > result.size)
				{
					result = candidate;
					//dictionaryIndex = -1;
					break;
				}
			}
		}

		// Look for matches beyond the cursor
		dictionarySize = dictionary.size;

		if ((dictionarySize-cursorPosition) > candidate.size)
		{
			// Check the dictionary for a match, brute force
			for (int dictionaryIndex = cursorPosition; dictionaryIndex <= (dictionarySize-candidate.size); ++dictionaryIndex)
			{
				int sizeAvailable = dictionarySize - dictionaryIndex;

				if (sizeAvailable > data.size) sizeAvailable = data.size;

				// this could index off the end of the dictionary!!! FIX ME
				for (int dataIndex = 0; dataIndex < sizeAvailable; ++dataIndex)
				{
					if (data.pData[ dataIndex ] == dictionary.pData[ dictionaryIndex + dataIndex ])
					{
						if (dataIndex >= candidate.size)
						{
							candidate.pData = dictionary.pData + dictionaryIndex;
							candidate.size = dataIndex + 1;
						}
						continue;
					}

					break;
				}

				if (candidate.size > result.size)
				{
					result = candidate;
					break;
				}
			}
		}

	}

	return result;
}

//------------------------------------------------------------------------------
//
// Return offset into dictionary where the string matches
//
// -1 means, no match
//
static int DictionaryMatch(const DataString& data, int dictionarySize)
{
	if( (0 == dictionarySize ) ||
		(0 == data.size) ||
		(data.size > 16384) ) // 16384 is largest string copy we can encode
	{
		return -1;
	}

	// Check the end of the dictionary, to see if this data could be a
	// pattern "run" (where we can repeat a pattern for X many times for free
	// using the memcpy with overlapping source/dest buffers)
	// (This is a dictionary based pattern run/length)

	{
		// Check for pattern sizes, start small
		int max_pattern_size = 256;
		if (dictionarySize < max_pattern_size)  max_pattern_size = dictionarySize;
		if (data.size < max_pattern_size) max_pattern_size = data.size;

		for (int pattern_size = 1; pattern_size <= max_pattern_size; ++pattern_size)
		{
			bool bMatch = true;
			int pattern_start = dictionarySize - pattern_size;

			for (int dataIndex = 0; dataIndex < data.size; ++dataIndex)
			{
				if (data.pData[ dataIndex ] == pDictionary[ pattern_start + (dataIndex % pattern_size) ])
					continue;

				bMatch = false;
				break;
			}

			if (bMatch)
			{
				// Return a RLE Style match result
				return pattern_start;
			}
		}
	}

	// As an optimization
	dictionarySize -= 1;	// This last string has already been checked by, the
						    // run-length matcher above

	if (dictionarySize < data.size)
	{
		return -1;
	}

	int result = -1;

	// Check the dictionary for a match, brute force
	for (int idx = 0; idx <= (dictionarySize-data.size); ++idx)
	{
		bool bMatch = true;
		for (int dataIdx = 0; dataIdx < data.size; ++dataIdx)
		{
			if (data.pData[ dataIdx ] == pDictionary[ idx + dataIdx ])
				continue;

			bMatch = false;
			break;
		}

		if (bMatch)
		{
			result = idx;
			break;
		}
	}

	return result;
}

//------------------------------------------------------------------------------
//
// Emit a literal, that appends itself to an existing literal
//
static int ConcatLiteral(unsigned char *pDest, DataString& data)
{
	// Return Size
	int outSize = (int)data.size;

	int opCode  = pDest[0];
	    opCode |= (int)(((pDest[1])&0x7F)<<8);

	opCode>>=1;
	opCode+=1;
	// opCode contains the length of the literal that's already encoded

    int skip = opCode;
	opCode += outSize;

	// Opcode
	opCode -= 1;
	opCode <<=1;
	opCode |= 1;

	*pDest++ = (unsigned char)(opCode & 0xFF);
	*pDest++ = (unsigned char)((opCode >> 8) & 0x7F);

	pDest += skip;

	// Literal Data
	for (int idx = 0; idx < data.size; ++idx)
	{
		*pDest++ = data.pData[ idx ];
	}

	// Clear
	data.pData += data.size;
	data.size = 0;

	return outSize;
}

//------------------------------------------------------------------------------

static int EmitLiteral(unsigned char *pDest, DataString& data)
{
	// Return Size
	int outSize = 2 + (int)data.size;

	unsigned short length  = (unsigned short)data.size;
	length -= 1;

	unsigned short opcode = length<<1;
	opcode |= 0x0001;

	// Opcode out
	*pDest++ = (unsigned char)( opcode & 0xFF );
	*pDest++ = (unsigned char)(( opcode>>8)&0xFF);

	// Literal Data
	for (int idx = 0; idx < data.size; ++idx)
	{
		*pDest++ = data.pData[ idx ];
	}

	// Clear
	data.pData += data.size;
	data.size = 0;

	return outSize;
}

//------------------------------------------------------------------------------

static int EmitReference(unsigned char *pDest, int dictionaryOffset, DataString& data)
{
	// Return Size
	int outSize = 2 + 2;

	unsigned short length  = (unsigned short)data.size;
	length -= 1;

	unsigned short opcode = length<<1;
	opcode |= 0x8000;

	// Opcode out
	*pDest++ = (unsigned char)( opcode & 0xFF );
	*pDest++ = (unsigned char)(( opcode>>8)&0xFF);

	// Destination Address out
	unsigned short address = (unsigned short)dictionaryOffset;
	address += 0x2000;	// So we don't have to add $2000 in the animation player

	*pDest++ = (unsigned char)(address & 0xFF);
	*pDest++ = (unsigned char)((address>>8)&0xFF);

	// Clear
	data.pData += data.size;
	data.size = 0;

	return outSize;
}

//------------------------------------------------------------------------------
//
// Std C memcpy seems to be stopping the copy from happening, when I overlap
// the buffer to get a pattern run copy (overlapped buffers)
//
static void my_memcpy(u8* pDest, u8* pSrc, int length)
{
	while (length-- > 0)
	{
		*pDest++ = *pSrc++;
	}
}

//------------------------------------------------------------------------------
//
// Emit a Cursor Skip forward opcode
//
int EmitSkip(unsigned char* pDest, int skipSize)
{
	int outSize = 2;

	unsigned short length  = (unsigned short)skipSize;
	length -= 1;

	unsigned short opcode = length<<1;
	opcode |= 0x8001;
	// Opcode out
	*pDest++ = (unsigned char)( opcode & 0xFF );
	*pDest++ = (unsigned char)(( opcode>>8)&0xFF);

	return outSize;
}

//------------------------------------------------------------------------------
//
// Forcibly Emit a source Skip Opcode
// 
// return space_left_in_Bank
//
int EmitSourceSkip(unsigned char*& pDest, int space_left_in_bank)
{
	assert(space_left_in_bank >= 2);

	*pDest++ = 0;
	*pDest++ = 0;
	space_left_in_bank-=2;

	while (space_left_in_bank)
	{
		space_left_in_bank--;
		*pDest++ = 0;
	}

	return 0x10000;
}

//------------------------------------------------------------------------------
//
// Conditionally shit out the Source Bank Skip
//
int CheckEmitSourceSkip(int checkSpace, unsigned char*& pDest, int space_left_in_bank)
{
	if ((checkSpace+2) > space_left_in_bank)
	{
		return EmitSourceSkip(pDest, space_left_in_bank);
	}

	return space_left_in_bank;
}

//------------------------------------------------------------------------------
//
// Compress a Frame in the GSLA LZB Format
// 
// The dictionary is also the canvas, so when we're finished the dictionary
// buffer will match the original pSource buffer
// 
// If they both match to begin with, we just crap out an End of Frame opcode
//
int LZBA_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize,
				  unsigned char* pDataStart, unsigned char* pDictionary,
				  int dictionarySize)
{
//	printf("LZBA Compress %d bytes\n", sourceSize);

	// Used for bank skip opcode emission
	int bankOffset = (int)((pDest - pDataStart) & 0xFFFF);

	// So we can track how big our compressed data ends up being
	unsigned char *pOriginalDest = pDest;

	DataString sourceData;
	DataString dictionaryData;
	DataString candidateData;

	// Source Data Stream - will compress until the size is zero
	sourceData.pData = pSource;
	sourceData.size  = sourceSize;

	// Dictionary is the Frame Buffer
	dictionaryData.pData = pDictionary;
	dictionaryData.size = dictionarySize;

	// dumb last emit is a literal stuff
	bool bLastEmitIsLiteral = false;
	unsigned char* pLastLiteralDest = nullptr;

	int lastEmittedCursorPosition = 0; // This is the default for each frame

	int space_left_in_bank = (int)0x10000 - (int)((pDest - pDataStart)&0xFFFF);

   	space_left_in_bank = CheckEmitSourceSkip(0, pDest, space_left_in_bank);

	for (int cursorPosition = 0; cursorPosition < dictionarySize;)
	{
		if (pSource[ cursorPosition ] != pDictionary[ cursorPosition ])
		{
			// Here is some data that has to be processed, so let's decide
			// how large of a chunk of data we're looking at here

			// Do we need to emit a Skip opcode?, compare cursor to last emit
			// and emit a skip command if we need it (I'm going want a gap of
			// at least 3 bytes? before we call it the end
			int skipSize = cursorPosition - lastEmittedCursorPosition;

			if (skipSize)
			{
				space_left_in_bank = CheckEmitSourceSkip(2, pDest, space_left_in_bank);

				// We need to Skip
				pDest += EmitSkip(pDest, skipSize);
				bLastEmitIsLiteral = false;
				lastEmittedCursorPosition = cursorPosition;

				space_left_in_bank = (int)0x10000 - (int)((pDest - pDataStart)&0xFFFF);
			}

			int tempCursorPosition = cursorPosition;
			int gapCount = 0;
			for (; tempCursorPosition < dictionarySize; ++tempCursorPosition)
			{
				if (pSource[ cursorPosition ] != pDictionary[ cursorPosition ])
				{
					gapCount = 0;
				}
				else
				{
					gapCount++;
					if (gapCount >= 3)
						break;
				}
			}

			tempCursorPosition -= gapCount;

			// Now we know from cursorPosition to tempCursorPosition is data
			// that we want to encode, we either literally copy it, or look
			// to see if this data is already in the dictionary (so we can copy
			// it from one part of the frame buffer to another part)

			sourceData.pData = &pSource[ cursorPosition ];
			sourceData.size = tempCursorPosition - cursorPosition;

			while (sourceData.size > 0)
			{
				candidateData = LongestMatch(sourceData, dictionaryData, cursorPosition);

				// If no match, or the match is too small, then take the next byte
				// and emit as literal
				if ((0 == candidateData.size)) // || (candidateData.size < 4))
				{
					candidateData.size = 1;
					candidateData.pData = sourceData.pData;
				}

				// Adjust source stream
				sourceData.pData += candidateData.size;
				sourceData.size  -= candidateData.size;

				// Modify the dictionary
				cursorPosition = AddDictionary(candidateData, cursorPosition);
				lastEmittedCursorPosition = cursorPosition;

				if (candidateData.size > 3)
				{
					space_left_in_bank = CheckEmitSourceSkip(4, pDest, space_left_in_bank);

					// Emit a dictionary reference
					pDest += (int)EmitReference(pDest, (int)(candidateData.pData - dictionaryData.pData), candidateData);
					bLastEmitIsLiteral = false;

					space_left_in_bank = (int)0x10000 - (int)((pDest - pDataStart)&0xFFFF);
				}
				else if (bLastEmitIsLiteral)
				{
					// This is a problem for the source bank skip, we can't
					// concatenate if we end up injecting a source bank skip opcode
					// into the stream...  what to do???, if insert, we will need to
					// do a "normal" literal emission, ugly

					int space = CheckEmitSourceSkip(candidateData.size, pDest, space_left_in_bank);

					if (space != space_left_in_bank)
					{
						// Emit a new literal
						pLastLiteralDest = pDest;
						bLastEmitIsLiteral = true;
						pDest += EmitLiteral(pDest, candidateData);

						space_left_in_bank = (int)0x10000 - (int)((pDest - pDataStart)&0xFFFF);
					}
					else
					{
						// Concatenate this literal onto the previous literal
						pDest += ConcatLiteral(pLastLiteralDest, candidateData);
					}
				}
				else
				{
					space_left_in_bank = CheckEmitSourceSkip(2 + candidateData.size, pDest, space_left_in_bank);

					// Emit a new literal
					pLastLiteralDest = pDest;
					bLastEmitIsLiteral = true;
					pDest += EmitLiteral(pDest, candidateData);

					space_left_in_bank = (int)0x10000 - (int)((pDest - pDataStart)&0xFFFF);
				}
			}
		}
		else
		{
			// no change
			cursorPosition++;
		}
	}

   	space_left_in_bank = CheckEmitSourceSkip(2, pDest, space_left_in_bank);

	// Emit the End of Frame Opcode
	*pDest++ = 0x02;
	*pDest++ = 0x00;

	return (int)(pDest - pOriginalDest);

}

//------------------------------------------------------------------------------

