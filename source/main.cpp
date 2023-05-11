//
// GSLA - GS LZB Animation Tool
// 
// Look in gsla_file.cpp/h for more information about the file format
//

#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "c2_file.h"
#include "gsla_file.h"

//------------------------------------------------------------------------------
static void helpText()
{
	printf("GSLA - v1.1\n");
	printf("--------------\n");
	printf("GS Lzb Animation Creation Tool\n");
	printf("\n");
	printf("\ngsla [options] <input_file> <outfile>\n\n");
	printf("options:\n");
	printf("\t-t<bytes> Throttle bytes per frame\n");
	printf("\t-g<x>x<y> Grid size, default 8x8\n");
	printf("\nExample: gsla -t2960 -g8x8 movie.c2 movie.gsla\n\n");
	printf("Converts from C2 to GSLA\n");

	exit(-1);
}
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Local helper functions

static bool contains(char x, const char* pSeparators)
{
	while (*pSeparators)
	{
		if (x == *pSeparators)
		{
			return true;
		}
		pSeparators++;
	}

	return false;
}

static std::vector<std::string> split(const std::string& s, const char* separators)
{
	std::vector<std::string> output;
	std::string::size_type prev_pos = 0, pos = 0;

	for (int index = 0; index < s.length(); ++index)
	{
		pos = index;

		// if the index is a separator
		if (contains(s[index], separators))
		{
			// if we've skipped a token, collect it
			if (prev_pos != index)
			{
				output.push_back(s.substr(prev_pos, index-prev_pos));

				// skip white space here
				while (index < s.length())
				{
					if (contains(s[index], separators))
					{
						++index;
					}
					else
					{
						prev_pos = index;
						pos = index;
						break;
					}
				}
			}
			else
			{
				prev_pos++;
			}
		}
	}

    output.push_back(s.substr(prev_pos, pos-prev_pos+1)); // Last word

    return output;
}

static std::string toLower(const std::string s)
{
	std::string result = s;

	for (int idx = 0; idx < result.size(); ++idx)
	{
		result[ idx ] = (char)tolower(result[idx]);
	}

	return result;
}

// Case Insensitive
static bool endsWith(const std::string& S, const std::string& SUFFIX)
{
	bool bResult = false;

	std::string s = toLower(S);
	std::string suffix = toLower(SUFFIX);

    bResult = s.rfind(suffix) == (s.size()-suffix.size());

	return bResult;
}
//------------------------------------------------------------------------------


int main(int argc, char* argv[])
{
	char* pInfilePath  = nullptr;
	char* pOutfilePath = nullptr;

	int throttle=0;  // no throttling
					 // default 8x8 grid
	int gridSizeX = 8;
	int gridSizeY = 8;


	// index 0 is the executable name

	if (argc < 2) helpText();

	for (int idx = 1; idx < argc; ++idx )
	{
		char* arg = argv[ idx ];

		if ('-' == arg[0])
		{
			// Parse as an option
			if ('t' == arg[1])
			{
				// Set throttle size, which enables throttling
				throttle = atoi(&arg[2]);
				printf("Throttle = %d bytes per frame\n",throttle);

			}
			if ('g' == arg[1])
			{
				// set grid size for the throttler (this effects the pattern, when throttling kicks in)
				std::string gridstring = toLower(&arg[2]);
				std::vector<std::string> values = split(gridstring, "x");

				if (values.size() != 2)
				{
					printf("invalid grid size setting: %s\n", arg);
					helpText();
				}

				gridSizeX = atoi(values[0].c_str());
				gridSizeY = atoi(values[1].c_str());

			}
		}
		else if (nullptr == pInfilePath)
		{
			// Assume the first non-option is an input file path
			pInfilePath = argv[ idx ];
		}
		else if (nullptr == pOutfilePath)
		{
			// Assume second non-option is an output file path
			pOutfilePath = argv[ idx ];
		}
		else
		{
			// Oh Crap, we have a non-option, but we don't know what to do with
			// it
			printf("ERROR: Invalid option, Arg %d = %s\n\n", idx, argv[ idx ]);
			helpText();
		}
	}

	if (pInfilePath)
	{
		// See what we can do with the input file path
		// could be a .gsla file, for a .c2 file, or maybe a series of .c1 files
		if (endsWith(pInfilePath, ".c2") || endsWith(pInfilePath, "#c20000"))
		{
			// It's a C2 file

			printf("Loading C2 File %s\n", pInfilePath);

			C2File c2data( pInfilePath );

			int frameCount = c2data.GetFrameCount();

			if (frameCount < 1)
			{
				// c2 file can't be valid, if there are no frames
				printf("C2 File seems invalid.\n");
				helpText();
			}

			if (pOutfilePath)
			{
				if (0 != throttle)
				{
					printf("Grid Size  = %dx%d\n", gridSizeX, gridSizeY);

					int numCellsX = 320/gridSizeX;
					int numCellsY = 200/gridSizeY;
					printf("Cell Count = %dx%d\n", numCellsX, numCellsY);

					int numCells = numCellsX*numCellsY;
					printf("Cell Total = %d\n", numCells);

					printf("Each cell min budget = %d bytes\n", throttle/numCells);

					c2data.ApplyThrottle(throttle, gridSizeX, gridSizeY);
				}

				const std::vector<unsigned char*>& c1Datas = c2data.GetPixelMaps();

				printf("Saving %s with %d frames\n", pOutfilePath, (int)c1Datas.size());

				GSLAFile anim(320,200, 0x8000);

				anim.AddImages(c1Datas);
				
				anim.SaveToFile(pOutfilePath);

				#if 1
				{
					// Verify the conversion is good
					// Load the file back in
					GSLAFile verify(pOutfilePath);

					const std::vector<unsigned char *> &frames = verify.GetPixelMaps();

					for (int idx = 0; idx < frames.size(); ++idx)
					{
						int result = memcmp(c1Datas[idx % c1Datas.size()], frames[idx], verify.GetFrameSize());
						printf("Verify Frame %d - %s\n", idx, result ? "Failed" : "Good");
					}
				}
				#endif
			}
		}

	}
	else
	{
		helpText();
	}


	return 0;
}

