//
// LZB Encode / Decode
// 
#ifndef LZB_H
#define LZB_H
 
// 
// returns the size of data saved into the pDest Buffer
//  
int LZB_Compress(unsigned char* pDest, unsigned char* pSource, int sourceSize);
void LZB_Decompress(unsigned char* pDest, unsigned char* pSource, int destSize);

#endif // LZB_H

