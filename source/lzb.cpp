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
static int EmitReference(unsigned char *pDest, int dictionaryOffset, std::vector<unsigned char>& data);
static int DictionaryMatch(const std::vector<unsigned char>& data, int dictionarySize);

int LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize)
{
	printf("LZB_Compress %d bytes\n", sourceSize);

	// anything less than 3 bytes, is going to be a literal match

	int processedBytes = 0;
	int bytesInDictionary = 0;
	int bytesEmitted = 0;

	std::vector<unsigned char> candidate_data;

	while (processedBytes < sourceSize)
	{
		unsigned char byte_data = pSource[ processedBytes++ ];
		candidate_data.push_back(byte_data);

		// The dictionary only contains bytes that have been emitted, so we
		// can't add this byte until we've emitted it?

		if (candidate_data.size() < 3) continue;

		if (DictionaryMatch(candidate_data, bytesInDictionary) < 0)
		{
			// Was there a dictionary match
			std::vector<unsigned char> prev_data = candidate_data;
			prev_data.pop_back();

			int MatchOffset = DictionaryMatch(prev_data, bytesInDictionary);

			if ((MatchOffset >= 0) && prev_data.size() > 2)
			{
				bytesInDictionary = AddDictionary(prev_data, bytesInDictionary);
				bytesEmitted += EmitReference(pDest + bytesEmitted, MatchOffset, prev_data);
				candidate_data[0] = candidate_data[ candidate_data.size() - 1 ];
				candidate_data.resize(1);
			}
			else
			{
				// Add Dictionary
				bytesInDictionary = AddDictionary(candidate_data, bytesInDictionary);
				bytesEmitted += EmitLiteral(pDest + bytesEmitted, candidate_data);
			}
		}
	}

	if (candidate_data.size() > 0)
	{
		// Emit as a literal? (we have 1 more chance here for a match
		// Add Dictionary
		bytesInDictionary = AddDictionary(candidate_data, bytesInDictionary);
		bytesEmitted += EmitLiteral(pDest + bytesEmitted, candidate_data);
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
	if(dictionarySize < data.size())
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

			memcpy(&pDest[ decompressedBytes ], &pDest[ offset ], opcode);
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

