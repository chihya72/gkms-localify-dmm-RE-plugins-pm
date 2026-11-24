using System.Reflection;
using System.Text;
using Gkms.Sdk;

namespace Gkms.Plugins.PhotoSwap;

public sealed class Plugin : ILocalifyPlugin
{
    private const string PhotoUtility = "Campus.Common.Photo.PhotoUtility";

    public void Start(IPluginContext context)
    {
        var methods = context.RequireType("Assembly-CSharp", PhotoUtility).GetMethods(GameMethods.Declared);
        // Resolve both paths before registering either: capture uses NativeArray<byte>,
        // album validation uses byte[]. The two-argument helper must remain untouched.
        var capture = RequireOverload(methods, IsNativeByteArray, "NativeArray<byte>");
        var save = RequireOverload(methods, Interop.IsByteArray, "byte[]");
        ValidateResult(capture.ReturnType);
        ValidateResult(save.ReturnType);
        var prefix = GameMethods.Handler<Plugin>(nameof(NameOnlyHash));
        context.Patch(capture, prefix: prefix);
        context.Patch(save, prefix: prefix);
    }

    private static MethodInfo RequireOverload(IEnumerable<MethodInfo> methods, Func<Type, bool> isBuffer, string bufferName)
    {
        var candidates = methods.Where(method => method.Name == "GetB" && method.IsStatic &&
            !method.ContainsGenericParameters && method.GetParameters() is var parameters &&
            parameters.Length == 3 && parameters[0].ParameterType == typeof(string) &&
            isBuffer(parameters[1].ParameterType) && isBuffer(parameters[2].ParameterType)).ToArray();
        return candidates.Length == 1 ? candidates[0] : throw new MissingMethodException(
            $"需要唯一的 {PhotoUtility}.GetB(string, {bufferName}, {bufferName})，找到 {candidates.Length} 个。请检查游戏版本。");
    }

    private static bool IsNativeByteArray(Type type) => type.IsValueType && type.IsGenericType &&
        !type.ContainsGenericParameters && type.GetGenericTypeDefinition().FullName == "Unity.Collections.NativeArray`1" &&
        type.GetGenericArguments().SequenceEqual(new[] { typeof(byte) });

    private static void ValidateResult(Type type)
    {
        if (!type.IsValueType || !type.IsGenericType ||
            type.GetGenericTypeDefinition().FullName != "System.ValueTuple`2")
            throw new NotSupportedException($"GetB 返回类型不是 ValueTuple<Hash128, Hash128>：{type.FullName}");
        foreach (var name in new[] { "Item1", "Item2" })
        {
            var hashType = RequireField(type, name).FieldType;
            if (!hashType.IsValueType || hashType.FullName != "UnityEngine.Hash128" ||
                RequireField(hashType, "u64_0").FieldType != typeof(ulong) ||
                RequireField(hashType, "u64_1").FieldType != typeof(ulong))
                throw new NotSupportedException($"GetB.{name} 的 Hash128 布局不兼容。请检查游戏版本。");
        }
    }

    private static FieldInfo RequireField(Type type, string name) =>
        type.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new MissingFieldException(type.FullName, name);

    public static bool NameOnlyHash(string? __0, MethodInfo __originalMethod, ref object __result)
    {
        var (a, b, c, d) = PhotoHash.Compute(__0);
        // Construct native boxed values via the SDK. A CLR ValueTuple/Hash128 cannot
        // be marshalled as an IL2CPP value, and native pointers must not be retained.
        var pairType = __originalMethod.ReturnType;
        var pair = Interop.New(pairType) ?? throw new InvalidOperationException("无法创建照片哈希返回值。");
        WriteHash(pair, RequireField(pairType, "Item1"), a, b);
        WriteHash(pair, RequireField(pairType, "Item2"), c, d);
        __result = pair;
        return false;
    }

    private static void WriteHash(object pair, FieldInfo item, ulong low, ulong high)
    {
        var hash = Interop.New(item.FieldType) ?? throw new InvalidOperationException("无法创建 Hash128。");
        Interop.Set(hash, "u64_0", low);
        Interop.Set(hash, "u64_1", high);
        // Nested structs are boxed copies: explicitly write each hash back to the tuple.
        item.SetValue(pair, hash);
    }

    // The host removes all context-owned hooks before Dispose, including failed Start.
    // This plugin retains no game objects, global settings or unmanaged resources.
    public void Dispose() { }
}

internal static class PhotoHash
{
    internal static (ulong A, ulong B, ulong C, ulong D) Compute(string? name)
    {
        // Preserve the legacy C++ algorithm exactly, including its nonstandard seed.
        ulong h1 = 1469598103934665603UL, h2 = 0x9E3779B97F4A7C15UL;
        unchecked
        {
            if (name != null)
                foreach (var ch in Encoding.UTF8.GetBytes(name))
                {
                    h1 = (h1 ^ ch) * 1099511628211UL;
                    h2 = ((h2 ^ ch) * 1099511628211UL) ^ (h2 >> 29);
                }
            return (h1, h2, h1 ^ 0x5bd1e995UL, h2 ^ 0xc2b2ae35UL);
        }
    }
}
