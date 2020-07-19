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
int LZBA_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize,
				  unsigned char* pDataStart, unsigned char* pDictionary,
				  int dictionarySize=0);

#endif // LZB_H

