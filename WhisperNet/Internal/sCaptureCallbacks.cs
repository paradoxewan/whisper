﻿using System.Runtime.InteropServices;

namespace Whisper.Internal
{
	/// <summary>Unmanaged code calls this to check for cancellation</summary>
	[UnmanagedFunctionPointer( CallingConvention.StdCall )]
	public delegate int pfnShouldCancel( IntPtr pv );

	/// <summary>Unmanaged code calls this to notify about the status</summary>
	[UnmanagedFunctionPointer( CallingConvention.StdCall )]
	public delegate int pfnCaptureStatus( IntPtr pv, eCaptureStatus status );

	/// <summary>Capture callbacks for unmanaged code</summary>
	public struct sCaptureCallbacks
	{
		/// <summary>Cancellation function pointer</summary>
		public pfnShouldCancel shouldCancel;
		/// <summary>Capture status function pointer</summary>
		public pfnCaptureStatus captureStatus;
		/// <summary>COntext pointer, only needed for C++ compatibility</summary>
		public IntPtr pv;
	}
}