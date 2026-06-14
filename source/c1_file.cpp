//
// C++ Loader
// For a directory of loose C1 files (Apple IIgs $C1/0000 Super Hi-Res screens)
//
// See c1_file.h for the format/behavior summary.
//
#include "c1_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>	// memcpy
#include <cctype>	// tolower, isdigit
#include <cerrno>	// errno

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

// A C1 frame is exactly this many bytes (raw Super Hi-Res screen)
static const size_t kC1FrameSize = 0x8000;

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

static std::string toLower(const std::string& s)
{
	std::string result = s;
	for (size_t idx = 0; idx < result.size(); ++idx)
	{
		result[idx] = (char)tolower((unsigned char)result[idx]);
	}
	return result;
}

// Case insensitive "does S end with SUFFIX"
static bool endsWith(const std::string& S, const std::string& SUFFIX)
{
	if (SUFFIX.size() > S.size())
		return false;

	std::string s = toLower(S);
	std::string suffix = toLower(SUFFIX);

	return 0 == s.compare(s.size() - suffix.size(), suffix.size(), suffix);
}

// Is this filename a C1 image (by extension/naming convention)?
static bool isC1Filename(const std::string& name)
{
	return endsWith(name, ".c1") || endsWith(name, "#c10000");
}

// Natural/numeric comparison: like a normal string compare, except runs of
// digits are compared as integers, so "frame2" sorts before "frame10".
static bool naturalLess(const std::string& a, const std::string& b)
{
	std::string la = toLower(a);
	std::string lb = toLower(b);

	size_t i = 0;
	size_t j = 0;

	while (i < la.size() && j < lb.size())
	{
		bool aDigit = 0 != isdigit((unsigned char)la[i]);
		bool bDigit = 0 != isdigit((unsigned char)lb[j]);

		if (aDigit && bDigit)
		{
			// Skip leading zeros
			size_t aStart = i;
			size_t bStart = j;
			while (aStart < la.size() && '0' == la[aStart]) ++aStart;
			while (bStart < lb.size() && '0' == lb[bStart]) ++bStart;

			// Find end of each digit run
			size_t aEnd = aStart;
			size_t bEnd = bStart;
			while (aEnd < la.size() && isdigit((unsigned char)la[aEnd])) ++aEnd;
			while (bEnd < lb.size() && isdigit((unsigned char)lb[bEnd])) ++bEnd;

			size_t aLen = aEnd - aStart;
			size_t bLen = bEnd - bStart;

			// Longer significant-digit run is the larger number
			if (aLen != bLen)
				return aLen < bLen;

			// Same length, compare digit by digit
			for (size_t k = 0; k < aLen; ++k)
			{
				if (la[aStart + k] != lb[bStart + k])
					return la[aStart + k] < lb[bStart + k];
			}

			// Numerically equal; shorter original run (more leading zeros) sorts first
			size_t aRun = aEnd - i;
			size_t bRun = bEnd - j;
			if (aRun != bRun)
				return aRun < bRun;

			i = aEnd;
			j = bEnd;
		}
		else
		{
			if (la[i] != lb[j])
				return la[i] < lb[j];
			++i;
			++j;
		}
	}

	// Shorter string sorts first when one is a prefix of the other
	return (la.size() - i) < (lb.size() - j);
}

// Enumerate the regular-file names in a directory (just the names, not paths).
static std::vector<std::string> listDirectoryFiles(const char* pDirectoryPath)
{
	std::vector<std::string> names;

#ifdef _WIN32
	std::string pattern = pDirectoryPath;
	if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
		pattern += '\\';
	pattern += '*';

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);
	if (INVALID_HANDLE_VALUE != hFind)
	{
		do
		{
			if (0 == (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				names.push_back(findData.cFileName);
			}
		} while (FindNextFileA(hFind, &findData));
		FindClose(hFind);
	}
#else
	DIR* pDir = opendir(pDirectoryPath);
	if (pDir)
	{
		struct dirent* pEntry = nullptr;
		while (nullptr != (pEntry = readdir(pDir)))
		{
			std::string name = pEntry->d_name;
			if ("." == name || ".." == name)
				continue;
			names.push_back(name);
		}
		closedir(pDir);
	}
#endif

	return names;
}

// Join a directory path and a filename with the platform separator.
static std::string joinPath(const std::string& dir, const std::string& name)
{
#ifdef _WIN32
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	if (dir.empty())
		return name;

	char last = dir[dir.size() - 1];
	if (last == '/' || last == '\\')
		return dir + name;

	return dir + sep + name;
}

//------------------------------------------------------------------------------
C1File::C1File(const char *pDirectoryPath)
	: m_widthPixels(320)
	, m_heightPixels(200)
{
	LoadFromDirectory(pDirectoryPath);
}
//------------------------------------------------------------------------------

C1File::~C1File()
{
	// Free Up the memory
	for (unsigned int idx = 0; idx < m_pC1PixelMaps.size(); ++idx)
	{
		delete[] m_pC1PixelMaps[idx];
		m_pC1PixelMaps[ idx ] = nullptr;
	}
}

//------------------------------------------------------------------------------
void C1File::LoadFromDirectory(const char* pDirectoryPath)
{
	// Free Up any existing memory
	for (unsigned int idx = 0; idx < m_pC1PixelMaps.size(); ++idx)
	{
		delete[] m_pC1PixelMaps[idx];
		m_pC1PixelMaps[ idx ] = nullptr;
	}
	m_pC1PixelMaps.clear();
	//--------------------------------------------------------------------------

	// Gather the C1 filenames
	std::vector<std::string> allNames = listDirectoryFiles(pDirectoryPath);

	std::vector<std::string> c1Names;
	for (size_t idx = 0; idx < allNames.size(); ++idx)
	{
		if (isC1Filename(allNames[idx]))
			c1Names.push_back(allNames[idx]);
	}

	// Natural sort so the frames play back in the expected order
	std::sort(c1Names.begin(), c1Names.end(), naturalLess);

	// Load each frame
	for (size_t idx = 0; idx < c1Names.size(); ++idx)
	{
		std::string fullPath = joinPath(pDirectoryPath, c1Names[idx]);

		FILE* pFile = nullptr;
#ifdef _WIN32
		errno_t err = fopen_s(&pFile, fullPath.c_str(), "rb");
#else
		pFile = fopen(fullPath.c_str(), "rb");
		int err = (pFile == nullptr) ? errno : 0;	// errno_t is Windows-only (Annex K)
#endif

		if (0 != err || nullptr == pFile)
		{
			printf("WARNING: Could not open %s - skipping\n", fullPath.c_str());
			continue;
		}

		fseek(pFile, 0, SEEK_END);
		size_t length = ftell(pFile);	// get file size
		fseek(pFile, 0, SEEK_SET);

		if (kC1FrameSize != length)
		{
			printf("WARNING: %s is %d bytes, expected %d (0x8000) - skipping\n",
				   fullPath.c_str(), (int)length, (int)kC1FrameSize);
			fclose(pFile);
			continue;
		}

		unsigned char* pFrame = new unsigned char[kC1FrameSize];
		size_t bytesRead = fread(pFrame, sizeof(unsigned char), kC1FrameSize, pFile);
		fclose(pFile);

		if (kC1FrameSize != bytesRead)
		{
			printf("WARNING: %s short read (%d of %d bytes) - skipping\n",
				   fullPath.c_str(), (int)bytesRead, (int)kC1FrameSize);
			delete[] pFrame;
			continue;
		}

		m_pC1PixelMaps.push_back(pFrame);
	}
}
