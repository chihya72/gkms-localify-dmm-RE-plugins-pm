using System.Numerics;
using ImGuiNET;
using Gkms.Sdk;

namespace Gkms.Plugins.PhotoSwap;

internal sealed class PhotoFilePicker
{
    private static readonly HashSet<string> Extensions = new(StringComparer.OrdinalIgnoreCase)
        { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff" };
    private string directory = "", selected = "", error = "", title = "";
    private string[] folders = Array.Empty<string>(), files = Array.Empty<string>();
    private Action<string>? accept;
    private bool open, requested, folderMode;

    internal void Open(string label, string initial, bool chooseFolder, Action<string> callback)
    {
        title = label; accept = callback; folderMode = chooseFolder;
        directory = Directory.Exists(initial) ? initial : Path.GetDirectoryName(initial) ?? "";
        if (!Directory.Exists(directory)) directory = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        if (!Directory.Exists(directory)) directory = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        selected = ""; open = requested = true;
        Refresh();
    }

    private void Refresh()
    {
        try
        {
            directory = Path.GetFullPath(directory);
            folders = Directory.EnumerateDirectories(directory).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToArray();
            files = folderMode ? Array.Empty<string>() : Directory.EnumerateFiles(directory)
                .Where(p => Extensions.Contains(Path.GetExtension(p))).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToArray();
            error = "";
        }
        catch (Exception failure) { folders = files = Array.Empty<string>(); error = failure.Message; }
    }

    internal void Draw()
    {
        if (!open) return;
        var label = title + "###photoswap-picker";
        if (requested) { ImGui.OpenPopup(label); requested = false; }
        var display = ImGui.GetIO().DisplaySize;
        ImGui.SetNextWindowSize(new Vector2(Math.Max(300, Math.Min(760, display.X - 48)), Math.Max(300, Math.Min(560, display.Y - 48))), ImGuiCond.Appearing);
        if (!ImGui.BeginPopupModal(label, ref open, ImGuiWindowFlags.NoSavedSettings)) return;
        Action<string>? callback = null;
        string? result = null;
        try
        {
            ImGui.TextUnformatted("目录路径");
            ImGui.SetNextItemWidth(Math.Max(120, ImGui.GetContentRegionAvail().X - 80));
            var navigate = ImGui.InputText("##picker-path", ref directory, 4096, ImGuiInputTextFlags.EnterReturnsTrue);
            ImGui.SameLine();
            if (ImGui.Button("前往") || navigate) { selected = ""; Refresh(); }
            if (ImGui.Button("上一级"))
            {
                try { directory = Directory.GetParent(directory)?.FullName ?? directory; selected = ""; Refresh(); }
                catch (Exception failure) { error = failure.Message; }
            }
            ImGui.SameLine();
            if (ImGui.Button("图片目录")) { directory = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures); selected = ""; Refresh(); }
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (ImGui.GetContentRegionAvail().X > 90) ImGui.SameLine();
                if (ImGui.SmallButton(drive.Name)) { directory = drive.Name; selected = ""; Refresh(); }
            }
            string? next = null;
            ImGui.BeginChild("picker-entries", new Vector2(0, Math.Max(90, ImGui.GetContentRegionAvail().Y - 115)), true);
            try
            {
                foreach (var folder in folders)
                    if (ImGui.Selectable("[目录] " + Path.GetFileName(folder))) next = folder;
                foreach (var file in files)
                    if (ImGui.Selectable(Path.GetFileName(file), selected == file)) selected = file;
            }
            finally { ImGui.EndChild(); }
            if (next != null) { directory = next; selected = ""; Refresh(); }
            PluginUiText.Wrapped(folderMode ? directory : selected.Length == 0 ? "选择 JPG、PNG、BMP、GIF 或 TIFF 图片" : Path.GetFileName(selected));
            if (error.Length != 0) PluginUiText.Wrapped("无法读取：" + error);
            ImGui.BeginDisabled(error.Length != 0 || (!folderMode && selected.Length == 0));
            if (ImGui.Button(folderMode ? "使用此目录" : "选择此图片"))
            { callback = accept; result = folderMode ? directory : selected; open = false; ImGui.CloseCurrentPopup(); }
            ImGui.EndDisabled();
            ImGui.SameLine();
            if (ImGui.Button("取消")) { open = false; ImGui.CloseCurrentPopup(); }
        }
        finally { ImGui.EndPopup(); }
        if (result != null) callback?.Invoke(result);
    }

    internal void Close() { open = requested = false; accept = null; }
}
