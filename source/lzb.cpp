//
// LZB Encode / Decode
// 
#include "lzb.h"

#include <stdio.h>
#include <vector>

#include "bctypes.h"

#define DICTIONARY_SIZE (32 * 1024)
//
// Yes This is a 32K Buffer, of bytes, with no structure to it
//
static unsigned char Dictionary[ DICTIONARY_SIZE ];

static int AddDictionary(const std::vector<unsigned char>&data, int dictionarySize);
static int EmitLiteral(unsigned char *pDest, std::vector<unsigned char>& data);
static int ConcatLiteral(unsigned char *pDest, std::vector<unsigned char>& data);
static int EmitReference(unsigned char *pDest, int dictionaryOffset, std::vector<unsigned char>& data);
static int DictionaryMatch(const std::vector<unsigned char>& data, int dictionarySize);

int LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize)
{
	printf("LZB_Compress %d bytes\n", sourceSize);

	// anything less than 3 bytes, is going to be a literal match

	int processedBytes = 0;
	int bytesInDictionary = 0;
	int bytesEmitted = 0;

	// dumb last emit is a literal stuff
	bool bLastEmitIsLiteral = false;
	int  lastEmittedLiteralOffset = 0;

	std::vector<unsigned char> candidate_data;

	while (processedBytes < sourceSize)
	{
		unsigned char byte_data = pSource[ processedBytes++ ];
		candidate_data.push_back(byte_data);

		// If there's a match, then add to the candidate data, and see if
		// there's a bigger match (use previous result to speed up search)
		// (KMP is probably the last planned optmization here)


		// The dictionary only contains bytes that have been emitted, so we
		// can't add this byte until we've emitted it?
		if (DictionaryMatch(candidate_data, bytesInDictionary) < 0)
		{
			// Was there a dictionary match
			std::vector<unsigned char> prev_data = candidate_data;
			prev_data.pop_back();

			int MatchOffset = DictionaryMatch(prev_data, bytesInDictionary);

			if ((MatchOffset >= 0) && prev_data.size() > 3)
			{
				bytesInDictionary = AddDictionary(prev_data, bytesInDictionary);
				bytesEmitted += EmitReference(pDest + bytesEmitted, MatchOffset, prev_data);
				candidate_data[0] = candidate_data[ candidate_data.size() - 1 ];
				candidate_data.resize(1);
				bLastEmitIsLiteral = false;
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
					lastEmittedLiteralOffset = bytesEmitted;
					bytesEmitted += EmitLiteral(pDest + bytesEmitted, candidate_data);
				}
				bLastEmitIsLiteral = true;
			}
		}
	}

	if (candidate_data.size() > 0)
	{

		int MatchOffset = DictionaryMatch(candidate_data, bytesInDictionary);

		if ((MatchOffset >=0) && candidate_data.size() > 2)
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
static int AddDictionary(const std::vector<unsigned char>&data, int dictionarySize)
{
	int dataIndex = 0;

	while ((dictionarySize < DICTIONARY_SIZE) && (dataIndex < data.size()))
	{
		Dictionary[ dictionarySize++ ] = data[ dataIndex++ ];
	}

	return dictionarySize;
}

//------------------------------------------------------------------------------
//
// Return offset into dictionary where the string matches
//
// -1 means, no match
//
static int DictionaryMatch(const std::vector<unsigned char>& data, int dictionarySize)
{
	if( (0 == dictionarySize ) ||
		(0 == data.size()) ||
		(data.size() > 16384) ) // 16384 is largest string copy we can encode
	{
		return -1;
	}

	// Check the end of the dictionary, to see if this data could be a
	// pattern "run" (where we can repeat a pattern for X many times for free
	// using the memcpy with overlapping source/dest buffers)
	// (This is a dictionary based pattern run/length)

	{
		// Check for pattern sizes, start small
		int max_pattern_size = dictionarySize;

		for (int pattern_size = 1; pattern_size <= max_pattern_size; ++pattern_size)
		{
			bool bMatch = true;
			int pattern_start = dictionarySize - pattern_size;

			for (int dataIndex = 0; dataIndex < data.size(); ++dataIndex)
			{
				if (data[ dataIndex ] == Dictionary[ pattern_start + (dataIndex % pattern_size) ])
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

	if (dictionarySize < data.size())
	{
		return -1;
	}

	int dictionaryOffset = 0;

	int result = -1;

	// Check the dictionary for a match, brute force
	for (int idx = 0; idx <= (dictionarySize-data.size()); ++idx)
	{
		bool bMatch = true;
		for (int dataIdx = 0; dataIdx < data.size(); ++dataIdx)
		{
			if (data[ dataIdx ] == Dictionary[ idx + dataIdx ])
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
static int ConcatLiteral(unsigned char *pDest, std::vector<unsigned char>& data)
{
	// Return Size
	int outSize = (int)data.size();

	int opCode  = pDest[0];
	    opCode |= (int)(((pDest[1])&0x7F)<<8);

    int skip = opCode;
	opCode += outSize;

	// Opcode
	*pDest++ = (unsigned char)(opCode & 0xFF);
	*pDest++ = (unsigned char)((opCode >> 8) & 0x7F);

	pDest += skip;

	// Literal Data
	for (int idx = 0; idx < data.size(); ++idx)
	{
		*pDest++ = data[ idx ];
	}

	data.clear();

	return outSize;
}

//------------------------------------------------------------------------------

static int EmitLiteral(unsigned char *pDest, std::vector<unsigned char>& data)
{
	// Return Size
	int outSize = 2 + (int)data.size();

	// Opcode
	*pDest++ = (unsigned char)(data.size() & 0xFF);
	*pDest++ = (unsigned char)((data.size() >> 8) & 0x7F);

	// Literal Data
	for (int idx = 0; idx < data.size(); ++idx)
	{
		*pDest++ = data[ idx ];
	}

	data.clear();

	return outSize;
}

//------------------------------------------------------------------------------

static int EmitReference(unsigned char *pDest, int dictionaryOffset, std::vector<unsigned char>& data)
{
	// Return Size
	int outSize = 2 + 2;

	// Opcode
	*pDest++ = (unsigned char)(data.size() & 0xFF);
	*pDest++ = (unsigned char)((data.size() >> 8) & 0x7F) | 0x80;

	*pDest++ = (unsigned char)(dictionaryOffset & 0xFF);
	*pDest++ = (unsigned char)((dictionaryOffset>>8) & 0xFF);

	data.clear();

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
//  Simple Decompress, for validation
//
void LZB_Decompress(unsigned char* pDest, unsigned char* pSource, int destSize)
{
	int decompressedBytes = 0;

	while (decompressedBytes < destSize)
	{
		u16 opcode  = *pSource++;
		    opcode |= ((u16)(*pSource++))<<8;

		if (opcode & 0x8000)
		{
			// Dictionary
			opcode &= 0x7FFF;

			// Dictionary Copy from the output stream
			u16 offset  = *pSource++;
			    offset |= ((u16)(*pSource++))<<8;

			my_memcpy(&pDest[ decompressedBytes ], &pDest[ offset ], opcode);
			decompressedBytes += opcode;
		}
		else
		{
			// Literal Copy, from compressed stream
			memcpy(&pDest[ decompressedBytes ], pSource, opcode);
			decompressedBytes += opcode;
			pSource += opcode;
		}
	}
}

//------------------------------------------------------------------------------

