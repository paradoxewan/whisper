// This compute shader implements matrix*matrix product, using tiling and other tricks to improve the performance
#ifndef TILE_SIZE
static const uint TILE_SIZE = 32;
#endif

#ifndef THREADS_Y
// Performance measures on Ryzen 7 5700G iGPU, the time is just for this shader:
// 1 (32 threads per group) - 17.1 seconds, 2 - 9.02424 seconds, 4 - 6.95762 seconds, 6 - 6.79011 seconds, 8 - 6.67279 seconds, 10 - 6.9456 seconds, 16 - 7.20502 seconds
// On nVidia, 8 is also the fastest option.
static const uint THREADS_Y = 8;
#endif

#ifndef STREAM_SECOND_MATRIX
#define STREAM_SECOND_MATRIX 0
#endif

#ifndef LOAD_ORDER

// Load with coalesced loads from global memory whenever possible, store into groupshared buffer with random stores
// #define LOAD_ORDER bool2( ( 1 == arg0Strides[ 0 ] ) || ( 1 != arg0Strides[ 1 ] ), ( 1 == arg1Strides[ 0 ] ) || ( 1 != arg1Strides[ 1 ] ) )

// Load with random loads from global memory, store into groupshared buffer with coalesced stores
// On my AMD iGPU inside Ryzen 7 5700G, there's whopping 15% performance win with that tactics, from 6.67 to 5.66 seconds for this shader.
// My nVidia GPU does about the same
#define LOAD_ORDER bool2( false, true )

#endif

Buffer<float> arg0: register( t0 );
Buffer<float> arg1: register( t1 );
RWBuffer<float> result: register( u0 );

cbuffer Constants: register( b0 )
{
	uint4 arg0Size: packoffset( c0 );
	uint4 arg0Strides: packoffset( c1 );
	uint4 arg1Strides: packoffset( c3 );
	uint4 resultSize: packoffset( c4 );
	uint4 resultStrides: packoffset( c5 );
}

groupshared float tile0[ TILE_SIZE ][ TILE_SIZE ];
#if !STREAM_SECOND_MATRIX
groupshared float tile1[ TILE_SIZE ][ TILE_SIZE ];
#endif
groupshared float resTemp[ TILE_SIZE ][ TILE_SIZE ];

#if STREAM_SECOND_MATRIX
void multiplyTiles( uint rsi, const uint3 thread, const uint w, const uint h )
{
	for( uint i = thread.y; i < h; i += THREADS_Y, rsi += THREADS_Y * arg1Strides.y )
	{
		float r = 0;
		uint rsiRow = rsi;
		for( uint j = 0; j < w; j++, rsiRow += arg1Strides.x )
		{
			// One TILE_SIZE * 4 bytes coalesced load, broadcasted into THREADS_Y copies
			const float s0 = tile0[ j ][ thread.x ];
			// THREADS_Y broadcasts from global memory, each one is 4 bytes broadcasted into TILE_SIZE copies
			const float s1 = arg1[ rsiRow ];
			// Multiply and accumulate
			r = mad( s0, s1, r );
		}
		// Accumulate into the output tile
		// THREADS_Y * 128 bytes coalesced loads and stores
		resTemp[ i ][ thread.x ] += r;
	}
}
#else
// Compute resTemp += tile0 * tile1, for TILE_SIZE^2 square matrices
// The group size is TILE_SIZE*THREADS_Y threads in this shader
void multiplyTiles( const uint3 thread )
{
	for( uint i = thread.y; i < TILE_SIZE; i += THREADS_Y )
	{
		float r = 0;
		for( uint j = 0; j < TILE_SIZE; j++ )
		{
			// One TILE_SIZE * 4 bytes coalesced load, broadcasted into THREADS_Y copies
			const float s0 = tile0[ j ][ thread.x ];
			// THREADS_Y broadcasts, each one is 4 bytes broadcasted into TILE_SIZE copies
			const float s1 = tile1[ i ][ j ];
			// Multiply and accumulate
			r = mad( s0, s1, r );
		}
		// Accumulate into the output tile
		// THREADS_Y * 128 bytes coalesced loads and stores
		resTemp[ i ][ thread.x ] += r;
	}
}
#endif

// Note we transposed these tiles while loading
void loadTile0( uint rsi, const uint3 thread, const uint w, const uint h, const bool rowMajor )
{
	uint i;
	if( rowMajor )
	{
		rsi += arg0Strides.y * thread.y;
		for( i = thread.y; i < h; i += THREADS_Y, rsi += arg0Strides.y * THREADS_Y )
		{
			if( thread.x < w )
				tile0[ thread.x ][ i ] = arg0[ rsi + thread.x * arg0Strides.x ];
			else
				tile0[ thread.x ][ i ] = 0.0;
		}
	}
	else
	{
		// Unlike width which is smaller for the last tile, the height is always the same, and all these tiles are zero-initialized
		if( thread.x >= h )
			return;

		rsi += arg0Strides.x * thread.y;
		for( i = thread.y; i < w; i += THREADS_Y, rsi += arg0Strides.x * THREADS_Y )
			tile0[ i ][ thread.x ] = arg0[ rsi + thread.x * arg0Strides.y ];

		if( i >= TILE_SIZE )
			return;
		for( ; i < TILE_SIZE; i += THREADS_Y )
			tile0[ i ][ thread.x ] = 0.0;
	}
}

#if !STREAM_SECOND_MATRIX
void loadTile1( uint rsi, const uint3 thread, const uint w, const uint h, const bool rowMajor )
{
	uint i;
	if( rowMajor )
	{
		rsi += thread.y * arg1Strides.y;

		for( i = thread.y; i < h; i += THREADS_Y, rsi += arg1Strides.y * THREADS_Y )
		{
			if( thread.x < w )
				tile1[ i ][ thread.x ] = arg1[ rsi + thread.x * arg1Strides.x ];
			else
				tile1[ i ][ thread.x ] = 0.0;
		}
	}
	else
	{
		// Unlike width which is smaller for the last tile, the height is always the same, and all these tiles are zero-initialized
		if( thread.x >= h )
			return;

		rsi += thread.y * arg1Strides.x;
		for( i = thread.y; i < w; i += THREADS_Y, rsi += arg1Strides.x * THREADS_Y )
			tile1[ thread.x ][ i ] = arg1[ rsi + thread.x * arg0Strides.y ];
		if( i >= TILE_SIZE )
			return;
		for( ; i < TILE_SIZE; i += THREADS_Y )
			tile1[ thread.x ][ i ] = 0.0;
	}
}
#endif

void storeTile( const uint3 thread, const uint4 pos, const uint2 size )
{
	if( thread.x >= size.x )
		return;
	const uint4 prod4 = pos * resultStrides;
	const uint2 prod2 = prod4.xy + prod4.zw;
	uint rdi = prod2.x + prod2.y;
	rdi += resultStrides.y * thread.y;
	for( uint i = thread.y; i < size.y; i += THREADS_Y, rdi += resultStrides.y * THREADS_Y )
		result[ rdi + thread.x * resultStrides.x ] = resTemp[ i ][ thread.x ];
}

[ numthreads( TILE_SIZE, THREADS_Y, 1 ) ]
void main( uint3 group: SV_GroupID, uint3 thread : SV_GroupThreadID )
{
	// Zero out these shared buffers
	for( uint i = 0; i < TILE_SIZE; i += THREADS_Y )
	{
		tile0[ i + thread.y ][ thread.x ] = 0.0;
#if !STREAM_SECOND_MATRIX
		tile1[ i + thread.y ][ thread.x ] = 0.0;
#endif
		resTemp[ i + thread.y ][ thread.x ] = 0.0;
	}

	const uint2 resultPos = group.xy * TILE_SIZE;
	const uint2 layer = uint2( group.z % resultSize.z, group.z / resultSize.z );
	uint rsi0 = resultPos.x * arg0Strides.y + layer.x * arg0Strides.z + layer.y * arg0Strides.w;
	uint rsi1 = resultPos.y * arg1Strides.y + layer.x * arg1Strides.z + layer.y * arg1Strides.w;

	const uint rsi0Inc = TILE_SIZE * arg0Strides.x;
	const uint rsi1Inc = TILE_SIZE * arg1Strides.x;

	const uint completeTiles = arg0Size.x / TILE_SIZE;
	const uint rsi0AndAligned = rsi0 + rsi0Inc * completeTiles;
	// Output tile size
	// Normally TILE_SIZE^2, less than that for the tiles at the right and bottom edges of the output matrix
	const uint2 outputSize = min( TILE_SIZE, resultSize.xy - resultPos );

	const bool2 loadOrder = LOAD_ORDER;

#if STREAM_SECOND_MATRIX
	rsi1 += thread.y * arg1Strides.y;
#endif
	for( ; rsi0 < rsi0AndAligned; rsi0 += rsi0Inc, rsi1 += rsi1Inc )
	{
		loadTile0( rsi0, thread, TILE_SIZE, outputSize.x, loadOrder.x );
#if STREAM_SECOND_MATRIX
		GroupMemoryBarrierWithGroupSync();
		multiplyTiles( rsi1, thread, TILE_SIZE, outputSize.y );
#else
		loadTile1( rsi1, thread, TILE_SIZE, outputSize.y, loadOrder.y );
		GroupMemoryBarrierWithGroupSync();
		multiplyTiles( thread );
#endif
		// Need one moar barrier here.
		// Otherwise, some threads of the group are loading the next tile into tile0/tile1 groupshared buffers on the next iteration of the loop,
		// while other threads of the same group are still computing the matrix product, and getting incorrect values from that groupshared buffer.
		// The missing barrier only caused a bug on AMD, and only with "ggml-large.bin" model; no idea why that is.
		GroupMemoryBarrierWithGroupSync();
	}

	const uint rem = arg0Size.x % TILE_SIZE;
	if( 0 != rem )
	{
		loadTile0( rsi0, thread, rem, outputSize.x, loadOrder.x );
#if STREAM_SECOND_MATRIX
		GroupMemoryBarrierWithGroupSync();
		multiplyTiles( rsi1, thread, rem, outputSize.y );
#else
		loadTile1( rsi1, thread, rem, outputSize.y, loadOrder.y );
		GroupMemoryBarrierWithGroupSync();
		multiplyTiles( thread );
#endif
	}

	GroupMemoryBarrierWithGroupSync();
	storeTile( thread, uint4( resultPos, layer ), outputSize );
}