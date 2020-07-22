/*
	bctypes.h

	Because I need Standard Types
*/

#ifndef _bctypes_h
#define _bctypes_h

typedef	signed char			i8;
typedef unsigned char		u8;
typedef signed short		i16;
typedef unsigned short		u16;
typedef signed long			i32;
typedef unsigned long		u32;

typedef signed long long	i64;
typedef unsigned long long	u64;

// If we're using C, I still like having a bool around
#ifndef __cplusplus
typedef	i32					bool;
#define false (0)
#define true (!false)
#define nullptr 0
#endif

typedef float				f32;
typedef float				r32;
typedef double				f64;
typedef double				r64;


#define null (0)

// Odd Types
typedef union {
//  u128       ul128;
  u64        ul64[2];
  u32        ui32[4];
} QWdata;


#endif // _bctypes_h

// EOF - bctypes.h


