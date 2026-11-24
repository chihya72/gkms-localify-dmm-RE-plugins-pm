using System.Runtime.InteropServices;
using ImGuiNET;

namespace Gkms.Plugins.PhotoSwap;

// RE API 2 renders ImGui using DX11 SRV texture IDs. Reuse the device referenced
// by its font SRV; never create a game device, hook Present, or retain a swapchain.
// COM methods used here are public Windows APIs, not Host/Bridge implementation APIs.
internal sealed unsafe class PreviewTexture : IDisposable
{
    internal nint Id { get; private set; }
    internal int Width { get; }
    internal int Height { get; }

    private PreviewTexture(nint id, int width, int height) => (Id, Width, Height) = (id, width, height);

    internal static PreviewTexture FromCurrentContext(ImagePixels pixels)
    {
        if (ImGui.GetCurrentContext() == IntPtr.Zero) throw new InvalidOperationException("预览需要 RE 的游戏内界面。");
        var font = ImGui.GetIO().Fonts.TexID;
        if (font == IntPtr.Zero) throw new InvalidOperationException("RE 图像设备尚未就绪，请重新打开配置页。");
        nint device = 0;
        var table = *(nint**)font;
        ((delegate* unmanaged[Stdcall]<nint, nint*, void>)table[3])(font, &device); // ID3D11DeviceChild::GetDevice
        if (device == 0) throw new InvalidOperationException("无法取得 RE 的图像设备。");
        try { return FromDevice(device, pixels); }
        finally { Marshal.Release(device); }
    }

    internal static PreviewTexture FromDevice(nint device, ImagePixels pixels)
    {
        if (pixels.Width <= 0 || pixels.Height <= 0 || pixels.Rgba.Length != checked(pixels.Width * pixels.Height * 4))
            throw new ArgumentException("预览图片像素长度不正确。");
        var description = new TextureDescription
        {
            Width = (uint)pixels.Width, Height = (uint)pixels.Height, MipLevels = 1, ArraySize = 1,
            Format = 28, SampleCount = 1, Usage = 1, BindFlags = 8 // RGBA8, IMMUTABLE, SHADER_RESOURCE
        };
        nint texture = 0, view = 0;
        var table = *(nint**)device;
        try
        {
            fixed (byte* data = pixels.Rgba)
            {
                var initial = new Subresource { Data = (nint)data, Pitch = (uint)pixels.Width * 4 };
                Marshal.ThrowExceptionForHR(((delegate* unmanaged[Stdcall]<nint, TextureDescription*, Subresource*, nint*, int>)table[5])(
                    device, &description, &initial, &texture));
            }
            Marshal.ThrowExceptionForHR(((delegate* unmanaged[Stdcall]<nint, nint, nint, nint*, int>)table[7])(
                device, texture, 0, &view));
            return new PreviewTexture(view, pixels.Width, pixels.Height);
        }
        finally { if (texture != 0) Marshal.Release(texture); }
    }

    public void Dispose()
    {
        var pointer = Id;
        Id = 0;
        if (pointer != 0) Marshal.Release(pointer);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TextureDescription
    {
        public uint Width, Height, MipLevels, ArraySize, Format, SampleCount, SampleQuality, Usage, BindFlags, CpuAccess, Misc;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct Subresource { public nint Data; public uint Pitch, SlicePitch; }
}
