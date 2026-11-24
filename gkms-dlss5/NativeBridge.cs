using System.Runtime.InteropServices;
using System.Text;

namespace Gkms.Dlss5;

internal sealed class NativeBridge : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct Configuration
    {
        public uint Size, Neural, Preset, AutoMask;
        public float Intensity, Tone, Structure, Skin, MotionX, MotionY;
        public uint ComparisonSplit;
        public float ModelScale, WhitePoint, ExposureEV, TransferStrength, ColorStrength, MaxRatio;
        public uint Codec, AutoExposure, InputEncoding, Style, Capture, FlipY;
        public uint DownFilter,TransferMode,ApplyModel,HoldFrame,CompareMode,CompareSwap,CompareLabels,DebugView;
        public float ComparePosition,CompareZoom,DebugGain;
        public uint UseDepth,DepthInverted;
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private delegate ulong OpenFn(ref Configuration options, string runtimeDirectory, string dataDirectory);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CloseFn(ulong session);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr EventFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int QueueFn(ulong session, IntPtr color, IntPtr motion, int frame, long camera, IntPtr depth);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CancelFn(ulong session, int ticket);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private delegate int StatusFn(ulong session, StringBuilder text, int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private delegate int UpdateFn(ulong session, ref Configuration options, string directory);

    private IntPtr library;
    private ulong session;
    private readonly CloseFn close;
    private readonly QueueFn queue;
    private readonly CancelFn cancel;
    private readonly StatusFn status;
    private readonly UpdateFn update;
    public IntPtr Callback { get; }

    public NativeBridge(string packageDirectory, string dataDirectory, Options options)
    {
        library = NativeLibrary.Load(Path.Combine(packageDirectory, "Gkms.Dlss5.Native.dll"));
        try
        {
            T Get<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));
            close = Get<CloseFn>("GkmsNrClose"); queue = Get<QueueFn>("GkmsNrQueueEx");
            cancel = Get<CancelFn>("GkmsNrCancel"); status = Get<StatusFn>("GkmsNrStatus");
            update = Get<UpdateFn>("GkmsNrUpdate");
            Callback = Get<EventFn>("GkmsNrRenderEvent")();
            var configuration = ConfigurationOf(options);
            session = Get<OpenFn>("GkmsNrOpen")(ref configuration, options.RuntimeDirectory, dataDirectory);
            if (session == 0 || Callback == IntPtr.Zero) throw new InvalidOperationException("原生渲染会话创建失败。");
        }
        catch { NativeLibrary.Free(library); library = IntPtr.Zero; throw; }
    }
    public int Queue(IntPtr color, IntPtr motion, int frame, nint camera, IntPtr depth) => queue(session, color, motion, frame, (long)camera,depth);
    public void Cancel(int ticket) => cancel(session, ticket);
    private static Configuration ConfigurationOf(Options options) => new()
    {
        Size = (uint)Marshal.SizeOf<Configuration>(), Neural = options.EnableNeuralRendering ? 1u : 0u,
        Preset = (uint)options.Preset, AutoMask = options.AutoMask ? 1u : 0u,
        Intensity = options.Intensity, Tone = options.ToneStrength, Structure = options.StructureStrength,
        Skin = options.SkinStrength, MotionX = options.MotionScaleX, MotionY = options.MotionScaleY,
        ComparisonSplit = options.ComparisonSplit ? 1u : 0u,
        ModelScale = options.ModelScale, WhitePoint = options.WhitePoint, ExposureEV = options.ExposureEV,
        TransferStrength = options.TransferStrength, ColorStrength = options.ColorStrength, MaxRatio = options.MaxRatio,
        Codec = (uint)options.Codec, AutoExposure = options.AutoExposure ? 1u : 0u,
        InputEncoding = (uint)options.InputEncoding, Style = (uint)options.Style, Capture = options.Capture ? 1u : 0u,
        FlipY = options.FlipY ? 1u : 0u,
        DownFilter=(uint)options.DownFilter,TransferMode=(uint)options.TransferMode,ApplyModel=options.ApplyModel?1u:0u,HoldFrame=options.HoldFrame?1u:0u,
        CompareMode=(uint)options.CompareMode,CompareSwap=options.CompareSwap?1u:0u,CompareLabels=options.CompareLabels?1u:0u,DebugView=(uint)options.DebugView,
        ComparePosition=options.ComparePosition,CompareZoom=options.CompareZoom,DebugGain=options.DebugGain,UseDepth=options.UseDepth?1u:0u,DepthInverted=options.DepthInverted?1u:0u
    };
    public void Update(Options options)
    {
        var configuration = ConfigurationOf(options);
        if (update(session, ref configuration, options.RuntimeDirectory) != 1)
            throw new InvalidOperationException("参数更新未被接受；保留原配置。渲染会话可能已经停止，请查看日志。");
    }
    public (string Message, bool Failed) Status()
    {
        var text = new StringBuilder(2048);
        var result = status(session, text, text.Capacity);
        return (text.ToString(), result != 0);
    }
    public void Dispose()
    {
        if (session != 0) { close(session); session = 0; }
        if (library != IntPtr.Zero) { NativeLibrary.Free(library); library = IntPtr.Zero; }
    }
}
