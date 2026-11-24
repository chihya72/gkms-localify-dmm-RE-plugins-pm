using System.Drawing;
using System.IO.Compression;
using System.Text.Json;

namespace Gkms.Plugins.PhotoSwap;

internal sealed record PhotoCandidate(string Section, string Name, string FullPath, string? ThumbnailPath,
    string Format, int Width, int Height, DateTime ModifiedUtc);
internal sealed record ScanResult(PhotoCandidate[] Photos, string[] Warnings);
internal sealed record PhotoChange(string RelativePath, byte[] Original, byte[] Replacement);
internal sealed record PreparedPhoto(string CacheRoot, PhotoCandidate Photo, PhotoChange[] Changes,
    ImagePixels OriginalPreview, ImagePixels ReplacementPreview);
internal sealed record BackupInfo(string Path, string Name, DateTime CreatedUtc);

internal static class PhotoFiles
{
    private static readonly string[] Sections = { "OutGame", "Produce", "MemoryReview" };
    internal static string DefaultCacheRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        "AppData", "LocalLow", "BANDAI NAMCO Entertainment Inc_", "gakumas", "07578AE1495C51B9C5AA9A74E48AD00A");
    // UI sessions have no StorageDirectory service. Keep UI-owned backups in their
    // own stable user-data location, independent of versioned plugin installations.
    internal static string DefaultBackupRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Gkms.PhotoSwap", "backups");

    internal static ScanResult Scan(string root, CancellationToken token)
    {
        root = Path.GetFullPath(root);
        var photos = new List<PhotoCandidate>();
        var warnings = new List<string>();
        if (!Directory.Exists(root)) return new ScanResult(Array.Empty<PhotoCandidate>(), new[] { "缓存目录不存在。请先拍照并停在保存选择界面。" });
        foreach (var section in Sections)
        {
            token.ThrowIfCancellationRequested();
            var directory = Path.Combine(root, section);
            if (!Directory.Exists(directory)) continue;
            try
            {
                RejectLinks(directory);
                var files = Directory.GetFiles(directory).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToArray();
                foreach (var path in files)
                {
                    token.ThrowIfCancellationRequested();
                    var stem = Path.GetFileNameWithoutExtension(path);
                    if (stem.EndsWith("_t", StringComparison.OrdinalIgnoreCase)) continue;
                    try
                    {
                        RejectLinks(path);
                        var bytes = PhotoImages.ReadFile(path);
                        var format = PhotoImages.Format(bytes);
                        if (format == null) continue;
                        using var image = PhotoImages.Decode(bytes);
                        var thumbs = files.Where(p => Path.GetFileNameWithoutExtension(p).Equals(stem + "_t", StringComparison.OrdinalIgnoreCase)).ToArray();
                        if (thumbs.Length > 1) throw new InvalidDataException("同名缩略图不唯一，请检查缓存。");
                        var thumb = thumbs.SingleOrDefault();
                        if (thumb != null)
                        {
                            RejectLinks(thumb);
                            var thumbBytes = PhotoImages.ReadFile(thumb);
                            if (PhotoImages.Format(thumbBytes) == null) throw new InvalidDataException("缩略图不是 JPEG/PNG。");
                            using var thumbImage = PhotoImages.Decode(thumbBytes);
                        }
                        photos.Add(new PhotoCandidate(section, Path.GetFileName(path), path, thumb, format,
                            image.Width, image.Height, File.GetLastWriteTimeUtc(path)));
                    }
                    catch (Exception error) when (error is not OperationCanceledException)
                    { warnings.Add(Path.GetFileName(path) + "：" + error.Message); }
                }
            }
            catch (Exception error) when (error is not OperationCanceledException) { warnings.Add(section + "：" + error.Message); }
        }
        return new ScanResult(photos.OrderByDescending(p => p.ModifiedUtc).ToArray(), warnings.ToArray());
    }

    internal static PreparedPhoto Prepare(string root, PhotoCandidate photo, string source, FitMode mode, Color background, CancellationToken token)
    {
        root = Path.GetFullPath(root);
        token.ThrowIfCancellationRequested();
        using var input = PhotoImages.Decode(PhotoImages.ReadFile(source), orient: true);
        var changes = new List<PhotoChange>();
        ImagePixels? before = null, after = null;
        foreach (var path in new[] { photo.FullPath, photo.ThumbnailPath }.OfType<string>())
        {
            token.ThrowIfCancellationRequested();
            var relative = Path.GetRelativePath(root, path);
            var target = Target(root, relative);
            var original = PhotoImages.ReadFile(target);
            var format = PhotoImages.Format(original) ?? throw new InvalidDataException("缓存格式发生变化，请重新扫描。");
            using var image = PhotoImages.Decode(original);
            using var fitted = PhotoImages.Fit(input, image.Size, mode, background);
            var replacement = PhotoImages.Encode(fitted, format);
            using var decoded = PhotoImages.Decode(replacement);
            if (decoded.Size != image.Size) throw new InvalidDataException("生成图片尺寸不匹配。");
            if (before == null) { before = PhotoImages.Preview(image); after = PhotoImages.Preview(decoded); }
            changes.Add(new PhotoChange(relative, original, replacement));
        }
        token.ThrowIfCancellationRequested();
        return new PreparedPhoto(root, photo, changes.ToArray(), before!, after!);
    }

    internal static string Apply(PreparedPhoto prepared, string backupRoot, CancellationToken token) =>
        Commit(prepared.CacheRoot, prepared.Changes, backupRoot, token);

    internal static BackupInfo[] ListBackups(string root) => !Directory.Exists(root) ? Array.Empty<BackupInfo>() :
        Directory.GetFiles(root, "*.zip").Select(p => new BackupInfo(p, Path.GetFileNameWithoutExtension(p), File.GetLastWriteTimeUtc(p)))
            .OrderByDescending(b => b.CreatedUtc).ToArray();

    internal static string Restore(string archivePath, string cacheRoot, string backupRoot, CancellationToken token)
    {
        cacheRoot = Path.GetFullPath(cacheRoot);
        var changes = new List<PhotoChange>();
        using (var archive = ZipFile.OpenRead(archivePath))
        {
            var rootEntry = archive.GetEntry("cache-root.json") ?? throw new InvalidDataException("不是本插件创建的备份。");
            if (rootEntry.Length > 16384) throw new InvalidDataException("备份目录记录无效。");
            using var reader = new StreamReader(rootEntry.Open());
            var originalRoot = JsonSerializer.Deserialize<string>(reader.ReadToEnd());
            if (!string.Equals(cacheRoot.TrimEnd(Path.DirectorySeparatorChar), originalRoot?.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("备份属于另一个缓存目录，请切回原缓存目录后还原。");
            var entries = archive.Entries.Where(e => e.FullName != "cache-root.json").ToArray();
            if (entries.Length is < 1 or > 2 || entries.Select(e => e.FullName).Distinct(StringComparer.OrdinalIgnoreCase).Count() != entries.Length)
                throw new InvalidDataException("备份必须包含一张原图及至多一张缩略图。");
            foreach (var entry in entries)
            {
                token.ThrowIfCancellationRequested();
                var path = Target(cacheRoot, entry.FullName);
                if (!File.Exists(path)) throw new FileNotFoundException("照片缓存已离开待保存状态，不能还原已消失的缓存。", path);
                if (entry.Length > PhotoImages.MaxFileBytes) throw new InvalidDataException("备份图片过大。");
                using var input = entry.Open();
                using var output = new MemoryStream();
                var buffer = new byte[81920];
                int count;
                while ((count = input.Read(buffer, 0, buffer.Length)) != 0)
                {
                    token.ThrowIfCancellationRequested();
                    if (output.Length + count > PhotoImages.MaxFileBytes) throw new InvalidDataException("备份图片过大。");
                    output.Write(buffer, 0, count);
                }
                var bytes = output.ToArray();
                if (PhotoImages.Format(bytes) == null) throw new InvalidDataException("备份中含非照片文件。");
                using var restoredImage = PhotoImages.Decode(bytes);
                var current = PhotoImages.ReadFile(path);
                if (PhotoImages.Format(current) == null) throw new InvalidDataException("目标已不是照片缓存。");
                using var currentImage = PhotoImages.Decode(current);
                if (currentImage.Size != restoredImage.Size) throw new InvalidDataException("当前缓存尺寸与备份不同，请重新扫描。");
                changes.Add(new PhotoChange(entry.FullName, current, bytes));
            }
        }
        // Restoring also backs up the current files, so restoration itself can be undone.
        return Commit(cacheRoot, changes.ToArray(), backupRoot, token);
    }

    private static string Commit(string root, PhotoChange[] changes, string backupRoot, CancellationToken token)
    {
        if (changes.Length is < 1 or > 2) throw new InvalidDataException("只能处理一组原图及缩略图。");
        var targets = changes.Select(c => Target(root, c.RelativePath)).ToArray();
        if (targets.Distinct(StringComparer.OrdinalIgnoreCase).Count() != targets.Length) throw new InvalidDataException("重复的照片路径。");
        var streams = new List<FileStream>();
        var touched = 0;
        string? backup = null;
        try
        {
            for (var i = 0; i < changes.Length; i++)
            {
                token.ThrowIfCancellationRequested();
                // FileMode.Open never resurrects a cache removed by the game. Keep both
                // files exclusively locked through backup, commit, and any rollback.
                var stream = new FileStream(targets[i], FileMode.Open, FileAccess.ReadWrite, FileShare.None);
                streams.Add(stream);
                if (stream.Length != changes[i].Original.Length) throw new IOException("缓存已变化，请重新扫描并预览。");
                var current = new byte[changes[i].Original.Length];
                var offset = 0;
                while (offset < current.Length)
                {
                    var read = stream.Read(current, offset, current.Length - offset);
                    if (read == 0) throw new EndOfStreamException("缓存读取不完整。");
                    offset += read;
                }
                if (!current.AsSpan().SequenceEqual(changes[i].Original)) throw new IOException("缓存已变化，请重新扫描并预览。");
            }
            token.ThrowIfCancellationRequested();
            Directory.CreateDirectory(backupRoot);
            backup = Path.Combine(backupRoot, DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + "-" + Guid.NewGuid().ToString("N")[..8] + ".zip");
            using (var archive = ZipFile.Open(backup, ZipArchiveMode.Create))
            {
                using (var writer = new StreamWriter(archive.CreateEntry("cache-root.json").Open()))
                    writer.Write(JsonSerializer.Serialize(Path.GetFullPath(root)));
                foreach (var change in changes)
                {
                    using var destination = archive.CreateEntry(change.RelativePath.Replace('\\', '/'), CompressionLevel.NoCompression).Open();
                    destination.Write(change.Original);
                }
            }
            token.ThrowIfCancellationRequested();
            for (var i = 0; i < changes.Length; i++)
            {
                // Once commit begins, complete or roll back even if the UI closes.
                touched = i + 1;
                Write(streams[i], changes[i].Replacement);
            }
            return backup;
        }
        catch (Exception failure)
        {
            var errors = new List<Exception> { failure };
            for (var i = 0; i < touched; i++)
            {
                try { Write(streams[i], changes[i].Original); }
                catch (Exception rollback) { errors.Add(rollback); }
            }
            if (errors.Count > 1) throw new AggregateException("写入失败且自动还原未完成，请保留备份并手动还原：" + backup, errors);
            throw;
        }
        finally { foreach (var stream in streams) stream.Dispose(); }
    }

    private static void Write(FileStream stream, byte[] bytes)
    {
        stream.Position = 0;
        stream.Write(bytes);
        stream.SetLength(bytes.Length);
        stream.Flush(flushToDisk: true);
    }

    internal static string Target(string root, string relative)
    {
        var parts = relative.Replace('\\', '/').Split('/');
        if (parts.Length != 2 || !Sections.Contains(parts[0], StringComparer.Ordinal) ||
            string.IsNullOrWhiteSpace(parts[1]) || parts[1] is "." or ".." ||
            parts[1].IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || parts[1].EndsWith('.') || parts[1].EndsWith(' '))
            throw new InvalidDataException("照片路径必须位于 OutGame、Produce 或 MemoryReview 的直接子级。");
        var path = Path.GetFullPath(Path.Combine(root, parts[0], parts[1]));
        RejectLinks(path);
        return path;
    }

    private static void RejectLinks(string path)
    {
        for (var current = Path.GetFullPath(path); !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
            if ((File.Exists(current) || Directory.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("照片缓存不能位于符号链接或目录联接中。");
    }
}
