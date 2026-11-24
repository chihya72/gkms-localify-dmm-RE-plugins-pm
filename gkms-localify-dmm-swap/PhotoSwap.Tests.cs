using System.Reflection;
using System.Text.Json;
using Gkms.Plugins.PhotoSwap;
using Gkms.Sdk;
using Unity.Collections;
using UnityEngine;

[assembly: System.Runtime.Versioning.SupportedOSPlatform("windows")]

namespace Gkms.PhotoSwap.Tests
{
    internal static class Program
    {
        private static int passed;

        private static void Main(string[] args)
        {
            if (args is ["--package", var package]) { PhotoEditorTests.ValidatePackage(package); return; }
            Check("enabling the plugin installs both hooks with default or legacy disabled settings", () =>
            {
                foreach (var json in new[] { "{}", "{\"photoHashBypass\":false}" })
                {
                    var context = new Context(json);
                    using var plugin = new Plugin();
                    plugin.Start(context);
                    Assert(context.Lookups == 1 && context.Hooks.Count == 2);
                }
            });
            Check("obsolete setting values cannot block automatic initialization", () =>
            {
                var context = new Context("{\"photoHashBypass\":\"true\"}");
                using var plugin = new Plugin();
                plugin.Start(context);
                Assert(context.Lookups == 1 && context.Hooks.Count == 2);
            });
            Check("only the two supported three-argument overloads are patched", () =>
            {
                var context = new Context();
                using var plugin = new Plugin();
                plugin.Start(context);
                Assert(context.Hooks.Count == 2);
                Assert(context.Hooks.All(h => h.Target.GetParameters().Length == 3));
                Assert(context.Hooks.Any(h => h.Target.GetParameters()[1].ParameterType == typeof(byte[])));
                Assert(context.Hooks.Any(h => h.Target.GetParameters()[1].ParameterType == typeof(NativeArray<byte>)));
            });
            Check("legacy ASCII hash and capture/save parity", () =>
                AssertHashes("photo_001.png", (2808117765090409184UL, 11512885059505489182UL,
                    2808117765976396661UL, 11512885058386570027UL)));
            Check("legacy UTF-8 hash including supplementary characters", () =>
                AssertHashes("写真_咲季_😀.png", (13570997312246648545UL, 6180925590187734954UL,
                    13570997312979724148UL, 6180925589091801503UL)));
            Check("null and empty filename preserve legacy seeds", () =>
            {
                var expected = (1469598103934665603UL, 11400714819323198485UL,
                    1469598102671125014UL, 11400714820374811168UL);
                AssertHashes(null, expected);
                AssertHashes("", expected);
            });
            Check("different filenames produce different results", () =>
            {
                var context = new Context();
                using var plugin = new Plugin();
                plugin.Start(context);
                var hook = context.Hooks[0];
                Assert(Invoke(hook, "one.png") != Invoke(hook, "two.png"));
            });
            Check("missing game type fails explicitly", () =>
            {
                var context = new Context("{}", null);
                using var plugin = new Plugin();
                Throws<TypeLoadException>(() => plugin.Start(context));
                Assert(context.Hooks.Count == 0);
            });
            Check("missing capture path does not install half a bypass", () =>
                AssertStartupFailure<MissingMethodException>(typeof(MissingCapture)));
            Check("missing save path does not install half a bypass", () =>
                AssertStartupFailure<MissingMethodException>(typeof(MissingSave)));
            Check("NativeArray<int> is not treated as a byte buffer", () =>
                AssertStartupFailure<MissingMethodException>(typeof(WrongBuffer)));
            Check("incompatible return layout fails before patching", () =>
                AssertStartupFailure<NotSupportedException>(typeof(WrongReturn)));
            Check("ambiguous capture path fails before patching", () =>
                AssertStartupFailure<MissingMethodException>(new DuplicateMethods()));
            Check("Dispose tolerates construction and partial Start failure", () =>
            {
                var plugin = new Plugin();
                plugin.Dispose();
                plugin.Dispose();
                var context = new Context { FailOnPatch = 2 };
                var failed = new Plugin();
                Throws<InvalidOperationException>(() => failed.Start(context));
                // Context owns the first registration; the RE host removes it on failure.
                Assert(context.Hooks.Count == 1);
                failed.Dispose();
                failed.Dispose();
                var next = new Context("{}");
                using var restarted = new Plugin();
                restarted.Start(next);
                Assert(next.Hooks.Count == 2);
            });
            Check("package manifest and runtime dependency boundary", () =>
            {
                using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "plugin.json")));
                var manifest = document.RootElement;
                Assert(manifest.GetProperty("apiVersion").GetInt32() == PluginApi.Current);
                Assert(manifest.GetProperty("entryPoint").GetString() == typeof(Plugin).FullName);
                Assert(manifest.GetProperty("assembly").GetString() == typeof(Plugin).Assembly.GetName().Name + ".dll");
                Assert(manifest.GetProperty("version").GetString() == typeof(Plugin).Assembly.GetName().Version!.ToString(3));
                var setting = manifest.GetProperty("settings").EnumerateArray().Single();
                Assert(setting.GetProperty("key").GetString() == "cacheRoot");
                Assert(manifest.GetProperty("settingsUi").GetString() == typeof(PhotoSettingsUi).FullName);
                Assert(typeof(Plugin).Assembly.GetReferencedAssemblies().All(a =>
                    a.Name is "Gkms.Sdk" or "ImGui.NET" || a.Name!.StartsWith("System.", StringComparison.Ordinal)));
            });
            PhotoEditorTests.Run();
            Console.WriteLine($"PASS: {passed} hook/package checks plus editor tests. Native game execution is not covered.");
        }

        private static void AssertHashes(string? name, (ulong, ulong, ulong, ulong) expected)
        {
            var context = new Context();
            using var plugin = new Plugin();
            plugin.Start(context);
            foreach (var hook in context.Hooks)
                Assert(Invoke(hook, name) == expected);
        }

        private static (ulong, ulong, ulong, ulong) Invoke(Hook hook, string? name)
        {
            // ref object is the SDK's representation of a boxed IL2CPP return value.
            object?[] arguments = { name, hook.Target, default(ValueTuple<Hash128, Hash128>) };
            Assert(hook.Prefix.Invoke(null, arguments) is false);
            var pair = ((Hash128, Hash128))arguments[2]!;
            return (pair.Item1.Low, pair.Item1.High, pair.Item2.Low, pair.Item2.High);
        }

        private static void AssertStartupFailure<T>(Type type) where T : Exception
        {
            var context = new Context("{}", type);
            using var plugin = new Plugin();
            Throws<T>(() => plugin.Start(context));
            Assert(context.Hooks.Count == 0);
        }

        private static void Check(string name, Action test)
        {
            test();
            passed++;
            Console.WriteLine("PASS " + name);
        }

        private static void Assert(bool condition)
        {
            if (!condition) throw new InvalidOperationException("Assertion failed.");
        }

        private static void Throws<T>(Action action) where T : Exception
        {
            try { action(); }
            catch (T) { return; }
            throw new InvalidOperationException("Expected " + typeof(T).Name);
        }

        private sealed record Hook(MethodInfo Target, MethodInfo Prefix);
        private sealed class Context : IPluginContext
        {
            private readonly Type? gameType;
            public Context(string settings = "{}") : this(settings, typeof(PhotoUtility)) { }
            public Context(string settings, Type? gameType)
            {
                using var document = JsonDocument.Parse(settings);
                Settings = document.RootElement.Clone();
                this.gameType = gameType;
            }
            public List<Hook> Hooks { get; } = new();
            public int Lookups { get; private set; }
            public int FailOnPatch { get; init; }
            public string DataDirectory => throw new InvalidOperationException("Plugin must not access legacy configuration.");
            public JsonElement Settings { get; }
            public void Log(string message) { }
            public Type? FindType(string assembly, string fullName)
            {
                Assert(assembly == "Assembly-CSharp" && fullName == "Campus.Common.Photo.PhotoUtility");
                Lookups++;
                return gameType;
            }
            public void Patch(MethodInfo target, MethodInfo? prefix = null, MethodInfo? postfix = null, int priority = 400)
            {
                if (Hooks.Count + 1 == FailOnPatch) throw new InvalidOperationException("Simulated registration failure.");
                Assert(prefix != null && postfix == null);
                Hooks.Add(new Hook(target, prefix!));
            }
        }

        private sealed class DuplicateMethods : TypeDelegator
        {
            public DuplicateMethods() : base(typeof(PhotoUtility)) { }
            public override MethodInfo[] GetMethods(BindingFlags flags) =>
                base.GetMethods(flags).Concat(base.GetMethods(flags).Where(m =>
                    m.GetParameters() is var p && p.Length == 3 && p[1].ParameterType == typeof(NativeArray<byte>))).ToArray();
        }

        private static class PhotoUtility
        {
            public static (Hash128, Hash128) GetB(string name, byte[] full, byte[] thumb) => throw new NotSupportedException();
            public static (Hash128, Hash128) GetB(string name, NativeArray<byte> full, NativeArray<byte> thumb) => throw new NotSupportedException();
            public static Hash128 GetB(string name, byte[] data) => throw new NotSupportedException();
            public static (Hash128, Hash128) GetB(string name, NativeArray<int> full, NativeArray<int> thumb) => throw new NotSupportedException();
        }
        private static class MissingCapture
        {
            public static (Hash128, Hash128) GetB(string name, byte[] full, byte[] thumb) => default;
        }
        private static class MissingSave
        {
            public static (Hash128, Hash128) GetB(string name, NativeArray<byte> full, NativeArray<byte> thumb) => default;
        }
        private static class WrongBuffer
        {
            public static (Hash128, Hash128) GetB(string name, byte[] full, byte[] thumb) => default;
            public static (Hash128, Hash128) GetB(string name, NativeArray<int> full, NativeArray<int> thumb) => default;
        }
        private static class WrongReturn
        {
            public static (int, int) GetB(string name, byte[] full, byte[] thumb) => default;
            public static (int, int) GetB(string name, NativeArray<byte> full, NativeArray<byte> thumb) => default;
        }
    }
}

// Managed fixtures expose the same field names and generic signatures as game metadata.
namespace UnityEngine
{
    public struct Hash128
    {
#pragma warning disable CS0649 // Assigned through the same SDK reflection used for native fields.
        private ulong u64_0;
        private ulong u64_1;
#pragma warning restore CS0649
        public readonly ulong Low => u64_0;
        public readonly ulong High => u64_1;
    }
}

namespace Unity.Collections
{
    public struct NativeArray<T> where T : struct { }
}
