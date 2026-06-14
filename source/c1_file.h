//
// C++ Loader
// For a directory of loose C1 files (Apple IIgs $C1/0000 Super Hi-Res screens)
//
// A C1 file is exactly 0x8000 bytes of raw Super Hi-Res screen data, which is a
// 1:1 match for the in-memory C1 frame the GSLA encoder consumes.  This loader
// scans a directory for C1 images and gathers them into the same
// std::vector<unsigned char*> of 0x8000-byte frames that C2File produces, so the
// rest of the pipeline (GSLAFile::AddImages) is unchanged.
//
// Frames are ordered by a natural/numeric filename sort (so frame2 precedes
// frame10).  Only files ending in ".c1" or "#c10000" (case-insensitive) are
// considered, and each must be exactly 0x8000 bytes.
//
#ifndef C1_FILE_H
#define C1_FILE_H

#include <vector>

class C1File
{
public:
	// Load all C1 files in a directory
	C1File(const char *pDirectoryPath);

	~C1File();

	// Retrieval
	void LoadFromDirectory(const char* pDirectoryPath);
	int GetFrameCount() { return (int)m_pC1PixelMaps.size(); }
	int GetWidth()  { return m_widthPixels; }
	int GetHeight() { return m_heightPixels; }

	const std::vector<unsigned char*>& GetPixelMaps() { return m_pC1PixelMaps; }

private:

	int m_widthPixels;		// Width of image in pixels
	int m_heightPixels;		// Height of image in pixels

	std::vector<unsigned char*> m_pC1PixelMaps;

};


#endif // C1_FILE_H
