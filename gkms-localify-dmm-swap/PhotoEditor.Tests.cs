using System.Drawing;
using System.Drawing.Imaging;
using System.IO.Compression;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text.Json;
using Gkms.Plugins.PhotoSwap;
using Gkms.Sdk;
using ImGuiNET;

namespace Gkms.PhotoSwap.Tests;

internal static class PhotoEditorTests
{
    private static int passed;
    internal static void Run()
    {
        var root = Path.Combine(AppContext.BaseDirectory, "editor-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        var cache = Path.Combine(root, "cache");
        var outGame = Directory.CreateDirectory(Path.Combine(cache, "OutGame")).FullName;
        var backups = Path.Combine(root, "backups");
        var source = Path.Combine(root, "my-picture.png");
        using (var image = new Bitmap(400, 200))
        {
            using var graphics = Graphics.FromImage(image);
            graphics.Clear(Color.CornflowerBlue);
            graphics.FillRectangle(Brushes.OrangeRed, 0, 0, 200, 200);
            graphics.FillEllipse(Brushes.Gold, 150, 25, 100, 150);
            image.Save(source, ImageFormat.Png);
        }
        var original = MakeImage(new Size(640, 360), Color.MediumSeaGreen, "PNG");
        var thumbnail = MakeImage(new Size(160, 90), Color.DarkGreen, "PNG");
        var fullPath = Path.Combine(outGame, "photo-one");
        var thumbPath = Path.Combine(outGame, "photo-one_t");
        File.WriteAllBytes(fullPath, original); File.WriteAllBytes(thumbPath, thumbnail);
        File.WriteAllBytes(Path.Combine(outGame, "photo-one-extra.png"), MakeImage(new Size(320, 180), Color.Pink, "JPEG"));
        File.WriteAllBytes(Path.Combine(outGame, "photo-one-extra_t.jpg"), MakeImage(new Size(80, 45), Color.Purple, "JPEG"));

        Check("scan recognizes extensionless PNG/JPEG and exact thumbnail pairs", () =>
        {
            var result = PhotoFiles.Scan(cache, default);
            Assert(result.Photos.Length == 2 && result.Warnings.Length == 0);
            var first = result.Photos.Single(p => p.Name == "photo-one");
            Assert(first.ThumbnailPath == thumbPath && first.Format == "PNG");
            Assert(result.Photos.Single(p => p.Name == "photo-one-extra.png").Format == "JPEG");
        });
        Check("cover/contain/stretch dimensions and background", () =>
        {
            using var image = PhotoImages.Decode(PhotoImages.ReadFile(source));
            foreach (var fit in Enum.GetValues<FitMode>())
            {
                using var result = PhotoImages.Fit(image, new Size(200, 200), fit, Color.White);
                Assert(result.Width == 200 && result.Height == 200);
                if (fit == FitMode.Contain) Assert(result.GetPixel(100, 0).ToArgb() == Color.White.ToArgb());
                else Assert(result.GetPixel(100, 0).ToArgb() != Color.White.ToArgb());
            }
        });
        var candidate = PhotoFiles.Scan(cache, default).Photos.Single(p => p.Name == "photo-one");
        var prepared = PhotoFiles.Prepare(cache, candidate, source, FitMode.Contain, Color.Black, default);
        Check("preview and output preserve original/thumbnail dimensions and formats", () =>
        {
            Assert(prepared.Changes.Length == 2);
            for (var i = 0; i < 2; i++)
            {
                using var oldImage = PhotoImages.Decode(prepared.Changes[i].Original);
                using var newImage = PhotoImages.Decode(prepared.Changes[i].Replacement);
                Assert(oldImage.Size == newImage.Size);
                Assert(PhotoImages.Format(prepared.Changes[i].Replacement) == "PNG");
            }
            Assert(File.ReadAllBytes(fullPath).SequenceEqual(original));
        });
        Check("changed cache is refused before either file is modified", () =>
        {
            File.WriteAllBytes(thumbPath, MakeImage(new Size(160, 90), Color.Black, "PNG"));
            Throws<IOException>(() => PhotoFiles.Apply(prepared, backups, default));
            Assert(File.ReadAllBytes(fullPath).SequenceEqual(original) && !Directory.Exists(backups));
            File.WriteAllBytes(thumbPath, thumbnail);
        });
        Check("locked thumbnail leaves the full image unchanged", () =>
        {
            using var locked = new FileStream(thumbPath, FileMode.Open, FileAccess.Read, FileShare.Read);
            Throws<IOException>(() => PhotoFiles.Apply(prepared, backups, default));
            Assert(File.ReadAllBytes(fullPath).SequenceEqual(original));
        });
        Check("cancellation before commit leaves both originals intact", () =>
        {
            using var stop = new CancellationTokenSource(); stop.Cancel();
            Throws<OperationCanceledException>(() => PhotoFiles.Apply(prepared, backups, stop.Token));
            Assert(File.ReadAllBytes(fullPath).SequenceEqual(original) && File.ReadAllBytes(thumbPath).SequenceEqual(thumbnail));
        });
        string? backup = null;
        Check("replacement backs up both originals and commits both outputs", () =>
        {
            backup = PhotoFiles.Apply(prepared, backups, default);
            Assert(File.Exists(backup));
            Assert(File.ReadAllBytes(fullPath).SequenceEqual(prepared.Changes[0].Replacement));
            Assert(File.ReadAllBytes(thumbPath).SequenceEqual(prepared.Changes[1].Replacement));
            using var archive = ZipFile.OpenRead(backup);
            using var stream = archive.GetEntry("OutGame/photo-one")!.Open();
            using var copy = new MemoryStream(); stream.CopyTo(copy);
            Assert(copy.ToArray().SequenceEqual(original));
        });
        Check("restore is byte exact and itself creates an undo backup", () =>
        {
            var undo = PhotoFiles.Restore(backup!, cache, backups, default);
            Assert(undo != backup && File.Exists(undo));
            Assert(File.ReadAllBytes(fullPath).SequenceEqual(original));
            Assert(File.ReadAllBytes(thumbPath).SequenceEqual(thumbnail));
            Assert(PhotoFiles.ListBackups(backups).Length == 2);
        });
        Check("restore rejects the wrong cache root and never resurrects removed cache", () =>
        {
            Throws<InvalidDataException>(() => PhotoFiles.Restore(backup!, Path.Combine(root, "other-cache"), backups, default));
            File.Delete(thumbPath);
            Throws<FileNotFoundException>(() => PhotoFiles.Restore(backup!, cache, backups, default));
            Assert(!File.Exists(thumbPath) && File.ReadAllBytes(fullPath).SequenceEqual(original));
            File.WriteAllBytes(thumbPath, thumbnail);
        });
        Check("cache paths reject traversal, ADS and non-photo sections", () =>
        {
            foreach (var path in new[] { "../other.png", "OutGame/../x.png", "OutGame/a:stream", "Other/a.png", "OutGame/a.png.", "OutGame/a.png " })
                Throws<InvalidDataException>(() => PhotoFiles.Target(cache, path));
        });
        Check("JPEG path supports all fitting modes and preserves both sizes", () =>
        {
            var jpeg = PhotoFiles.Scan(cache, default).Photos.Single(p => p.Format == "JPEG");
            foreach (var mode in Enum.GetValues<FitMode>())
            {
                var result = PhotoFiles.Prepare(cache, jpeg, source, mode, Color.White, default);
                foreach (var change in result.Changes)
                {
                    Assert(PhotoImages.Format(change.Replacement) == "JPEG");
                    using var before = PhotoImages.Decode(change.Original);
                    using var after = PhotoImages.Decode(change.Replacement);
                    Assert(before.Size == after.Size);
                }
            }
        });
        Check("real DX11 preview upload and ImGui wide/narrow rendering", () => RenderUi(root, prepared, source));
        Check("settings UI supports failed initialization cleanup and repeated Dispose", () =>
        {
            var neverStarted = new PhotoSettingsUi(); neverStarted.Dispose(); neverStarted.Dispose();
            var context = new UiContext(); var ui = new PhotoSettingsUi(); ui.Initialize(context);
            Assert(context.DrawSubscribers == 1 && context.OpenSubscribers == 1);
            ui.Dispose(); ui.Dispose();
            Assert(context.DrawSubscribers == 0 && context.OpenSubscribers == 0);
        });
        Console.WriteLine($"PASS: {passed} editor checks. Synthetic test artifacts: {root}");
    }

    private static byte[] MakeImage(Size size, Color color, string format)
    {
        using var bitmap = new Bitmap(size.Width, size.Height);
        using (var graphics = Graphics.FromImage(bitmap)) graphics.Clear(color);
        return PhotoImages.Encode(bitmap, format);
    }
    private static void Check(string label, Action test) { test(); passed++; Console.WriteLine("PASS " + label); }
    private static void Assert(bool condition) { if (!condition) throw new InvalidOperationException("Editor assertion failed."); }
    private static void Throws<T>(Action action) where T : Exception
    { try { action(); } catch (T) { return; } throw new InvalidOperationException("Expected " + typeof(T).Name); }

    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int D3D11CreateDevice(nint adapter, uint driverType, nint software, uint flags, nint levels,
        uint levelCount, uint sdkVersion, out nint device, out uint level, out nint immediate);

    private static unsafe void RenderUi(string root, PreparedPhoto photo, string source)
    {
        Marshal.ThrowExceptionForHR(D3D11CreateDevice(0, 5, 0, 0, 0, 0, 7, out var device, out _, out var immediate));
        var imgui = ImGui.CreateContext();
        try
        {
            var io = ImGui.GetIO();
            io.BackendFlags |= ImGuiBackendFlags.RendererHasVtxOffset;
            io.NativePtr->IniFilename = null;
            var fontPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Fonts), "msyh.ttc");
            if (File.Exists(fontPath)) io.Fonts.AddFontFromFileTTF(fontPath, 18, default, io.Fonts.GetGlyphRangesChineseSimplifiedCommon());
            else io.Fonts.AddFontDefault();
            io.Fonts.GetTexDataAsRGBA32(out byte* data, out int fw, out int fh, out _);
            var fontPixels = new byte[fw * fh * 4]; Marshal.Copy((nint)data, fontPixels, 0, fontPixels.Length);
            var fontImage = new ImagePixels(fw, fh, fontPixels);
            using var font = PreviewTexture.FromDevice(device, fontImage);
            io.Fonts.SetTexID(font.Id);
            using var probe = PreviewTexture.FromCurrentContext(photo.ReplacementPreview);
            Assert(probe.Width == photo.ReplacementPreview.Width && probe.Id != 0);
            var context = new UiContext();
            using var ui = new PhotoSettingsUi();
            ui.Initialize(context); context.Open();
            Set(ui, "cacheRoot", photo.CacheRoot); Set(ui, "photos", new[] { photo.Photo });
            Set(ui, "selected", photo.Photo); Set(ui, "source", source); Set(ui, "prepared", photo);
            Set(ui, "mode", (int)FitMode.Contain);
            Set(ui, "beforePixels", photo.OriginalPreview); Set(ui, "afterPixels", photo.ReplacementPreview);
            Set(ui, "updateTextures", true);
            Set(ui, "status", "预览已生成，将处理原图与缩略图。替换前会自动备份。");
            foreach (var width in new[] { 1000, 480 })
            {
                io.DisplaySize = new Vector2(width, 1300); io.DeltaTime = 1f / 60;
                // Two frames let Dear ImGui finish initial window sizing.
                for (var frame = 0; frame < 2; frame++)
                {
                    ImGui.NewFrame();
                    ImGui.SetNextWindowPos(Vector2.Zero); ImGui.SetNextWindowSize(io.DisplaySize);
                    ImGui.Begin("照片替换支持 · RE", ImGuiWindowFlags.NoResize | ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoSavedSettings);
                    context.Render(); ImGui.End(); ImGui.Render();
                }
                var draw = ImGui.GetDrawData();
                Assert(draw.TotalVtxCount > 0 && draw.CmdListsCount > 0);
                var before = (PreviewTexture)Get(ui, "beforeTexture")!;
                var after = (PreviewTexture)Get(ui, "afterTexture")!;
                var textures = new Dictionary<nint, ImagePixels> { [font.Id] = fontImage, [before.Id] = photo.OriginalPreview, [after.Id] = photo.ReplacementPreview };
                SaveDrawData(draw, textures, width, 1300, Path.Combine(root, $"ui-{width}.png"));
            }
        }
        finally { ImGui.DestroyContext(imgui); Marshal.Release(immediate); Marshal.Release(device); }
    }

    private static void Set(object target, string name, object value) => target.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(target, value);
    private static object? Get(object target, string name) => target.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(target);

    internal static void ValidatePackage(string package)
    {
        var directory = Path.Combine(AppContext.BaseDirectory, "package-tests", Guid.NewGuid().ToString("N"));
        ZipFile.ExtractToDirectory(package, directory);
        var loader = new PackageLoader(Path.Combine(directory, "Gkms.PhotoSwap.dll"));
        try
        {
            var assembly = loader.LoadFromAssemblyPath(Path.Combine(directory, "Gkms.PhotoSwap.dll"));
            var bitmapType = assembly.GetType("Gkms.Plugins.PhotoSwap.PhotoImages", true)!;
            var input = MakeImage(new Size(32, 18), Color.DodgerBlue, "PNG");
            var decode = bitmapType.GetMethod("Decode", BindingFlags.Static | BindingFlags.NonPublic)!;
            using var bitmap = (IDisposable)decode.Invoke(null, new object[] { input, false })!;
            Assert((int)bitmap.GetType().GetProperty("Width")!.GetValue(bitmap)! == 32);
            var events = loader.LoadFromAssemblyName(new AssemblyName("Microsoft.Win32.SystemEvents"));
            Assert(events.Location.StartsWith(Path.Combine(directory, "runtimes", "win"), StringComparison.OrdinalIgnoreCase));
            var context = new UiContext();
            using var ui = (IPluginSettingsUi)Activator.CreateInstance(assembly.GetType("Gkms.Plugins.PhotoSwap.PhotoSettingsUi", true)!)!;
            ui.Initialize(context);
            Assert(context.DrawSubscribers == 1);
            Assert(!File.Exists(Path.Combine(directory, "Gkms.Sdk.dll")) && !File.Exists(Path.Combine(directory, "ImGui.NET.dll")));
            Console.WriteLine("PASS install ZIP: isolated UI entry, image decoder, and Windows private dependency load.");
        }
        finally { loader.Unload(); }
    }

    private sealed class PackageLoader : AssemblyLoadContext
    {
        private readonly AssemblyDependencyResolver resolver;
        internal PackageLoader(string path) : base(isCollectible: true) => resolver = new AssemblyDependencyResolver(path);
        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == "Gkms.Sdk") return typeof(ILocalifyPlugin).Assembly;
            if (name.Name == "ImGui.NET") return typeof(ImGui).Assembly;
            return resolver.ResolveAssemblyToPath(name) is { } path ? LoadFromAssemblyPath(path) : null;
        }
        protected override nint LoadUnmanagedDll(string name) => resolver.ResolveUnmanagedDllToPath(name) is { } path ? LoadUnmanagedDllFromPath(path) : 0;
    }

    // Offline CPU rendering of actual ImGui draw data, including its font and our
    // image textures. This is visual QA; no game/window/renderer injection is used.
    private static unsafe void SaveDrawData(ImDrawDataPtr draw, Dictionary<nint, ImagePixels> textures, int width, int height, string path)
    {
        var canvas = new byte[width * height * 4];
        for (var i = 3; i < canvas.Length; i += 4) canvas[i] = 255;
        for (var listIndex = 0; listIndex < draw.CmdListsCount; listIndex++)
        {
            var list = draw.CmdListsRange[listIndex];
            for (var c = 0; c < list.CmdBuffer.Size; c++)
            {
                var command = list.CmdBuffer[c];
                if (!textures.TryGetValue(command.TextureId, out var texture)) throw new InvalidOperationException("Unknown UI texture.");
                for (uint index = 0; index < command.ElemCount; index += 3)
                {
                    var a = list.VtxBuffer[(int)(command.VtxOffset + list.IdxBuffer[(int)(command.IdxOffset + index)])];
                    var b = list.VtxBuffer[(int)(command.VtxOffset + list.IdxBuffer[(int)(command.IdxOffset + index + 1)])];
                    var cVertex = list.VtxBuffer[(int)(command.VtxOffset + list.IdxBuffer[(int)(command.IdxOffset + index + 2)])];
                    var minX = Math.Max(0, Math.Max((int)command.ClipRect.X, (int)MathF.Floor(Math.Min(a.pos.X, Math.Min(b.pos.X, cVertex.pos.X)))));
                    var minY = Math.Max(0, Math.Max((int)command.ClipRect.Y, (int)MathF.Floor(Math.Min(a.pos.Y, Math.Min(b.pos.Y, cVertex.pos.Y)))));
                    var maxX = Math.Min(width, Math.Min((int)command.ClipRect.Z, (int)MathF.Ceiling(Math.Max(a.pos.X, Math.Max(b.pos.X, cVertex.pos.X)))));
                    var maxY = Math.Min(height, Math.Min((int)command.ClipRect.W, (int)MathF.Ceiling(Math.Max(a.pos.Y, Math.Max(b.pos.Y, cVertex.pos.Y)))));
                    var denominator = (b.pos.Y - cVertex.pos.Y) * (a.pos.X - cVertex.pos.X) + (cVertex.pos.X - b.pos.X) * (a.pos.Y - cVertex.pos.Y);
                    if (Math.Abs(denominator) < .0001f) continue;
                    for (var y = minY; y < maxY; y++) for (var x = minX; x < maxX; x++)
                    {
                        var wa = ((b.pos.Y - cVertex.pos.Y) * (x + .5f - cVertex.pos.X) + (cVertex.pos.X - b.pos.X) * (y + .5f - cVertex.pos.Y)) / denominator;
                        var wb = ((cVertex.pos.Y - a.pos.Y) * (x + .5f - cVertex.pos.X) + (a.pos.X - cVertex.pos.X) * (y + .5f - cVertex.pos.Y)) / denominator;
                        var wc = 1 - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0) continue;
                        var uv = a.uv * wa + b.uv * wb + cVertex.uv * wc;
                        var tx = Math.Clamp((int)(uv.X * texture.Width), 0, texture.Width - 1);
                        var ty = Math.Clamp((int)(uv.Y * texture.Height), 0, texture.Height - 1);
                        var tp = (ty * texture.Width + tx) * 4;
                        var cp = (y * width + x) * 4;
                        var alpha = texture.Rgba[tp + 3] / 255f * (((a.col >> 24) * wa + (b.col >> 24) * wb + (cVertex.col >> 24) * wc) / 255f);
                        for (var channel = 0; channel < 3; channel++)
                        {
                            var color = ((a.col >> (8 * channel)) & 255) * wa + ((b.col >> (8 * channel)) & 255) * wb + ((cVertex.col >> (8 * channel)) & 255) * wc;
                            var value = texture.Rgba[tp + channel] * color / 255f;
                            canvas[cp + (2 - channel)] = (byte)Math.Clamp(value * alpha + canvas[cp + (2 - channel)] * (1 - alpha), 0, 255);
                        }
                    }
                }
            }
        }
        using var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        var locked = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        try { for (var y = 0; y < height; y++) Marshal.Copy(canvas, y * width * 4, locked.Scan0 + y * locked.Stride, width * 4); }
        finally { bitmap.UnlockBits(locked); }
        bitmap.Save(path, ImageFormat.Png);
    }

    private sealed class UiContext : ISettingsUiContext, IPluginUiBuilder
    {
        private Action? draw, open;
        public int DrawSubscribers => draw?.GetInvocationList().Length ?? 0;
        public int OpenSubscribers => open?.GetInvocationList().Length ?? 0;
        public string Id => "pm.photoswap";
        public string Name => "照片替换支持";
        public string Description => "";
        public string Language => "zh-CN";
        public JsonElement Settings { get; private set; } = JsonSerializer.SerializeToElement(new { });
        public IReadOnlyList<PluginSetting> Fields => Array.Empty<PluginSetting>();
        public IReadOnlyList<PluginPreset> Presets => Array.Empty<PluginPreset>();
        public IPluginUiBuilder UiBuilder => this;
        public CancellationToken Stopping => default;
        public bool CanReloadResources => false;
        public event Action Draw { add => draw += value; remove => draw -= value; }
        public event Action OpenConfigUi { add => open += value; remove => open -= value; }
        public void Enqueue(Action action) => throw new InvalidOperationException("UI render test must not start background work.");
        internal void Open() => open?.Invoke();
        internal void Render() => draw?.Invoke();
        public void ReloadSettings() { }
        public void SaveSettings(JsonElement values) => Settings = values.Clone();
        public void ReplaceSettings(JsonElement values) => SaveSettings(values);
        public Task<string> ReloadResourcesAsync() => throw new NotSupportedException();
        public T Own<T>(T value) where T : IDisposable => value;
        public void Log(string value) { }
    }
}
