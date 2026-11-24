using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Gkms.Sdk;

namespace Gkms.Dlss5;

public sealed class Plugin : ILocalifyPlugin, IPluginDiagnostics, ISettingsReloadable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void IssueEventInjected(IntPtr commandBuffer, IntPtr callback, int eventId);
    private static Plugin? active;
    private IPluginContext? context;
    private NativeBridge? native;
    private Type? commandType;
    private MethodInfo? issueManaged, nativeTexture;
    private PropertyInfo? frameCount;
    private IssueEventInjected? issueInjected;
    private readonly FrameSelection<object> motionFrames = new();
    private readonly FrameSelection<object> depthFrames = new();
    private string? warning;
    private string lastMessage = "";
    private string lastNativeStatus = "";
    private int lastLogFrame = -600;
    private bool awaitingSettings;
    private bool loggedFinalCopy;
    public IReadOnlyList<string> Warnings
    {
        get { var current = warning; return current == null ? Array.Empty<string>() : new[] { current }; }
    }
    public void ReloadSettings(JsonElement settings)
    {
        var options = settings.ValueKind == JsonValueKind.Undefined ? new Options()
            : settings.Deserialize<Options>(new JsonSerializerOptions(JsonSerializerDefaults.Web))
                ?? throw new InvalidDataException("配置必须为 JSON 对象。");
        options.Validate();
        (native ?? throw new InvalidOperationException("渲染会话未启动。")).Update(options);
        awaitingSettings = true;
        Report("参数已接收，等待下一渲染帧验证并应用；无需重启游戏。", false);
    }

    public void Start(IPluginContext pluginContext)
    {
        var options = pluginContext.ReadSettings<Options>(); options.Validate();
        if (active != null) throw new InvalidOperationException("已有 DLSS 5 插件会话。");
        context = pluginContext;
        commandType = context.RequireType("UnityEngine.CoreModule", "UnityEngine.Rendering.CommandBuffer");
        issueManaged = commandType.GetMethods(Interop.Members).FirstOrDefault(m =>
            m.Name == "IssuePluginEvent" && m.GetParameters().Select(p => p.ParameterType.FullName)
                .SequenceEqual(new[] { "System.IntPtr", "System.Int32" }));
        if (issueManaged == null && commandType is INativeInternalCalls calls)
        {
            // Unity 6 generated binding: explicit native m_Ptr, then callback and event id.
            // Do not call older, differently-marshalled non-injected icalls with this ABI.
            var pointer = calls.ResolveInternalCall("UnityEngine.Rendering.CommandBuffer::IssuePluginEventInternal_Injected(System.IntPtr,System.IntPtr,System.Int32)");
            if (pointer != IntPtr.Zero) issueInjected = Marshal.GetDelegateForFunctionPointer<IssueEventInjected>(pointer);
        }
        if (issueManaged == null && issueInjected == null)
            throw new NotSupportedException("此客户端未提供 CommandBuffer 插件事件入口，无法保证渲染顺序；未启用画面处理。");
        nativeTexture = context.Require("UnityEngine.CoreModule", "UnityEngine.Texture", "GetNativeTexturePtr");
        frameCount = context.RequireType("UnityEngine.CoreModule", "UnityEngine.Time").GetProperty("frameCount")
            ?? throw new MissingMemberException("Time.frameCount");
        var post = FindExecute("UnityEngine.Rendering.Universal.PostProcessPass");
        var final = context.RequireType("Unity.RenderPipelines.Universal.Runtime", "UnityEngine.Rendering.Universal.Internal.FinalBlitPass")
            .GetMethods(GameMethods.Declared).Single(m => m.Name == "ExecutePass" && m.IsStatic && m.GetParameters().Length == 4 &&
                m.GetParameters()[0].ParameterType.GetElementType()?.FullName == "UnityEngine.Rendering.Universal.RenderingData" &&
                m.GetParameters()[3].ParameterType.FullName == "UnityEngine.Rendering.RTHandle");
        var services = context.Services();
        native = new NativeBridge(services.PackageDirectory, services.StorageDirectory, options);
        active = this;
        try
        {
            Patch(post, nameof(BeforePostProcess)); Patch(final, nameof(BeforeFinalCopy));
            Report(options.EnableNeuralRendering
                ? "实验性 NR：等待匹配的颜色/运动矢量帧，尚未确认生效。"
                : "诊断模式：仅验证 DX11/DX12 纹理传递，不增强画面。", false);
        }
        catch { Dispose(); throw; }
    }
    private MethodInfo FindExecute(string name) => context!.RequireType("Unity.RenderPipelines.Universal.Runtime", name)
        .GetMethods(GameMethods.Declared).Single(m => m.Name == "Execute" && m.GetParameters().Length == 2);
    private void Patch(MethodInfo method, string callback)
    {
        var parameters = method.GetParameters().Select(p =>
            Interop.CallbackType(p.ParameterType.IsByRef ? p.ParameterType.GetElementType()! : p.ParameterType)).ToArray();
        context!.Patch(method, GameMethods.Handler<Plugin>(callback).MakeGenericMethod(parameters));
    }
    private static object Camera(object renderingData) => Interop.Get(Interop.Get(renderingData, "cameraData")!, "camera")!;
    private static object? Texture(object? handle) => handle == null ? null : Interop.Get(handle, "rt");
    private int Frame => Convert.ToInt32(frameCount!.GetValue(null));

    public static void BeforePostProcess<TContext, TData>(object __instance, TContext __0, TData __1)
    {
        var self = active;
        if (self == null || __1 == null) return;
        try
        {
            var camera = Camera(__1);
            if (Convert.ToInt32(Interop.Get(camera, "cameraType")) != 1) return;
            var texture = Texture(Interop.Get(__instance, "m_MotionVectors"));
            if (texture != null) self.motionFrames.Record(self.Frame, Interop.PointerOf(camera), texture);
            var depth=Texture(Interop.Get(__instance,"m_Depth"));
            if(depth!=null)self.depthFrames.Record(self.Frame,Interop.PointerOf(camera),depth);
        }
        catch (Exception error) { self.Report("读取运动矢量失败: " + error.GetBaseException().Message); }
    }
    public static void BeforeFinalCopy<TData, TMaterial, TTarget, TSource>(TData __0, TMaterial __1, TTarget __2, TSource __3)
    {
        var self = active;
        if (self == null || __0 == null || __3 == null) return;
        var ticket = 0;
        try
        {
            var camera = Camera(__0); var cameraId = Interop.PointerOf(camera); var frame = self.Frame;
            if (Convert.ToInt32(Interop.Get(camera, "cameraType")) != 1) return;
            if (!self.motionFrames.TryTake(frame, cameraId, out var motion) || motion == null)
            { self.Report("当前相机/帧缺少匹配的运动矢量，保留原画面。"); return; }
            // ExecutePass receives the source AFTER the renderer's front-buffer substitution.
            var color = Texture(__3);
            if (color == null) { self.Report("最终颜色不是可访问的 RenderTexture，保留原画面。"); return; }
            var commands = Interop.Get(__0, "commandBuffer") ?? throw new InvalidOperationException("最终拷贝命令缓冲不可用。");
            var colorPointer = (IntPtr)self.nativeTexture!.Invoke(color, null)!;
            var motionPointer = (IntPtr)self.nativeTexture.Invoke(motion, null)!;
            IntPtr depthPointer=IntPtr.Zero;
            if(self.depthFrames.TryTake(frame,cameraId,out var depth)&&depth!=null)depthPointer=(IntPtr)self.nativeTexture.Invoke(depth,null)!;
            ticket = self.native!.Queue(colorPointer, motionPointer, frame, cameraId,depthPointer);
            if (ticket <= 0) { self.Report("帧未能排队：" + self.native.Status().Message); return; }
            if (self.issueManaged != null) self.issueManaged.Invoke(commands, new object[] { self.native.Callback, ticket });
            else self.issueInjected!((IntPtr)Interop.Get(commands, "m_Ptr")!, self.native.Callback, ticket);
            // Append to URP's OWN command buffer. Pending color-producing commands
            // run first; ExecutePass appends the actual final blit after this event.
            // The renderer owns submission, Clear and Dispose of this buffer.
            ticket = 0;
            if (!self.loggedFinalCopy)
            {
                self.loggedFinalCopy = true;
                self.context?.Log("FinalCopy: 已将 NR 事件插入最终拷贝的原命令缓冲，使用 ExecutePass 实际源纹理。");
            }
            var status = self.native.Status();
            // Native return code carries failure independently of the progress text.
            // Never submit normal frame counters through IPluginDiagnostics.Warnings.
            if (status.Failed) { self.awaitingSettings = false; self.Report(status.Message); }
            else if (self.awaitingSettings && status.Message.StartsWith("Settings applied", StringComparison.Ordinal))
            { self.awaitingSettings = false; self.lastNativeStatus = status.Message; self.lastLogFrame = frame; self.Report(status.Message, false); }
            else if (status.Message != self.lastNativeStatus && frame - self.lastLogFrame >= 300)
            { self.lastNativeStatus = status.Message; self.lastLogFrame = frame; self.Report(status.Message, false); }
        }
        catch (Exception error) { self.Report("未处理画面: " + error.GetBaseException().Message); }
        finally
        {
            if (ticket > 0) self.native?.Cancel(ticket);
        }
    }
    private void Report(string message, bool failed = true)
    {
        warning = failed ? message : null;
        if (lastMessage == message) return;
        lastMessage = message; context?.Log(message);
    }
    public void Dispose()
    {
        if (ReferenceEquals(active, this)) active = null;
        motionFrames.Clear(); depthFrames.Clear(); native?.Dispose(); native = null;
        issueInjected = null; context = null;
    }
}
