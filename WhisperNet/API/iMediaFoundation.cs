﻿using ComLight;
using System.Runtime.InteropServices;
using Whisper.Internal;

namespace Whisper
{
	/// <summary>Exposes a small subset of MS Media Foundation framework.</summary>
	/// <remarks>That framework is a part of Windows OS, since Vista.</remarks>
	/// <seealso href="https://learn.microsoft.com/en-us/windows/win32/medfound/microsoft-media-foundation-sdk" />
	[ComInterface( "fb9763a5-d77d-4b6e-aff8-f494813cebd8", eMarshalDirection.ToManaged ), CustomConventions( typeof( NativeLogger ) )]
	public interface iMediaFoundation: IDisposable
	{
		/// <summary>Decode complete audio file into a new memory buffer.</summary>
		/// <returns>
		/// Under the hood, the method asks MF to resample and convert audio into the suitable type for the Whisper model.<br/>
		/// If the path is a video file, the method will decode the first audio track.
		/// </returns>
		[RetValIndex( 2 )]
		iAudioBuffer loadAudioFile( [MarshalAs( UnmanagedType.LPWStr )] string path, [MarshalAs( UnmanagedType.U1 )] bool stereo = false );

		/// <summary>Create a reader to stream the audio file from disk</summary>
		/// <returns>
		/// Under the hood, the method asks MF to resample and convert audio into the suitable type for the Whisper model.<br/>
		/// If the path is a video file, the method will decode the first audio track.
		/// </returns>
		[RetValIndex( 2 )]
		iAudioReader openAudioFile( [MarshalAs( UnmanagedType.LPWStr )] string path, [MarshalAs( UnmanagedType.U1 )] bool stereo = false );

		/// <summary>List capture devices</summary>
		void listCaptureDevices( [MarshalAs( UnmanagedType.FunctionPtr )] pfnFoundCaptureDevices pfn, IntPtr pv );

		/// <summary>Open audio capture device</summary>
		[RetValIndex( 2 )]
		iAudioCapture openCaptureDevice( [MarshalAs( UnmanagedType.LPWStr )] string endpoint, [In] ref sCaptureParams captureParams );
	}
}