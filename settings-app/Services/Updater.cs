using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http.Headers;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace HctpSettings.Services;

/// <summary>A release on GitHub: its version, its notes and the zip a player downloads.</summary>
public sealed record Release(Version Version, string Tag, string Notes, string PageUrl, string ZipUrl, long ZipSize, string? Sha256);

/// <summary>Why an update stopped: the words the player sees, and which kind it was (the advice differs).</summary>
public sealed class UpdateException : Exception
{
    public UpdateException(string kind, string message) : base(message) => Kind = kind;
    /// <summary>download | damaged | build | busy | interrupted | other</summary>
    public string Kind { get; }
}

/// <summary>What an update is doing, kept in <c>files\update\journal.json</c> so that a closed program or a PC turned off
/// halfway can be undone (or the rebuild carried on) the next time this program opens.</summary>
public sealed class UpdateJournal
{
    public string From { get; set; } = "";
    public string To { get; set; } = "";
    public string Tag { get; set; } = "";
    public string Notes { get; set; } = "";
    public string PageUrl { get; set; } = "";
    /// <summary>applying (files being put in place) | rebuild (in place, the game being rebuilt) | failed (undone)</summary>
    public string State { get; set; } = "";
    public string FailKind { get; set; } = "";
    public string FailReason { get; set; } = "";
    public List<UpdateEntry> Entries { get; set; } = new();
}

/// <summary>One file of an update, by its path in the install folder: changed, added or removed.</summary>
public sealed class UpdateEntry
{
    public string Path { get; set; } = "";
    public string Kind { get; set; } = "";
}

/// <summary>Updates from this project's releases on GitHub. A release's zip lists its files with their checksums
/// (<c>files\release-manifest.txt</c>); an update replaces only the files that differ (new ones get the current time,
/// so the build sees them; unchanged ones keep theirs, so only what changed is rebuilt), keeps every file it replaces
/// until the game is rebuilt, and puts them all back if anything fails. Nothing the setup made (the game's files from
/// the disc, the saves, settings.toml, mods, the build folders, the generated code) is in a release, so nothing of it
/// is touched. The program's own file can be renamed while it runs: the new one starts after the swap.</summary>
public static class Updater
{
    public const string Repo = "BigDaddyJRich/hctp-recomp";
    public const string ReleasesUrl = "https://github.com/" + Repo + "/releases";
    public const string ZipName = "HCTP-Recompiled-Windows.zip";
    public const string ManifestName = "release-manifest.txt";
    public const string ProgramName = "HCTP Recompiled Settings.exe";
    private const string Top = "HCTP Recompiled/";
    private const string ManifestPath = "files/" + ManifestName;

    public static Version Current { get; } = Normalise(Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 1, 0));

    public static string CurrentText => Current.ToString(3);

    /// <summary>Where the newest release is asked for. HCTP_UPDATE_URL (testing): another address answering the same
    /// way, such as a server on this PC.</summary>
    private static string LatestUrl =>
        Environment.GetEnvironmentVariable("HCTP_UPDATE_URL") is { Length: > 0 } url
            ? url
            : $"https://api.github.com/repos/{Repo}/releases/latest";

    private static readonly HttpClient Http = MakeClient();

    private static HttpClient MakeClient()
    {
        var client = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("HCTPRecompiled", CurrentText));
        return client;
    }

    private static Version Normalise(Version v) => new(v.Major, v.Minor, Math.Max(0, v.Build));

    public static bool TryParseVersion(string tag, out Version version)
    {
        version = new Version(0, 0, 0);
        if (!Version.TryParse(tag.Trim().TrimStart('v', 'V'), out var parsed))
            return false;
        version = Normalise(parsed);
        return true;
    }

    // --- 1. is there a newer one? ---

    /// <summary>The newest release with the zip attached, or null when there is none yet. Throws when it can't ask.</summary>
    public static async Task<Release?> CheckAsync(CancellationToken cancel = default)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        timeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var request = new HttpRequestMessage(HttpMethod.Get, LatestUrl);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        using var response = await Http.SendAsync(request, timeout.Token);
        if (response.StatusCode == System.Net.HttpStatusCode.NotFound)
            return null;
        response.EnsureSuccessStatusCode();
        using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(timeout.Token));
        var root = doc.RootElement;
        var tag = Text(root, "tag_name");
        if (!TryParseVersion(tag, out var version))
            return null;
        string? url = null;
        string? sha = null;
        long size = 0;
        if (root.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
        {
            foreach (var asset in assets.EnumerateArray())
            {
                if (Text(asset, "name") != ZipName)
                    continue;
                url = Text(asset, "browser_download_url");
                size = asset.TryGetProperty("size", out var s) && s.TryGetInt64(out var n) ? n : 0;
                var digest = Text(asset, "digest");
                if (digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase))
                    sha = digest[7..].ToLowerInvariant();
            }
        }
        if (string.IsNullOrEmpty(url))
            return null;
        var page = Text(root, "html_url");
        return new Release(version, tag, Text(root, "body"), page.Length > 0 ? page : ReleasesUrl, url, size, sha);
    }

    private static string Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

    // --- 2. the download ---

    public static string ZipPath(GameFolder folder) => Path.Combine(UpdateDir(folder), ZipName);

    /// <summary>Downloads the zip, then checks its size and (when GitHub gives one) its checksum.</summary>
    public static async Task DownloadAsync(Release release, string path, Action<long, long> progress, CancellationToken cancel)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var part = path + ".part";
        try
        {
            using var response = await Http.GetAsync(release.ZipUrl, HttpCompletionOption.ResponseHeadersRead, cancel);
            response.EnsureSuccessStatusCode();
            var total = release.ZipSize > 0 ? release.ZipSize : response.Content.Headers.ContentLength ?? 0;
            await using var input = await response.Content.ReadAsStreamAsync(cancel);
            await using var output = File.Create(part);
            var buffer = new byte[1 << 16];
            long done = 0;
            while (true)
            {
                // a connection that goes quiet counts as dropped
                using var stall = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                stall.CancelAfter(TimeSpan.FromSeconds(30));
                int read;
                try
                {
                    read = await input.ReadAsync(buffer, stall.Token);
                }
                catch (OperationCanceledException) when (!cancel.IsCancellationRequested)
                {
                    throw new UpdateException("download", "The download stopped: nothing arrived for 30 seconds.");
                }
                if (read == 0)
                    break;
                await output.WriteAsync(buffer.AsMemory(0, read), cancel);
                done += read;
                progress(done, total);
            }
        }
        catch (HttpRequestException ex)
        {
            throw new UpdateException("download", "The download stopped: " + (ex.StatusCode is { } code
                ? $"the server answered {(int)code}."
                : "no connection to the internet."));
        }
        catch (IOException ex) when (ex is not FileNotFoundException)
        {
            throw new UpdateException("download", "The download stopped: " + ex.Message);
        }
        var size = new FileInfo(part).Length;
        if (release.ZipSize > 0 && size != release.ZipSize)
            throw new UpdateException("damaged", $"The download is incomplete ({size:N0} of {release.ZipSize:N0} bytes).");
        if (release.Sha256 != null && !HashFile(part).Equals(release.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new UpdateException("damaged", "The download was damaged: it doesn't match the release's checksum.");
        File.Move(part, path, overwrite: true);
    }

    // --- 3. putting it in place ---

    /// <summary>Works out which files differ, writes the journal, then replaces them one by one (each new file checked
    /// against the list's checksum first). On any problem everything is put back and an UpdateException thrown.
    /// Returns true when this program's own file was replaced (it has to start again).</summary>
    public static bool Apply(GameFolder folder, string zipPath, Release release, Action<double> progress)
    {
        using var zip = OpenZip(zipPath);
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in zip.Entries)
            if (entry.FullName.StartsWith(Top, StringComparison.Ordinal) && !entry.FullName.EndsWith('/'))
                entries[entry.FullName[Top.Length..]] = entry;
        if (!entries.TryGetValue(ManifestPath, out var manifestEntry))
            throw new UpdateException("damaged", "The download has no list of its files.");
        Dictionary<string, (string Hash, long Size)> wanted;
        using (var stream = manifestEntry.Open())
            wanted = ReadManifest(stream);
        if (wanted.Count == 0 || wanted.Keys.Any(p => !entries.ContainsKey(p)))
            throw new UpdateException("damaged", "The download is missing some of its files.");

        var installed = File.Exists(InstalledManifest(folder))
            ? ReadManifest(File.OpenRead(InstalledManifest(folder)))
            : new Dictionary<string, (string Hash, long Size)>();
        var journal = new UpdateJournal
        {
            From = CurrentText,
            To = release.Version.ToString(3),
            Tag = release.Tag,
            Notes = release.Notes,
            PageUrl = release.PageUrl,
            State = "applying",
        };
        foreach (var (path, (hash, size)) in wanted)
        {
            var target = Full(folder, path);
            if (!File.Exists(target))
                journal.Entries.Add(new UpdateEntry { Path = path, Kind = "added" });
            else if (new FileInfo(target).Length != size || !HashFile(target).Equals(hash, StringComparison.OrdinalIgnoreCase))
                journal.Entries.Add(new UpdateEntry { Path = path, Kind = "changed" });
        }
        foreach (var path in installed.Keys.Where(p => !wanted.ContainsKey(p)))
            if (File.Exists(Full(folder, path)))
                journal.Entries.Add(new UpdateEntry { Path = path, Kind = "removed" });
        // the list itself last: the install's list says what is in place
        journal.Entries.Add(new UpdateEntry { Path = ManifestPath, Kind = File.Exists(InstalledManifest(folder)) ? "changed" : "added" });
        Log(folder, $"update {journal.From} -> {journal.To}: {journal.Entries.Count(e => e.Kind == "changed")} changed, " +
                    $"{journal.Entries.Count(e => e.Kind == "added")} added, {journal.Entries.Count(e => e.Kind == "removed")} removed");
        SaveJournal(folder, journal);

        try
        {
            for (int i = 0; i < journal.Entries.Count; i++)
            {
                var e = journal.Entries[i];
                var target = Full(folder, e.Path);
                var fresh = target + ".update-new";
                if (e.Kind != "removed")
                    Extract(entries[e.Path], fresh, e.Path == ManifestPath ? null : wanted[e.Path].Hash);
                if (e.Kind != "added" && File.Exists(target))
                {
                    var backup = Backup(folder, e.Path);
                    Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                    File.Move(target, backup, overwrite: true);
                }
                if (e.Kind != "removed")
                    File.Move(fresh, target, overwrite: true);
                progress((i + 1) / (double)journal.Entries.Count);
            }
        }
        catch (Exception ex)
        {
            Log(folder, "putting the files in place failed: " + ex);
            RollBack(folder, journal);
            DeleteJournal(folder);
            throw ex as UpdateException ?? new UpdateException("other", "Couldn't put the new files in place: " + ex.Message);
        }
        journal.State = "rebuild";
        SaveJournal(folder, journal);
        return journal.Entries.Any(e => IsProgram(e.Path));
    }

    private static ZipArchive OpenZip(string path)
    {
        try
        {
            return ZipFile.OpenRead(path);
        }
        catch (InvalidDataException)
        {
            throw new UpdateException("damaged", "The download was damaged: it isn't a readable zip.");
        }
    }

    private static void Extract(ZipArchiveEntry entry, string path, string? hash)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        try
        {
            using var input = entry.Open();
            using var output = File.Create(path);
            var buffer = new byte[1 << 16];
            int read;
            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
            {
                sha.AppendData(buffer, 0, read);
                output.Write(buffer, 0, read);
            }
        }
        catch (InvalidDataException)
        {
            File.Delete(path);
            throw new UpdateException("damaged", $"The download was damaged ({entry.Name} couldn't be unpacked).");
        }
        if (hash != null && !Convert.ToHexStringLower(sha.GetHashAndReset()).Equals(hash, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(path);
            throw new UpdateException("damaged", $"The download was damaged ({entry.Name} doesn't match its checksum).");
        }
    }

    /// <summary>Puts back every file the journal replaced, removes the ones it added, and marks the ones put back as
    /// changed now (so the next build uses them again). Returns true when this program's file was put back.</summary>
    public static bool RollBack(GameFolder folder, UpdateJournal journal)
    {
        var programBack = false;
        foreach (var e in Enumerable.Reverse(journal.Entries))
        {
            var target = Full(folder, e.Path);
            var backup = Backup(folder, e.Path);
            TryDelete(target + ".update-new");
            if (e.Kind == "added")
            {
                if (File.Exists(target))
                    MoveAside(folder, target);
                continue;
            }
            if (!File.Exists(backup))
                continue; // never replaced: the old one is still in place
            if (File.Exists(target))
                MoveAside(folder, target);
            File.Move(backup, target);
            File.SetLastWriteTimeUtc(target, DateTime.UtcNow);
            programBack |= IsProgram(e.Path);
        }
        Log(folder, $"update {journal.From} -> {journal.To} undone");
        TryDeleteDir(Path.Combine(UpdateDir(folder), "backup"));
        return programBack;
    }

    /// <summary>After a good rebuild: the old files, the download and the journal go.</summary>
    public static void Finish(GameFolder folder)
    {
        TryDeleteDir(Path.Combine(UpdateDir(folder), "backup"));
        TryDelete(ZipPath(folder));
        DeleteJournal(folder);
    }

    /// <summary>At start: what a previous run left (files moved aside, an old download) goes.</summary>
    public static void CleanUp(GameFolder folder)
    {
        TryDeleteDir(Path.Combine(UpdateDir(folder), "aside"));
        if (LoadJournal(folder) == null)
        {
            TryDelete(ZipPath(folder) + ".part");
            TryDelete(ZipPath(folder));
        }
    }

    /// <summary>Starts this program again (its new file after an update, its old one after an undo) and closes this one.</summary>
    public static void Restart(GameFolder folder)
    {
        var program = Path.Combine(folder.Root, ProgramName);
        if (!File.Exists(program) && Environment.ProcessPath is { } self)
            program = self;
        Process.Start(new ProcessStartInfo(program) { UseShellExecute = true, WorkingDirectory = folder.Root });
    }

    // --- the journal, the lists, the paths ---

    public static UpdateJournal? LoadJournal(GameFolder folder)
    {
        try
        {
            var path = JournalPath(folder);
            return File.Exists(path) ? JsonSerializer.Deserialize<UpdateJournal>(File.ReadAllText(path)) : null;
        }
        catch
        {
            return null;
        }
    }

    public static void SaveJournal(GameFolder folder, UpdateJournal journal)
    {
        Directory.CreateDirectory(UpdateDir(folder));
        var path = JournalPath(folder);
        File.WriteAllText(path + ".new", JsonSerializer.Serialize(journal, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(path + ".new", path, overwrite: true);
    }

    public static void DeleteJournal(GameFolder folder) => TryDelete(JournalPath(folder));

    /// <summary>A player's install (made from a release zip, with its list of files) can update itself; a developer's
    /// copy of the repository can't (git does that).</summary>
    public static bool CanUpdate(GameFolder folder) => folder.Data != folder.Root && File.Exists(InstalledManifest(folder));

    public static string LogPath(GameFolder folder) => Path.Combine(folder.LogsPath, "update.log");

    public static void Log(GameFolder folder, string line)
    {
        try
        {
            Directory.CreateDirectory(folder.LogsPath);
            File.AppendAllText(LogPath(folder), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
        }
        catch
        {
            // the log is only a help
        }
    }

    /// <summary>"sha256 size path" lines (paths below the zip's top folder, with /), # comments.</summary>
    private static Dictionary<string, (string Hash, long Size)> ReadManifest(Stream stream)
    {
        var files = new Dictionary<string, (string, long)>(StringComparer.OrdinalIgnoreCase);
        using var reader = new StreamReader(stream);
        string? line;
        while ((line = reader.ReadLine()) != null)
        {
            if (line.Length == 0 || line[0] == '#')
                continue;
            var parts = line.Split(' ', 3);
            if (parts.Length == 3 && long.TryParse(parts[1], out var size) && !parts[2].Contains(".."))
                files[parts[2]] = (parts[0], size);
        }
        return files;
    }

    private static bool IsProgram(string path) => path.Equals(ProgramName, StringComparison.OrdinalIgnoreCase);
    private static string UpdateDir(GameFolder folder) => Path.Combine(folder.Data, "update");
    private static string JournalPath(GameFolder folder) => Path.Combine(UpdateDir(folder), "journal.json");
    private static string InstalledManifest(GameFolder folder) => Path.Combine(folder.Root, "files", ManifestName);
    private static string Full(GameFolder folder, string path) => Path.Combine(folder.Root, path.Replace('/', Path.DirectorySeparatorChar));
    private static string Backup(GameFolder folder, string path) =>
        Path.Combine(UpdateDir(folder), "backup", path.Replace('/', Path.DirectorySeparatorChar));

    /// <summary>Out of the way, into update\aside (a running program can be moved, not deleted); emptied at the next start.</summary>
    private static void MoveAside(GameFolder folder, string path)
    {
        var aside = Path.Combine(UpdateDir(folder), "aside", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(aside);
        File.Move(path, Path.Combine(aside, Path.GetFileName(path)));
    }

    public static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }

    private static void TryDeleteDir(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, recursive: true); } catch { }
    }
}
