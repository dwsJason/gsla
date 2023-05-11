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
#include "c2_file.h"
#include <stdio.h>

// If these structs are the wrong size, there's an issue with type sizes, and
// your compiler
static_assert(sizeof(C2File_Header)==0x800C, "C2File_Header is supposed to be 0x800C bytes");

//------------------------------------------------------------------------------
// Load in a FanFile constructor
//
C2File::C2File(const char *pFilePath)
	: m_widthPixels(320)
	, m_heightPixels(200)
{
	LoadFromFile(pFilePath);
}
//------------------------------------------------------------------------------

C2File::~C2File()
{
	// Free Up the memory
	for (int idx = 0; idx < m_pC1PixelMaps.size(); ++idx)
	{
		delete[] m_pC1PixelMaps[idx];
		m_pC1PixelMaps[ idx ] = nullptr;
	}
}

//------------------------------------------------------------------------------

void C2File::LoadFromFile(const char* pFilePath)
{
	// Free Up the memory
	for (int idx = 0; idx < m_pC1PixelMaps.size(); ++idx)
	{
		delete[] m_pC1PixelMaps[idx];
		m_pC1PixelMaps[ idx ] = nullptr;
	}

	m_pC1PixelMaps.clear();
	//--------------------------------------------------------------------------

	std::vector<unsigned char> bytes;

	//--------------------------------------------------------------------------
	// Read the file into memory
	FILE* pFile = nullptr;
	errno_t err = fopen_s(&pFile, pFilePath, "rb");

	if (0==err)
	{
		fseek(pFile, 0, SEEK_END);
		size_t length = ftell(pFile);	// get file size
		fseek(pFile, 0, SEEK_SET);

		bytes.resize( length );			// make sure buffer is large enough

		// Read in the file
		fread(&bytes[0], sizeof(unsigned char), bytes.size(), pFile);
		fclose(pFile);
	}

	if (bytes.size())
	{
		size_t file_offset = 0;	// File Cursor

		// Bytes are in the buffer, so let's start looking at what we have
		C2File_Header* pHeader = (C2File_Header*) &bytes[0];

		// Early out if things don't look right
		if (!pHeader->IsValid((unsigned int)bytes.size()))
			return;

		// Grab Initial Frame, and put it in the list

		unsigned char* pFrame  = new unsigned char[ 0x8000 ];
		unsigned char* pCanvas = new unsigned char[ 0x8001 ]; // each frame changes the canvas
		m_pC1PixelMaps.push_back(pFrame);
		memcpy(pFrame, &bytes[0], 0x8000);
		memcpy(pCanvas, &bytes[0], 0x8000);

		//----------------------------------------------------------------------
		// Process Frames as we encounter them
		file_offset += sizeof(C2File_Header);

		// Since we always pull 4 bytes
		// let's keep us from pulling bytes outside our buffer
		size_t eof_size = bytes.size() - 4;

		// While we're not at the end of the file
		unsigned short offset = 0;
		//unsigned short prev_offset = 0;
		unsigned short data   = 0xFFFF;
		while (file_offset <= eof_size)
		{
			//prev_offset = offset;

			offset  = (unsigned short)bytes[ file_offset++ ];
			offset |= ((unsigned short)bytes[ file_offset++ ])<<8;

			data  = (unsigned short)bytes[ file_offset++ ];
			data |= ((unsigned short)bytes[ file_offset++ ])<<8;

			//if (((offset == 0)&&(data == 0xFFFF))||(offset < prev_offset))
			if (0 == offset)
			{
				// End of Frame, capture a copy
				pFrame  = new unsigned char[ 0x8000 ];
				memcpy(pFrame, pCanvas, 0x8000);
				m_pC1PixelMaps.push_back(pFrame);
			}
			else
			{
				// Apply Change to the Canvas
				offset &= 0x7FFF; // force the offset into the Canvas
				// probably should ignore offsets outside the canvas
				pCanvas[ offset ] = data & 0xFF;
				pCanvas[ offset+1 ] = (data >> 8) & 0xFF;
			}
		}

		delete[] pCanvas;
	}
}

//------------------------------------------------------------------------------
//
// How many bytes are different between these frames
//
int C2File::CalcDifference(unsigned char* pC1Data0, unsigned char* pC1Data1)
{
	int total = 0;

	for (int idx = 0; idx < 160*200; ++idx)
	{
		if (pC1Data0[idx] != pC1Data1[idx])
		{
			total+=1;
		}
	}

	return total;
}

//------------------------------------------------------------------------------

//
// I'm only going to apply the throttle to the pixels, not SCB + Palette
// changes, most animations, should not change the SCB or CLUT
//
void C2File::ApplyThrottle(int ThrottleSize, int CellSizeX, int CellSizeY)
{
	// I want cells to be updated "Round Robin", we start updating at whichever
	// cell we left off on the previous frame.

	// Make Sure CellSizes are valid range
	if (CellSizeX > 320)
	{
		CellSizeX = 320;
	}

	if (CellSizeY > 200)
	{
		CellSizeY = 200;
	}

	CellSizeX >>= 1;  //from pixels to bytes
	if (CellSizeX < 1)
	{
		CellSizeX = 1;
	}
	if (CellSizeY < 1)
	{
		CellSizeY = 1;
	}


	int numCellsX = 160/CellSizeX;
	int numCellsY = 200/CellSizeY;

	//-------------------------------------------------------------------------
	int totalCells = numCellsX * numCellsY;
	int bytesPerCell = CellSizeX * CellSizeY;

	// prepare state

	int CellIndex = 0; // For Round Robin

	std::vector<int> indices;

	// Set all initial indices to 0
	for (int idx = 0; idx < totalCells; ++idx)
	{
		indices.push_back((0));
	}

	// Go through all the frames
	for (int index = 1; index < m_pC1PixelMaps.size(); ++index)
	{
		unsigned char* p0 = m_pC1PixelMaps[index - 1];
		unsigned char* p1 = m_pC1PixelMaps[index];

		int deltaSize = CalcDifference(p0,p1);

		while (deltaSize > ThrottleSize)
		{
			// Reduce the difference, while the size is too big
			// this works by removing differences from the target, until we're in budget
			int subIndex = indices[CellIndex];

			// Calculate the buffer position by converting CellIndex, and subIndex
			// into X,Y position in the buffer

			int pixel_y = (CellIndex / numCellsX) * CellSizeY;
			int pixel_x = (CellIndex % numCellsX) * CellSizeX;

			int sub_pixel_y = (subIndex / CellSizeX);
			int sub_pixel_x = (subIndex % CellSizeX);

			pixel_x += sub_pixel_x;
			pixel_y += sub_pixel_y;

			if (pixel_x < 0) pixel_x = 0;
			if (pixel_y < 0) pixel_y = 0;
			if (pixel_x >= 160) pixel_x=159;
			if (pixel_y >= 200) pixel_y=199;

			int pixel_index = (pixel_y*160) + pixel_x;

			if (p0[pixel_index] != p1[pixel_index])
			{
				// remove delta
				p1[pixel_index] = p0[pixel_index];
				deltaSize--;
			}

			// go to next index in the cell, for the next time we process the cell
			subIndex++;
			if (subIndex >= bytesPerCell)
			{
				subIndex = 0;
			}
			indices[CellIndex] = subIndex;

			// next cell
			CellIndex++;
			if (CellIndex >= totalCells)
			{
				CellIndex = 0;
			}

		}
	}
}

//------------------------------------------------------------------------------

