using System.Drawing;
using System.Numerics;
using System.Text.Json;
using Gkms.Sdk;
using ImGuiNET;

namespace Gkms.Plugins.PhotoSwap;

public sealed class PhotoSettingsUi : IPluginSettingsUi
{
    private ISettingsUiContext? context;
    private CancellationTokenSource? stopping;
    private readonly PhotoFilePicker picker = new();
    private PhotoCandidate[] photos = Array.Empty<PhotoCandidate>();
    private BackupInfo[] backups = Array.Empty<BackupInfo>();
    private PreparedPhoto? prepared;
    private PhotoCandidate? selected;
    private PreviewTexture? beforeTexture, afterTexture;
    private ImagePixels? beforePixels, afterPixels;
    private string cacheRoot = PhotoFiles.DefaultCacheRoot, source = "", status = "拍照后停在游戏的保存选择界面，再扫描缓存。";
    private string backupRoot = PhotoFiles.DefaultBackupRoot;
    private int mode, backupIndex = -1;
    private Vector3 background = Vector3.Zero;
    private bool busy, visible, disposed, updateTextures;

    public void Initialize(ISettingsUiContext context)
    {
        this.context = context;
        stopping = CancellationTokenSource.CreateLinkedTokenSource(context.Stopping);
        LoadSettings();
        context.UiBuilder.Draw += Draw;
        context.UiBuilder.OpenConfigUi += Open;
    }

    private void Open() { visible = true; }
    private void LoadSettings()
    {
        var options = context!.ReadSettings<Options>();
        cacheRoot = string.IsNullOrWhiteSpace(options.CacheRoot) ? PhotoFiles.DefaultCacheRoot : options.CacheRoot;
    }

    private void Draw()
    {
        if (!visible || disposed) return;
        UploadPreviews();
        ImGui.PushID("pm.photoswap");
        try
        {
            PluginUiText.Wrapped("照片替换 · 扫描缓存 / 选择图片 / 预览 / 替换后返回游戏保存");
            ImGui.Spacing();
            ImGui.BeginDisabled(busy);
            ImGui.TextUnformatted("1. 选择待保存的照片");
            ImGui.TextDisabled("缓存目录");
            ImGui.SetNextItemWidth(-1);
            if (ImGui.InputText("##cache-root", ref cacheRoot, 4096)) ClearSelection();
            if (ImGui.Button("选择缓存目录")) picker.Open("选择照片缓存根目录", cacheRoot, true, path => { cacheRoot = path; Scan(); });
            ImGui.SameLine();
            if (ImGui.Button("重新扫描")) Scan();
            ImGui.SameLine();
            if (ImGui.Button("默认目录")) { cacheRoot = PhotoFiles.DefaultCacheRoot; Scan(); }
            ImGui.SameLine();
            if (ImGui.Button("保存目录"))
            {
                try { context!.SaveSettings(new { cacheRoot }); status = "缓存目录已保存。"; }
                catch (Exception failure) { status = "保存失败：" + failure.Message; }
            }
            ImGui.BeginChild("pending-photos", new Vector2(0, ImGui.GetTextLineHeightWithSpacing() * 6), true);
            try
            {
                if (photos.Length == 0) PluginUiText.Wrapped("没有待保存照片。拍照后保持在保存选择界面，然后点击“重新扫描”。");
                foreach (var photo in photos)
                {
                    var label = $"{photo.Section} · {photo.Name} · {photo.Width}×{photo.Height} {photo.Format}";
                    if (ImGui.Selectable(label, selected == photo))
                    {
                        selected = photo; InvalidatePreview();
                        if (source.Length > 0) Prepare();
                        else LoadOriginal(photo);
                    }
                    if (ImGui.IsItemHovered()) ImGui.SetTooltip(photo.FullPath + (photo.ThumbnailPath == null ? "\n未发现缩略图" : "\n原图与缩略图将一起处理"));
                }
            }
            finally { ImGui.EndChild(); }

            ImGui.Spacing();
            ImGui.TextUnformatted("2. 选择图片并预览");
            ImGui.TextDisabled("替换图片路径");
            ImGui.SetNextItemWidth(-1);
            if (ImGui.InputText("##source-path", ref source, 4096)) InvalidatePreview();
            if (ImGui.Button("选择我的图片")) picker.Open("选择替换图片", source, false, path => { source = path; InvalidatePreview(); if (selected != null) Prepare(); });
            ImGui.SameLine();
            ImGui.BeginDisabled(selected == null || string.IsNullOrWhiteSpace(source));
            if (ImGui.Button("生成预览")) Prepare();
            ImGui.EndDisabled();
            ImGui.SetNextItemWidth(Math.Min(300, ImGui.GetContentRegionAvail().X));
            if (ImGui.Combo("适配方式", ref mode, new[] { "裁切填满", "完整放入", "拉伸" }, 3)) InvalidatePreview();
            if (mode == (int)FitMode.Contain)
            {
                ImGui.SetNextItemWidth(Math.Min(300, ImGui.GetContentRegionAvail().X));
                if (ImGui.ColorEdit3("留边颜色", ref background)) InvalidatePreview();
            }
            DrawPreviews();
            if (selected is { ThumbnailPath: null }) PluginUiText.Wrapped("这张照片没有配对缩略图，将仅替换原图。请确认选择的是待保存照片。");

            ImGui.Spacing(); ImGui.Separator(); ImGui.Spacing();
            ImGui.TextUnformatted("3. 备份并替换");
            ImGui.BeginDisabled(prepared == null);
            if (ImGui.Button("自动备份并替换这张照片", new Vector2(Math.Min(360, ImGui.GetContentRegionAvail().X), 0))) Apply();
            ImGui.EndDisabled();
            if (prepared == null) PluginUiText.Wrapped("请选择待保存照片和替换图片，并生成预览。");
            ImGui.EndDisabled();

            ImGui.Spacing();
            if (busy) ImGui.TextUnformatted("正在处理，请稍候…");
            PluginUiText.Wrapped(status);
            ImGui.Spacing();
            DrawBackups();
            picker.Draw();
        }
        finally { ImGui.PopID(); }
    }

    private void Scan()
    {
        ClearSelection();
        var root = cacheRoot;
        Run("正在扫描照片缓存…", token => PhotoFiles.Scan(root, token), result =>
        {
            photos = result.Photos;
            status = $"找到 {photos.Length} 张照片。" + (result.Warnings.Length == 0 ? "请选择一张待保存照片。" : "\n" + string.Join("\n", result.Warnings.Take(4)));
        });
    }

    private void LoadOriginal(PhotoCandidate photo) => Run("正在读取原图…", token =>
    {
        token.ThrowIfCancellationRequested();
        using var image = PhotoImages.Decode(PhotoImages.ReadFile(photo.FullPath));
        return PhotoImages.Preview(image);
    }, pixels => { beforePixels = pixels; afterPixels = null; updateTextures = true; status = "已选中照片，请选择要换上的图片。"; });

    private void Prepare()
    {
        if (selected == null) return;
        var photo = selected;
        var root = cacheRoot;
        var input = source;
        var fit = (FitMode)mode;
        var color = Color.FromArgb((int)(Math.Clamp(background.X, 0, 1) * 255), (int)(Math.Clamp(background.Y, 0, 1) * 255), (int)(Math.Clamp(background.Z, 0, 1) * 255));
        InvalidatePreview();
        Run("正在生成原图与缩略图…", token => PhotoFiles.Prepare(root, photo, input, fit, color, token), result =>
        {
            prepared = result; beforePixels = result.OriginalPreview; afterPixels = result.ReplacementPreview; updateTextures = true;
            status = $"预览已生成，将处理 {result.Changes.Length} 个文件。替换前会自动备份。";
        });
    }

    private void Apply()
    {
        if (prepared == null) return;
        var snapshot = prepared;
        Run("正在备份并替换…", token => PhotoFiles.Apply(snapshot, backupRoot, token), backup =>
        {
            prepared = null;
            beforePixels = snapshot.ReplacementPreview; afterPixels = null; updateTextures = true;
            status = "替换完成。请返回游戏保存照片。\n原文件备份：" + backup;
            backups = PhotoFiles.ListBackups(backupRoot); backupIndex = backups.Length > 0 ? 0 : -1;
        });
    }

    private void DrawBackups()
    {
        if (!ImGui.CollapsingHeader("备份与还原")) return;
        PluginUiText.Wrapped("备份目录：" + backupRoot);
        ImGui.BeginDisabled(busy);
        if (ImGui.Button("刷新备份列表"))
            Run("正在读取备份…", _ => PhotoFiles.ListBackups(backupRoot), result => { backups = result; backupIndex = result.Length > 0 ? 0 : -1; status = $"找到 {result.Length} 个备份。"; });
        if (backups.Length > 0)
        {
            ImGui.SetNextItemWidth(-1);
            ImGui.Combo("##backups", ref backupIndex, backups.Select(b => b.Name).ToArray(), backups.Length);
            PluginUiText.Wrapped("只还原仍存在的待保存缓存；还原前也会备份当前图片。已保存到相册的照片不会被修改。");
            ImGui.BeginDisabled(backupIndex < 0 || backupIndex >= backups.Length);
            if (ImGui.Button("还原选中的备份"))
            {
                var archive = backups[backupIndex].Path;
                var root = cacheRoot;
                ClearSelection();
                Run("正在还原…", token => PhotoFiles.Restore(archive, root, backupRoot, token), undo =>
                {
                    backups = PhotoFiles.ListBackups(backupRoot); backupIndex = 0;
                    status = "照片已还原，请重新扫描。还原前的图片已备份到：\n" + undo;
                });
            }
            ImGui.EndDisabled();
        }
        else PluginUiText.Wrapped("还没有读取到备份。替换照片后会自动生成备份。");
        ImGui.EndDisabled();
    }

    private void DrawPreviews()
    {
        if (beforeTexture == null && afterTexture == null) return;
        if (ImGui.GetContentRegionAvail().X >= 580 && ImGui.BeginTable("photo-previews", 2, ImGuiTableFlags.SizingStretchSame))
        {
            ImGui.TableNextColumn(); DrawImage("当前照片", beforeTexture);
            ImGui.TableNextColumn(); DrawImage("替换后", afterTexture);
            ImGui.EndTable();
        }
        else { DrawImage("当前照片", beforeTexture); DrawImage("替换后", afterTexture); }
    }

    private static void DrawImage(string title, PreviewTexture? texture)
    {
        ImGui.TextUnformatted(title);
        if (texture == null) { ImGui.TextDisabled("尚未生成预览"); return; }
        var width = Math.Max(1, Math.Min(420, ImGui.GetContentRegionAvail().X));
        var height = width * texture.Height / texture.Width;
        if (height > 250) { width *= 250 / height; height = 250; }
        ImGui.Image(texture.Id, new Vector2(width, height));
    }

    private void UploadPreviews()
    {
        if (!updateTextures) return;
        updateTextures = false;
        beforeTexture?.Dispose(); afterTexture?.Dispose(); beforeTexture = afterTexture = null;
        try
        {
            if (beforePixels != null) beforeTexture = PreviewTexture.FromCurrentContext(beforePixels);
            if (afterPixels != null) afterTexture = PreviewTexture.FromCurrentContext(afterPixels);
        }
        catch (Exception failure) { prepared = null; status = "图片预览失败，请重新生成：" + failure.Message; }
    }

    private void InvalidatePreview()
    {
        prepared = null; beforePixels = afterPixels = null;
        // Release textures at the start of the next frame, after this frame's draw
        // commands have been submitted. Never release an SRV referenced by this frame.
        updateTextures = true;
    }
    private void ClearSelection() { selected = null; photos = Array.Empty<PhotoCandidate>(); InvalidatePreview(); }

    private void Run<T>(string message, Func<CancellationToken, T> operation, Action<T> complete)
    {
        if (busy || disposed || context == null || stopping == null) return;
        busy = true; status = message;
        var token = stopping.Token;
        var ui = context.UiBuilder;
        _ = Task.Run(() =>
        {
            try
            {
                var result = operation(token);
                token.ThrowIfCancellationRequested();
                ui.Enqueue(() =>
                {
                    if (disposed) return;
                    busy = false;
                    try { complete(result); }
                    catch (Exception failure) { status = "操作完成，但界面更新失败：" + failure.Message; }
                });
            }
            catch (Exception failure)
            {
                try { ui.Enqueue(() => { if (!disposed) { busy = false; status = "操作失败：" + failure.GetBaseException().Message; } }); }
                catch (ObjectDisposedException) { }
            }
        });
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        if (context != null) { context.UiBuilder.Draw -= Draw; context.UiBuilder.OpenConfigUi -= Open; }
        stopping?.Cancel(); stopping?.Dispose(); stopping = null;
        picker.Close(); beforeTexture?.Dispose(); afterTexture?.Dispose(); beforeTexture = afterTexture = null;
        prepared = null; beforePixels = afterPixels = null;
    }

    public sealed class Options
    {
        public string CacheRoot { get; set; } = "";
    }
}
