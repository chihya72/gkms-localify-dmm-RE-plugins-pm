using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

[assembly: SupportedOSPlatform("windows")]
[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("Gkms.PhotoSwap.Tests")]

namespace Gkms.Plugins.PhotoSwap;

internal enum FitMode { Cover, Contain, Stretch }
internal sealed record ImagePixels(int Width, int Height, byte[] Rgba);

internal static class PhotoImages
{
    internal const int MaxFileBytes = 128 * 1024 * 1024;
    private const long MaxPixels = 48_000_000;

    internal static string? Format(ReadOnlySpan<byte> bytes) =>
        bytes.StartsWith(new byte[] { 0xff, 0xd8, 0xff }) ? "JPEG" :
        bytes.StartsWith(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }) ? "PNG" : null;

    internal static byte[] ReadFile(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length > MaxFileBytes) throw new InvalidDataException("图片超过 128 MiB，请选择较小的图片。");
        using var copy = new MemoryStream();
        stream.CopyTo(copy);
        if (copy.Length > MaxFileBytes) throw new InvalidDataException("图片过大。");
        return copy.ToArray();
    }

    internal static Bitmap Decode(byte[] bytes, bool orient = false)
    {
        using var stream = new MemoryStream(bytes, false);
        using var image = Image.FromStream(stream, false, true);
        if ((long)image.Width * image.Height > MaxPixels) throw new InvalidDataException("图片超过 4800 万像素，请先缩小图片。");
        if (orient && image.PropertyIdList.Contains(0x112))
        {
            var data = image.GetPropertyItem(0x112)?.Value;
            if (data is { Length: >= 2 })
                image.RotateFlip(BitConverter.ToUInt16(data, 0) switch
                {
                    2 => RotateFlipType.RotateNoneFlipX, 3 => RotateFlipType.Rotate180FlipNone,
                    4 => RotateFlipType.Rotate180FlipX, 5 => RotateFlipType.Rotate90FlipX,
                    6 => RotateFlipType.Rotate90FlipNone, 7 => RotateFlipType.Rotate270FlipX,
                    8 => RotateFlipType.Rotate270FlipNone, _ => RotateFlipType.RotateNoneFlipNone
                });
        }
        return new Bitmap(image);
    }

    internal static Bitmap Fit(Image source, Size size, FitMode mode, Color background)
    {
        var output = new Bitmap(size.Width, size.Height, PixelFormat.Format24bppRgb);
        try
        {
            using var graphics = Graphics.FromImage(output);
            graphics.Clear(background);
            graphics.CompositingQuality = CompositingQuality.HighQuality;
            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            var rectangle = new RectangleF(0, 0, size.Width, size.Height);
            if (mode != FitMode.Stretch)
            {
                var x = (float)size.Width / source.Width;
                var y = (float)size.Height / source.Height;
                var scale = mode == FitMode.Cover ? Math.Max(x, y) : Math.Min(x, y);
                var width = source.Width * scale;
                var height = source.Height * scale;
                rectangle = new RectangleF((size.Width - width) / 2, (size.Height - height) / 2, width, height);
            }
            using var attributes = new ImageAttributes();
            attributes.SetWrapMode(WrapMode.TileFlipXY);
            graphics.DrawImage(source, Rectangle.Round(rectangle), 0, 0, source.Width, source.Height, GraphicsUnit.Pixel, attributes);
            return output;
        }
        catch { output.Dispose(); throw; }
    }

    internal static byte[] Encode(Bitmap image, string format)
    {
        using var stream = new MemoryStream();
        if (format == "PNG") image.Save(stream, ImageFormat.Png);
        else if (format == "JPEG")
        {
            var codec = ImageCodecInfo.GetImageEncoders().Single(c => c.FormatID == ImageFormat.Jpeg.Guid);
            using var parameters = new EncoderParameters(1);
            parameters.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 95L);
            image.Save(stream, codec, parameters);
        }
        else throw new InvalidDataException("仅支持替换 JPEG/PNG 拍摄缓存。");
        return stream.ToArray();
    }

    internal static ImagePixels Preview(Image image)
    {
        var scale = Math.Min(1, Math.Min(640d / image.Width, 360d / image.Height));
        var size = new Size(Math.Max(1, (int)(image.Width * scale)), Math.Max(1, (int)(image.Height * scale)));
        using var preview = new Bitmap(size.Width, size.Height, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(preview))
        {
            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            graphics.DrawImage(image, new Rectangle(Point.Empty, size));
        }
        var data = preview.LockBits(new Rectangle(Point.Empty, size), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            var pixels = new byte[size.Width * size.Height * 4];
            for (var row = 0; row < size.Height; row++)
                Marshal.Copy(data.Scan0 + row * data.Stride, pixels, row * size.Width * 4, size.Width * 4);
            for (var i = 0; i < pixels.Length; i += 4) (pixels[i], pixels[i + 2]) = (pixels[i + 2], pixels[i]);
            return new ImagePixels(size.Width, size.Height, pixels);
        }
        finally { preview.UnlockBits(data); }
    }
}
