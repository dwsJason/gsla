# gsla
Apple IIgs animation tool, alternative to Paintworks/$C2 animation format

This is a C++ command line tool, that will convert a C2/Paintworks Animation
file into the more efficient GS Lzb Animation file format.

See https://github.com/dwsJason/gslaplay, for a GSOS Sample Application
that can play these animations


## Mac Build instructions

Requires xcode (g++/clang++).


Debug build (default):
```
make
```
Release build:
```
make CONFIG=Release
```
Clean build artifacts:
```
make clean
```