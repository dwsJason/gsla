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
	printf("GSLA - v1.0\n");
	printf("--------------\n");
	printf("GS Lzb Animation Creation Tool\n");
	printf("\n");
	printf("\ngsla [options] <input_file> <outfile>\n");
	printf("\n\n There are no [options] yet\n");
	printf("Converts from C2 to GSLA\n");

	exit(-1);
}
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Local helper functions

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


	// index 0 is the executable name

	if (argc < 2) helpText();

	for (int idx = 1; idx < argc; ++idx )
	{
		char* arg = argv[ idx ];

		if ('-' == arg[0])
		{
			// Parse as an option
			// Currently I have no options, so I'll just skip
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

