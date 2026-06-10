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
static unsigned char *pGlobalDictionary = nullptr;

struct DataString {
	// Information about the data we're trying to match
	int size;
	unsigned char *pData;
};

//------------------------------------------------------------------------------
// Hash chain match finder.
//
// 4-byte rolling hash → doubly-linked list of dictionary positions per bucket.
// Replaces the prior O(D × M) brute-force scan in LongestMatch with O(chain × M).
//
// Doubly linked because variant 2 (LZBA) re-hashes positions when their bytes
// change.  A singly-linked chain would lose entries on re-insertion (the old
// chain becomes disconnected past the moved node), causing missed matches.
// O(1) remove-then-insert keeps both old and new buckets correct.
//
// One global state, reset at the start of each compressor invocation
// (HashChainReset) and fed new/changed positions via HashChainInsertRange.
//
// Tuning:
//   HASH_BITS   16   → 64K buckets, ~0.5 positions/bucket at 32K dict
//   MAX_CHAIN 4096   → walk at most this many positions per query
//   MIN_MATCH    4   → refs need length >=4 to win against literal concat;
//                      shorter matches are useless and a 4-byte hash filters
//                      out the catastrophic "00 00 00" bucket pile-up that a
//                      3-byte hash creates in image data.
#define HASH_BITS  16
#define HASH_SIZE  (1 << HASH_BITS)
#define HASH_MASK  (HASH_SIZE - 1)
#define MAX_CHAIN  4096
#define MIN_MATCH  4

static int s_hashHead  [HASH_SIZE];
static int s_hashNext  [MAX_DICTIONARY_SIZE];   // next  (older) position in bucket, -1 if tail
static int s_hashPrevDL[MAX_DICTIONARY_SIZE];   // prev  (newer) position in bucket, -1 if head
static int s_hashBucket[MAX_DICTIONARY_SIZE];   // which bucket position is in, -1 if none

static inline unsigned int Hash4(const unsigned char* p)
{
	unsigned int v = (unsigned int)p[0]
	               | ((unsigned int)p[1] << 8)
	               | ((unsigned int)p[2] << 16)
	               | ((unsigned int)p[3] << 24);
	return (v * 2654435761U) >> (32 - HASH_BITS);
}

static void HashChainReset()
{
	for (int i = 0; i < HASH_SIZE; ++i) s_hashHead[i] = -1;
	for (int i = 0; i < MAX_DICTIONARY_SIZE; ++i) s_hashBucket[i] = -1;
}

static inline void HashChainRemove(int p)
{
	int b = s_hashBucket[p];
	if (b < 0) return;
	int prev = s_hashPrevDL[p];
	int next = s_hashNext[p];
	if (prev >= 0) s_hashNext[prev] = next;
	else           s_hashHead[b]    = next;
	if (next >= 0) s_hashPrevDL[next] = prev;
	s_hashBucket[p] = -1;
}

// Insert positions [start, endExcl) into the chain, hashing the 4-byte window
// at each.  If a position was already in some bucket (from a prior insertion),
// remove it first so it ends up in exactly one bucket.  Skip positions that
// are already at the head of the correct bucket (already hashed identically).
// Caller is responsible for ensuring p+4 <= dictionary capacity for every p.
static void HashChainInsertRange(int start, int endExcl, const unsigned char* base)
{
	if (start < 0) start = 0;
	for (int p = start; p < endExcl; ++p)
	{
		int h = (int)Hash4(base + p);
		if (s_hashBucket[p] == h) continue; // already correct, nothing changed
		HashChainRemove(p);
		int oldHead = s_hashHead[h];
		s_hashNext  [p] = oldHead;
		s_hashPrevDL[p] = -1;
		if (oldHead >= 0) s_hashPrevDL[oldHead] = p;
		s_hashHead  [h] = p;
		s_hashBucket[p] = h;
	}
}

// Find the longest match for source[0..sourceSize) starting at any position
// in the chain that lies in [posLow, posHighExcl).  Reads from base[pos..pos+L)
// where L is bounded by both sourceSize and (dictReadLimitExcl - pos).
static DataString HashChainLongestMatch(
	const unsigned char* source, int sourceSize,
	const unsigned char* base,
	int posLow, int posHighExcl,
	int dictReadLimitExcl,
	int minLenInclusive)
{
	DataString result;
	result.pData = nullptr;
	result.size  = 0;

	if (sourceSize < MIN_MATCH) return result;
	if (posLow >= posHighExcl)  return result;

	// Match length is encoded in 14 bits + 1; matches longer than
	// MAX_STRING_SIZE cannot be represented in a single ref opcode.
	if (sourceSize > MAX_STRING_SIZE) sourceSize = MAX_STRING_SIZE;

	unsigned int h = Hash4(source);
	int pos = s_hashHead[h];
	int chainCount = 0;
	int bestLen = (minLenInclusive > 0) ? (minLenInclusive - 1) : 0; // strictly greater

	while (pos >= 0 && chainCount < MAX_CHAIN)
	{
		if (pos >= posLow && pos < posHighExcl)
		{
			int maxExtend = sourceSize;
			int dictAvail = dictReadLimitExcl - pos;
			if (dictAvail < maxExtend) maxExtend = dictAvail;

			// Quick reject: even the best possible extension can't beat current best.
			if (maxExtend > bestLen)
			{
				int len = 0;
				while (len < maxExtend && source[len] == base[pos + len]) ++len;
				if (len > bestLen)
				{
					bestLen = len;
					result.pData = (unsigned char*)(base + pos);
					result.size  = len;
				}
			}
		}
		pos = s_hashNext[pos];
		++chainCount;
	}

	return result;
}

static int AddDictionary(const DataString& data, int dictionarySize);
static int EmitLiteral(unsigned char *pDest, DataString& data);
static int ConcatLiteral(unsigned char *pDest, DataString& data);
static int EmitReference(unsigned char *pDest, int dictionaryOffset, DataString& data);
static int DictionaryMatch(const DataString& data, int dictionarySize);

// Stuff I need for a faster version
static DataString LongestMatch(const DataString& data, const DataString& dictionary);
static DataString LongestMatch(const DataString& data, const DataString& dictionary, int cursorPosition);

// Read the encoded length of an existing literal opcode (in bytes of data).
static inline int ExistingLiteralLength(const unsigned char* pLiteralOpcode)
{
	int opcode = pLiteralOpcode[0] | ((pLiteralOpcode[1] & 0x7F) << 8);
	return (opcode >> 1) + 1;
}

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
	pGlobalDictionary = pSource;
	dictionaryData.pData = pSource;
	dictionaryData.size = 0;

	// Reset the hash chain for this encode.  Variant 1: the dictionary grows
	// monotonically as we emit; HashChainInsertRange below extends the chain
	// each iteration with newly-formed 3-byte windows.
	HashChainReset();

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

		// Lazy matching: if this would be a ref, peek one byte ahead.  If a
		// strictly longer match starts at p+1, prefer emitting p as a literal
		// and let the next iteration take the longer ref.  The 4-byte cost of
		// the literal byte (1 in concat, ≤2 fresh) is dwarfed by the saving
		// from a longer ref over a shorter one in the typical case.
		if (candidateData.size > 3 && sourceData.size > 1)
		{
			DataString peekSource;
			peekSource.pData = sourceData.pData + 1;
			peekSource.size  = sourceData.size  - 1;
			DataString peek = LongestMatch(peekSource, dictionaryData);
			if (peek.size > candidateData.size)
			{
				candidateData.size = 1;
				candidateData.pData = sourceData.pData;
			}
		}

		// Adjust source stream
		sourceData.pData += candidateData.size;
		sourceData.size  -= candidateData.size;

		int oldDictSize = dictionaryData.size;
		dictionaryData.size = AddDictionary(candidateData, dictionaryData.size);

		// Newly-formed 4-byte windows: positions p where p+4 was previously
		// > oldDictSize and is now <= dictionaryData.size.  Range is
		// [oldDictSize-3, dictionaryData.size-3) clamped to [0, ...).
		HashChainInsertRange(oldDictSize - 3, dictionaryData.size - 3, dictionaryData.pData);

		// A 4-byte ref costs 4 output bytes; a 4-byte literal CONCATENATED onto
		// a previous literal opcode also costs 4 (no opcode overhead, just data).
		// Emitting the ref ends the literal stream, so the next non-ref emission
		// pays a fresh +2 opcode. Therefore prefer concat over ref when sizes tie.
		bool emitAsRef = (candidateData.size > 3) &&
		                 !(bLastEmitIsLiteral && candidateData.size == 4);

		if (emitAsRef)
		{
			// Emit a dictionary reference
			pDest += (int)EmitReference(pDest, (int)(candidateData.pData - dictionaryData.pData), candidateData);
			bLastEmitIsLiteral = false;
		}
		else if (bLastEmitIsLiteral &&
		         ExistingLiteralLength(pLastLiteralDest) + candidateData.size <= MAX_STRING_SIZE)
		{
			// Concatenate this literal onto the previous literal
			pDest += ConcatLiteral(pLastLiteralDest, candidateData);
		}
		else
		{
			// Emit a new literal (also the fallback when concat would overflow
			// the 15-bit length field).
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
	pGlobalDictionary = pSource;

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
		pGlobalDictionary[ dictionarySize++ ] = data.pData[ dataIndex++ ];
	}

	//dictionarySize += data.size;

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

		// Match length is encoded in 14 bits + 1; matches longer than
		// MAX_STRING_SIZE can't be represented in a single ref opcode.
		int searchSize = data.size;
		if (searchSize > MAX_STRING_SIZE) searchSize = MAX_STRING_SIZE;

		// First Check for a pattern / run-length style match
		// Check the end of the dictionary, to see if this data could be a
		// pattern "run" (where we can repeat a pattern for X many times for free
		// using the memcpy with overlapping source/dest buffers)
		// (This is a dictionary based pattern run/length)
		{
			// Check for pattern sizes, start small
			int max_pattern_size = 4096;
			if (dictionary.size < max_pattern_size)  max_pattern_size = dictionary.size;
			if (searchSize < max_pattern_size) max_pattern_size = searchSize;

			for (int pattern_size = 1; pattern_size <= max_pattern_size; ++pattern_size)
			{
				int pattern_start = dictionary.size - pattern_size;

				for (int dataIndex = 0; dataIndex < searchSize; ++dataIndex)
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

		// Hash-chain replacement for the prior O(D × M) brute-force scan.
		// Pattern-run loop above already covers tail-of-dictionary repeats;
		// hash chain finds matches anywhere else in [0, dictionary.size).
		DataString chainHit = HashChainLongestMatch(
			data.pData, data.size,
			dictionary.pData,
			0, dictionary.size,            // candidate position range
			dictionary.size,               // read bound
			result.size + 1);              // require strictly better than current

		if (chainHit.size > result.size)
		{
			result = chainHit;
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

		// Match length is encoded in 14 bits + 1; matches longer than
		// MAX_STRING_SIZE can't be represented in a single ref opcode.
		int searchSize = data.size;
		if (searchSize > MAX_STRING_SIZE) searchSize = MAX_STRING_SIZE;

		// First Check for a pattern / run-length style match
		// Check the end of the dictionary, to see if this data could be a
		// pattern "run" (where we can repeat a pattern for X many times for free
		// using the memcpy with overlapping source/dest buffers)
		// (This is a dictionary based pattern run/length)
		{
			// Check for pattern sizes, start small
			int max_pattern_size = 4096;
			if (cursorPosition < max_pattern_size)  max_pattern_size = cursorPosition;
			if (searchSize < max_pattern_size) max_pattern_size = searchSize;

			for (int pattern_size = 1; pattern_size <= max_pattern_size; ++pattern_size)
			{
				int pattern_start = cursorPosition - pattern_size;

				for (int dataIndex = 0; dataIndex < searchSize; ++dataIndex)
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

		// Not getting better than this
		if (result.size == data.size)
			return result;

		// Pre-cursor: candidates in [0, cursorPosition); reads bounded by
		// cursorPosition (positions there hold this-frame emitted bytes).
		DataString preHit = HashChainLongestMatch(
			data.pData, data.size,
			dictionary.pData,
			0, cursorPosition,
			cursorPosition,
			result.size + 1);
		if (preHit.size > result.size) result = preHit;

		// Not getting better than this
		if (result.size == data.size)
			return result;

		// Post-cursor: candidates in (cursorPosition, dictionary.size); reads
		// bounded by dictionary.size (positions there hold prior-frame bytes,
		// not yet overwritten this frame, still valid as ref source per the
		// format).
		DataString postHit = HashChainLongestMatch(
			data.pData, data.size,
			dictionary.pData,
			cursorPosition + 1, dictionary.size,
			dictionary.size,
			result.size + 1);
		if (postHit.size > result.size) result = postHit;
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
		(data.size > MAX_STRING_SIZE) ) // 16384 is largest string copy we can encode
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
				if (data.pData[ dataIndex ] == pGlobalDictionary[ pattern_start + (dataIndex % pattern_size) ])
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
			if (data.pData[ dataIdx ] == pGlobalDictionary[ idx + dataIdx ])
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

	assert(length < MAX_STRING_SIZE);

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

	assert(length < MAX_STRING_SIZE);

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
//static void my_memcpy(u8* pDest, u8* pSrc, int length)
//{
//	while (length-- > 0)
//	{
//		*pDest++ = *pSrc++;
//	}
//}

//------------------------------------------------------------------------------
//
// Emit one or more Cursor Skip forward opcode
//
int EmitSkip(unsigned char* pDest, int skipSize)
{
	int outSize = 0;
	int thisSkip = 0;

	while (skipSize > 0)
	{
		outSize+=2;

		thisSkip = skipSize;
		if (thisSkip > MAX_STRING_SIZE)
		{
			thisSkip = MAX_STRING_SIZE;
		}
		skipSize -= thisSkip;


		unsigned short length  = (unsigned short)thisSkip;
		length -= 1;

		assert(length < MAX_STRING_SIZE);

		unsigned short opcode = length<<1;
		opcode |= 0x8001;
		// Opcode out
		*pDest++ = (unsigned char)( opcode & 0xFF );
		*pDest++ = (unsigned char)(( opcode>>8)&0xFF);
	}

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

	space_left_in_bank -= checkSpace;

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
				  int dictionarySize, int gapMergeThreshold)
{
//	printf("LZBA Compress %d bytes\n", sourceSize);

	pGlobalDictionary = pDictionary;

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

	// Variant 2: pre-populate the hash chain over the entire canvas (all
	// positions hold prior-frame bytes that may serve as ref source).  As the
	// cursor advances and AddDictionary overwrites bytes, we re-insert the
	// affected positions; old chain entries become orphans but the byte-by-byte
	// compare in HashChainLongestMatch keeps results correct.
	HashChainReset();
	if (dictionarySize >= MIN_MATCH)
	{
		HashChainInsertRange(0, dictionarySize - 3, pDictionary);
	}

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
				int numSkips = (skipSize / MAX_STRING_SIZE) + 1;

				space_left_in_bank = CheckEmitSourceSkip(2 * numSkips, pDest, space_left_in_bank);

				// We need to Skip
				pDest += EmitSkip(pDest, skipSize);
				bLastEmitIsLiteral = false;
				lastEmittedCursorPosition = cursorPosition;
			}

			int tempCursorPosition = cursorPosition;
			int gapCount = 0;
			for (; tempCursorPosition < dictionarySize; ++tempCursorPosition)
			{
				if (pSource[ tempCursorPosition ] != pDictionary[ tempCursorPosition ])
				{
					gapCount = 0;
				}
				else
				{
					// if there's a small amount of matching data, let's include
					// it in the clump (try and reduce opcode emissions)
					if (gapCount >= gapMergeThreshold)
						break;
					gapCount++;
				}
			}

			tempCursorPosition -= gapCount;

			// Now we know from cursorPosition to tempCursorPosition is data
			// that we want to encode, we either literally copy it, or look
			// to see if this data is already in the dictionary (so we can copy
			// it from one part of the frame buffer to another part)

			sourceData.pData = &pSource[ cursorPosition ];
			sourceData.size = tempCursorPosition - cursorPosition;

			#if 0 // This Works
			//--------------------------  Dump, so skip dump only
			space_left_in_bank = CheckEmitSourceSkip(2+sourceData.size, pDest, space_left_in_bank);

			cursorPosition = AddDictionary(sourceData, cursorPosition);

			pDest += EmitLiteral(pDest, sourceData);
			lastEmittedCursorPosition = cursorPosition;
			#endif

			while (sourceData.size > 0)
			{
				// Inline skip detection: when the gap-merge threshold is high, a
				// chunk can contain long internal runs where pSource matches
				// pDictionary.  Skip = 2 bytes regardless of length; concat
				// literal = N bytes; fresh literal = 2+N; ref = 4.  Skip beats
				// concat at N>=3 immediately, but skip ENDS the literal stream
				// so the next emit pays an extra +2 (fresh opcode).  Net break-
				// even with concat is N=4; we use N=5 for a small safety margin.
				int matchRun = 0;
				while (matchRun < sourceData.size &&
				       sourceData.pData[matchRun] ==
				       pDictionary[cursorPosition + matchRun])
				{
					++matchRun;
				}
				if (matchRun >= 5)
				{
					// Emit one or more cursor-skip opcodes, with bank-skip
					// guarding (an EmitSkip emits ceil(matchRun/MAX_STRING_SIZE)
					// 2-byte opcodes; reserve worst-case in advance).
					int numSkips = (matchRun / MAX_STRING_SIZE) + 1;
					space_left_in_bank = CheckEmitSourceSkip(2 * numSkips, pDest, space_left_in_bank);
					pDest += EmitSkip(pDest, matchRun);
					bLastEmitIsLiteral = false;

					sourceData.pData += matchRun;
					sourceData.size  -= matchRun;
					cursorPosition += matchRun;
					lastEmittedCursorPosition = cursorPosition;
					continue;
				}

				candidateData = LongestMatch(sourceData, dictionaryData, cursorPosition);

				// If no match, or the match is too small, then take the next byte
				// and emit as literal
				if ((0 == candidateData.size)) // || (candidateData.size < 4))
				{
					candidateData.size = 1;
					candidateData.pData = sourceData.pData;
				}

				// Lazy matching: if this match would emit a ref, peek one byte
				// ahead.  If a strictly longer match starts at cursor+1, take a
				// 1-byte literal here and let the next iteration emit the longer
				// ref.  Cheap branch in an offline encoder; typical 5-10% win.
				if (candidateData.size > 3 && sourceData.size > 1)
				{
					DataString peekSource;
					peekSource.pData = sourceData.pData + 1;
					peekSource.size  = sourceData.size  - 1;
					DataString peek = LongestMatch(peekSource, dictionaryData, cursorPosition + 1);
					if (peek.size > candidateData.size)
					{
						candidateData.size = 1;
						candidateData.pData = sourceData.pData;
					}
				}

				// Capture the source bytes before sourceData advances. Literal
				// emission must read from pSource, not from candidateData.pData
				// (which may point into pDictionary at a position AddDictionary
				// is about to overwrite — this is what restricted the future-
				// cursor search to cursorPosition+3 in the original encoder).
				DataString litData;
				litData.pData = sourceData.pData;
				litData.size = candidateData.size;

				// Adjust source stream
				sourceData.pData += candidateData.size;
				sourceData.size  -= candidateData.size;

				// Modify the dictionary
				int oldCursor = cursorPosition;
				cursorPosition = AddDictionary(candidateData, cursorPosition);
				lastEmittedCursorPosition = cursorPosition;

				// Re-insert hash entries for positions whose 4-byte window
				// touches the modified range [oldCursor, cursorPosition).
				// That's start positions [oldCursor-3, cursorPosition-1],
				// bounded so p+4 <= dictionary capacity (frame size).
				int reinsertEnd = cursorPosition;
				if (reinsertEnd > dictionaryData.size - 3) reinsertEnd = dictionaryData.size - 3;
				HashChainInsertRange(oldCursor - 3, reinsertEnd, pDictionary);

				// A 4-byte ref costs 4 output bytes; a 4-byte literal CONCATENATED
				// onto a previous literal also costs 4. Emitting the ref ends the
				// literal stream, so the next non-ref emission pays +2. Prefer
				// concat over a tied-cost ref.
				bool emitAsRef = (candidateData.size > 3) &&
				                 !(bLastEmitIsLiteral && candidateData.size == 4);

				if (emitAsRef)
				{
					space_left_in_bank = CheckEmitSourceSkip(4, pDest, space_left_in_bank);

					// Emit a dictionary reference
					pDest += (int)EmitReference(pDest, (int)(candidateData.pData - dictionaryData.pData), candidateData);
					bLastEmitIsLiteral = false;

				}
				else if (bLastEmitIsLiteral)
				{
					// Two reasons we may have to fall back to a fresh literal:
					//   (1) a source-bank-skip opcode would be injected mid-stream,
					//   (2) concatenation would overflow the 15-bit length field.
					bool concatOverflow =
					    ExistingLiteralLength(pLastLiteralDest) + litData.size > MAX_STRING_SIZE;

					int space = CheckEmitSourceSkip(litData.size, pDest, space_left_in_bank);

					if (concatOverflow || space != (space_left_in_bank - litData.size))
					{
						space_left_in_bank = (space != (space_left_in_bank - litData.size))
						                     ? (space - 2)
						                     : space;

						// Emit a new literal
						pLastLiteralDest = pDest;
						pDest += EmitLiteral(pDest, litData);
					}
					else
					{
						// Concatenate this literal onto the previous literal
						space_left_in_bank = space;
						pDest += ConcatLiteral(pLastLiteralDest, litData);
					}
				}
				else
				{
					space_left_in_bank = CheckEmitSourceSkip(2 + litData.size, pDest, space_left_in_bank);

					// Emit a new literal
					pLastLiteralDest = pDest;
					bLastEmitIsLiteral = true;
					pDest += EmitLiteral(pDest, litData);
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

	for (int idx = 0; idx < dictionarySize; ++idx)
	{
		if (pSource[ idx ] != pDictionary[ idx ])
		{
			assert(0);
		}
	}

	return (int)(pDest - pOriginalDest);

}

//==============================================================================
//
// Phase 2: Optimal-parse encoder
//
// The greedy encoders above decide literal/ref/skip locally (with 1-byte lazy
// matching and a fixed gap-merge threshold).  This encoder instead runs a
// shortest-path dynamic program over the whole frame with the exact opcode
// cost model, so every literal/ref/skip boundary is globally optimal.
//
// Why a DP is valid here: when the decode cursor sits at position i, the
// canvas state is fully determined no matter which opcodes got it there —
// positions < i hold new-frame bytes (everything emitted writes new data, and
// skipped bytes were already equal), positions >= i still hold old-frame
// bytes.  So the set of matches available at i is path-independent.
//
// Match semantics against that virtual canvas (player copies forward,
// byte-by-byte, with overlap allowed):
//   d < i : every read position either holds an already-emitted new byte, or
//           is inside the copy's own dest range and was just written with a
//           new byte (the overlapped pattern-run trick).  Either way the
//           condition reduces to pSource[d+k] == pSource[i+k] — a plain
//           self-match on the NEW frame.
//   d > i : reads always run ahead of the writes, so every read sees the old
//           canvas: condition is pDictionary[d+k] == pSource[i+k].
//
// Cost model (output bytes):
//   ref               4, ends the literal stream
//   skip              2 per <=16384, ends the literal stream, only over bytes
//                     where new == old
//   literal byte      1 while a literal opcode is open, +2 to open one
//
// States: (position, 0=no open literal, 1=literal open).  Refs and skips are
// flat-cost over a contiguous range of landing positions, so they're relaxed
// with a range-min segment tree (range update, point query) instead of one
// edge per length.
//
//==============================================================================

// Tuning (sizes/times on falcon-new.gsla, 256 frames):
//   chain 1024 → 280,565 @ 4.0s;  2048 → 279,870 @ 6.3s;  4096 → 279,154 @ 11.5s
//   margin 8 + landing hints sits within 0.3% of an unpruned DP (279,990 @ 313s)
#define OPT_MAX_CHAIN        2048     // chain walk cap at queried positions
#define OPT_RUN_QUERY_MARGIN 8        // query matches this close to a run end
#define OPT_DEEP_RUN_CHAIN   0        // no match query deep inside unchanged runs
#define OPT_SEG_LEAVES       65536    // power of two >= MAX_DICTIONARY_SIZE+1
#define OPT_COST_INF         0x3FFFFFFF

#define OPT_TYPE_LIT   0
#define OPT_TYPE_SKIP  1
#define OPT_TYPE_REF   2

// Two independent hash chains (the greedy encoders' single chain tracks the
// mutating canvas; the DP needs both views of it at every position).
struct OptHashChain
{
	int head  [HASH_SIZE];
	int next  [MAX_DICTIONARY_SIZE];
	int prevDL[MAX_DICTIONARY_SIZE];
	int bucket[MAX_DICTIONARY_SIZE];
};

static OptHashChain s_chainPre;   // new-frame self matches, positions < cursor
static OptHashChain s_chainPost;  // old-frame matches, positions > cursor

static void OptChainReset(OptHashChain& c)
{
	for (int i = 0; i < HASH_SIZE; ++i) c.head[i] = -1;
	for (int i = 0; i < MAX_DICTIONARY_SIZE; ++i) c.bucket[i] = -1;
}

static inline void OptChainInsert(OptHashChain& c, int p, const unsigned char* base)
{
	int h = (int)Hash4(base + p);
	int oldHead = c.head[h];
	c.next  [p] = oldHead;
	c.prevDL[p] = -1;
	if (oldHead >= 0) c.prevDL[oldHead] = p;
	c.head  [h] = p;
	c.bucket[p] = h;
}

static inline void OptChainRemove(OptHashChain& c, int p)
{
	int b = c.bucket[p];
	if (b < 0) return;
	int prev = c.prevDL[p];
	int next = c.next[p];
	if (prev >= 0) c.next[prev] = next;
	else           c.head[b]    = next;
	if (next >= 0) c.prevDL[next] = prev;
	c.bucket[p] = -1;
}

// Longest match for src[0..maxLen) among chain positions; reads from
// base[pos..) bounded by readLimitExcl.  Returns length (>= MIN_MATCH) or 0.
static int OptChainQuery(const OptHashChain& c, const unsigned char* src, int maxLen,
                         const unsigned char* base, int readLimitExcl, int maxChain,
                         int* pBestOff)
{
	if (maxLen < MIN_MATCH) return 0;

	int pos = c.head[Hash4(src)];
	int chainCount = 0;
	int bestLen = MIN_MATCH - 1;   // require strictly better
	int bestOff = -1;

	while (pos >= 0 && chainCount < maxChain)
	{
		int maxExtend = readLimitExcl - pos;
		if (maxExtend > maxLen) maxExtend = maxLen;

		// Quick reject: can't beat current best, or best+1'th byte differs.
		if (maxExtend > bestLen && base[pos + bestLen] == src[bestLen])
		{
			const unsigned char* p = base + pos;
			int len = 0;
			while (len < maxExtend && p[len] == src[len]) ++len;
			if (len > bestLen)
			{
				bestLen = len;
				bestOff = pos;
				if (len == maxLen) break;   // can't do better
			}
		}
		pos = c.next[pos];
		++chainCount;
	}

	if (bestOff < 0) return 0;
	*pBestOff = bestOff;
	return bestLen;
}

//------------------------------------------------------------------------------
// Range-min segment tree over landing positions, values packed as
// (cost << 18) | (fromPos << 2) | opType so min-by-cost carries provenance.
// fromPos < 32768 (15 bits), cost < ~100K (17 bits) — fits in 35 bits.

static long long s_seg[2 * OPT_SEG_LEAVES];

static inline long long OptPack(int cost, int from, int type)
{
	return ((long long)cost << 18) | ((long long)from << 2) | (long long)type;
}

static void SegUpdate(int l, int r, long long v)   // inclusive [l, r]
{
	if (l > r) return;
	l += OPT_SEG_LEAVES;
	r += OPT_SEG_LEAVES + 1;
	while (l < r)
	{
		if (l & 1) { if (v < s_seg[l]) s_seg[l] = v; ++l; }
		if (r & 1) { --r; if (v < s_seg[r]) s_seg[r] = v; }
		l >>= 1;
		r >>= 1;
	}
}

static long long SegQuery(int j)
{
	long long v = OptPack(OPT_COST_INF, 0, 0);
	for (int p = j + OPT_SEG_LEAVES; p >= 1; p >>= 1)
	{
		if (s_seg[p] < v) v = s_seg[p];
	}
	return v;
}

//------------------------------------------------------------------------------
// DP state, parse provenance, and the reconstructed op list.

static int           s_cost0 [MAX_DICTIONARY_SIZE + 1]; // best cost arriving with no open literal
static int           s_cost1 [MAX_DICTIONARY_SIZE + 1]; // best cost arriving with open literal
static long long     s_par0  [MAX_DICTIONARY_SIZE + 1]; // packed (cost,from,type) for state 0
static unsigned char s_par1  [MAX_DICTIONARY_SIZE + 1]; // prior state for the literal byte
static int           s_preOff [MAX_DICTIONARY_SIZE];
static int           s_preLen [MAX_DICTIONARY_SIZE];
static int           s_postOff[MAX_DICTIONARY_SIZE];
static int           s_postLen[MAX_DICTIONARY_SIZE];
static int           s_runEnd [MAX_DICTIONARY_SIZE + 1];
static unsigned char s_queryHint[MAX_DICTIONARY_SIZE + 1];

struct OptOp
{
	int type;   // OPT_TYPE_LIT / SKIP / REF
	int pos;    // source/cursor position the op starts at
	int len;
	int off;    // dictionary offset (refs only)
};
static OptOp s_ops[MAX_DICTIONARY_SIZE + 2];

//------------------------------------------------------------------------------
//
// bDelta = true : delta frame (canvas dictionary, skips allowed, bank-skip
//                 guarded output, trailing End-of-Frame, canvas updated)
// bDelta = false: INIT frame (dictionary grows from empty in pSource itself,
//                 no skips — every byte must be written — no bank logic,
//                 caller appends the end-of-animation opcode)
//
static int OptimalCompressCore(unsigned char* pDest, unsigned char* pSource, int frameSize,
                               unsigned char* pDataStart, unsigned char* pDictionary, bool bDelta)
{
	unsigned char* pOriginalDest = pDest;

	//---- per-frame reset -----------------------------------------------------

	OptChainReset(s_chainPre);
	if (bDelta)
	{
		OptChainReset(s_chainPost);
		for (int p = 0; p + MIN_MATCH <= frameSize; ++p)
		{
			OptChainInsert(s_chainPost, p, pDictionary);
		}

		s_runEnd[frameSize] = frameSize;
		for (int i = frameSize - 1; i >= 0; --i)
		{
			s_runEnd[i] = (pSource[i] == pDictionary[i]) ? s_runEnd[i + 1] : i;
		}
	}

	memset(s_queryHint, 0, frameSize + 1);

	const long long INFPACK = OptPack(OPT_COST_INF, 0, 0);
	for (int i = 0; i < 2 * OPT_SEG_LEAVES; ++i) s_seg[i] = INFPACK;

	for (int i = 0; i <= frameSize; ++i) s_cost1[i] = OPT_COST_INF;

	// If the unchanged tail of the frame reaches the end, the parse can stop
	// there (no trailing skip needed) — track the best such finish.
	int finBest = OPT_COST_INF;
	int finPos  = -1;

	//---- forward DP ------------------------------------------------------------

	for (int i = 0; i <= frameSize; ++i)
	{
		long long q = SegQuery(i);
		int c0 = (0 == i) ? 0 : (int)(q >> 18);
		s_cost0[i] = c0;
		s_par0[i]  = q;

		int c1 = s_cost1[i];

		if (i == frameSize) break;

		// Position i is no longer "ahead of the cursor" for later positions.
		if (bDelta) OptChainRemove(s_chainPost, i);

		int best = (c0 < c1) ? c0 : c1;

		// Skip: flat 2 bytes to land anywhere inside the unchanged run.
		if (bDelta && (pSource[i] == pDictionary[i]))
		{
			int E = s_runEnd[i];
			if (E == frameSize && best < finBest)
			{
				finBest = best;
				finPos  = i;
			}
			int hi = i + MAX_STRING_SIZE;
			if (hi > E) hi = E;
			SegUpdate(i + 1, hi, OptPack(best + 2, i, OPT_TYPE_SKIP));
		}

		// Ref: flat 4 bytes to land anywhere in [i+3, i+L] (length-3 prefixes
		// of a >=4 match are still valid refs and beat a fresh literal).
		int maxLen = frameSize - i;
		if (maxLen > MAX_STRING_SIZE) maxLen = MAX_STRING_SIZE;

		// Speed: deep inside an unchanged run, a ref start is rarely part of
		// the optimal parse — a fresh skip+ref from the run boundary usually
		// costs the same (suffix property lets a bridging match start later).
		// The exception is a previous ref LANDING deep in the run and chaining
		// straight into another ref; ref edges mark their max-length landing
		// in s_queryHint to keep exactly that case covered.  Unpruned, match
		// queries at the ~90% unchanged positions were ~99% of encode time.
		int maxChain = OPT_MAX_CHAIN;
		if (bDelta && !s_queryHint[i] && (pSource[i] == pDictionary[i]) &&
		    (s_runEnd[i] - i) > OPT_RUN_QUERY_MARGIN)
		{
			maxChain = OPT_DEEP_RUN_CHAIN;
		}

		int Lp = 0, Lq = 0, pOff = -1, qOff = -1;
		if (maxChain > 0 && maxLen >= MIN_MATCH)
		{
			Lp = OptChainQuery(s_chainPre, pSource + i, maxLen, pSource, frameSize,
			                   maxChain, &pOff);
			if (bDelta && Lp < maxLen)
			{
				Lq = OptChainQuery(s_chainPost, pSource + i, maxLen, pDictionary, frameSize,
				                   maxChain, &qOff);
			}
		}
		s_preLen [i] = Lp; s_preOff [i] = pOff;
		s_postLen[i] = Lq; s_postOff[i] = qOff;

		int L = (Lp > Lq) ? Lp : Lq;
		if (L >= MIN_MATCH)
		{
			SegUpdate(i + (MIN_MATCH - 1), i + L, OptPack(best + 4, i, OPT_TYPE_REF));

			// A follow-on ref plausibly starts where this one lands.
			if (Lp >= MIN_MATCH) s_queryHint[i + Lp] = 1;
			if (Lq >= MIN_MATCH) s_queryHint[i + Lq] = 1;
		}

		// Literal byte: continue the open literal, or open a fresh one.
		{
			int viaCont  = c1 + 1;
			int viaFresh = c0 + 3;
			int nl;
			unsigned char par;
			if (viaCont <= viaFresh) { nl = viaCont;  par = 1; }
			else                     { nl = viaFresh; par = 0; }
			if (nl < s_cost1[i + 1])
			{
				s_cost1[i + 1] = nl;
				s_par1 [i + 1] = par;
			}
		}

		// Position i now holds a new-frame byte for everything after it.
		if (i + MIN_MATCH <= frameSize) OptChainInsert(s_chainPre, i, pSource);
	}

	//---- pick the end point ----------------------------------------------------

	int endPos   = frameSize;
	int endState = 0;
	int bestEnd  = s_cost0[frameSize];
	if (s_cost1[frameSize] < bestEnd)
	{
		bestEnd  = s_cost1[frameSize];
		endState = 1;
	}
	if (bDelta && finBest < bestEnd)
	{
		bestEnd  = finBest;
		endPos   = finPos;
		endState = (s_cost1[finPos] < s_cost0[finPos]) ? 1 : 0;
	}

	//---- backward reconstruction -----------------------------------------------

	int nOps = 0;
	int pos  = endPos;
	int st   = endState;
	while (pos > 0)
	{
		if (1 == st)
		{
			// Walk back through consecutive literal bytes, merge into one op.
			int end = pos;
			while (pos > 0 && 1 == st)
			{
				st = s_par1[pos];
				--pos;
			}
			s_ops[nOps].type = OPT_TYPE_LIT;
			s_ops[nOps].pos  = pos;
			s_ops[nOps].len  = end - pos;
			s_ops[nOps].off  = 0;
			++nOps;
		}
		else
		{
			long long q = s_par0[pos];
			int from = (int)((q >> 2) & 0xFFFF);
			int type = (int)(q & 3);
			int len  = pos - from;

			s_ops[nOps].type = type;
			s_ops[nOps].pos  = from;
			s_ops[nOps].len  = len;
			s_ops[nOps].off  = (OPT_TYPE_REF == type)
			                   ? ((len <= s_preLen[from]) ? s_preOff[from] : s_postOff[from])
			                   : 0;
			++nOps;

			pos = from;
			st  = (s_cost1[pos] < s_cost0[pos]) ? 1 : 0;
		}
	}

	//---- forward emission (ops are stored in reverse) ---------------------------

	int space_left_in_bank = 0;
	if (bDelta)
	{
		space_left_in_bank = (int)0x10000 - (int)((pDest - pDataStart) & 0xFFFF);
		space_left_in_bank = CheckEmitSourceSkip(0, pDest, space_left_in_bank);
	}

	for (int opIdx = nOps - 1; opIdx >= 0; --opIdx)
	{
		const OptOp& op = s_ops[opIdx];

		switch (op.type)
		{
		case OPT_TYPE_SKIP:
		{
			int numSkips = (op.len / MAX_STRING_SIZE) + 1;
			space_left_in_bank = CheckEmitSourceSkip(2 * numSkips, pDest, space_left_in_bank);
			pDest += EmitSkip(pDest, op.len);
			break;
		}
		case OPT_TYPE_REF:
		{
			if (bDelta)
			{
				space_left_in_bank = CheckEmitSourceSkip(4, pDest, space_left_in_bank);
			}
			assert(op.len >= 1 && op.len <= MAX_STRING_SIZE);
			unsigned short opcode = (unsigned short)(((op.len - 1) << 1) | 0x8000);
			*pDest++ = (unsigned char)(opcode & 0xFF);
			*pDest++ = (unsigned char)((opcode >> 8) & 0xFF);
			unsigned short address = (unsigned short)(op.off + 0x2000);
			*pDest++ = (unsigned char)(address & 0xFF);
			*pDest++ = (unsigned char)((address >> 8) & 0xFF);
			break;
		}
		default: // OPT_TYPE_LIT
		{
			// Literal data must come from pSource (new-frame bytes).  Split
			// runs longer than the 14-bit length field into fresh opcodes.
			int n = op.len;
			int p = op.pos;
			while (n > 0)
			{
				int chunk = (n > MAX_STRING_SIZE) ? MAX_STRING_SIZE : n;
				if (bDelta)
				{
					space_left_in_bank = CheckEmitSourceSkip(2 + chunk, pDest, space_left_in_bank);
				}
				unsigned short opcode = (unsigned short)(((chunk - 1) << 1) | 0x0001);
				*pDest++ = (unsigned char)(opcode & 0xFF);
				*pDest++ = (unsigned char)((opcode >> 8) & 0xFF);
				memcpy(pDest, pSource + p, chunk);
				pDest += chunk;
				p += chunk;
				n -= chunk;
			}
			break;
		}
		}
	}

	if (bDelta)
	{
		space_left_in_bank = CheckEmitSourceSkip(2, pDest, space_left_in_bank);

		// End of Frame opcode
		*pDest++ = 0x02;
		*pDest++ = 0x00;

		// The ops transform the canvas into the new frame; just say so.
		memcpy(pDictionary, pSource, frameSize);
	}

	return (int)(pDest - pOriginalDest);
}

//------------------------------------------------------------------------------

int LZB_CompressOptimal(unsigned char* pDest, unsigned char* pSource, int sourceSize)
{
	if (sourceSize > MAX_DICTIONARY_SIZE)
	{
		return LZB_Compress(pDest, pSource, sourceSize);
	}
	return OptimalCompressCore(pDest, pSource, sourceSize, nullptr, nullptr, false);
}

//------------------------------------------------------------------------------

int LZBA_CompressOptimal(unsigned char* pDest, unsigned char* pSource, int sourceSize,
                         unsigned char* pDataStart, unsigned char* pDictionary, int dictionarySize)
{
	if ((sourceSize != dictionarySize) || (sourceSize > MAX_DICTIONARY_SIZE))
	{
		return LZBA_Compress(pDest, pSource, sourceSize, pDataStart, pDictionary,
		                     dictionarySize, 256);
	}
	return OptimalCompressCore(pDest, pSource, sourceSize, pDataStart, pDictionary, true);
}

//------------------------------------------------------------------------------

