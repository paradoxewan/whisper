#include "params.h"
#include "../../Whisper/API/iContext.cl.h"
#include "../../Whisper/API/iMediaFoundation.cl.h"
#include "../../ComLightLib/comLightClient.h"
#include "miscUtils.h"
#include <array>
#include <atomic>
using namespace Whisper;

#define STREAM_AUDIO 1

static HRESULT loadWhisperModel( const wchar_t* path, iModel** pp )
{
	using namespace Whisper;
	constexpr eModelImplementation impl = eModelImplementation::GPU;
	// constexpr eModelImplementation impl = eModelImplementation::Reference;
	return Whisper::loadModel( path, impl, nullptr, pp );
}

namespace
{
	struct sPrintUserData
	{
		const whisper_params* params;
		// const std::vector<std::vector<float>>* pcmf32s;
	};

	// Terminal color map. 10 colors grouped in ranges [0.0, 0.1, ..., 0.9]
	// Lowest is red, middle is yellow, highest is green.
	static const std::array<const char*, 10> k_colors =
	{
		"\033[38;5;196m", "\033[38;5;202m", "\033[38;5;208m", "\033[38;5;214m", "\033[38;5;220m",
		"\033[38;5;226m", "\033[38;5;190m", "\033[38;5;154m", "\033[38;5;118m", "\033[38;5;82m",
	};

	std::string to_timestamp( sTimeSpan ts, bool comma = false )
	{
		sTimeSpanFields fields = ts;
		uint32_t msec = fields.ticks / 10'000;
		uint32_t hr = fields.days * 24 + fields.hours;
		uint32_t min = fields.minutes;
		uint32_t sec = fields.seconds;

		char buf[ 32 ];
		snprintf( buf, sizeof( buf ), "%02d:%02d:%02d%s%03d", hr, min, sec, comma ? "," : ".", msec );
		return std::string( buf );
	}

	static int colorIndex( const sToken& tok )
	{
		const float p = tok.probability;
		const float p3 = p * p * p;
		int col = (int)( p3 * float( k_colors.size() ) );
		col = std::max( 0, std::min( (int)k_colors.size() - 1, col ) );
		return col;
	}

	HRESULT __cdecl newSegmentCallback( iContext* context, uint32_t n_new, void* user_data ) noexcept
	{
		ComLight::CComPtr<iTranscribeResult> results;
		CHECK( context->getResults( eResultFlags::Timestamps | eResultFlags::Tokens, &results ) );

		sTranscribeLength length;
		CHECK( results->getSize( length ) );

		const whisper_params& params = *( (sPrintUserData*)user_data )->params;
		// const std::vector<std::vector<float>>& pcmf32s = *( (sPrintUserData*)user_data )->pcmf32s;

		// print the last n_new segments
		const uint32_t s0 = length.countSegments - n_new;
		if( s0 == 0 )
			printf( "\n" );

		const sSegment* const segments = results->getSegments();
		const sToken* const tokens = results->getTokens();

		for( uint32_t i = s0; i < length.countSegments; i++ )
		{
			const sSegment& seg = segments[ i ];

			if( params.no_timestamps )
			{
				if( params.print_colors )
				{
					for( uint32_t j = 0; j < seg.countTokens; j++ )
					{
						const sToken& tok = tokens[ seg.firstToken + j ];
						if( !params.print_special && ( tok.flags & eTokenFlags::Special ) )
							continue;
						wprintf( L"%S%s%S", k_colors[ colorIndex( tok ) ], utf16( tok.text ).c_str(), "\033[0m" );
					}
				}
				else
					wprintf( L"%s", utf16( seg.text ).c_str() );
				fflush( stdout );
				continue;
			}

			std::string speaker = "";
#if 0
			if( params.diarize && pcmf32s.size() == 2 )
			{
				const size_t n_samples = pcmf32s[ 0 ].size();
				const int64_t is0 = SourceAudio::sampleFromTimestamp( seg.time.begin, n_samples );
				const int64_t is1 = SourceAudio::sampleFromTimestamp( seg.time.end, n_samples );

				double energy0 = 0.0f;
				double energy1 = 0.0f;

				for( int64_t j = is0; j < is1; j++ )
				{
					energy0 += fabs( pcmf32s[ 0 ][ j ] );
					energy1 += fabs( pcmf32s[ 1 ][ j ] );
				}

				if( energy0 > 1.1 * energy1 )
					speaker = "(speaker 0)";
				else if( energy1 > 1.1 * energy0 )
					speaker = "(speaker 1)";
				else
					speaker = "(speaker ?)";

				//printf("is0 = %lld, is1 = %lld, energy0 = %f, energy1 = %f, %s\n", is0, is1, energy0, energy1, speaker.c_str());
			}
#endif

			if( params.print_colors )
			{
				printf( "[%s --> %s]  ", to_timestamp( seg.time.begin ).c_str(), to_timestamp( seg.time.end ).c_str() );

				for( uint32_t j = 0; j < seg.countTokens; j++ )
				{
					const sToken& tok = tokens[ seg.firstToken + j ];
					if( !params.print_special && ( tok.flags & eTokenFlags::Special ) )
						continue;
					wprintf( L"%S%S%s%S", speaker.c_str(), k_colors[ colorIndex( tok ) ], utf16( tok.text ).c_str(), "\033[0m" );
				}
				printf( "\n" );
			}
			else
				wprintf( L"[%S --> %S]  %S%s\n", to_timestamp( seg.time.begin ).c_str(), to_timestamp( seg.time.end ).c_str(), speaker.c_str(), utf16( seg.text ).c_str() );
		}
		return S_OK;
	}

	HRESULT __cdecl beginSegmentCallback( iContext* context, void* user_data ) noexcept
	{
		std::atomic_bool* flag = (std::atomic_bool*)user_data;
		bool aborted = flag->load();
		return aborted ? S_FALSE : S_OK;
	}

	HRESULT setupConsoleColors()
	{
		HANDLE h = GetStdHandle( STD_OUTPUT_HANDLE );
		if( h == INVALID_HANDLE_VALUE )
			return HRESULT_FROM_WIN32( GetLastError() );

		DWORD mode = 0;
		if( !GetConsoleMode( h, &mode ) )
			return HRESULT_FROM_WIN32( GetLastError() );
		if( 0 != ( mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING ) )
			return S_FALSE;

		mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		if( !SetConsoleMode( h, mode ) )
			return HRESULT_FROM_WIN32( GetLastError() );
		return S_OK;
	}
}

int wmain( int argc, wchar_t* argv[] )
{
	// Whisper::dbgCompareTraces( LR"(C:\Temp\2remove\Whisper\ref.bin)", LR"(C:\Temp\2remove\Whisper\gpu.bin )" ); return 0;

	// Tell logger to use the standard output stream for the messages
	{
		Whisper::sLoggerSetup logSetup;
		logSetup.flags = eLoggerFlags::UseStandardError;
		logSetup.level = eLogLevel::Debug;
		Whisper::setupLogger( logSetup );
	}

	whisper_params params;
	if( !params.parse( argc, argv ) )
		return 1;

	if( params.print_colors )
	{
		if( FAILED( setupConsoleColors() ) )
			params.print_colors = false;
	}

	if( params.fname_inp.empty() )
	{
		fprintf( stderr, "error: no input files specified\n" );
		whisper_print_usage( argc, argv, params );
		return 2;
	}

	if( Whisper::findLanguageKeyA( params.language.c_str() ) == UINT_MAX )
	{
		fprintf( stderr, "error: unknown language '%s'\n", params.language.c_str() );
		whisper_print_usage( argc, argv, params );
		return 3;
	}

	ComLight::CComPtr<iModel> model;
	HRESULT hr = loadWhisperModel( params.model.c_str(), &model );
	if( FAILED( hr ) )
	{
		printError( "failed to load the model", hr );
		return 4;
	}

	ComLight::CComPtr<iContext> context;
	hr = model->createContext( &context );
	if( FAILED( hr ) )
	{
		printError( "failed to initialize whisper context", hr );
		return 5;
	}

	ComLight::CComPtr<iMediaFoundation> mf;
	hr = initMediaFoundation( &mf );
	if( FAILED( hr ) )
	{
		printError( "failed to initialize Media Foundation runtime", hr );
		return 5;
	}

	for( const std::wstring& fname : params.fname_inp )
	{
		// print some info about the processing
		{
			if( model->isMultilingual() == S_FALSE )
			{
				if( params.language != "en" || params.translate )
				{
					params.language = "en";
					params.translate = false;
					fprintf( stderr, "%s: WARNING: model is not multilingual, ignoring language and translation options\n", __func__ );
				}
			}

			/*
			fwprintf( stderr, L"%S: processing '%s' (%zu samples, %.1f sec), %d threads, %d processors, lang = %S, task = %S, timestamps = %d ...\n",
				__func__, fname.c_str(), audio.pcmf32.size(), audio.seconds(),
				params.n_threads, params.n_processors,
				params.language.c_str(),
				params.translate ? "translate" : "transcribe",
				params.no_timestamps ? 0 : 1 );
			*/
		}

		// run the inference
		Whisper::sFullParams wparams;
		context->fullDefaultParams( eSamplingStrategy::Greedy, &wparams );

		wparams.resetFlag( eFullParamsFlags::PrintRealtime | eFullParamsFlags::PrintProgress );
		wparams.setFlag( eFullParamsFlags::PrintTimestamps, !params.no_timestamps );
		wparams.setFlag( eFullParamsFlags::PrintSpecial, params.print_special );
		wparams.setFlag( eFullParamsFlags::Translate, params.translate );
		wparams.language = Whisper::makeLanguageKey( params.language.c_str() );
		wparams.cpuThreads = params.n_threads;
		if( params.max_context != UINT_MAX )
			wparams.n_max_text_ctx = params.max_context;
		wparams.offset_ms = params.offset_t_ms;
		wparams.duration_ms = params.duration_ms;

		wparams.setFlag( eFullParamsFlags::TokenTimestamps, params.output_wts || params.max_len > 0 );
		wparams.thold_pt = params.word_thold;
		wparams.max_len = params.output_wts && params.max_len == 0 ? 60 : params.max_len;

		wparams.setFlag( eFullParamsFlags::SpeedupAudio, params.speed_up );
		// sPrintUserData user_data = { &params, &audio.pcmf32s };
		sPrintUserData user_data = { &params };

		// this callback is called on each new segment
		if( !wparams.flag( eFullParamsFlags::PrintRealtime ) )
		{
			wparams.new_segment_callback = &newSegmentCallback;
			wparams.new_segment_callback_user_data = &user_data;
		}

		// example for abort mechanism
		// in this example, we do not abort the processing, but we could if the flag is set to true
		// the callback is called before every encoder run - if it returns false, the processing is aborted
		std::atomic_bool is_aborted = false;
		{
			wparams.encoder_begin_callback = &beginSegmentCallback;
			wparams.encoder_begin_callback_user_data = &is_aborted;
		}

#if STREAM_AUDIO
		ComLight::CComPtr<iAudioReader> reader;
		CHECK( mf->openAudioFile( fname.c_str(), params.diarize, &reader ) );
		sProgressSink progressSink{ nullptr, nullptr };
		hr = context->runStreamed( wparams, progressSink, reader );
#else
		ComLight::CComPtr<iAudioBuffer> buffer;
		CHECK( mf->loadAudioFile( fname.c_str(), params.diarize, &buffer ) );
		hr = context->runFull( wparams, buffer );
#endif
		if( FAILED( hr ) )
		{
			fwprintf( stderr, L"%s: failed to process audio\n", argv[ 0 ] );
			return 10;
		}
	}

	context->timingsPrint();
	context = nullptr;
	return 0;
}