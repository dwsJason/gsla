//
// C++ Encoder/Decoder
// For C2, Paintworks Animation File Format
//
// File Format summary here from Brutal Deluxe
//
//0000.7fff first pic
//8000.8003 length of frame data - 8008
//8004.8007 timing
//8008.800b length of first frame data
//800c.800d first frame index
//800e.800f first frame data 
//
// Offset 0, Value FFFF indicates the end of a frame
// 	00 00 FF FF 
//
#ifndef C2_FILE_H
#define C2_FILE_H

#include <vector>

#pragma pack(push, 1)

typedef struct C2File_Header
{
	char 			image[0x8000];   // C1 Initial Image

	unsigned int 	file_length;  // length of frame_data (file length - 8008)
	unsigned int 	timing;
	unsigned int	frame_length; // first encoded frame length

//------------------------------------------------------------------------------
// If you're doing C, just get rid of these methods
	bool IsValid(unsigned int fileLength)
	{
		if ((file_length+0x8008) != fileLength)
			return false;				// size isn't right

		return true;
	}

} C2File_Header;

#pragma pack(pop)

class C2File
{
public:
	// Load in a C2 File
	C2File(const char *pFilePath);

	~C2File();

	// Retrieval
	void LoadFromFile(const char* pFilePath);
	int GetFrameCount() { return (int)m_pC1PixelMaps.size(); }
	int GetWidth()  { return m_widthPixels; }
	int GetHeight() { return m_heightPixels; }

	const std::vector<unsigned char*>& GetPixelMaps() { return m_pC1PixelMaps; }

private:

	int m_widthPixels;		// Width of image in pixels
	int m_heightPixels;		// Height of image in pixels

	std::vector<unsigned char*> m_pC1PixelMaps;

};


#endif // C2_FILE_H

