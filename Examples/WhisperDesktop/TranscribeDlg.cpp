#include "stdafx.h"
#include "TranscribeDlg.h"
#include "Utils/logger.h"

HRESULT TranscribeDlg::show()
{
	auto res = DoModal( nullptr );
	if( res == -1 )
		return HRESULT_FROM_WIN32( GetLastError() );
	switch( res )
	{
	case IDC_BACK:
		return SCREEN_MODEL;
	case IDC_CAPTURE:
		return SCREEN_CAPTURE;
	}
	return S_OK;
}

constexpr int progressMaxInteger = 1024 * 8;

static const LPCTSTR regValInput = L"sourceMedia";
static const LPCTSTR regValOutFormat = L"resultFormat";
static const LPCTSTR regValOutPath = L"resultPath";

LRESULT TranscribeDlg::OnInitDialog( UINT nMessage, WPARAM wParam, LPARAM lParam, BOOL& bHandled )
{
	// First DDX call, hooks up variables to controls.
	DoDataExchange( false );
	printModelDescription();
	languageSelector.initialize( m_hWnd, IDC_LANGUAGE, appState );
	cbConsole.initialize( m_hWnd, IDC_CONSOLE, appState );
	cbTranslate.initialize( m_hWnd, IDC_TRANSLATE, appState );
	populateOutputFormats();

	pendingState.initialize(
		{
			languageSelector,
			sourceMediaPath, GetDlgItem( IDC_BROWSE_MEDIA ),
			transcribeOutFormat,
			transcribeOutputPath, GetDlgItem( IDC_BROWSE_RESULT ),
			GetDlgItem( IDC_TRANSCRIBE ),
			GetDlgItem( IDCANCEL ),
			GetDlgItem( IDC_BACK ),
			GetDlgItem( IDC_CAPTURE )
		},
		{
			progressBar, GetDlgItem( IDC_PENDING_TEXT )
		} );

	HRESULT hr = work.create( this );
	if( FAILED( hr ) )
	{
		reportError( m_hWnd, L"CreateThreadpoolWork failed", nullptr, hr );
		EndDialog( IDCANCEL );
	}

	progressBar.SetRange32( 0, progressMaxInteger );
	progressBar.SetStep( 1 );

	sourceMediaPath.SetWindowText( appState.stringLoad( regValInput ) );
	transcribeOutFormat.SetCurSel( (int)appState.dwordLoad( regValOutFormat, 0 ) );
	transcribeOutputPath.SetWindowText( appState.stringLoad( regValOutPath ) );
	BOOL unused;
	OnOutFormatChange( 0, 0, nullptr, unused );

	appState.lastScreenSave( SCREEN_TRANSCRIBE );
	appState.setupIcon( this );
	ATLVERIFY( CenterWindow() );
	return 0;
}

void TranscribeDlg::printModelDescription()
{
	CString text;
	if( S_OK == appState.model->isMultilingual() )
		text = L"Multilingual";
	else
		text = L"Single-language";
	text += L" model \"";
	LPCTSTR path = appState.source.path;
	path = ::PathFindFileName( path );
	text += path;
	text += L"\", ";
	const int64_t cb = appState.source.sizeInBytes;
	if( cb < 1 << 30 )
	{
		constexpr double mul = 1.0 / ( 1 << 20 );
		double mb = (double)cb * mul;
		text.AppendFormat( L"%.1f MB", mb );
	}
	else
	{
		constexpr double mul = 1.0 / ( 1 << 30 );
		double gb = (double)cb * mul;
		text.AppendFormat( L"%.2f GB", gb );
	}
	text += L" on disk, ";
	text += implString( appState.source.impl );
	text += L" implementation";

	modelDesc.SetWindowText( text );
}

void TranscribeDlg::populateOutputFormats()
{
	transcribeOutFormat.AddString( L"None" );
	transcribeOutFormat.AddString( L"Text File" );
	transcribeOutFormat.AddString( L"SubRip subtitles" );
	transcribeOutFormat.AddString( L"WebVTT subtitles" );
}

enum struct TranscribeDlg::eOutputFormat : uint8_t
{
	None = 0,
	Text = 1,
	SubRip = 2,
	WebVTT = 3
};

LRESULT TranscribeDlg::OnOutFormatChange( UINT, INT, HWND, BOOL& bHandled )
{
	const BOOL enabled = transcribeOutFormat.GetCurSel() != 0;
	transcribeOutputPath.EnableWindow( enabled );
	transcribeOutputBrowse.EnableWindow( enabled );
	return 0;
}

void TranscribeDlg::onBrowseMedia()
{
	LPCTSTR title = L"Input audio file to transcribe";
	LPCTSTR filters = L"Multimedia Files\0*.wav;*.wave;*.mp3;*.wma;*.mp4;*.mpeg4;*.mkv\0\0";

	CString path;
	sourceMediaPath.GetWindowText( path );
	if( getOpenFileName( m_hWnd, title, filters, path ) )
		sourceMediaPath.SetWindowText( path );
}

static const LPCTSTR outputFilters = L"Text files (*.txt)\0*.txt\0SubRip subtitles (*.srt)\0*.srt\0WebVTT subtitles (*.vtt)\0*.vtt\0\0";
static const std::array<LPCTSTR, 3> outputExtensions =
{
	L".txt", L".srt", L".vtt"
};

void TranscribeDlg::onBrowseOutput()
{
	const DWORD origFilterIndex = (DWORD)transcribeOutFormat.GetCurSel() - 1;

	LPCTSTR title = L"Output Text File";
	CString path;
	transcribeOutputPath.GetWindowText( path );
	DWORD filterIndex = origFilterIndex;
	if( !getSaveFileName( m_hWnd, title, outputFilters, path, &filterIndex ) )
		return;

	LPCTSTR ext = PathFindExtension( path );
	if( 0 == *ext && filterIndex < outputExtensions.size() )
	{
		wchar_t* const buffer = path.GetBufferSetLength( path.GetLength() + 5 );
		PathRenameExtension( buffer, outputExtensions[ filterIndex ] );
		path.ReleaseBuffer();
	}

	transcribeOutputPath.SetWindowText( path );
	if( filterIndex != origFilterIndex )
		transcribeOutFormat.SetCurSel( filterIndex + 1 );
}

void TranscribeDlg::setPending( bool nowPending )
{
	pendingState.setPending( nowPending );
}

void TranscribeDlg::transcribeError( LPCTSTR text, HRESULT hr )
{
	reportError( m_hWnd, text, L"Unable to transcribe audio", hr );
}

void TranscribeDlg::onTranscribe()
{
	// Validate input
	sourceMediaPath.GetWindowText( transcribeArgs.pathMedia );
	if( transcribeArgs.pathMedia.GetLength() <= 0 )
	{
		transcribeError( L"Please select an input audio file" );
		return;
	}

	if( !PathFileExists( transcribeArgs.pathMedia ) )
	{
		transcribeError( L"Input audio file does not exist", HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) );
		return;
	}

	transcribeArgs.language = languageSelector.selectedLanguage();
	transcribeArgs.translate = cbTranslate.checked();
	if( isInvalidTranslate( m_hWnd, transcribeArgs.language, transcribeArgs.translate ) )
		return;

	transcribeArgs.format = (eOutputFormat)(uint8_t)transcribeOutFormat.GetCurSel();
	if( transcribeArgs.format != eOutputFormat::None )
	{
		transcribeOutputPath.GetWindowText( transcribeArgs.pathOutput );
		if( transcribeArgs.pathOutput.GetLength() <= 0 )
		{
			transcribeError( L"Please select an output text file" );
			return;
		}
		appState.stringStore( regValOutPath, transcribeArgs.pathOutput );
	}
	else
		cbConsole.ensureChecked();

	appState.dwordStore( regValOutFormat, (uint32_t)(int)transcribeArgs.format );
	languageSelector.saveSelection( appState );
	cbTranslate.saveSelection( appState );
	appState.stringStore( regValInput, transcribeArgs.pathMedia );

	setPending( true );

	work.post();
}

void __stdcall TranscribeDlg::poolCallback() noexcept
{
	HRESULT hr = transcribe();
	PostMessage( WM_CALLBACK_STATUS, (WPARAM)hr );
}

static void printTime( CString& rdi, int64_t ticks )
{
	const Whisper::sTimeSpan ts{ (uint64_t)ticks };
	const Whisper::sTimeSpanFields fields = ts;

	if( fields.days != 0 )
	{
		rdi.AppendFormat( L"%i days, %i hours", fields.days, (int)fields.hours );
		return;
	}
	if( ( fields.hours | fields.minutes ) != 0 )
	{
		rdi.AppendFormat( L"%02d:%02d:%02d", (int)fields.hours, (int)fields.minutes, (int)fields.seconds );
		return;
	}
	rdi.AppendFormat( L"%.3f seconds", (double)ticks / 1E7 );
}

LRESULT TranscribeDlg::onCallbackStatus( UINT, WPARAM wParam, LPARAM, BOOL& bHandled )
{
	setPending( false );
	const HRESULT hr = (HRESULT)wParam;
	if( FAILED( hr ) )
	{
		LPCTSTR failMessage = L"Transcribe failed";

		if( transcribeArgs.errorMessage.GetLength() > 0 )
		{
			CString tmp = failMessage;
			tmp += L"\n";
			tmp += transcribeArgs.errorMessage;
			transcribeError( tmp, hr );
		}
		else
			transcribeError( failMessage, hr );

		return 0;
	}

	const int64_t elapsed = ( GetTickCount64() - transcribeArgs.startTime ) * 10'000;
	const int64_t media = transcribeArgs.mediaDuration;
	CString message = L"Transcribed the audio\nMedia duration: ";
	printTime( message, media );
	message += L"\nProcessing time: ";
	printTime( message, elapsed );
	message += L"\nRelative processing speed: ";
	double mul = (double)media / (double)elapsed;
	message.AppendFormat( L"%g", mul );

	MessageBox( message, L"Transcribe Completed", MB_OK | MB_ICONINFORMATION );
	return 0;
}

void TranscribeDlg::getThreadError()
{
	getLastError( transcribeArgs.errorMessage );
}

#define CHECK_EX( hr ) { const HRESULT __hr = ( hr ); if( FAILED( __hr ) ) { getThreadError(); return __hr; } }

HRESULT TranscribeDlg::transcribe()
{
	transcribeArgs.startTime = GetTickCount64();
	clearLastError();
	transcribeArgs.errorMessage = L"";

	using namespace Whisper;
	CComPtr<iAudioReader> reader;

	CHECK_EX( appState.mediaFoundation->openAudioFile( transcribeArgs.pathMedia, false, &reader ) );
	CHECK_EX( reader->getDuration( transcribeArgs.mediaDuration ) );

	const eOutputFormat format = transcribeArgs.format;
	CAtlFile outputFile;
	if( format != eOutputFormat::None )
		CHECK( outputFile.Create( transcribeArgs.pathOutput, GENERIC_WRITE, 0, CREATE_ALWAYS ) );

	transcribeArgs.resultFlags = eResultFlags::Timestamps | eResultFlags::Tokens;

	CComPtr<iContext> context;
	CHECK_EX( appState.model->createContext( &context ) );

	sFullParams fullParams;
	CHECK_EX( context->fullDefaultParams( eSamplingStrategy::Greedy, &fullParams ) );
	fullParams.language = transcribeArgs.language;
	fullParams.setFlag( eFullParamsFlags::Translate, transcribeArgs.translate );
	fullParams.resetFlag( eFullParamsFlags::PrintRealtime );

	fullParams.new_segment_callback_user_data = this;
	fullParams.new_segment_callback = &newSegmentCallbackStatic;

	// Setup the progress indication sink
	sProgressSink progressSink{ &progressCallbackStatic, this };
	// Run the transcribe
	CHECK_EX( context->runStreamed( fullParams, progressSink, reader ) );

	context->timingsPrint();

	if( format == eOutputFormat::None )
		return S_OK;

	CComPtr<iTranscribeResult> result;
	CHECK_EX( context->getResults( transcribeArgs.resultFlags, &result ) );

	sTranscribeLength len;
	CHECK_EX( result->getSize( len ) );
	const sSegment* const segments = result->getSegments();

	switch( format )
	{
	case eOutputFormat::Text:
		return writeTextFile( segments, len.countSegments, outputFile );
	case eOutputFormat::SubRip:
		return writeSubRip( segments, len.countSegments, outputFile );
	case eOutputFormat::WebVTT:
		return writeWebVTT( segments, len.countSegments, outputFile );
	default:
		return E_FAIL;
	}
}

#undef CHECK_EX

inline HRESULT TranscribeDlg::progressCallback( double p ) noexcept
{
	constexpr double mul = progressMaxInteger;
	int pos = lround( mul * p );
	progressBar.PostMessage( PBM_SETPOS, pos, 0 );
	return S_OK;
}

HRESULT __cdecl TranscribeDlg::progressCallbackStatic( double p, Whisper::iContext* ctx, void* pv ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)pv;
	return dlg->progressCallback( p );
}

namespace
{
	HRESULT write( CAtlFile& file, const CStringA& line )
	{
		if( line.GetLength() > 0 )
			CHECK( file.Write( cstr( line ), (DWORD)line.GetLength() ) );
		return S_OK;
	}

	void printTime( CStringA& rdi, Whisper::sTimeSpan time, bool comma )
	{
		Whisper::sTimeSpanFields fields = time;
		const char separator = comma ? ',' : '.';
		rdi.AppendFormat( "%02d:%02d:%02d%c%03d",
			(int)fields.hours,
			(int)fields.minutes,
			(int)fields.seconds,
			separator,
			fields.ticks / 10'000 );
	}

	const char* skipBlank( const char* rsi )
	{
		while( true )
		{
			const char c = *rsi;
			if( c == ' ' || c == '\t' )
			{
				rsi++;
				continue;
			}
			return rsi;
		}
	}
}

using Whisper::sSegment;


HRESULT TranscribeDlg::writeTextFile( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	using namespace Whisper;
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		line = skipBlank( segments[ i ].text );
		line += "\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeSubRip( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];

		line.Format( "%zu\r\n", i + 1 );
		printTime( line, seg.time.begin, true );
		line += " --> ";
		printTime( line, seg.time.end, true );
		line += "\r\n";
		line += skipBlank( seg.text );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeWebVTT( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	line = "WEBVTT\r\n\r\n";
	CHECK( write( file, line ) );

	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];
		line = "";

		printTime( line, seg.time.begin, false );
		line += " --> ";
		printTime( line, seg.time.end, false );
		line += "\r\n";
		line += skipBlank( seg.text );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

inline HRESULT TranscribeDlg::newSegmentCallback( Whisper::iContext* ctx, uint32_t n_new )
{
	using namespace Whisper;
	CComPtr<iTranscribeResult> result;
	CHECK( ctx->getResults( transcribeArgs.resultFlags, &result ) );
	return logNewSegments( result, n_new );
}

HRESULT __cdecl TranscribeDlg::newSegmentCallbackStatic( Whisper::iContext* ctx, uint32_t n_new, void* user_data ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)user_data;
	return dlg->newSegmentCallback( ctx, n_new );
}

void TranscribeDlg::onWmClose()
{
	if( GetDlgItem( IDCANCEL ).IsWindowEnabled() )
	{
		EndDialog( IDCANCEL );
		return;
	}

	constexpr UINT flags = MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2;
	const int res = this->MessageBox( L"Transcribe is in progress.\nDo you want to quit anyway?", L"Confirm exit", flags );
	if( res != IDYES )
		return;

	// TODO: instead of ExitProcess(), implement another callback in the DLL API, for proper cancellation of the background task
	ExitProcess( 1 );
}