//
// C++ Encoder/Decoder
// For GSLA, GS Lzb Animation File Format
// 
//
// Care is taken in the encoder, to make sure the 65816 does not have to cross
// bank boundaries during any copy.  This is so we can use the MVN instruction,
// and so we can reduce the number of bank checks in the code.  We will have an
// opcode, that says “source data bank has changed”
// 
// The file will be laid out such that you load the file in at a 64K memory
// boundary
// 
// Please see the gsla_file.cpp, for documentation on the actual
// format of the file
//
#ifndef GSLA_FILE_H
#define GSLA_FILE_H

#include <vector>

#pragma pack(push, 1)

// Data Stream Header
typedef struct GSLA_Header
{
	char	G,S,L,A;
	unsigned int 	file_length;  // length of the file, including this header
	unsigned short  version;
	unsigned short  width; 		  // data width  in bytes
	unsigned short  height;		  // data height in bytes
	unsigned short  frame_size;   // frame size in bytes (0 means 64K)
	unsigned int    frame_count;  // total number of frames in the file

//------------------------------------------------------------------------------
// If you're doing C, just get rid of these methods
	bool IsValid(unsigned int fileLength)
	{
		if ((G!='G')||(S!='S')||(L!='L')||(A!='A'))
			return false;				// signature is not right

		if (file_length != fileLength)
			return false;				// size isn't right

		if (0x8000 != frame_size)
			return false;

		if (160 != width)
			return false;

		if (200 != height)
			return false;

		return true;
	}

} GSLA_Header;

// INITial Frame Chunk
typedef struct GSLA_INIT
{
	char		  I,N,i,T;		// 'I','N','I','T'
	unsigned int  chunk_length; // in bytes, including the 9 bytes header of this chunk

	// Commands Coded Data Follows
//------------------------------------------------------------------------------
// If you're doing C, just get rid of these methods
	bool IsValid()
	{
		if ((I!='I')||(N!='N')||(i!='I')||(T!='T'))
			return false;				// signature is not right

		return true;
	}


} GSLA_INIT;

// Animated Frames Chunk
typedef struct GSLA_ANIM
{
	char		  A,N,I,M;		// 'A','N','I','M'
	unsigned int  chunk_length; // in bytes, including the 8 bytes header of this chunk

	// Commands Coded Data Follows

//------------------------------------------------------------------------------
// If you're doing C, just get rid of these methods
	bool IsValid()
	{
		if ((A!='A')||(N!='N')||(I!='I')||(M!='M'))
			return false;				// signature is not right

		return true;
	}

} GSLA_ANIM;

// Generic Unknown Chunk
typedef struct GSLA_CHUNK
{
	char id0,id1,id2,id3;
	unsigned int chunk_length;

} GSLA_CHUNK;


#pragma pack(pop)

class GSLAFile
{
public:
	// Load in a GSLA File
	GSLAFile(const char *pFilePath);
	~GSLAFile();

	// Creation
	GSLAFile(int iWidthPixels, int iHeightPixels, int iFrameSizeBytes);
	void AddImages( const std::vector<unsigned char*>& pFrameBytes );
	void SaveToFile(const char* pFilenamePath);

	// Retrieval
	void LoadFromFile(const char* pFilePath);
	int GetFrameCount() { return (int)m_pC1PixelMaps.size(); }
	int GetWidth()  { return m_widthPixels; }
	int GetHeight() { return m_heightPixels; }

	const std::vector<unsigned char*>& GetPixelMaps() { return m_pC1PixelMaps; }

	int DecompressFrame(unsigned char* pTarget, unsigned char* pData, unsigned char* pDataBaseAddress);

private:

	void UnpackInitialFrame(GSLA_INIT* pINIT, GSLA_Header* pHeader);
	void UnpackAnimation(GSLA_ANIM* pANIM, GSLA_Header* pHeader);


	int m_frameSize;  // frame buffer size in bytes

	int m_widthPixels;		// Width of image in pixels
	int m_heightPixels;		// Height of image in pixels

	std::vector<unsigned char*> m_pC1PixelMaps;

};


#endif // C2_FILE_H

