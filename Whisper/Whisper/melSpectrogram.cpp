#include "stdafx.h"
#include <cmath>
#include "melSpectrogram.h"

namespace Whisper
{
	HanningWindow::HanningWindow()
	{
		for( int i = 0; i < FFT_SIZE; i++ )
		{
			// TODO [low]: use XMVectorCos instead
			hann[ i ] = (float)( 0.5 * ( 1.0 - std::cos( ( 2.0 * M_PI * i ) / ( FFT_SIZE ) ) ) );
		}
	}
	const HanningWindow s_hanning;
}

namespace
{
	using namespace Whisper;

	uint32_t tempVectorSizeRecursion( uint32_t len )
	{
		// out.resize( in.size() * 2 );
		const uint32_t res = len * 2;
		if( len == 1 )
			return res;
		if( len % 2 == 1 )
			return res;	// dft

		const uint32_t even = ( len + 1 ) / 2;
		const uint32_t odd = len / 2;
		const uint32_t evenFft = tempVectorSizeRecursion( even );
		const uint32_t oddFft = tempVectorSizeRecursion( odd );
		return res + even + odd + evenFft + oddFft;
	}

	// 6000
	// const uint32_t tempBufferSize = FFT_SIZE + tempVectorSizeRecursion( FFT_SIZE );
	constexpr uint32_t tempBufferSize = 6000;

	// naive Discrete Fourier Transform
	// input is real-valued
	// output is complex-valued
	inline void dft( const float* rsi, size_t len, float* rdi )
	{
		for( size_t k = 0; k < len; k++ )
		{
			float re = 0;
			float im = 0;

			for( int n = 0; n < len; n++ )
			{
				float angle = (float)( 2 * M_PI * (int)k * n / len );
				re += (float)( rsi[ n ] * std::cosf( angle ) );
				im -= (float)( rsi[ n ] * std::sinf( angle ) );
			}

			rdi[ k * 2 + 0 ] = re;
			rdi[ k * 2 + 1 ] = im;
		}
	}

	inline void splitEvenOdd( const float* rsi, size_t len, float* rdiEven, float* rdiOdd )
	{
		const float* const rsiEndAligned = rsi + ( len & ( ~(size_t)7 ) );
		const size_t rem = len % 8;

		for( ; rsi < rsiEndAligned; rsi += 8, rdiEven += 4, rdiOdd += 4 )
		{
			const __m128 v1 = _mm_loadu_ps( rsi );
			const __m128 v2 = _mm_loadu_ps( rsi + 4 );
			const __m128 e = _mm_shuffle_ps( v1, v2, _MM_SHUFFLE( 2, 0, 2, 0 ) );
			const __m128 o = _mm_shuffle_ps( v1, v2, _MM_SHUFFLE( 3, 1, 3, 1 ) );
			_mm_storeu_ps( rdiEven, e );
			_mm_storeu_ps( rdiOdd, o );
		}

#pragma loop( no_vector )
		for( size_t i = 0; i < rem; i++, rsi++ )
		{
			if( i % 2 == 0 )
			{
				*rdiEven = *rsi;
				rdiEven++;
			}
			else
			{
				*rdiOdd = *rsi;
				rdiOdd++;
			}
		}
	}
	inline __m128 set2( float f )
	{
		__m128 v = _mm_set_ss( f );
		return _mm_moveldup_ps( v );
	}
	inline __m128 load2( const float* rsi )
	{
		return _mm_castpd_ps( _mm_load_sd( (const double*)rsi ) );
	}
	inline void store2( float* rdi, __m128 vec )
	{
		_mm_store_sd( (double*)rdi, _mm_castps_pd( vec ) );
	}
	// [ x, y ] => [ x, y, x, y ]
	inline __m128 dup2( __m128 x )
	{
		__m128d v = _mm_castps_pd( x );
		v = _mm_movedup_pd( v );
		return _mm_castpd_ps( v );
	}
	inline __m128 load2dup( const float* rsi )
	{
		return _mm_castpd_ps( _mm_loaddup_pd( (const double*)rsi ) );
	}
	inline void store2high( float* rdi, __m128 vec )
	{
		_mm_storeh_pd( (double*)rdi, _mm_castps_pd( vec ) );
	}
}

using namespace Whisper;

SpectrogramContext::SpectrogramContext( const Filters& flt ) :
	filters( flt )
{
	assert( tempBufferSize == FFT_SIZE + tempVectorSizeRecursion( FFT_SIZE ) );
	tempBuffer = std::make_unique<float[]>( tempBufferSize );
}

// Cooley-Tukey FFT
// poor man's implementation - use something better
// input is real-valued
// output is complex-valued
float* SpectrogramContext::fftRecursion( float* temp, const float* const rsi, const size_t len )
{
	float* const out = temp;
	temp += len * 2;
	if( len == 1 )
	{
		out[ 0 ] = rsi[ 0 ];
		out[ 1 ] = 0;
		return temp;
	}

	if( len % 2 == 1 )
	{
		dft( rsi, len, out );
		return temp;
	}

	const size_t lenEven = ( len + 1 ) / 2;
	const size_t lenOdd = len / 2;
	float* const even = temp;
	temp += lenEven;

	float* const odd = temp;
	temp += lenOdd;
	splitEvenOdd( rsi, len, even, odd );

	const float* const evenFft = temp;
	temp = fftRecursion( temp, even, lenEven );

	const float* const oddFft = temp;
	temp = fftRecursion( temp, odd, lenOdd );

	const size_t N = len;
	const __m128 maskNegateHigh = _mm_setr_ps( 0, 0, -0.0f, -0.0f );
	for( size_t k = 0; k < N / 2; k++ )
	{
		const float theta = (float)( 2 * M_PI * (double)(int)k / N );

		/*
		const float re = std::cosf( theta );
		const float im = -std::sinf( theta );

		float re_odd = oddFft[ 2 * k + 0 ];
		float im_odd = oddFft[ 2 * k + 1 ];

		out[ 2 * k + 0 ] = evenFft[ 2 * k + 0 ] + re * re_odd - im * im_odd;
		out[ 2 * k + 1 ] = evenFft[ 2 * k + 1 ] + re * im_odd + im * re_odd;

		out[ 2 * ( k + N / 2 ) + 0 ] = evenFft[ 2 * k + 0 ] - re * re_odd + im * im_odd;
		out[ 2 * ( k + N / 2 ) + 1 ] = evenFft[ 2 * k + 1 ] - re * im_odd - im * re_odd;
		*/

		const __m128 re = _mm_set_ss( std::cosf( theta ) );
		const __m128 im = _mm_set_ss( std::sinf( theta ) );
		__m128 reIm = _mm_shuffle_ps( re, im, _MM_SHUFFLE( 0, 0, 0, 0 ) );
		// [ re, re, im, im ]
		reIm = _mm_xor_ps( reIm, maskNegateHigh );

		// [ re_odd, im_odd ]
		__m128 odd = load2( oddFft + 2 * k );
		// [ re_odd, im_odd, im_odd, re_odd ]
		odd = _mm_shuffle_ps( odd, odd, _MM_SHUFFLE( 0, 1, 1, 0 ) );

		// re_odd * re, im_odd * re, im_odd * im, re_odd * im ]
		const __m128 products4 = _mm_mul_ps( reIm, odd );

		// re_odd * re, im_odd * re, re_odd * re, im_odd * re
		__m128 prod1 = dup2( products4 );
		// im_odd * im, re_odd * im, im_odd * im, re_odd * im
		__m128 prod2 = _mm_movehl_ps( products4, products4 );

		// re_odd * re, im_odd * re, -re_odd * re, -im_odd * re
		prod1 = _mm_xor_ps( prod1, maskNegateHigh );
		// im_odd * im, re_odd * im, -im_odd * im, -re_odd * im
		prod2 = _mm_xor_ps( prod2, maskNegateHigh );

		const __m128 even = load2dup( evenFft + 2 * k );
		__m128 res;
		res = _mm_add_ps( even, prod1 );
		res = _mm_addsub_ps( res, prod2 );
		store2( out + 2 * k, res );
		store2high( out + 2 * ( k + N / 2 ), res );
	}

	return temp;
}

void SpectrogramContext::fft( std::array<float, N_MEL>& rdi, const float* pcm, size_t length )
{
	assert( length > 0 );
	length = std::min( length, (size_t)FFT_SIZE );

	float* const temp = tempBuffer.get();
	// Apply Hanning window
	for( size_t i = 0; i < length; i++ )
		temp[ i ] = pcm[ i ] * s_hanning[ i ];
	if( length < FFT_SIZE )
		memset( temp + length, 0, ( FFT_SIZE - length ) * 4 );

	float* const fftOut = temp + FFT_SIZE;
	float* bufferEnd = fftRecursion( fftOut, temp, FFT_SIZE );
	assert( bufferEnd == tempBuffer.get() + tempBufferSize );

	// for( size_t j = 0; j < FFT_SIZE; j++ )
	//	fft_out[ j ] = ( fft_out[ 2 * j + 0 ] * fft_out[ 2 * j + 0 ] + fft_out[ 2 * j + 1 ] * fft_out[ 2 * j + 1 ] );
	for( size_t j = 0; j < 4; j++ )
	{
		__m128 tmp = load2( fftOut + 2 * j );
		tmp = _mm_mul_ps( tmp, tmp );
		tmp = _mm_add_ss( tmp, _mm_movehdup_ps( tmp ) );
		_mm_store_ss( fftOut + j, tmp );
	}
	for( size_t j = 4; j < FFT_SIZE; j += 4 )
	{
		__m128 low = _mm_loadu_ps( fftOut + 2 * j );
		__m128 high = _mm_loadu_ps( fftOut + 2 * j + 4 );
		low = _mm_mul_ps( low, low );
		high = _mm_mul_ps( high, high );
		__m128 res = _mm_hadd_ps( low, high );
		_mm_storeu_ps( fftOut + j, res );
	}

	// for( size_t j = 1; j < FFT_SIZE / 2; j++ )
	// 	fftOut[ j ] += fftOut[ FFT_SIZE - j ];
	for( size_t j = 1; j < 4; j++ )
		fftOut[ j ] += fftOut[ FFT_SIZE - j ];
	for( size_t j = 4; j < FFT_SIZE / 2; j += 4 )
	{
		__m128 curr = _mm_loadu_ps( fftOut + j );
		// Too bad _mm_loadr_ps requires alignment
		__m128 high = _mm_loadu_ps( fftOut + ( FFT_SIZE - 3 ) - j );
		high = _mm_shuffle_ps( high, high, _MM_SHUFFLE( 0, 1, 2, 3 ) );
		curr = _mm_add_ps( curr, high );
		_mm_storeu_ps( fftOut + j, curr );
	}

	constexpr size_t n_fft = 1 + ( FFT_SIZE / 2 );

	// mel spectrogram
	for( size_t j = 0; j < N_MEL; j++ )
	{
		double sum = 0.0;
		for( size_t k = 0; k < n_fft; k++ )
			sum += fftOut[ k ] * filters.data[ j * n_fft + k ];
		if( sum < 1e-10 )
			sum = 1e-10;
		sum = log10( sum );
		rdi[ j ] = (float)sum;
	}

	/*
	const float* ptr = rdi.data();
	const float* const ptrEnd = ptr + rdi.size();
	static_assert( 0 == N_MEL % 4 );
	__m128 ax = _mm_loadu_ps( ptr );
	for( ptr += 4; ptr < ptrEnd; ptr += 4 )
		ax = _mm_max_ps( ax, _mm_loadu_ps( ptr ) );
	ax = _mm_max_ps( ax, _mm_movehl_ps( ax, ax ) );
	ax = _mm_max_ss( ax, _mm_movehdup_ps( ax ) );
	return _mm_cvtss_f32( ax );
	*/
}