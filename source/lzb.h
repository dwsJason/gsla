//
// LZB Encode / Decode
// 
#ifndef LZB_H
#define LZB_H
 
// 
// returns the size of data saved into the pDest Buffer
//  
int LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize);
int Old_LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize);
void LZB_Decompress(unsigned char* pDest, unsigned char* pSource, int destSize);

//
// LZB Compressor that uses GSLA Opcodes while encoding
//
int LZBA_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize,
				  unsigned char* pDataStart, unsigned char* pDictionary,
				  int dictionarySize=0);
int LZBA_Decompress(unsigned char* pDest, unsigned char* pSource, unsigned char* pDataStart);

#endif // LZB_H

