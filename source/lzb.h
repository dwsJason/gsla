//
// LZB Encode
// 
#ifndef LZB_H
#define LZB_H
 
// 
// returns the size of data saved into the pDest Buffer
//  
int LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize);
int Old_LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize);

//
// LZB Compressor that uses GSLA Opcodes while encoding
//
// gapMergeThreshold: runs of unchanged bytes shorter than this are absorbed
// into the surrounding changed-chunk; runs of this length or longer split the
// chunk and emit a 2-byte cursor-skip opcode.  The optimum is content-dependent
// (denser change → smaller threshold).  Pass 3 for the original behavior.
//
int LZBA_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize,
				  unsigned char* pDataStart, unsigned char* pDictionary,
				  int dictionarySize, int gapMergeThreshold = 3);

//
// Optimal-parse variants (Phase 2): same output format, globally minimal
// literal/ref/skip parse via dynamic programming.
//
int LZB_CompressOptimal(unsigned char* pDest, unsigned char* pSource, int sourceSize);
int LZBA_CompressOptimal(unsigned char* pDest, unsigned char* pSource, int sourceSize,
						 unsigned char* pDataStart, unsigned char* pDictionary,
						 int dictionarySize);

#endif // LZB_H

